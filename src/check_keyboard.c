#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aambundle.h"

/* Each target is described by the printable ASCII its keyboard cannot
 * produce; everything outside 20-7e is unreachable on both. The c64 set comes
 * straight from the keymap table in 6502/c64_frontend.s. The apple2 set is the
 * Apple II+ keyboard, which has no bracket key; it also has no lowercase, but
 * the engine lowercases input, so that does not narrow what can be matched. */

// How many offending words to name before summarizing the rest.

#define MAXREPORT 8

// Longest word we will spell out in a warning, in aa-characters.

#define MAXWORD 32

static uint16_t get16(uint8_t *data) {
	return (data[0] << 8) | data[1];
}

// Map an aa-character to a Unicode codepoint, using the extended character
// table in the LANG chunk (see check_charset). Returns 0 for a character the
// table does not cover.

static uint32_t codepoint(uint8_t *lang, uint32_t langsize, uint8_t aach) {
	uint32_t offs;
	int n;

	if(aach >= 0x20 && aach < 0x7f) {
		return aach;
	}

	if(!lang || langsize < 8) {
		return 0;
	}

	offs = get16(lang + 2);
	if(offs >= langsize) {
		return 0;
	}

	n = lang[offs++];
	if(aach < 0x80 || aach - 0x80 >= n) {
		return 0;
	}

	offs += (aach - 0x80) * 5;
	if(offs + 5 > langsize) {
		return 0;
	}

	return (lang[offs + 2] << 16) | (lang[offs + 3] << 8) | lang[offs + 4];
}

static int put_utf8(char *buf, int len, int size, uint32_t cp) {
	if(cp < 0x80) {
		if(len + 1 >= size) return len;
		buf[len++] = cp;
	} else if(cp < 0x800) {
		if(len + 2 >= size) return len;
		buf[len++] = 0xc0 | (cp >> 6);
		buf[len++] = 0x80 | (cp & 0x3f);
	} else if(cp < 0x10000) {
		if(len + 3 >= size) return len;
		buf[len++] = 0xe0 | (cp >> 12);
		buf[len++] = 0x80 | ((cp >> 6) & 0x3f);
		buf[len++] = 0x80 | (cp & 0x3f);
	} else {
		if(len + 4 >= size) return len;
		buf[len++] = 0xf0 | (cp >> 18);
		buf[len++] = 0x80 | ((cp >> 12) & 0x3f);
		buf[len++] = 0x80 | ((cp >> 6) & 0x3f);
		buf[len++] = 0x80 | (cp & 0x3f);
	}

	buf[len] = 0;
	return len;
}

// A word is unreachable if any of its characters cannot come out of the
// keyboard. Input is lowercased before it is looked up (tolower in
// 6502/engine.s), so a key that yields an uppercase letter counts as a way to
// type the lowercase one, and every keyboard we care about has all 26. What
// is left is the punctuation the key matrix happens not to reach, plus the
// whole extended range: no 6502 frontend has a compose key or a dead key, so
// 80-ff can never be typed at all.

static int typeable(uint32_t cp, const char *untypeable) {
	if(cp < 0x20 || cp >= 0x7f) {
		return 0;
	}

	return !strchr(untypeable, cp);
}

// Mark every dictionary word that MAPS refers to. Entries are {key, value}
// pairs and a key in 2000..3dff is a dictionary word (see put_value in
// aamshow.c); the values name objects, which do not matter here.
//
// Returns a bitmap of nword bits, or null if the story has no usable MAPS.

static uint8_t *parse_targets(int nword) {
	uint8_t *maps, *bits;
	uint32_t mapssize, off, p;
	int nmap, nentry, i, k, key;

	maps = find_chunk("MAPS", &mapssize);
	if(!maps || mapssize < 2) {
		return 0;
	}

	bits = calloc((nword + 7) / 8, 1);
	if(!bits) {
		return 0;
	}

	nmap = get16(maps);
	for(i = 0; i < nmap; i++) {
		if(2 + i * 2 + 2 > (int) mapssize) {
			break;
		}
		off = get16(maps + 2 + i * 2);
		if(off + 2 > mapssize) {
			continue;
		}
		nentry = get16(maps + off);
		p = off + 2;
		for(k = 0; k < nentry; k++) {
			if(p + 4 > mapssize) {
				break;
			}
			key = get16(maps + p);
			p += 4;
			if(key >= 0x2000 && key < 0x3e00) {
				key &= 0x1fff;
				if(key < nword) {
					bits[key >> 3] |= 1 << (key & 7);
				}
			}
		}
	}

	return bits;
}

