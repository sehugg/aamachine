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
// binary table defined in STYLE_SPEC.md. USTY replaces LOOK on the 6502
// targets: the c64 and apple2 interpreters have no style sheet parser left
// (NO_CSS_PARSER), so the table is a requirement rather than an
// optimization, and gen_usty_check() aborts the bundle if one cannot be
// built.
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

// A body record has to fit databuf on the 6502 (engine.s:138), which is what
// keeps a readdata-based reader available to a target too tight to hold the
// array resident. Caught here rather than in a comment, because the number
// lives in struct sty_target and a new target would set its own.
typedef char xsty_fits_databuf[(XSTY_SIZE_C64 <= USTY_MAX_XSTYSIZE)? 1 : -1];

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
	{"purple", 4},
	{"green", 5},
	{"blue", 6},
	{"yellow", 7},
	{"orange", 8},
	{"brown", 9},
	{"pink", 10},
	{"darkgrey", 11},
	{"mediumgrey", 12},
	{"lightgreen", 13},
	{"lightblue", 14},
	{"lightgrey", 15},
	// synonyms
	{"maroon", 2},
	{"aqua", 3},
	{"teal", 3},
	{"magenta", 4},
	{"fuchsia", 4},
	{"lime", 5},
	{"navy", 6},
	{"lightred", 10},
	{"gray", 11},
	{"grey", 11},
	{"darkgray", 11},
	{"lightgray", 15},
	{"silver", 15},
};

// Map an rgb triplet to the nearest VIC-II color, using a perceptual
// distance (weighted RGB, a standard cheap approximation of CIE lightness).
// Style warnings, routed through swarn() so that they can be silenced
// while parsing declarations that a -iftf-sys- declaration in the same
// class overrides anyway: the prefixed pass is the author speaking to this
// machine, and it gets to say what it replaces.
static int sty_quiet;

