#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aambundle.h"
#include "aavm.h"

// ============================================================================
// USTY chunk generation: precomputed style table for the 6502 engines.
//
// Parses the LOOK chunk (CSS declarations) once, in C, and emits the compact
// binary table defined in STYLE_SPEC.md. The 6502 engines will later read
// this instead of parsing CSS at boot. For now the LOOK chunk stays in
// place; this only adds a chunk.
//
// The parser mirrors both the dialog compiler's effective CSS subset
// (~/if/dialog/src/frontend.c:2899-3000) and the 6502 engine's parser
// (initengine4 in engine.s): case-insensitive keys, unknown properties
// ignored, width/height accept % (relative) and em/ch/en (absolute), margins
// absolute only, fractional units truncated.
// ============================================================================

#define STY_VERSION	0x04
#define STY_TAG_AAMBOX  (0x00 | STY_VERSION)
#define STY_TAG_C64     (0x10 | STY_VERSION)
#define STY_TAG_APPLE2  (0x20 | STY_VERSION)

// The per-class index tables hold byte offsets rather than slot numbers, so
// the last record of each array must start below 256.
#define STY_MAXGEO	32      // (STY_MAXGEO - 1) * 8 < 256
#define STY_MAXSTY	64      // (STY_MAXSTY - 1) * 4 < 256

#define STY_RELW  0x01
#define STY_RELH  0x02

#define NOCOLOR   0x80    // "$80 = not set" sentinel for the sty fg field

#define XSTY_SIZE 5       // bytes per xsty record, incl. the class index

// A body record's six colors, in xsty nibble order. Only declarations that
// name the target explicitly (-iftf-c64-background-color and friends) fill
// these in; see the body-record note above build_usty().
enum {
	BODY_BG, BODY_BORDER,
	BODY_NORMAL, BODY_BOLD, BODY_ITALIC, BODY_BOLDITALIC,
	BODY_REVERSE,
	BODY_N
};

static const char *bodyprops[BODY_N] = {
	"background-color", "border-color",
	"normal-color", "bold-color", "italic-color", "bold-italic-color",
	"reverse-color"
};

// The c64 frontend's compiled-in defaults: a light grey screen and border
// (c64_frontend.s:2564) and the four style colors of the palette table
// (c64_frontend.s:521). The bundler resolves undeclared fields to these, so
// SET_BODY writes every nibble with no presence tests.
//
// Reverse is emitted ahead of the frontend that will use it: the c64 does
// not render reverse video at all yet (io_mstyle drops the bit with
// lsr / and #3, and nothing sets bit 7 of a screen code), and the Apple II,
// which does render it (a2_frontend.s:1397), has no per-character color.
// $0 is a placeholder default, matching normal -- a reversed cell paints
// currfg as a solid block, and black blocks under light grey glyphs is
// what the default screen colors want. Worth revisiting when the c64
// actually draws reversed text.
static const uint8_t c64_bodydef[BODY_N] = {0x0f, 0x0f, 0x00, 0x0b, 0x06, 0x0e, 0x00};

struct sty_target {
	const char *name;
	uint8_t tag;
	int have_color;         // per-character fg color (c64)
	const uint8_t *bodydef; // BODY_N defaults, or null if the target has
	                        // no body records at all
};

static const struct sty_target sty_aambox = {
	"aambox", STY_TAG_AAMBOX, 0, 0
};
static const struct sty_target sty_c64 = {
	"c64", STY_TAG_C64, 1, c64_bodydef
};
static const struct sty_target sty_apple2 = {
	"apple2", STY_TAG_APPLE2, 0, 0
};

static const struct sty_target *sty_target;
static uint8_t *sty_payload;    // malloc'd USTY chunk payload (leaked, tool lifetime)
static uint32_t sty_size;
static int sty_emitted;