// Only the words MAPS refers to are checked. MAPS is the parser's
// word-to-object table, so a word listed there is one the story expects the
// player to type at it; most of the rest of DICT is not. Strings in WRIT
// reference dictionary words as a compression device, and Dialog emits an
// entry per spelling it has seen, so the bulk of the dictionary is prose --
// "don't", "noel", "'adam'" -- that is never matched against input and that no
// keyboard needs to be able to produce. Checking all of DICT warns about those
// too, which on the stories to hand is more than half the output.
//
// Two blind spots come with that:
//
//   * MAPS only holds words that name objects. A verb, preposition or topic
//     synonym reaches the parser as a constant in CODE instead, so an
//     untypeable one of those goes unreported. Catching them means walking
//     CODE with aaopinfo, which aambundle links but does not otherwise use.
//
//   * A word named here may still be harmless, because the story registers a
//     typeable spelling of it as well: forensic has both "looter's" and the
//     curly-quoted form, and only the second is flagged. Deciding that
//     automatically means folding the untypeable characters away
//     (fontdef.txt already carries transliterations for them) and asking
//     whether the result is a dictionary word covering the same objects --
//     a suppression pass layered on this one rather than a change to it.

void check_keyboard(const char *target_name, const char *untypeable) {
	uint8_t *dict, *lang, *targets;
	uint32_t dictsize, langsize, start, cp;
	char buf[MAXREPORT * (MAXWORD * 4 + 4) + 8];
	int nword, ntarget = 0, len, i, j, bad, nbad = 0, buflen = 0;

	if(keyboard_warning_level == WARN_NEVER) {
		return;
	}

	dict = find_chunk("DICT", &dictsize);
	if(!dict || dictsize < 2) {
		return;
	}
	lang = find_chunk("LANG", &langsize);

	nword = get16(dict);
	targets = parse_targets(nword);
	if(!targets) {
		return;
	}

	for(i = 0; i < nword; i++) {
		if(!(targets[i >> 3] & (1 << (i & 7)))) {
			continue;
		}
		if(2 + i * 3 + 3 > (int) dictsize) {
			break;
		}
		ntarget++;

		len = dict[2 + i * 3];
		start = get16(dict + 2 + i * 3 + 1);
		if(start + len > dictsize) {
			continue;
		}

		bad = 0;
		for(j = 0; j < len; j++) {
			if(!typeable(codepoint(lang, langsize, dict[start + j]), untypeable)) {
				bad = 1;
				break;
			}
		}
		if(!bad) {
			continue;
		}

		if(nbad < MAXREPORT) {
			if(nbad) {
				buflen = put_utf8(buf, buflen, sizeof(buf), ' ');
			}
			buflen = put_utf8(buf, buflen, sizeof(buf), '"');
			for(j = 0; j < len && j < MAXWORD; j++) {
				cp = codepoint(lang, langsize, dict[start + j]);
				buflen = put_utf8(buf, buflen, sizeof(buf),
					cp? cp : 0xfffd);
			}
			if(len > MAXWORD) {
				buflen = put_utf8(buf, buflen, sizeof(buf), 0x2026);
			}
			buflen = put_utf8(buf, buflen, sizeof(buf), '"');
		}
		nbad++;
	}

	free(targets);

	if(!nbad) {
		return;
	}

	warning(
		"%d of the story's %d object names contain characters that "
		"cannot be typed on the %s keyboard, so the player can never "
		"name them: %s%s",
		nbad, ntarget, target_name, buf,
		(nbad > MAXREPORT)? " ..." : "");
}
