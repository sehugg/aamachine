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

// The high nibble of the tag byte names the target, the low nibble the
// format revision. The revision is OR'd in by whichever builder ran, so the
// two layouts cannot be confused for each other on the way in.
#define STY_TAG_AAMBOX  0x00
#define STY_TAG_C64     0x10
#define STY_TAG_APPLE2  0x20

#define STY_RELW  0x01
#define STY_RELH  0x02

#define NOCOLOR   0x80    // "$80 = not set" sentinel for the sty fg field

// Bytes per xsty record for the c64: class index, background|border, four
// palette bytes, and the cursor color. This is a *per-target* size -- it is
// what the c64's body record happens to need, not a property of the format
// -- so it lives in struct sty_target and travels in the chunk header. A
// target whose frontend has a different palette shape declares its own; one
// with no body records at all declares 0.
#define XSTY_SIZE_C64 7

// A body record's eleven colors, in xsty nibble order. Only declarations
// that name the target explicitly (-iftf-sys-c64-background-color and
// friends) fill these in; see the body-record note above build_usty().
// The eight palette entries are indexed by the low three AASTYLE bits
// exactly as they arrive: reverse | bold << 1 | italic << 2. So io_mstyle
// is "and #7 / tax / lda palette,x" -- no shifting, no special case for
// reverse, and every combination gets its own color.
enum {
	BODY_BG, BODY_BORDER,
	BODY_PAL,               // BODY_PAL + (style & 7)
	BODY_CURSOR,
	BODY_N = BODY_PAL + 9
};

// The cursor color (BODY_CURSOR) is not part of the style palette; it is
// the sprite color register $d027, which SET_BODY will rewrite alongside
// background and border. Like the other body colors it is only settable
// through -iftf-sys-<target>-cursor-color.

static const char *bodyprops[BODY_N] = {
	"background-color", "border-color",
	"normal-color",                 // 0  -
	"reverse-color",                // 1  reverse
	"bold-color",                   // 2  bold
	"bold-reverse-color",           // 3  bold reverse
	"italic-color",                 // 4  italic
	"italic-reverse-color",         // 5  italic reverse
	"bold-italic-color",            // 6  bold italic
	"bold-italic-reverse-color",    // 7  bold italic reverse
	"cursor-color"                  // $d027, not a palette entry
};

// The c64 frontend's compiled-in defaults: a light grey screen and border
// (c64_frontend.s:2564) and the palette table (c64_frontend.s:521). The
// bundler resolves undeclared fields to these, so SET_BODY writes every
// nibble with no presence tests.
//
// Today's palette has four entries indexed by bold | italic << 1, because
// io_mstyle drops the reverse bit. Each reverse variant therefore defaults
// to its non-reverse counterpart, which is exactly what the c64 renders
// today -- the eight-entry table starts out behaving like the four-entry
// one, and only differs where an author asks it to. (The c64 does not draw
// reverse video at all yet; the Apple II does, a2_frontend.s:1397, but has
// no per-character color.)
// TODO: sync with 6502 frontend
static const uint8_t c64_bodydef[BODY_N] = {
	0x0f, 0x0f,             // background, border
	0x00, 0x00,             // -, reverse
	0x0b, 0x0b,             // bold, bold reverse
	0x06, 0x06,             // italic, italic reverse
	0x0e, 0x0e,             // bold italic, bold italic reverse
	0x08                    // cursor (orange, c64_frontend.s $d027 init)
};

struct sty_target {
	const char *name;
	uint8_t tag;
	int have_vic_color;     // per-character fg color (c64)
	uint8_t stymask;        // AASTYLE_* bits the frontend can actually act on
	const uint8_t *bodydef; // BODY_N defaults, or null if the target has
	                        // no body records at all
	uint8_t xstysize;       // bytes per body record; 0 if bodydef is null.
	                        // Emitted in the header, so a decoder can stride
	                        // the array without knowing the target. Note that
	                        // the *packing* below is c64-shaped: a target with
	                        // a different palette needs its own packer, not
	                        // just a different size here.
	// Whether the target's interpreter still carries a style sheet parser.
	// The c64 and apple2 builds define NO_CSS_PARSER and drop it (802 bytes
	// of engine.s), so for them a USTY table is not an optimization but a
	// requirement: a disk shipped without one would boot to a story with no
	// style table at all. aambox keeps both paths, because the test suite
	// hands it raw .aastory files that have never been through here.
	int usty_required;