// Per-class record, one for every class in LOOK.
typedef struct {
	uint8_t styon, styoff;  // AASTYLE_* bits
	uint8_t fg;             // c64 palette index, NOCOLOR = inherit
	uint8_t mtop, mbottom, padleft;
	uint8_t width, height;
	uint8_t flo;            // 0 none, 1 left, 2 right
	uint8_t align;          // 0 left, 1 center, 2 right
	uint8_t flags;          // STY_RELW | STY_RELH
	uint8_t body[BODY_N];   // xsty fields, only valid if hasbody
	uint8_t hasbody;        // class declared at least one body color
} styclass;

// ----------------------------------------------------------------------------
// Color mapping. The C64 has a 16-color palette; web colors are reduced to
// the nearest VIC-II color by RGB distance. "inherit", "initial" and
// "transparent" (or alpha 0) leave the field unset.

static const uint8_t c64_rgb[16][3] = {
	{0, 0, 0}, {255, 255, 255}, {136, 0, 0}, {170, 255, 238},
	{204, 68, 204}, {0, 204, 85}, {0, 0, 170}, {238, 238, 119},
	{221, 136, 85}, {102, 68, 0}, {255, 119, 119}, {51, 51, 51},
	{119, 119, 119}, {170, 255, 102}, {0, 136, 255}, {187, 187, 187}
};

static const struct {
	const char *name;
	uint8_t r, g, b;
} css_colornames[] = {
	{"black", 0x00, 0x00, 0x00},
	{"silver", 0xc0, 0xc0, 0xc0},
	{"gray", 0x80, 0x80, 0x80},
	{"grey", 0x80, 0x80, 0x80},
	{"white", 0xff, 0xff, 0xff},
	{"maroon", 0x80, 0x00, 0x00},
	{"red", 0xff, 0x00, 0x00},
	{"purple", 0x80, 0x00, 0x80},
	{"fuchsia", 0xff, 0x00, 0xff},
	{"magenta", 0xff, 0x00, 0xff},
	{"green", 0x00, 0x80, 0x00},
	{"lime", 0x00, 0xff, 0x00},
	{"olive", 0x80, 0x80, 0x00},
	{"yellow", 0xff, 0xff, 0x00},
	{"navy", 0x00, 0x00, 0x80},
	{"blue", 0x00, 0x00, 0xff},
	{"teal", 0x00, 0x80, 0x80},
	{"aqua", 0x00, 0xff, 0xff},
	{"cyan", 0x00, 0xff, 0xff},
	{"orange", 0xff, 0xa5, 0x00},
	{"brown", 0xa5, 0x2a, 0x2a},
	{"darkgray", 0x33, 0x33, 0x33},
	{"darkgrey", 0x33, 0x33, 0x33},
	{"lightgray", 0xbb, 0xbb, 0xbb},
	{"lightgrey", 0xbb, 0xbb, 0xbb},
	{"lightred", 0xff, 0x77, 0x77},
	{"lightgreen", 0xaa, 0xff, 0x66},
	{"lightblue", 0x00, 0x88, 0xff}
};

// The 16 CSS basic colors map by name to their obvious VIC-II counterpart.
// Pure nearest-RGB is wrong here: CSS "green" (#008000) is closer to
// VIC-II dark grey (11) than to VIC-II green (#00cc55), which surprises
// authors far more than it helps. Hex/rgb() values still use nearest-match.
static const struct {
	const char *name;
	uint8_t vic;
} css2vic[] = {
	{"black", 0},
	{"white", 1},
	{"red", 2},
	{"cyan", 3},
	{"aqua", 3},
	{"magenta", 4},
	{"fuchsia", 4},
	{"purple", 4},
	{"green", 5},
	{"lime", 5},
	{"blue", 6},
	{"navy", 6},
	{"yellow", 7},
	{"orange", 8},
	{"brown", 9},
	{"maroon", 2},
	{"silver", 15},
	{"gray", 11},
	{"grey", 11},
	{"darkgray", 11},
	{"darkgrey", 11},
	{"lightgray", 15},
	{"lightgrey", 15},
	{"teal", 3}
};

