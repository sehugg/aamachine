#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aambundle.h"
#include "aavm.h"

// ============================================================================
// USTY chunk generation: precomputed style table for the 6502 engines.
// The chunk layout is specified in docs/aam-specification-1.1-8bit.adoc.
//
// Parses the LOOK chunk (CSS declarations) once, in C, and emits that table.
// USTY replaces LOOK on the 6502 targets, which have no style sheet parser.
//
// The parser mirrors both the dialog compiler's effective CSS subset
// (~/if/dialog/src/frontend.c:2899-3000): case-insensitive keys,
// unknown properties ignored, width/height accept % (relative)
// and em/ch/en (absolute), margins absolute only, fractional units truncated.
// ============================================================================

#define STY_RELW  0x01
#define STY_RELH  0x02

#define NOCOLOR   0x80    // "$80 = inherit" sentinel for the sty fg field
#define INITIAL   0x81    // "$81 = initial" sentinel: reset explicitly-coloured ancestors

// Bytes of body payload per c64 xsty record
#define XSTY_SIZE_C64 6

// xsty payload for C64.
enum {
	BODY_BG = 0,
	BODY_BORDER = 1,
	BODY_CURSOR = 2,
	BODY_STATUS = 3,
	BODY_PAL = 4,		// BODY_PAL + (style & 7)
	BODY_N = 12
};

static const char *bodyprops[BODY_N] = {
	"background-color", "border-color",
	"cursor-color", "status-color",
	"normal-color",                 // 0  -
	"reverse-color",                // 1  reverse
	"bold-color",                   // 2  bold
	"bold-reverse-color",           // 3  bold reverse
	"italic-color",                 // 4  italic
	"italic-reverse-color",         // 5  italic reverse
	"bold-italic-color",            // 6  bold italic
	"bold-italic-reverse-color",    // 7  bold italic reverse
};

// The c64 frontend's compiled-in defaults.
// Keep this in sync with c64 frontend.
static const uint8_t c64_bodydef[BODY_N] = {
	0x0f, 0x0f,             // background, border
	0x08, 0x0b,             // cursor, status
	0x00, 0x01,             // -, reverse
	0x0b, 0x07,             // bold, bold reverse
	0x06, 0x02,             // italic, italic reverse
	0x0e, 0x0a,             // bold italic, bold italic reverse
};

struct sty_target {
	const char *name;
	uint8_t platform;       // USTY_PLATFORM_* index written to the header
	int have_vic_color;     // per-character fg color (c64)
	uint8_t stymask;        // AASTYLE_* bits the frontend can actually act on
	const uint8_t *bodydef; // BODY_N defaults, or null if the target has no records
	uint8_t bodylen;        // body payload bytes per record (the record's datalen);
	                        // 0 if bodydef is null

	int mincols, maxcols;	// 40 for c64, 40-80 for apple2, 80 for aambox
	int maxrows;		// 20 for all
};

static const struct sty_target sty_aambox = {
	"aambox", USTY_PLATFORM_AAMBOX, 0,
	0, 0, 0,
	80, 80, 20
};
static const struct sty_target sty_c64 = {
	"c64", USTY_PLATFORM_C64, 1,
	AASTYLE_REVERSE | AASTYLE_BOLD | AASTYLE_ITALIC, c64_bodydef, XSTY_SIZE_C64,
	40, 40, 20
};
static const struct sty_target sty_apple2 = {
	"apple2", USTY_PLATFORM_APPLE2, 0,
	AASTYLE_REVERSE, 0, 0,
	40, 80, 20
};

// Every target a -iftf-sys-<target>- prefix may name. Used to tell a
// declaration meant for another machine (silent: the same source is built
// for each target in turn) from a typo in the target name (warned about).
static const struct sty_target *const sty_targets[] = {
	&sty_aambox, &sty_c64, &sty_apple2
};

static int known_target(const char *name) {
	int i;

	for(i = 0; i < (int) (sizeof(sty_targets) / sizeof(sty_targets[0])); i++) {
		if(!strcmp(name, sty_targets[i]->name)) return 1;
	}
	return 0;
}

