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

#define STY_VERSION	0x01
#define STY_TAG_AAMBOX  (0x00 | STY_VERSION)
#define STY_TAG_C64     (0x10 | STY_VERSION)
#define STY_TAG_APPLE2  (0x20 | STY_VERSION)

#define STY_RELW  0x01
#define STY_RELH  0x02

#define NOCOLOR   0x80    // "$80 = not set" sentinel for fg/bg/border

struct sty_target {
	const char *name;
	uint8_t tag;
	int have_color;         // per-character fg color (c64)
	uint8_t xstysize;       // bytes per xsty record incl. class index
};

static const struct sty_target sty_aambox = {
	"aambox", STY_TAG_AAMBOX, 0, 1
};
static const struct sty_target sty_c64 = {
	"c64", STY_TAG_C64, 1, 4
};
static const struct sty_target sty_apple2 = {
	"apple2", STY_TAG_APPLE2, 0, 1
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
	uint8_t bg, border;     // xsty fields, NOCOLOR = not set
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
	} else if(sty_target->have_color) {
		int ci;
		if(!strcmp(key, "color")) {
			if(parse_color(value, &ci)) c->fg = ci;
		} else if(!strcmp(key, "background-color")) {
			if(parse_color(value, &ci)) c->bg = ci;
		} else if(!strcmp(key, "border-color") || !strcmp(key, "border")) {
			if(parse_color(value, &ci)) c->border = ci;
		}
	}
	// Anything else (font-size, padding, text-indent, border-radius,
	// margin-right, --custom-properties, aria-*, style-name, ...) is
	// ignored, as an interpreter must per the spec.
}

// One entry in the dedup key space. The 11 bytes are exactly what an
// 8-bit engine can act on: the whole sty record, plus the whole geo record.
// TODO: unify with styclass, or at least check size
typedef struct {
	uint8_t rec[11]; // styon styoff fg mtop mbottom padleft width height flo align flags
} slotkey;

static uint16_t get16(const uint8_t *p) {
	return (p[0] << 8) | p[1];
}

static int key_geo(const slotkey *k) {
	int i;
	for(i = 3; i < 11; i++) {
		if(k->rec[i]) return 1;
	}
	return 0;
}