// Map an rgb triplet to the nearest VIC-II color, using a perceptual
// distance (weighted RGB, a standard cheap approximation of CIE lightness).
static int rgb_to_c64(int r, int g, int b) {
	int best = 0, bestdist = 0x7fffffff;
	int i;

	for(i = 0; i < 16; i++) {
		long dr = r - c64_rgb[i][0];
		long dg = g - c64_rgb[i][1];
		long db = b - c64_rgb[i][2];
		long dist = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
		if(dist < bestdist) {
			bestdist = dist;
			best = i;
		}
	}
	return best;
}

// Hex digit to value.
static int hex(char c) {
	return c >= '0' && c <= '9'? c - '0' : (c | 0x20) - 'a' + 10;
}

// Parse "  #rgb", "#rrggbb", "rgb(r,g,b)", "rgba(r,g,b,a)" or a name.
// Returns 1 and sets *out to a palette index; 0 if the value is not a
// usable color (inherit, initial, transparent, alpha 0, garbage).
static int parse_color(const char *v, int *out) {
	int i, r = 0, g = 0, b = 0, alpha = 255, n;

	while(*v == ' ' || *v == '\t') v++;

	if(!strncmp(v, "inherit", 7)
	|| !strncmp(v, "initial", 7)
	|| !strncmp(v, "transparent", 11)) {
		return 0;
	}

	if(v[0] == '#') {
		v++;
		n = 0;
		while(v[n] && ((v[n] >= '0' && v[n] <= '9') || (v[n] >= 'a' && v[n] <= 'f') || (v[n] >= 'A' && v[n] <= 'F'))) {
			n++;
		}
		if(n == 3) {
			r = hex(v[0]) * 0x11;
			g = hex(v[1]) * 0x11;
			b = hex(v[2]) * 0x11;
		} else if(n == 6) {
			r = (hex(v[0]) << 4) | hex(v[1]);
			g = (hex(v[2]) << 4) | hex(v[3]);
			b = (hex(v[4]) << 4) | hex(v[5]);
		} else {
			return 0;
		}
	} else if(!strncmp(v, "rgb", 3)) {
		float af;
		v += 3;
		if(*v == 'a') v++;
		if(*v != '(') return 0;
		v++;
		if(sscanf(v, "%d , %d , %d , %f", &r, &g, &b, &af) == 4) {
			// rgba: alpha is a 0..1 float
			alpha = (int)(af * 255 + 0.5);
			alpha = alpha < 0? 0 : alpha > 255? 255 : alpha;
			if(alpha == 0) return 0;
		} else {
			if(sscanf(v, "%d , %d , %d", &r, &g, &b) != 3) return 0;
		}
	} else {
		int vic = -1;
		for(i = 0; i < (int)(sizeof(css2vic) / sizeof(css2vic[0])); i++) {
			if(!strcasecmp(v, css2vic[i].name)) {
				vic = css2vic[i].vic;
				break;
			}
		}
		if(vic < 0) {
			for(i = 0; i < (int)(sizeof(css_colornames) / sizeof(css_colornames[0])); i++) {
				if(!strcasecmp(v, css_colornames[i].name)) {
					r = css_colornames[i].r;
					g = css_colornames[i].g;
					b = css_colornames[i].b;
					break;
				}
			}
			if(i == (int)(sizeof(css_colornames) / sizeof(css_colornames[0]))) {
				return 0;
			}
			*out = rgb_to_c64(r, g, b);
			return 1;
		}
		*out = vic;
		return 1;
	}

	// Hex and rgb() paths land here with r/g/b filled in.
	*out = rgb_to_c64(r, g, b);
	return 1;
}

// ----------------------------------------------------------------------------
// CSS value parsing.