static const struct sty_target *sty_target;
static uint8_t *sty_payload;    // malloc'd USTY chunk payload (leaked, tool lifetime)
static uint32_t sty_size;
static int sty_emitted;

// Per-class record, one for every class in LOOK.
typedef struct {
	uint8_t styon, styoff;  // AASTYLE_* bits
	uint8_t fg;             // c64 palette index, NOCOLOR = inherit, INITIAL = initial
	uint8_t mtop, mbottom;  // top/bottom margins in rows
	uint8_t width, height;  // width/height in columns
	uint8_t flo;            // 0 none, 1 left, 2 right
	uint8_t flags;          // STY_RELW | STY_RELH
	uint8_t body[BODY_N];   // xsty fields, only valid if hasbody
	uint8_t hasbody;        // class declared at least one body color
} styclass;

// ----------------------------------------------------------------------------
// Color mapping. The C64 has a fixed 16-color palette. Because it is so
// small, colors have to be named explicitly (or given as a bare 0..15
// index); the bundler does not approximate an arbitrary RGB value, so the
// author always knows which palette entry they are getting. "inherit",
// "initial" and "transparent" (or alpha 0) leave the field unset.

// CSS color names map by name to their obvious VIC-II counterparts
static const struct {
	const char *name;
	uint8_t vic;
} css2vic[] = {
	// canonical names (0..15 in order)
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
	{"lightred", 10},
	{"darkgrey", 11},
	{"mediumgrey", 12},
	{"lightgreen", 13},
	{"lightblue", 14},
	{"lightgrey", 15},
	// synonyms (undocumented)
	{"maroon", 2},
	{"aqua", 3},
	{"teal", 3},
	{"magenta", 4},
	{"fuchsia", 4},
	{"lime", 5},
	{"navy", 6},
	{"pink", 10},
	{"mediumgray", 12},
	{"darkgray", 11},
	{"lightgray", 15},
	{"silver", 15},
};

// Style warnings, routed through swarn() so that they can be silenced
// while parsing declarations that a -iftf-sys- declaration in the same
// class overrides anyway. They are prefixed with the style-name of the
// class being parsed (see sty_name), so the author can tell which
// declaration each complaint is about.
static int sty_quiet;

// Name of the style class currently being parsed; "class N" if the LOOK
// chunk does not carry a style-name: declaration for it.
static const char *sty_name = 0;

static void vswarn(int quietable, const char *fmt, va_list ap) {
	char msg[1024];
	if(quietable && sty_quiet) return;
	vsnprintf(msg, sizeof(msg), fmt, ap);
	warning(WARN_STYLE, "style class %s: %s", sty_name ? sty_name : "(unknown)", msg);
}

static void swarn(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vswarn(1, fmt, ap);
	va_end(ap);
}

// An author mistake, as opposed to a fallback declaration that a
// -iftf-sys- line in the same class overrides. Nothing in the class can
// make a malformed -iftf- prefix correct, so these are never silenced by
// sty_quiet (see parse_decl).
static void swarn_strict(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vswarn(0, fmt, ap);
	va_end(ap);
}