static void swarn(const char *fmt, ...) {
	va_list ap;
	if(sty_quiet) return;
	va_start(ap, fmt);
	vwarning(WARN_STYLE, fmt, ap);
	va_end(ap);
}

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
	if(bestdist > (2+3+4)*64*64) {
		swarn("The color #%02x%02x%02x is not accurately represented on %s, the closest is %s (index %d, #%02x%02x%02x).",
			r, g, b, sty_target->name, css2vic[best].name, best,
			c64_rgb[best][0], c64_rgb[best][1], c64_rgb[best][2]);
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
		swarn("Invalid VIC color index \"%s\", must be in range 0..15.", v_old);
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
			swarn("Unknown c64 color \"%s\".", v);
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

// Length scan, in the spirit of frontend.c's "sscanf(str, "width : %d %s")":
// one sscanf picks the number and the unit apart whatever whitespace sits
// between them. %f rather than %d so that the fractional part is truncated
// the way css_abs_rel in engine.s truncates it (".67em" is 0, not 67 rows).
// The first-character guard keeps scanf's "nan"/"inf" extensions out.
// Returns the number of conversions: 0 = not a length, 1 = bare number,
// 2 = number and unit (*unit is then "" for a bare number, "%" or
// "em"/"ch"/"en").
static int scan_length(const char *value, int *val, char *unit) {
	float f;
	int n;

	while(*value == ' ' || *value == '\t') value++;
	if((*value < '0' || *value > '9') && *value != '.') return 0;
	unit[0] = 0;
	n = sscanf(value, "%f %15s", &f, unit);
	if(n < 1) return 0;
	*val = (int) f;         // truncated, like the engine
	return n;
}

static int isabsunit(const char *unit) {
	return !strcmp(unit, "em") || !strcmp(unit, "ch") || !strcmp(unit, "en");
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
		char param[32];
		if(1 == sscanf(value, "%31s", param)) {
			if(!strcmp(param, "reverse")) c->styon |= AASTYLE_REVERSE;
			else if(!strcmp(param, "none")) c->styoff |= AASTYLE_REVERSE;
			else swarn("Invalid value for %s: %s", key, value);
		}
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

	if(!strcmp(key, "width") || !strcmp(key, "height")) {
		int v;
		char unit[16];
		int n = scan_length(value, &v, unit);
		matched = 1;
		if(n == 2 && !strcmp(unit, "%")) {
			if(v > 100) {
				// Bigger than the whole screen; the web overflows, an
				// 8-bit machine cannot. (For height it is doubly
				// pointless: the engine treats any relative height as
				// one row.)
				swarn("Percent %s \"%s\" is more than 100%% and will not fit the screen.",
					key, value);
			}
			if(*key == 'w') {
				c->width = v;
				c->flags |= STY_RELW;
			} else {
				c->height = v;
				c->flags |= STY_RELH;
			}
		} else if(n >= 1 && (!unit[0] || isabsunit(unit))) {
			// Bare number or em/ch/en: absolute.
			if(*key == 'w') c->width = v;
			else c->height = v;
		} else {
			swarn("Ignoring %s: unsupported value \"%s\".", key, value);
		}
	} else if(!strcmp(key, "margin-top") || !strcmp(key, "margin-bottom")
		|| !strcmp(key, "margin-left") || !strcmp(key, "padding-left")) {
		int v;
		char unit[16];
		int n = scan_length(value, &v, unit);
		matched = 1;
		if(n >= 1 && (!unit[0] || isabsunit(unit))) {
			// Absolute only: a percent of what, on a text screen?
			if(!strcmp(key, "margin-top")) c->mtop = v;
			else if(!strcmp(key, "margin-bottom")) c->mbottom = v;
			else c->padleft = v;
		} else if(n == 2 && !strcmp(unit, "%")) {
			swarn("Percent %s \"%s\" is not supported and was ignored.", key, value);
		} else {
			swarn("Ignoring %s: unsupported value \"%s\".", key, value);
		}
	} else if(!strcmp(key, "float")) {
		char param[32];
		matched = 1;
		if(1 == sscanf(value, "%31s", param)) {
			if(!strcmp(param, "left")) c->flo = 1;
			else if(!strcmp(param, "right")) c->flo = 2;
			else if(strcmp(param, "none") && strcmp(param, "inherit")) {
				swarn("Invalid value for float: %s (only left, right and none are supported).", value);
			}
		}
	} else if(!strcmp(key, "margin")) {
		char param[32];
		matched = 1;
		if(1 == sscanf(value, "%31s", param)) {
			if(!strcmp(param, "auto")) c->flo = 3;
			else swarn("Invalid value for margin: %s (use \"margin: auto\").", value);
		}
	} else if(!strcmp(key, "text-align")) {
		char param[32];
		matched = 1;
		if(1 == sscanf(value, "%31s", param)) {
			if(!strcmp(param, "center")) c->align = 3;
			else if(!strcmp(param, "right")) c->align = 2;
			else if(!strcmp(param, "left")) c->align = 1;
			else if(strcmp(param, "inherit")) {
				swarn("Invalid value for text-align: %s (only left, right and center are supported).", value);
			}
		}
	} else if(!strcmp(key, "font-style")) {
		char param[32];
		matched = 1;
		if(1 == sscanf(value, "%31s", param)) {
			if(!strcmp(param, "italic") || !strcmp(param, "oblique"))
				c->styon |= AASTYLE_ITALIC;
			else if(!strcmp(param, "normal")) c->styoff |= AASTYLE_ITALIC;
			else if(strcmp(param, "inherit")) {
				swarn("Invalid value for font-style: %s (only italic, oblique and normal are supported).", value);
			}
		}
	} else if(!strcmp(key, "font-weight")) {
		char param[32];
		matched = 1;
		if(1 == sscanf(value, "%31s", param)) {
			if(!strcmp(param, "bold")) c->styon |= AASTYLE_BOLD;
			else if(!strcmp(param, "normal")) c->styoff |= AASTYLE_BOLD;
			else if(strcmp(param, "inherit")) {
				// In particular: numeric weights (400, 700) are valid web
				// CSS but mean nothing here.
				swarn("Invalid value for font-weight: %s (only bold and normal are supported).", value);
			}
		}
	} else if(!strcmp(key, "font-family")) {
		char param[32];
		matched = 1;
		// %s stops at the first whitespace, so use the whole value for
		// the monospace test, frontend.c-style.
		sscanf(value, "%31s", param);
		if(strstr(value, "monospace")) c->styon |= AASTYLE_FIXED;
		else if(strcmp(param, "inherit")) c->styoff |= AASTYLE_FIXED;
	} else if(!strcmp(key, "text-decoration") || !strcmp(key, "reverse-video")) {
		char param[32];
		matched = 1;
		sscanf(value, "%31s", param);
		// "reverse" is not a standard CSS value, but there is no standard
		// CSS property for reverse video, and unrecognized values are
		// explicitly not an error in CSS.
		if(strstr(value, "reverse")) {
			c->styon |= AASTYLE_REVERSE;
		} else if(strcmp(param, "inherit")) {
			// "none" is the explicit off; any other value (underline is
			// the classic web one) neither sets nor unsets reverse on
			// the web, so warn rather than quietly turning it off.
			c->styoff |= AASTYLE_REVERSE;
			if(!strcmp(key, "text-decoration") && strcmp(param, "none")) {
				swarn("Invalid value for text-decoration: %s (only reverse, none and inherit are supported).", value);
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
					swarn(						"color %s matches the default background color on %s; "
						"text with this class will be invisible unless (body style $Class) is used.",
						value, sty_target->name);
				}
			}
		} else {
			swarn("color is not supported on %s and was ignored.", sty_target->name);
		}
	} else if(!strcmp(key, "background-color")) {
		matched = 1;
		if(!prefixed) {
			// Only -iftf-sys-<sys>-background-color fills the body record; the
			// unprefixed form does nothing on any 6502 target. (On the web
			// it colors the element itself, not the screen, so the two
			// cannot share a declaration.)
			if(sty_target->bodydef) {
				swarn(					"To set the screen background on %s, use -iftf-sys-%s-background-color.",
					sty_target->name, sty_target->name);
			}
		}
	} else if(!strcmp(key, "display")) {
		char param[32];
		matched = 1;
		if(1 == sscanf(value, "%31s", param) && !strcmp(param, "none")) {
			swarn("display: none is not supported; the element will still be shown.");
		}
	} else if(!strcmp(key, "visibility")) {
		char param[32];
		matched = 1;
		if(1 == sscanf(value, "%31s", param) && !strcmp(param, "hidden")) {
			swarn("visibility: hidden is not supported; the element will still be shown.");
		}
	}

	// A prefix that named the target but a property the bundler does not
	// know. This is the one case where the author addressed this machine
	// specifically, so silence would hide a real gap -- the -iftf- set is
	// young and grows.
	if(prefixed && !matched) {
		swarn("-iftf-sys-%s-%s is not supported yet.", sty_target->name, key);
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
		swarn(			"Story has %d style classes, more than a USTY table can index "
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
		uint32_t ptr, end, p;
		int hasiftf = 0;
		char pfx[32];
		size_t plen;

		if((uint32_t)(2 + 2 * i + 2) > looksize) break;
		ptr = get16(look + 2 + 2 * i);
		if(ptr >= looksize) continue;
		end = looksize;

		// Does this class carry declarations addressed to this machine?
		// If so, the generic ones are only a fallback that the prefixed
		// pass replaces wholesale -- parse pass 0 with warnings off, so
		// the author is not scolded twice for the same styling.
		snprintf(pfx, sizeof(pfx), "-iftf-sys-%s-", sty_target->name);
		plen = strlen(pfx);
		p = ptr;
		while(p < end && look[p]) {
			uint32_t linelen = 0;
			while(p + linelen < end && look[p + linelen]) linelen++;
			if(linelen > plen && !strncasecmp((const char *) look + p, pfx, plen))
				hasiftf = 1;
			p += linelen + 1;
		}

		// Two passes over the same block: unprefixed declarations first,
		// then the -iftf-sys- ones, so that a prefixed declaration overrides
		// its unprefixed counterpart no matter which comes last in the CSS.
		for(pass = 0; pass < 2; pass++) {
			sty_quiet = hasiftf && pass == 0;
			p = ptr;
			while(p < end && look[p]) {
				uint32_t linelen = 0;
				while(p + linelen < end && look[p + linelen]) linelen++;
				parse_decl(&cls[i], (const char *) look + p, linelen, pass);
				p += linelen + 1;
			}
		}
		sty_quiet = 0;
	}
	*nclassp = n;
	return cls;
}

// Revision 12: one 8-byte record per class, no dedup, no index table, and a
// header that states what the engine would otherwise have to work out.
//
// The record array is revision 11's, byte for byte: the record for class n
// is at stybase + n * 8, which is the addressing engine.s already used for
// the table it built by parsing CSS. What revision 12 adds is two header
// figures -- the total size of the resident block and the offset of the body
// array within it -- so that initengine4 is one allocwords and one
// readdatato with no multiplies at all. Deriving those two in the engine
// instead costs 45 bytes of 6502; stating them costs 4 bytes of chunk.
//
// The payload is padded to an even length after the header so that
// totalwords * 2 is exactly the number of bytes the engine reads.

// The number of body records a story would emit, which is wanted before the
// buffer is sized. A story that overflows the scan's one-page reach loses
// its body records (with a warning) rather than its whole style table --
// geometry and styles are what every story uses, themes are what one might.

static int count_body(styclass *cls, int nclass) {
	int i, n = 0;

	for(i = 0; i < nclass; i++) {
		if(cls[i].hasbody) n++;
	}
	if(!sty_target->xstysize) return 0;
	if(n * sty_target->xstysize > USTY_MAX_XSTYBYTES) {
		swarn(			"%d style classes carry -iftf-sys-%s- body colors, more than "
			"the %d bytes a USTY body array can reach (limit %d classes at "
			"%d bytes each); they were dropped, and SET_BODY will paint the "
			"interpreter's built-in colors.",
			n, sty_target->name, USTY_MAX_XSTYBYTES,
			USTY_MAX_XSTYBYTES / sty_target->xstysize,
			sty_target->xstysize);
		return 0;
	}
	return n;
}

static uint8_t *build_usty_flat(uint32_t *sizep) {
	styclass *cls;
	int nclass;
	uint8_t *out;
	uint32_t size, recoffs, xstyoffs, blockbytes, totalwords;
	int nbody, nxsty = 0, npadleft = 0, nauto = 0;
	int i;

	cls = parse_look(&nclass);
	if(!cls) return 0;

	nbody = count_body(cls, nclass);

	recoffs = USTY_EXT_HDRSIZE;
	xstyoffs = recoffs + nclass * USTY_FLAT_RECSIZE;

	// What the engine allocates and blits: the two arrays, rounded up to a
	// whole number of words because allocwords deals in words. The pad byte
	// is emitted rather than left implicit, so the engine's readdatato stops
	// inside the chunk instead of one byte past it.
	blockbytes = nclass * USTY_FLAT_RECSIZE + nbody * sty_target->xstysize;
	totalwords = (blockbytes + 1) / 2;
	size = recoffs + totalwords * 2;

	out = calloc(1, size);
	if(!out) {
		warning(WARN_ERROR, "Out of memory");
		exit(1);
	}

	out[0] = sty_target->tag | USTY_VERSION_EXT;
	out[1] = nclass;
	out[3] = sty_target->xstysize;  // per-target body record stride
	out[4] = totalwords >> 8;
	out[5] = totalwords & 0xff;
	out[6] = (xstyoffs - recoffs) >> 8;     // from the record base, which is
	out[7] = (xstyoffs - recoffs) & 0xff;   // the pointer the engine holds

	for(i = 0; i < nclass; i++) {
		make_flat(out + recoffs + i * USTY_FLAT_RECSIZE, &cls[i]);
		if(cls[i].padleft) npadleft++;
		if(cls[i].flo == 3) nauto++;
		if(cls[i].hasbody && nxsty < nbody) {
			make_xsty(out + xstyoffs + nxsty * sty_target->xstysize,
				i, &cls[i]);
			nxsty++;
		}
	}
	out[2] = nxsty;

	// See make_flat(): these two are parsed but have nowhere to go in an
	// eight-byte record. Say so once per story rather than once per class.
	if(npadleft) {
		swarn(			"%d style class%s set a left margin or padding, which a USTY "
			"record has no room for; it was ignored, as the interpreter's "
			"own style sheet parser ignores it.",
			npadleft, (npadleft == 1)? "" : "es");
	}
	if(nauto) {
		swarn(			"%d style class%s set \"margin: auto\", which a USTY record "
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

void gen_usty_check(void) {
	if(sty_target && sty_target->usty_required && !sty_emitted) {
		warning(WARN_ERROR,
			"Could not build the %s style table.",
			sty_target->name);
		exit(1);
	}
}

void gen_usty_set_target(const char *target) {
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