static int isabsunit(const char *v) {
	return (v[0] == 'e' && (v[1] == 'm' || v[1] == 'n'))
		|| (v[0] == 'c' && v[1] == 'h');
}

// Parse a length, mirroring css_abs_rel in engine.s: leading digits, then an
// optional fractional part (ignored), then a unit. Returns 0 on failure,
// 1 on success; *unit 0 = absolute, 1 = percent.
static int parse_length(const char *v, int *val, int *unit) {
	int n = 0;

	if(*v < '0' || *v > '9') return 0;
	while(*v >= '0' && *v <= '9') {
		if(n < 255) n = n * 10 + (*v - '0');
		v++;
	}
	while(*v == '.') {
		v++;
		while(*v >= '0' && *v <= '9') v++;
	}
	if(*v == '%') {
		*val = n;
		*unit = 1;
		return 1;
	}
	if(isabsunit(v)) {
		*val = n;
		*unit = 0;
		return 1;
	}
	return 0;
}

// Whole-value case-insensitive match, ignoring trailing whitespace.
static int matchval(const char *value, const char *word) {
	size_t len = strlen(word);

	if(strncasecmp(value, word, len)) return 0;
	value += len;
	while(*value == ' ' || *value == '\t') value++;
	return !*value;
}

// Parse one null-terminated declaration, e.g. "width: 100%" or
// "-iftf-c64-color: red". The line is copied so the key can be lowercased
// in place; 'key' starts at the buffer, 'value' points into it.
static void parse_decl(styclass *c, const char *p, int len) {
	char buf[256];
	char *colon, *key, *value, *bang, *q;
	int prefixed = 0;
	int i;

	if(len >= (int) sizeof(buf)) len = sizeof(buf) - 1;
	memcpy(buf, p, len);
	buf[len] = 0;

	colon = strchr(buf, ':');
	if(!colon) return;
	*colon = 0;
	value = colon + 1;
	while(*value == ' ' || *value == '\t') value++;
	bang = strchr(value, '!');     // strip "!important"
	if(bang) *bang = 0;
	while(*value == ' ' || *value == '\t') value++;

	key = buf;
	while(*key == ' ' || *key == '\t') key++;
	for(q = key; *q; q++) {
		if(*q >= 'A' && *q <= 'Z') *q |= 0x20;
	}
	while(q > key && (q[-1] == ' ' || q[-1] == '\t')) *--q = 0;

	// -iftf-<system>-<property>: only honored when <system> matches the
	// target being bundled.
	if(!strncmp(key, "-iftf-", 6)) {
		char *prop;
		key += 6;
		prop = strchr(key, '-');
		if(!prop) return;
		*prop = 0;
		if(strcmp(key, sty_target->name)) return;
		key = prop + 1;
		prefixed = 1;
	}

	// Body colors are only honored when the declaration named the target,
	// because on the web these properties either mean something different
	// (a div's background is not the screen background) or do not exist.
	if(prefixed && sty_target->bodydef) {
		for(i = 0; i < BODY_N; i++) {
			if(!strcmp(key, bodyprops[i])) {
				int ci;
				if(parse_color(value, &ci)) {
					if(!c->hasbody) {
						memcpy(c->body, sty_target->bodydef, BODY_N);
						c->hasbody = 1;
					}
					c->body[i] = ci;
				}
				return;
			}
		}
	}

	if(!strcmp(key, "width")) {
		int val, unit;
		if(parse_length(value, &val, &unit)) {
			c->width = val;
			if(unit) c->flags |= STY_RELW;
		}
	} else if(!strcmp(key, "height")) {
		int val, unit;
		if(parse_length(value, &val, &unit)) {
			c->height = val;
			if(unit) c->flags |= STY_RELH;
		}
	} else if(!strcmp(key, "margin-top")) {
		int val, unit;
		if(parse_length(value, &val, &unit) && !unit) c->mtop = val;
	} else if(!strcmp(key, "margin-bottom")) {
		int val, unit;
		if(parse_length(value, &val, &unit) && !unit) c->mbottom = val;
	} else if(!strcmp(key, "margin-left") || !strcmp(key, "padding-left")) {
		int val, unit;
		if(parse_length(value, &val, &unit) && !unit) c->padleft = val;
	} else if(!strcmp(key, "float")) {
		if(matchval(value, "left")) c->flo = 1;
		else if(matchval(value, "right")) c->flo = 2;
	} else if(!strcmp(key, "text-align")) {
		if(matchval(value, "center")) c->align = 1;
		else if(matchval(value, "right")) c->align = 2;
	} else if(!strcmp(key, "font-style")) {
		if(matchval(value, "italic") || matchval(value, "oblique")) c->styon |= AASTYLE_ITALIC;
		else if(matchval(value, "normal")) c->styoff |= AASTYLE_ITALIC;
	} else if(!strcmp(key, "font-weight")) {
		if(matchval(value, "bold")) c->styon |= AASTYLE_BOLD;
		else if(matchval(value, "normal")) c->styoff |= AASTYLE_BOLD;
	} else if(!strcmp(key, "font-family")) {
		if(strstr(value, "monospace")) c->styon |= AASTYLE_FIXED;
		else if(!matchval(value, "inherit")) c->styoff |= AASTYLE_FIXED;
	} else if(!strcmp(key, "text-decoration") || !strcmp(key, "reverse-video")) {
		if(strstr(value, "reverse")) c->styon |= AASTYLE_REVERSE;
		else if(!matchval(value, "inherit")) c->styoff |= AASTYLE_REVERSE;
	} else if(sty_target->have_color && !strcmp(key, "color")) {
		int ci;
		if(parse_color(value, &ci)) c->fg = ci;
	}
	// Anything else is ignored, as an interpreter must per the spec. That
	// includes background-color, border and border-color: a div's tint and
	// a div's border are not the screen background and border registers,
	// and every such declaration measured is a decorative web overlay on a
	// class SET_BODY never sees. An author who wants the c64 screen colors
	// says so with -iftf-c64-background-color.
}