// Parse a named VIC-II color or a bare palette index (0..15).
// Returns 1 and sets *out to a palette index, NOCOLOR (inherit/transparent)
// or INITIAL; returns 0 if the value is not a usable color
//   (hex, rgb(), garbage).
//
// Hex (#rgb/#rrggbb) and rgb()/rgba() are deliberately rejected. The VIC-II
// palette is fixed and small, so an author has to name the color they want
// (or pick an index) instead of having the bundler approximate it.
static int parse_color(const char *v, int *out) {
	int i, n;

	while(*v == ' ' || *v == '\t') v++;

	// transparent has no meaning for text on these targets; treat it as
	// inherit, i.e. leave whatever explicit color the enclosing element set.
	if(!strncmp(v, "inherit", 7)
	|| !strncmp(v, "transparent", 11)) {
		*out = NOCOLOR;
		return 1;
	}
	if(!strncmp(v, "initial", 7)) {
		*out = INITIAL;
		return 1;
	}

	if(v[0] == '#') {
		swarn("Hex colors are not supported on %s; use one of the 16 named C64 colors or an index 0..15.", sty_target->name);
		return 0;
	} else if(!strncmp(v, "rgb(", 4) || !strncmp(v, "rgba(", 5)) {
		swarn("rgb()/rgba() colors are not supported on %s; use one of the 16 named C64 colors or an index 0..15.", sty_target->name);
		return 0;
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
}

// ----------------------------------------------------------------------------
// CSS value parsing.
// Returns the number of conversions:
//   0 = not a length
//   1 = bare number,
//   2 = number and unit
//       (*unit is "" for a bare number, "%" or "em"/"ch"/"en")
static int scan_length(const char *value, int *val, char *unit) {
	int frac, n;

	while(*value == ' ' || *value == '\t') value++;
	if((*value < '0' || *value > '9') && *value != '.') return 0;
	unit[0] = 0;
	// Parse the integer and fractional parts separately. Using %f here is
	// not portable: libraries disagree about whether an incomplete exponent
	// such as "1e" in "1em" is consumed as part of the number (glibc bug
	// 1765), which would leave "m" as the unit. Fractions are truncated.
	n = sscanf(value, "%d.%d %15s", val, &frac, unit);
	if(n > 1) return n - 1; // 2 = number + unit, 1 = bare number
	n = sscanf(value, "%d %15s", val, unit);
	if(n < 1) return 0;
	return n;
}

static int isabsunit(const char *unit) {
	return !strcmp(unit, "em") || !strcmp(unit, "ch") || !strcmp(unit, "en");
}

// Parse one null-terminated declaration, e.g. "width: 100%" or
// "-iftf-sys-c64-color: red". The line is copied so the key can be lowercased
// in place; 'key' starts at the buffer, 'value' points into it.
//
// Declarations are parsed in two passes per class (see parse_look):
// - pass 0 takes the unprefixed ones
// - pass 1 the -iftf- ones so they override
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
		if(*q >= 'A' && *q <= 'Z') *q |= 0x20; // make lowercase
	}
	while(q > key && (q[-1] == ' ' || q[-1] == '\t')) *--q = 0;

	// -iftf-sys-<target>-* properties
	if(!strncmp(key, "-iftf-sys-", 10)) {
		prefixed = 1;
		char *prop;
		key += 10;
		prop = strchr(key, '-');
		if(!prop) {
			// "-iftf-sys-c64" with no property name after the target.
			if(!pass)
				swarn_strict("Malformed conditional declaration; expected -iftf-sys-<target>-<property>.");
			return;
		}
		*prop = 0;
		// A declaration that names another machine is silent, by design:
		// one story is built for every target in turn, and each build is
		// entitled to complain only about its own declarations. A name that
		// is no target at all is a typo, and can never take effect anywhere.
		if(strcmp(key, sty_target->name)) {
			if(!pass && !known_target(key))
				swarn_strict("Unknown target \"%s\" in a conditional declaration; the known targets are aambox, c64 and apple2.", key);
			return;
		}
		key = prop + 1;
	} else if(!strncmp(key, "-iftf-", 6) && strcmp(key, "-iftf-reverse-video")) {
		// -iftf-reverse-video is Dialog's own conditional property and
		// arrives without a target. Every other -iftf- key is either the
		// older -iftf-<target>-<property> spelling or a typo; the bundler
		// does not honour either, so say so rather than let a declaration
		// that can never take effect pass in silence.
		if(!pass) {
			char tgt[32];
			char *seg = key + 6, *dash = strchr(seg, '-');
			int legacy = 0;

			if(dash && (size_t) (dash - seg) < sizeof(tgt)) {
				memcpy(tgt, seg, (size_t) (dash - seg));
				tgt[dash - seg] = 0;
				legacy = known_target(tgt);
			}
			if(legacy)
				swarn_strict("%s is not a conditional declaration; write -iftf-sys-%s-<property>.", key, tgt);
			else
				swarn_strict("Unrecognized conditional declaration \"%s\"; conditional declarations look like -iftf-sys-<target>-<property>.", key);
		}
		return;
	}
	if(prefixed != pass) return;

	char param[32] = { 0 };
	sscanf(value, "%31s", param);

	if(!strcmp(key, "-iftf-reverse-video")) {
		if(!strcmp(param, "reverse")) c->styon |= AASTYLE_REVERSE;
		else if(!strcmp(param, "none")) c->styoff |= AASTYLE_REVERSE;
		else if(strcmp(param, "inherit"))
			swarn("Invalid value for %s: %s", key, value);
	}

	// Body colors require the -iftf-sys- prefix
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
	} else if(!strcmp(key, "margin-top") || !strcmp(key, "margin-bottom")) {
		int v;
		char unit[16];
		int n = scan_length(value, &v, unit);
		matched = 1;
		if(n >= 1 && (!unit[0] || isabsunit(unit))) {
			// Absolute only
			if(!strcmp(key, "margin-top")) c->mtop = v;
			else if(!strcmp(key, "margin-bottom")) c->mbottom = v;
		} else if(n == 2 && !strcmp(unit, "%")) {
			swarn("Percent %s \"%s\" is not supported and was ignored.", key, value);
		} else {
			swarn("Ignoring %s: unsupported value \"%s\".", key, value);
		}
	} else if(!strcmp(key, "float")) {
		matched = 1;
		if(!strcmp(param, "left")) c->flo = 1;
		else if(!strcmp(param, "right")) c->flo = 2;
		else if(strcmp(param, "none") && strcmp(param, "inherit")) {
			swarn("Invalid value for float: %s (only left, right and none are supported).", value);
		}
	} else if(!strcmp(key, "margin")) {
		matched = 1;
		if(!strcmp(param, "auto")) c->flo = 3;
		else swarn("Invalid value for margin: %s (use \"margin: auto\").", value);
	} else if(!strcmp(key, "font-style")) {
		matched = 1;
		if(!strcmp(param, "italic") || !strcmp(param, "oblique"))
			c->styon |= AASTYLE_ITALIC;
		else if(!strcmp(param, "normal")) c->styoff |= AASTYLE_ITALIC;
		else if(strcmp(param, "inherit")) {
			swarn("Invalid value for font-style: %s (only italic, oblique and normal are supported).", value);
		}
	} else if(!strcmp(key, "font-weight")) {
		matched = 1;
		if(!strcmp(param, "bold")) c->styon |= AASTYLE_BOLD;
		else if(!strcmp(param, "normal")) c->styoff |= AASTYLE_BOLD;
		else if(strcmp(param, "inherit")) {
			// In particular: numeric weights (400, 700) are valid web
			// CSS but mean nothing here.
			swarn("Invalid value for font-weight: %s (only bold and normal are supported).", value);
		}
	} else if(!strcmp(key, "font-family")) {
		matched = 1;
		if(strstr(value, "monospace")) c->styon |= AASTYLE_FIXED;
		else if(strcmp(param, "inherit")) c->styoff |= AASTYLE_FIXED;
	} else if(!strcmp(key, "reverse-video")) {
		matched = 1;
		if(!strcmp(param, "reverse")) c->styon |= AASTYLE_REVERSE;
		else if(!strcmp(param, "none")) c->styoff |= AASTYLE_REVERSE;
		else if(strcmp(param, "inherit"))
			swarn("Invalid value for %s: %s", key, value);
	} else if(!strcmp(key, "color")) {
		matched = 1;
		if(sty_target->have_vic_color) {
			int ci;
			if(parse_color(value, &ci)) {
				c->fg = ci;
				// warn if color is same as default background color
				if(!prefixed && sty_target->bodydef &&
					ci == sty_target->bodydef[BODY_BG]) {
					swarn("color %s matches the default background color on %s; "
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
			if(sty_target->bodydef) {
				swarn("The background-color property is web-only. To set the screen background on %s, use -iftf-sys-%s-background-color.",
					sty_target->name, sty_target->name);
			}
		}
	} else if(!strcmp(key, "display")) {
		matched = 1;
		if(!strcmp(param, "none")) {
			swarn("display: none is not supported; the element will still be shown.");
		}
	} else if(!strcmp(key, "visibility")) {
		matched = 1;
		if(!strcmp(param, "hidden")) {
			swarn("visibility: hidden is not supported; the element will still be shown.");
		}
	}

	// A prefix that named the target but a property the bundler does not know.
	if(prefixed && !matched) {
		swarn_strict("-iftf-sys-%s-%s is not a property %s supports; it was ignored.", sty_target->name, key, sty_target->name);
	}
	// Anything else is ignored, as an interpreter must per the spec.
}

static void make_flat(uint8_t *r, const styclass *c) {
	uint8_t flags = c->flags & (USTY_FL_RELW | USTY_FL_RELH);

	if(c->flo == 1) flags |= USTY_FL_FLOATL;
	else if(c->flo == 2) flags |= USTY_FL_FLOATR;

	r[USTY_F_WIDTH] = c->width;
	r[USTY_F_HEIGHT] = c->height;
	r[USTY_F_MTOP] = c->mtop;
	r[USTY_F_MBOTTOM] = c->mbottom;
	r[USTY_F_STYON] = c->styon & sty_target->stymask;
	r[USTY_F_STYOFF] = c->styoff & sty_target->stymask;
	r[USTY_F_FLAGS] = flags;
	r[USTY_F_FG] = c->fg;
}

static void make_xsty(uint8_t *x, int classidx, const styclass *c) {
	int j;

	x[0] = c->body[BODY_BG] | (c->body[BODY_BORDER] << 4);
	x[1] = c->body[BODY_CURSOR] | (c->body[BODY_STATUS] << 4);
	for(j = 0; j < 4; j++) {
		x[2 + j] = c->body[BODY_PAL + j * 2]
			| (c->body[BODY_PAL + j * 2 + 1] << 4);
	}
	// TODO: warn if background/text colors match
}

static uint16_t get16(const uint8_t *p) {
	return (p[0] << 8) | p[1];
}

// Length of the null-terminated line at look[p], clamped to end.
static uint32_t line_len(const uint8_t *look, uint32_t p, uint32_t end) {
	uint32_t len = 0;
	while(p + len < end && look[p + len]) len++;
	return len;
}

// ----------------------------------------------------------------------------
// LOOK parse: read the class count, then walk each class's declaration
// block (null-terminated strings, a lone null byte ends the block)

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
		warning(WARN_STYLE, "Story has %d style classes, more than a USTY table can index (255).", n);
		return 0;
	}
	// need at least one style, ok if it's empty
	if (!n) n++;
	cls = calloc(n, sizeof(styclass));
	if(!cls) return 0;

	// fg defaults to "not set", which is not the same as black.
	for(i = 0; i < n; i++) {
		cls[i].fg = NOCOLOR;
	}

	for(i = 0; i < n; i++) {
		uint32_t ptr, end, p;
		int hasiftf = 0;
		char pfx[32];
		char namebuf[64];
		size_t plen;

		if((uint32_t)(2 + 2 * i + 2) > looksize) break;
		ptr = get16(look + 2 + 2 * i);
		if(ptr >= looksize) continue;
		end = looksize;

		// One lookahead pass over the class's declaration lines: pick up
		// its style-name (so warnings can name it), and check whether it
		// carries -iftf-sys-<target>- declarations for this machine. If so,
		// the generic (unprefixed) declarations are only a fallback that the
		// prefixed pass replaces wholesale -- so pass 0 below runs with
		// warnings off, and the author is not scolded twice for the same
		// styling.
		snprintf(namebuf, sizeof(namebuf), "class %d", i);
		sty_name = namebuf; // this is on the stack so we have to unset it
		snprintf(pfx, sizeof(pfx), "-iftf-sys-%s-", sty_target->name);
		plen = strlen(pfx);
		p = ptr;
		while(p < end && look[p]) {
			uint32_t linelen = line_len(look, p, end);
			char line[64], nm[56];

			if(linelen > plen && !strncasecmp((const char *) look + p, pfx, plen))
				hasiftf = 1;
			// "style-name: title", always written in this exact case.
			if(linelen < sizeof(line)) {
				memcpy(line, look + p, linelen);
				line[linelen] = 0;
				if(1 == sscanf(line, " style-name : %55s", nm)) {
					snprintf(namebuf, sizeof(namebuf), "@%s", nm);
					sty_name = namebuf;
				}
			}
			p += linelen + 1;
		}

		// Two passes over the same block: unprefixed declarations first,
		// then the -iftf-sys- ones, so that a prefixed declaration overrides
		// its unprefixed counterpart no matter which comes last in the CSS.
		for(pass = 0; pass < 2; pass++) {
			sty_quiet = hasiftf && pass == 0;
			p = ptr;
			while(p < end && look[p]) {
				uint32_t linelen = line_len(look, p, end);
				parse_decl(&cls[i], (const char *) look + p, linelen, pass);
				p += linelen + 1;
			}
		}
		sty_quiet = 0; // re-enable warnings
		sty_name = 0; // unset style class name
	}
	*nclassp = n;
	return cls;
}