static int key_empty(const slotkey *k) {
	return !k->rec[0] && !k->rec[1] && k->rec[2] == NOCOLOR && !key_geo(k);
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
		// TODO: error
		fprintf(stderr, "Warning: LOOK has %d classes, too many for a USTY table; skipping.\n", n);
		return 0;
	}
	cls = calloc(n, sizeof(styclass));
	if(!cls) return 0;

	for(i = 0; i < n; i++) {
		uint32_t ptr, end;

		cls[i].fg = NOCOLOR;
		cls[i].bg = NOCOLOR;
		cls[i].border = NOCOLOR;

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
// Slot construction: dedupe classes on their 11-byte key, order slots so
// geometry-bearing ones come first, and collect xsty (body-style) records.
//
// A class with an all-default key maps to $ff (no slot): entering it does
// nothing on an 8-bit machine.

static int keycmp(const slotkey *a, const slotkey *b) {
	return memcmp(a->rec, b->rec, 11);
}

static uint8_t *build_usty(uint32_t *sizep) {
	styclass *cls;
	int nclass;
	slotkey geo[256], plain[256];
	int ngeo = 0, nplain = 0, nslot;
	uint8_t styidx[256];
	uint8_t pool[256];      // 0 = none, 1 = geo, 2 = plain
	uint8_t poolidx[256];
	uint8_t xsty[256 * 4];
	int nxsty = 0;
	uint8_t *out;
	uint32_t size;
	int i, s;

	cls = parse_look(&nclass);
	if(!cls) return 0;

	for(i = 0; i < nclass; i++) {
		slotkey k;
		int j;

		k.rec[0] = cls[i].styon;
		k.rec[1] = cls[i].styoff;
		k.rec[2] = cls[i].fg;
		k.rec[3] = cls[i].mtop;
		k.rec[4] = cls[i].mbottom;
		k.rec[5] = cls[i].padleft;
		k.rec[6] = cls[i].width;
		k.rec[7] = cls[i].height;
		k.rec[8] = cls[i].flo;
		k.rec[9] = cls[i].align;
		k.rec[10] = cls[i].flags;

		if(key_empty(&k)) {
			pool[i] = 0;
		} else if(key_geo(&k)) {
			for(j = 0; j < ngeo; j++) {
				if(!keycmp(&geo[j], &k)) break;
			}
			if(j < ngeo) {
				pool[i] = 1;
				poolidx[i] = j;
			} else {
				geo[ngeo] = k;
				pool[i] = 1;
				poolidx[i] = ngeo++;
			}
		} else {
			for(j = 0; j < nplain; j++) {
				if(!keycmp(&plain[j], &k)) break;
			}
			if(j < nplain) {
				pool[i] = 2;
				poolidx[i] = j;
			} else {
				plain[nplain] = k;
				pool[i] = 2;
				poolidx[i] = nplain++;
			}
		}

		// Body-style (xsty) candidates: classes that set a background or
		// border color. Keyed on the raw class index so two body classes
		// differing only in background cannot be deduped onto one slot.
		if(sty_target->have_color && (cls[i].bg != NOCOLOR || cls[i].border != NOCOLOR)) {
			uint8_t *x = xsty + nxsty * sty_target->xstysize;
			x[0] = i;
			if(sty_target->xstysize > 1) x[1] = cls[i].bg;
			if(sty_target->xstysize > 2) x[2] = cls[i].border;
			if(sty_target->xstysize > 3) x[3] = 0;
			// TODO: memset(0)?
			nxsty++;
		}
	}

	nslot = ngeo + nplain;
	if(nslot > 255 || nxsty > 255) {
		// TODO: error
		fprintf(stderr, "Warning: USTY table too large (nslot %d, nxsty %d); skipping.\n", nslot, nxsty);
		free(cls);
		return 0;
	}

	// Final slot numbering: geo slots first, then plain slots.
	for(i = 0; i < nclass; i++) {
		if(pool[i] == 0) {
			styidx[i] = 0xff;
		} else if(pool[i] == 1) {
			styidx[i] = poolidx[i];
		} else {
			styidx[i] = ngeo + poolidx[i];
		}
	}

	// Chunk layout. Offsets are from the start of the payload; 0 = absent.
	size = 12 + nclass + ngeo * 8 + nslot * 4 + nxsty * sty_target->xstysize;
	out = malloc(size);
	if(!out) {
		fprintf(stderr, "Out of memory.\n");
		exit(1);
	}

	out[0] = sty_target->tag;
	out[1] = nclass;
	out[2] = nslot;
	out[3] = ngeo;
	out[4] = nxsty;
	out[5] = sty_target->xstysize;
	out[6] = ngeo? (12 + nclass) >> 8 : 0;
	out[7] = ngeo? (12 + nclass) & 0xff : 0;
	out[8] = nslot? (12 + nclass + ngeo * 8) >> 8 : 0;
	out[9] = nslot? (12 + nclass + ngeo * 8) & 0xff : 0;
	out[10] = nxsty? (12 + nclass + ngeo * 8 + nslot * 4) >> 8 : 0;
	out[11] = nxsty? (12 + nclass + ngeo * 8 + nslot * 4) & 0xff : 0;
	memcpy(out + 12, styidx, nclass);

	for(s = 0; s < ngeo; s++) {
		uint8_t *g = out + 12 + nclass + s * 8;
		g[0] = geo[s].rec[3];
		g[1] = geo[s].rec[4];
		g[2] = geo[s].rec[5];
		g[3] = geo[s].rec[6];
		g[4] = geo[s].rec[7];
		g[5] = geo[s].rec[8];
		g[6] = geo[s].rec[9];
		g[7] = geo[s].rec[10];
	}
	for(s = 0; s < nslot; s++) {
		const slotkey *k = s < ngeo? &geo[s] : &plain[s - ngeo];
		uint8_t *st = out + 12 + nclass + ngeo * 8 + s * 4;
		st[0] = k->rec[0];
		st[1] = k->rec[1];
		st[2] = k->rec[2];
		st[3] = 0;
	}
	memcpy(out + 12 + nclass + ngeo * 8 + nslot * 4, xsty, nxsty * sty_target->xstysize);

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