// Build the two record types, in the exact byte order the 6502 engine
// indexes them by (see STYLE_SPEC.md).

static void make_geo(uint8_t *g, const styclass *c) {
	g[0] = c->mtop;
	g[1] = c->mbottom;
	g[2] = c->padleft;
	g[3] = c->width;
	g[4] = c->height;
	g[5] = c->flo;
	g[6] = c->align;
	g[7] = c->flags;
}

static void make_sty(uint8_t *s, const styclass *c) {
	s[0] = c->styon;
	s[1] = c->styoff;
	s[2] = c->fg;
	s[3] = 0;
}

// Add rec to a record pool unless an identical record is already in it.
// Returns the slot index, or -1 if the pool is full. Slot 0 of each pool is
// the pre-seeded all-default record, so a class that sets nothing interns
// to slot 0 and the engine needs no "no slot" sentinel.

static int intern(uint8_t *pool, int *n, int max, const uint8_t *rec, int recsize) {
	int i;

	for(i = 0; i < *n; i++) {
		if(!memcmp(pool + i * recsize, rec, recsize)) return i;
	}
	if(*n >= max) return -1;
	memcpy(pool + *n * recsize, rec, recsize);
	return (*n)++;
}

static uint16_t get16(const uint8_t *p) {
	return (p[0] << 8) | p[1];
}

// ----------------------------------------------------------------------------
// LOOK parse: read the class count, then walk each class's declaration
// block (null-terminated strings, a lone null byte ends the block), the
// same layout engine.s:9910-9961 and get_styles() in engine.js use.