// Build the USTY chunk. See docs/aam-specification-1.1-8bit.adoc for the
// layout.
static uint8_t *build_usty_flat(uint32_t *sizep) {
	styclass *cls;
	int nclass;
	uint8_t *out;
	uint32_t size, recoffs, xstyoffs, blockbytes, totalwords;
	int nxsty = 0, nover = 0, maxnsty, reclen;
	int i;

	cls = parse_look(&nclass);
	if(!cls) return 0;

	recoffs = USTY_HDRSIZE;
	xstyoffs = recoffs + nclass * USTY_RECSIZE;
	reclen = 2 + sty_target->bodylen;       // index, datalen, payload
	maxnsty = sty_target->bodylen?
		(USTY_MAX_XSTYBYTES - 1) / reclen : 0;

	// Upper bound: the full body array with its terminator, which is what
	// the engine's one-page scan can reach.
	size = xstyoffs + USTY_MAX_XSTYBYTES + 1;

	out = calloc(1, size);
	if(!out) {
		warning(WARN_ERROR, "Out of memory");
		exit(1);
	}

	out[0] = USTY_VERSION;
	out[1] = nclass;
	// out[2] = nxsty, filled in below; out[3] = platform index.
	out[3] = sty_target->platform;
	// out[6..7] = xstyoff, relative to the record base.
	out[6] = (xstyoffs - recoffs) >> 8;
	out[7] = (xstyoffs - recoffs) & 0xff;

	for(i = 0; i < nclass; i++) {
		make_flat(out + recoffs + i * USTY_RECSIZE, &cls[i]);
		if(cls[i].hasbody) {
			if(nxsty < maxnsty) {
				uint8_t *x = out + xstyoffs + nxsty * reclen;
				x[0] = i;
				x[1] = sty_target->bodylen;
				make_xsty(x + 2, i, &cls[i]);
				nxsty++;
			} else {
				nover++;
			}
		}
	}

	// The array always ends in $ff, even empty: the terminator is the only
	// thing that tells the engine's scan to stop.
	out[xstyoffs + nxsty * reclen] = USTY_XSTY_END;

	if(nover) {
		warning(WARN_STYLE, "Too many classes have -iftf-sys-%s- colors (max is %d) so some body styles were dropped.",
			sty_target->name, maxnsty);
	}

	blockbytes = nclass * USTY_RECSIZE + nxsty * reclen + 1;
	totalwords = (blockbytes + 1) / 2; // word = 2 bytes
	size = recoffs + totalwords * 2; // size is in bytes
	out[2] = nxsty;
	out[4] = totalwords >> 8;
	out[5] = totalwords & 0xff;

	free(cls);
	*sizep = size;
	return out;
}

// ----------------------------------------------------------------------------
// Public API.

// Called after rewriting USTY, exits program with an error code it didn't happen.
void gen_usty_check(void) {
	if(sty_target && !sty_emitted) {
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
	// drop FILE chunks
	if(!strcmp(id, "FILE")) return CHUNK_DROP;

	chunk_action_t act = CHUNK_KEEP;

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