	// Screen size in character cells, for range-checking absolute lengths.
	// mincols/maxcols bracket the machine's real text widths: the c64 is a
	// fixed 40; the apple2 is 40 or 80 depending on the card present, and
	// the bundler cannot know which; aambox is exactly 80. The engine clamps
	// heights to 19 (enter_status: cmp #20 / lda #19, engine.s:3931).
	int mincols, maxcols;
	int maxrows;
};

// stymask is what the target's io_mstyle looks at, and nothing else reads
// rstyle. Bits outside it cannot change a single pixel, so masking them out
// before interning collapses records that differ only in styles the target
// cannot render. AASTYLE_FIXED is in no mask: every 8-bit target is
// monospace already, and no frontend tests bit 3.
//
//   c64     io_mstyle: lsr / and #3 -> bold | italic (c64_frontend.s:508).
//           Reverse is masked in even though the c64 does not draw it yet,
//           because the body record already carries a reverse color for it.
//   apple2  mstyle_enter: lsr, carry -> set_inverse (a2_frontend.s:1394).
//           Reverse and nothing else.
//   aambox  io_mstyle is a bare rts (aambox_frontend.s:221). Nothing.

static const struct sty_target sty_aambox = {
	"aambox", STY_TAG_AAMBOX, 0, 0, 0, 0, 0, 80, 80, 20
};
static const struct sty_target sty_c64 = {
	"c64", STY_TAG_C64, 1,
	AASTYLE_REVERSE | AASTYLE_BOLD | AASTYLE_ITALIC, c64_bodydef, XSTY_SIZE_C64,
	1, 40, 40, 20
};
static const struct sty_target sty_apple2 = {
	"apple2", STY_TAG_APPLE2, 0, AASTYLE_REVERSE, 0, 0, 1, 40, 80, 20
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
	uint8_t flo;            // 0 none, 1 left, 2 right, 3 center
	uint8_t align;          // 1 left, 2 right, 3 center
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

// CSS color names map by name to their obvious VIC-II counterpart, so the
// full basic + extended CSS palettes fold into one table. Pure nearest-RGB
// is wrong here: CSS "green" (#008000) is closer to VIC-II dark grey (11)
// than to VIC-II green (#00cc55), which surprises authors far more than it
// helps. Hex/rgb() values still use nearest-match. lightred/lightgreen/
// lightblue coincide exactly with VIC-II colors 10/13/14, so those names can
// live here too instead of going through rgb_to_c64().
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
	{"teal", 3},
	{"lightred", 10},
	{"lightgreen", 13},
	{"lightblue", 14}
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

// Parse "  #rgb", "#rrggbb", "rgb(r,g,b)", "rgba(r,g,b,a)", a name,
//   or a specific VIC-II color index (0..15).
// Returns 1 and sets *out to a palette index;
// returns 0 if the value is not a usable color
//   (inherit, initial, transparent, alpha 0, garbage).
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
		}else if(n == 6) {
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
	} else if(*v >= '0' && *v <= '9') {
		// Naked VIC color index: 0..15 selects the palette entry itself,
		// so 3 is cyan no matter what hue CSS would call it. Reject
		// trailing junk so "10px" and friends don't quietly become 10.
		const char* v_old = v;
		n = atoi(v);
		while(*v >= '0' && *v <= '9') v++;
		while(*v == ' ' || *v == '\t') v++;
		if(!*v && n <= 15) {
			*out = n;
			return 1;
		}
		warning(WARN_STYLE, "Invalid VIC color index \"%s\", must be in range 0..15.", v_old);
		return 0;
	} else {
		int vic = -1;
		for(i = 0; i < (int)(sizeof(css2vic) / sizeof(css2vic[0])); i++) {
			if(!strcasecmp(v, css2vic[i].name)) {
				vic = css2vic[i].vic;
				break;
			}
		}
		if(vic < 0) {
			warning(WARN_STYLE, "Unknown c64 color \"%s\".", v);
			return 0;
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

// Parse a length, mirroring css_abs_rel in engine.s: an optional leading
// decimal point, then digits, then an optional fractional part (both dot
// forms are truncated -- ".3em" is 0, exactly as the engine computes), then
// a unit. Returns 0 on failure, 1 on success; *unit 0 = absolute,
// 1 = percent. *flags reports what was lost so the caller can warn:
//   PLEN_FRAC  a fractional part was dropped
//   PLEN_OVER  the value exceeds 255, the engine's one-byte storage, and
//              will wrap
#define PLEN_FRAC	1
#define PLEN_OVER	2

static int parse_length(const char *v, int *val, int *unit, int *flags) {
	long n = 0;
	int frac = 0;

	*flags = 0;
	if(*v == '.') {
		// ".3em" is 0.3em on the web, so there is no integer part to
		// accumulate -- the digits after the dot are the fractional part
		// and get dropped below. Reading them as the integer part turned
		// "margin-top:.3em" into three blank rows.
		if(v[1] < '0' || v[1] > '9') return 0;
	} else {
		if(*v < '0' || *v > '9') return 0;
		while(*v >= '0' && *v <= '9') {
			if(n < 1000000) n = n * 10 + (*v - '0');
			v++;
		}
	}
	// Truncated, not rounded, which is what css_abs_rel in engine.s does.
	while(*v == '.') {
		frac = 1;
		v++;
		while(*v >= '0' && *v <= '9') v++;
	}
	if(n > 255) *flags |= PLEN_OVER;
	if(frac) *flags |= PLEN_FRAC;
	if(*v == '%') {
		*val = (int) n;
		*unit = 1;
		return 1;
	}
	if(isabsunit(v)) {
		*val = (int) n;
		*unit = 0;
		return 1;
	}
	return 0;
}

// Warn about a length declaration the 8-bit machine will not honor the way
// the web would. prop is the CSS property, value the raw text; val/st/unit/
// flags come from parse_length(). relok says whether a percent is
// meaningful for this property (true for width/height, false for margins).
static void warn_length(const char *prop, const char *value, int val, int st, int unit, int flags, int relok) {
	if(st) {
		if(flags & PLEN_FRAC) {
			//warning(WARN_STYLE, "Fractional %s \"%s\" is truncated.", prop, value);
		}
		if(unit) {
			// Percent. A width or height above 100% means "bigger than the
			// whole screen" -- the web lays that out by overflowing, an
			// 8-bit machine cannot. (For height it is doubly pointless:
			// the engine treats any relative height as one row.) Margins
			// reject percent outright, below.
			if(relok && val > 100) {
				warning(WARN_STYLE,
					"Percent %s \"%s\" is more than 100%% and will not fit the screen.",
					prop, value);
			} else if(!relok) {
				warning(WARN_STYLE, "Percent %s \"%s\" is not supported and was ignored.", prop, value);
			}
		} else if(flags & PLEN_OVER) {
			// An absolute (em/ch) length that no 8-bit screen can show.
			int max = relok? sty_target->maxcols : sty_target->maxrows;
			warning(WARN_STYLE,
				"%s \"%s\" is more than %s's %d %s and will be clamped.",
				prop, value, sty_target->name, max,
				relok? "columns" : "rows");
		}
	} else {
		warning(WARN_STYLE, "Ignoring %s: unsupported value \"%s\".", prop, value);
	}
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
// "-iftf-sys-c64-color: red". The line is copied so the key can be lowercased
// in place; 'key' starts at the buffer, 'value' points into it.
//
// Declarations are parsed in two passes per class (see parse_look): pass 0
// takes the unprefixed ones, pass 1 the -iftf- ones. A -iftf- declaration
// is an author's statement about this machine specifically, so it wins
// over the same property without the prefix no matter which order the
// style sheet lists them in -- the pass does what "last declaration wins"
// would do only if the -iftf- line happened to come last.
static void parse_decl(styclass *c, const char *p, int len, int pass) {
	char buf[256];
	char *colon, *key, *value, *bang, *q;
	int prefixed = 0;
	int matched = 0;
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

	// -iftf-sys-<target>-* properties
	if(!strncmp(key, "-iftf-sys-", 10)) {
		prefixed = 1;
		char *prop;
		key += 10;
		prop = strchr(key, '-');
		if(!prop) return;
		*prop = 0;
		// ignore if not the target system
		if(strcmp(key, sty_target->name)) {
			return;
		}
		key = prop + 1;
	}
	if(prefixed != pass) return;

	if(!strcmp(key, "-iftf-text-decoration")) {
		if(matchval(value, "reverse")) c->styon |= AASTYLE_REVERSE;
		else if(matchval(value, "none")) c->styoff |= AASTYLE_REVERSE;
		else warning(WARN_STYLE, "Invalid value for %s: %s", key, value);
		return;
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
		int val, unit, flags, st;
		st = parse_length(value, &val, &unit, &flags);
		if(st == 1) {
			c->width = val;
			if(unit) c->flags |= STY_RELW;
		}
		warn_length(key, value, val, st, unit, flags, 1);
		matched = 1;
	} else if(!strcmp(key, "height")) {
		int val, unit, flags, st;
		st = parse_length(value, &val, &unit, &flags);
		if(st == 1) {
			c->height = val;
			if(unit) c->flags |= STY_RELH;
		}
		warn_length(key, value, val, st, unit, flags, 1);
		matched = 1;
	} else if(!strcmp(key, "margin-top")) {
		int val, unit, flags, st;
		st = parse_length(value, &val, &unit, &flags);
		if(st == 1 && !unit) c->mtop = val;
		warn_length(key, value, val, st, unit, flags, 0);
		matched = 1;
	} else if(!strcmp(key, "margin-bottom")) {
		int val, unit, flags, st;
		st = parse_length(value, &val, &unit, &flags);
		if(st == 1 && !unit) c->mbottom = val;
		warn_length(key, value, val, st, unit, flags, 0);
		matched = 1;
	} else if(!strcmp(key, "margin-left") || !strcmp(key, "padding-left")) {
		int val, unit, flags, st;
		st = parse_length(value, &val, &unit, &flags);
		if(st == 1 && !unit) c->padleft = val;
		warn_length(key, value, val, st, unit, flags, 0);
		matched = 1;
	} else if(!strcmp(key, "float")) {
		matched = 1;
		if(matchval(value, "left")) c->flo = 1;
		else if(matchval(value, "right")) c->flo = 2;
		else if(!matchval(value, "none") && !matchval(value, "inherit")) {
			warning(WARN_STYLE, "Invalid value for float: %s (only left, right and none are supported).", value);
		}
	} else if(!strcmp(key, "margin")) {
		matched = 1;
		if(matchval(value, "auto")) c->flo = 3;
		else warning(WARN_STYLE, "Invalid value for margin: %s (use \"margin: auto\").", value);
	} else if(!strcmp(key, "text-align")) {
		matched = 1;
		if(matchval(value, "center")) c->align = 3;
		else if(matchval(value, "right")) c->align = 2;
		else if(matchval(value, "left")) c->align = 1;
		else if(!matchval(value, "inherit")) {
			warning(WARN_STYLE, "Invalid value for text-align: %s (only left, right and center are supported).", value);
		}
	} else if(!strcmp(key, "font-style")) {
		matched = 1;
		if(matchval(value, "italic") || matchval(value, "oblique")) c->styon |= AASTYLE_ITALIC;
		else if(matchval(value, "normal")) c->styoff |= AASTYLE_ITALIC;
		else if(!matchval(value, "inherit")) {
			warning(WARN_STYLE, "Invalid value for font-style: %s (only italic, oblique and normal are supported).", value);
		}
	} else if(!strcmp(key, "font-weight")) {
		matched = 1;
		if(matchval(value, "bold")) c->styon |= AASTYLE_BOLD;
		else if(matchval(value, "normal")) c->styoff |= AASTYLE_BOLD;
		else if(!matchval(value, "inherit")) {
			// In particular: numeric weights (400, 700) are valid web CSS
			// but mean nothing here.
			warning(WARN_STYLE, "Invalid value for font-weight: %s (only bold and normal are supported).", value);
		}
	} else if(!strcmp(key, "font-family")) {
		matched = 1;
		if(strstr(value, "monospace")) c->styon |= AASTYLE_FIXED;
		else if(!matchval(value, "inherit")) c->styoff |= AASTYLE_FIXED;
	} else if(!strcmp(key, "text-decoration") || !strcmp(key, "reverse-video")) {
		matched = 1;
		if(strstr(value, "reverse")) {
			c->styon |= AASTYLE_REVERSE;
		} else if(matchval(value, "inherit")) {
			// Nothing: inherit is the no-op.
		} else {
			// "none" is the explicit off; any other value (underline is
			// the classic web one) neither sets nor unsets reverse on
			// the web, so warn rather than quietly turning it off.
			c->styoff |= AASTYLE_REVERSE;
			if(!strcmp(key, "text-decoration") && !matchval(value, "none")) {
				warning(WARN_STYLE, "Invalid value for text-decoration: %s (only reverse, none and inherit are supported).", value);
			}
		}
	} else if(!strcmp(key, "color")) {
		matched = 1;
		if(sty_target->have_vic_color) {
			int ci;
			if(parse_color(value, &ci)) {
				c->fg = ci;
				// An unprefixed color identical to the machine's default
				// screen background renders invisible text. An author who
				// wants that deliberately would have said so with
				// -iftf-sys-<sys>-color, which is exempt.
				if(!prefixed && sty_target->bodydef &&
					ci == sty_target->bodydef[BODY_BG]) {
					warning(WARN_STYLE,
						"color %s matches the default background color on %s; "
						"text with this class will be invisible unless (body style $Class) is used.",
						value, sty_target->name);
				}
			}
		} else {
			warning(WARN_STYLE, "color is not supported on %s and was ignored.", sty_target->name);
		}
	} else if(!strcmp(key, "background-color")) {
		matched = 1;
		if(!prefixed) {
			// Only -iftf-sys-<sys>-background-color fills the body record; the
			// unprefixed form does nothing on any 6502 target. (On the web
			// it colors the element itself, not the screen, so the two
			// cannot share a declaration.)
			if(sty_target->bodydef) {
				warning(WARN_STYLE,
					"To set the screen background on %s, use -iftf-sys-%s-background-color.",
					sty_target->name, sty_target->name);
			}
		}
	} else if(!strcmp(key, "display")) {
		matched = 1;
		if(matchval(value, "none")) {
			warning(WARN_STYLE, "display: none is not supported; the element will still be shown.");
		}
	} else if(!strcmp(key, "visibility")) {
		matched = 1;
		if(matchval(value, "hidden")) {
			warning(WARN_STYLE, "visibility: hidden is not supported; the element will still be shown.");
		}
	}

	// A prefix that named the target but a property the bundler does not
	// know. This is the one case where the author addressed this machine
	// specifically, so silence would hide a real gap -- the -iftf- set is
	// young and grows.
	if(prefixed && !matched) {
		warning(WARN_STYLE, "-iftf-sys-%s-%s is not supported yet.", sty_target->name, key);
	}
	// Anything else is ignored, as an interpreter must per the spec. That
	// includes background-color, border and border-color: a div's tint and
	// a div's border are not the screen background and border registers,
	// and every such declaration measured is a decorative web overlay on a
	// class SET_BODY never sees. An author who wants the c64 screen colors
	// says so with -iftf-c64-background-color.
}

// A revision 11 record: everything about one class in eight bytes, laid out
// in engine.s's STY_* order so that stybase + class * 8 keeps working and
// none of the engine's read sites move.
//
// Two properties the CSS parsers understand do not fit and are dropped
// here, which build_usty_flat() warns about: padding/margin-left has no
// byte left, and "margin: auto" centering has no flags encoding -- $c0
// would read as a right float to the engine's cpx #STYF_FLOATR test, which
// is worse than ignoring it. Neither is rendered by any 6502 frontend
// today, and the engine's own parser has always ignored both, so a story
// bundled with USTY looks exactly like the same story bundled without it.

static void make_flat(uint8_t *r, const styclass *c) {
	uint8_t flags = c->flags & (USTY_FL_RELW | USTY_FL_RELH);

	if(c->flo == 1) flags |= USTY_FL_FLOATL;
	else if(c->flo == 2) flags |= USTY_FL_FLOATR;
	flags |= (c->align & 3) << USTY_FL_ALIGN_SHIFT;

	r[USTY_F_WIDTH] = c->width;
	r[USTY_F_HEIGHT] = c->height;
	r[USTY_F_MTOP] = c->mtop;
	r[USTY_F_MBOTTOM] = c->mbottom;
	r[USTY_F_STYON] = c->styon & sty_target->stymask;
	r[USTY_F_STYOFF] = c->styoff & sty_target->stymask;
	r[USTY_F_FLAGS] = flags;
	r[USTY_F_FG] = c->fg;
}

// Body records are keyed on the raw class index because SET_BODY takes a
// class operand, and they are identical in both revisions.

static void make_xsty(uint8_t *x, int classidx, const styclass *c) {
	int j;

	x[0] = classidx;
	x[1] = c->body[BODY_BG] | (c->body[BODY_BORDER] << 4);
	for(j = 0; j < 4; j++) {
		x[2 + j] = c->body[BODY_PAL + j * 2]
			| (c->body[BODY_PAL + j * 2 + 1] << 4);
	}
	// Cursor color in the low nibble; the high nibble is reserved and
	// stays 0 so a future eleventh field can move in without moving this
	// one.
	x[6] = c->body[BODY_CURSOR];
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
	int pass;

	look = find_chunk("LOOK", &looksize);
	if(!look) return 0;

	n = get16(look);
	if(n > 255) {
		warning(WARN_STYLE,
			"Story has %d style classes, more than a USTY table can index "
			"(limit 255); the interpreter will parse the style sheet at "
			"startup instead.",
			n);
		return 0;
	}
	// need at least one style, ok if it's empty
	if (!n) n++;
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

		if((uint32_t)(2 + 2 * i + 2) > looksize) break;
		ptr = get16(look + 2 + 2 * i);
		if(ptr >= looksize) continue;
		end = looksize;
		// Two passes over the same block: unprefixed declarations first,
		// then the -iftf-sys- ones, so that a prefixed declaration overrides
		// its unprefixed counterpart no matter which comes last in the CSS.
		for(pass = 0; pass < 2; pass++) {
			uint32_t p = ptr;
			while(p < end && look[p]) {
				uint32_t linelen = 0;
				while(p + linelen < end && look[p + linelen]) linelen++;
				parse_decl(&cls[i], (const char *) look + p, linelen, pass);
				p += linelen + 1;
			}
		}
	}
	*nclassp = n;
	return cls;
}

// Revision 11: one 8-byte record per class, no dedup, no index table.
//
// Bigger than revision 9 on a story whose classes repeat, smaller on one
// whose classes are mostly distinct, and much simpler for the engine: the
// record for class n is at stybase + n * 8, which is the addressing
// engine.s already used for the table it built by parsing CSS. So the
// engine reads this by allocating nclass * 8 bytes and blitting, and the
// CSS parser goes away.
//
// The header states no offsets. The records begin at a fixed offset and the
// body records begin immediately after them, which the 6502 gets for free:
// readdata advances virdata past what it read, so the pointer is already
// where the xsty scan wants to start.

static uint8_t *build_usty_flat(uint32_t *sizep) {
	styclass *cls;
	int nclass;
	uint8_t *out;
	uint32_t size, recoffs, xstyoffs;
	int nxsty = 0, npadleft = 0, nauto = 0;
	int i;

	cls = parse_look(&nclass);
	if(!cls) return 0;

	recoffs = USTY_FLAT_HDRSIZE;
	xstyoffs = recoffs + nclass * USTY_FLAT_RECSIZE;
	size = xstyoffs;
	for(i = 0; i < nclass; i++) {
		if(cls[i].hasbody) size += sty_target->xstysize;
	}
	out = malloc(size);
	if(!out) {
		fprintf(stderr, "Out of memory.\n");
		exit(1);
	}

	out[0] = sty_target->tag | USTY_VERSION_FLAT;
	out[1] = nclass;
	out[3] = sty_target->xstysize;  // per-target body record stride

	for(i = 0; i < nclass; i++) {
		make_flat(out + recoffs + i * USTY_FLAT_RECSIZE, &cls[i]);
		if(cls[i].padleft) npadleft++;
		if(cls[i].flo == 3) nauto++;
		if(cls[i].hasbody) {
			make_xsty(out + xstyoffs + nxsty * sty_target->xstysize,
				i, &cls[i]);
			nxsty++;
		}
	}
	out[2] = nxsty;

	// See make_flat(): these two are parsed but have nowhere to go in an
	// eight-byte record. Say so once per story rather than once per class.
	if(npadleft) {
		warning(WARN_STYLE,
			"%d style class%s set a left margin or padding, which a USTY "
			"record has no room for; it was ignored, as the interpreter's "
			"own style sheet parser ignores it.",
			npadleft, (npadleft == 1)? "" : "es");
	}
	if(nauto) {
		warning(WARN_STYLE,
			"%d style class%s set \"margin: auto\", which a USTY record "
			"cannot encode; the div will not be centered.",
			nauto, (nauto == 1)? "" : "es");
	}

	free(cls);
	*sizep = size;
	return out;
}

// ----------------------------------------------------------------------------
// Public API.

// Called after rewrite_chunks(). On a target whose interpreter has no style
// sheet parser, failing to build the table is fatal rather than a warning:
// the alternative is a disk image that boots and then renders every div with
// whatever the heap happened to contain. Both ways of getting here --
// build_usty*() returning 0, and the story having no LOOK chunk for
// rewrite_6502_sty() to key off -- land in the same place.

void bundle_sty_check(void) {
	if(sty_target && sty_target->usty_required && !sty_emitted) {
		fprintf(stderr,
			"Error: could not build the %s style table, and the %s "
			"interpreter has no style sheet parser to fall back on.\n",
			sty_target->name, sty_target->name);
		exit(1);
	}
}

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

	// USTY takes LOOK's place in the chunk order: it is derived from
	// LOOK's payload and supersedes it, so shipping both would be dead
	// weight in the disk image. If the table cannot be built the story
	// keeps LOOK and the interpreter parses it at startup, which is why the
	// engine still carries that parser.
	if(sty_target && !sty_emitted && !strcmp(id, "LOOK")) {
		if(!sty_payload) {
			sty_payload = build_usty_flat(&sty_size);
		}
		if(sty_payload) {
			memcpy(newid, "USTY", 4);
			*newdata = sty_payload;
			*newsize = sty_size;
			sty_emitted = 1;
			return CHUNK_REPLACE;
		}
	}
	return act;
}