static styclass *parse_look(int *nclassp) {
	uint8_t *look;
	uint32_t looksize;
	uint16_t n;
	styclass *cls;
	int i;

	look = find_chunk("LOOK", &looksize);
	if(!look || looksize < 4) return 0;

	n = get16(look);
	if(n > 255) {
		warning(WARN_STYLE,
			"Story has %d style classes, more than a USTY table can index "
			"(limit 255); the interpreter will parse the style sheet at "
			"startup instead.",
			n);
		return 0;
	}
	cls = calloc(n, sizeof(styclass));
	if(!cls) return 0;

	// fg defaults to "not set", which is not the same as black. Done up
	// front so that a truncated LOOK leaves the unparsed tail of the array
	// looking like classes that set nothing, rather than black-on-black.
	for(i = 0; i < n; i++) {
		cls[i].fg = NOCOLOR;
	}

	for(i = 0; i < n; i++) {
		uint32_t ptr, end;

		if(2 + 2 * i + 2 > looksize) break;
		ptr = get16(look + 2 + 2 * i);
		if(ptr >= looksize) continue;
		end = looksize;
		while(ptr < end && look[ptr]) {
			uint32_t linelen = 0;
			while(ptr + linelen < end && look[ptr + linelen]) linelen++;
			parse_decl(&cls[i], (const char *) look + ptr, linelen);
			ptr += linelen + 1;
		}
	}
	*nclassp = n;
	return cls;
}

// ----------------------------------------------------------------------------
// Slot construction: dedupe the geo and sty records independently, and
// collect xsty (body-style) records.
//
// The two arrays are deduped separately because they are read at different
// times and dedupe very differently: geo keys are nearly all distinct
// (margins and text-align make them unique), while sty keys collapse hard
// (forensic: 37 classes, 14 geo records, 9 sty records). A joint slot made
// the sty array carry a duplicate for every distinct geometry.
//
// The per-class index tables hold *byte offsets* (slot * 8 for geo, slot * 4
// for sty), not slot numbers, so the engine indexes a record with no shift
// chain at all -- which is what the geo/sty record sizes were padded for in
// the first place. That caps the arrays at 32 geo and 64 sty records; the
// largest real story measured uses 14 and 9.
//
// Slot 0 of each array is the reserved all-default record, so a class that
// sets nothing is an ordinary slot rather than a sentinel: the engine
// applies a no-op record instead of branching around one.
//
// xsty holds body records, the one thing that genuinely cannot live in a
// slot: SET_BODY rewrites the frontend's whole style palette plus the
// screen and border registers, which is four bytes of payload against
// sty's one spare byte. They stay keyed on the raw class index because
// SET_BODY takes a class operand. The array is scanned, not indexed, so
// the record size need not be a power of two.
//
// A class earns a body record only by naming the target explicitly
// (-iftf-c64-background-color and friends). Deriving them from plain
// background-color/border was wrong in every story measured: those
// declarations sit on decorative inline divs that SET_BODY never sees,
// while the real body classes carry nothing but CSS custom properties.

