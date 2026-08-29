#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aambundle.h"

#include "table_charset.h"

// How many offending codepoints to name before summarizing the rest.

#define MAXREPORT 12

static uint16_t get16(uint8_t *data) {
	return (data[0] << 8) | data[1];
}

// Both targets render printable ASCII without consulting the font
// definition: the c64 from the base half of font.bin, the apple2 from its
// character generator ROM.

static int supported(uint32_t cp, uint8_t need) {
	int lo = 0, hi = AA_CHARSET_N - 1, mid;

	if(cp >= 0x20 && cp < 0x7f) {
		return 1;
	}

	while(lo <= hi) {
		mid = lo + (hi - lo) / 2;
		if(aa_charset[mid].cp == cp) {
			return !!(aa_charset[mid].flags & need);
		} else if(aa_charset[mid].cp < cp) {
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}

	return 0;
}

// The extended character table in the LANG chunk is the only place a story
// names a Unicode codepoint, so it is the whole of the check. Entries are
// {lowercase, uppercase, codepoint[3]}, the codepoint big-endian, matching
// aachar() in js/engine.js.

void check_charset(const char* target_name, int need_flags) {
	uint8_t *lang;
	uint32_t langsize, offs, cp;
	uint32_t bad[MAXREPORT];
	char buf[MAXREPORT * 16];
	int i, n, nbad = 0, len = 0;

	if(charset_warning_level == WARN_NEVER) {
		return;
	}

	lang = find_chunk("LANG", &langsize);
	if(!lang || langsize < 8) {
		// A story without a usable LANG chunk will not run at all; the
		// engines report that far more clearly than we could here.
		return;
	}

	offs = get16(lang + 2);
	if(offs >= langsize) {
		return;
	}

	n = lang[offs++];
	for(i = 0; i < n; i++) {
		if(offs + 5 > langsize) {
			break;
		}
		cp = (lang[offs + 2] << 16) | (lang[offs + 3] << 8) | lang[offs + 4];
		offs += 5;
		if(!supported(cp, need_flags)) {
			if(nbad < MAXREPORT) {
				bad[nbad] = cp;
			}
			nbad++;
		}
	}

	if(!nbad) {
		return;
	}

	for(i = 0; i < nbad && i < MAXREPORT; i++) {
		len += snprintf(buf + len, sizeof(buf) - len, " U+%04X", bad[i]);
		if(len >= (int) sizeof(buf)) {
			// Cannot happen at the current MAXREPORT, but snprintf
			// returns what it would have written, so a longer list
			// would otherwise walk off the end.
			len = sizeof(buf) - 1;
			break;
		}
	}

	warning(
		WARN_CHARSET,
		"%d of the story's %d extended characters cannot be rendered "
		"on %s and will appear as a fallback character:%s%s",
		nbad, n, target_name, buf,
		(nbad > MAXREPORT)? " ..." : "");
}