static uint8_t *build_usty(uint32_t *sizep) {
	styclass *cls;
	int nclass;
	uint8_t geo[STY_MAXGEO * 8], sty[STY_MAXSTY * 4];
	int ngeo = 1, nsty = 1;         // slot 0 of each is the default record
	uint8_t geoidx[256], styidx[256];
	uint8_t xsty[256 * XSTY_SIZE];
	int nxsty = 0;
	uint8_t *out;
	uint32_t size, styoffs, geooffs, xstyoffs;
	int i;

	cls = parse_look(&nclass);
	if(!cls) return 0;

	memset(geo, 0, 8);
	sty[0] = 0;
	sty[1] = 0;
	sty[2] = NOCOLOR;
	sty[3] = 0;

	for(i = 0; i < nclass; i++) {
		uint8_t g[8], s[4];
		int gs, ss;

		make_geo(g, &cls[i]);
		make_sty(s, &cls[i]);
		gs = intern(geo, &ngeo, STY_MAXGEO, g, 8);
		ss = intern(sty, &nsty, STY_MAXSTY, s, 4);
		if(gs < 0 || ss < 0) {
			warning(WARN_STYLE,
				"Story has more distinct %s styles than a USTY table can hold "
				"(limit %d); the interpreter will parse the style sheet at "
				"startup instead.",
				(gs < 0)? "layout" : "text",
				(gs < 0)? STY_MAXGEO - 1 : STY_MAXSTY - 1);
			free(cls);
			return 0;
		}
		geoidx[i] = gs * 8;
		styidx[i] = ss * 4;

		// Body records, keyed on the raw class index because SET_BODY
		// takes a class operand and this data is deliberately outside the
		// slot dedup key. Two nibbles per byte; all six fields are always
		// present, the undeclared ones resolved to the target's defaults.
		if(cls[i].hasbody) {
			uint8_t *x = xsty + nxsty * XSTY_SIZE;
			x[0] = i;
			x[1] = cls[i].body[BODY_BG] | (cls[i].body[BODY_BORDER] << 4);
			x[2] = cls[i].body[BODY_NORMAL] | (cls[i].body[BODY_BOLD] << 4);
			x[3] = cls[i].body[BODY_ITALIC] | (cls[i].body[BODY_BOLDITALIC] << 4);
			x[4] = cls[i].body[BODY_REVERSE];        // high nibble reserved
			nxsty++;
		}
	}

	// Chunk layout: a fixed header, then the two index tables, then the
	// hot sty records, then the cold geo and xsty records. Every array
	// offset follows from the counts, so the header carries no offsets.
	styoffs = 5 + 2 * nclass;
	geooffs = styoffs + nsty * 4;
	xstyoffs = geooffs + ngeo * 8;
	size = xstyoffs + nxsty * XSTY_SIZE;
	out = malloc(size);
	if(!out) {
		fprintf(stderr, "Out of memory.\n");
		exit(1);
	}

	out[0] = sty_target->tag;
	out[1] = nclass;
	out[2] = ngeo;
	out[3] = nsty;
	out[4] = nxsty;
	memcpy(out + 5, geoidx, nclass);
	memcpy(out + 5 + nclass, styidx, nclass);
	memcpy(out + styoffs, sty, nsty * 4);
	memcpy(out + geooffs, geo, ngeo * 8);
	memcpy(out + xstyoffs, xsty, nxsty * XSTY_SIZE);

	free(cls);
	*sizep = size;
	return out;
}

// ----------------------------------------------------------------------------
// Public API.

void bundle_sty_set_target(const char *target) {
	sty_target = 0;
	if(!strcmp(target, "c64")) {
		sty_target = &sty_c64;
	} else if(!strcmp(target, "apple2")) {
		sty_target = &sty_apple2;
	} else if(!strcmp(target, "aambox")) {
		sty_target = &sty_aambox;
	}
	sty_payload = 0;
	sty_size = 0;
	sty_emitted = 0;
}

chunk_action_t rewrite_6502_sty(
	const char *id,
	uint8_t *data,
	uint32_t size,
	char *newid,
	uint8_t **newdata,
	uint32_t *newsize)
{
	chunk_action_t act = rewrite_6502(id, data, size, newid, newdata, newsize);

	if(act == CHUNK_DROP) return act;

	// Insert the USTY chunk right after LOOK, whose payload it derives
	// from. HEAD must remain the first chunk (the c64 loader copies block 0
	// expecting HEAD), so we key off LOOK rather than the first chunk.
	if(sty_target && !sty_emitted && !strcmp(id, "LOOK")) {
		if(!sty_payload) {
			sty_payload = build_usty(&sty_size);
		}
		if(sty_payload) {
			memcpy(newid, "USTY", 4);
			*newdata = sty_payload;
			*newsize = sty_size;
			sty_emitted = 1;
			return CHUNK_INSERT;
		}
	}
	return act;
}