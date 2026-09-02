#if (defined(_WIN32) || defined(__WIN32__))
#define mkdir(Path, Mode) mkdir(Path)
#endif

#include <stdarg.h>

typedef void (*chunk_visitor_t)(char *head, char *dirname, uint8_t *chunk, uint32_t size);

extern uint8_t *story;
extern uint32_t storysize;

void visit_chunks(char *storyname, int storynamesize, chunk_visitor_t chunk_visitor);

/* Locate a chunk in the story, returning its payload and (if sizep is
 * non-null) its size, or null if the story has no such chunk.
 *
 * The returned pointer is only valid until the next rewrite_chunks(), which
 * replaces the story buffer wholesale. */

uint8_t *find_chunk(const char *id, uint32_t *sizep);

/* Rewrite the story in a single pass. For every chunk, the rewriter says
 * whether to keep it as it is, drop it, or replace it -- in which case it
 * fills in newid (four characters, not terminated), newdata and newsize. A
 * replacement buffer must stay alive until rewrite_chunks() returns.
 *
 * A null rewriter keeps everything. */

typedef enum {
	CHUNK_KEEP,
	CHUNK_DROP,
	CHUNK_REPLACE,
	CHUNK_INSERT
} chunk_action_t;

typedef chunk_action_t (*chunk_rewriter_t)(
	const char *id,
	uint8_t *data,
	uint32_t size,
	char *newid,
	uint8_t **newdata,
	uint32_t *newsize);

void rewrite_chunks(chunk_rewriter_t rewriter, int align_writ);

/* Warnings, similar to how dialogc does them */

#define WARN_DEFAULT	0
#define WARN_ALWAYS	1
#define WARN_NEVER	2

extern int charset_warning_level;
extern int input_warning_level;
extern int style_warning_level;
extern int nwarning;

/* One entry per warning kind; warn_info[] in aambundle.c maps a
 * warn_id_t to the flag that disables it and to its level variable. */
typedef enum {
	WARN_ERROR,
	WARN_CHARSET,
	WARN_INPUT,
	WARN_STYLE,
	WARN_COUNT
} warn_id_t;

/* Print a warning. Unless the warning was forced on with --warn-<kind>,
 * a hint suggesting its disabling flag is printed after the message. */
void warning(warn_id_t id, const char *fmt, ...);
void vwarning(warn_id_t id, const char *fmt, va_list ap);

/* Warn about codepoints the story declares that the target cannot render */
void check_charset(const char* target_name, int aaglyph_flags);

#define AAGLYPH_BITMAP          1       /* has a font bitmap (c64) */
#define AAGLYPH_TRANSLIT        2       /* has a transliteration (apple2) */

/* Warn about dictionary words the player cannot type on the target. */
void check_keyboard(const char *target_name, const char *untypeable);

/* 6502 rewrite chunks helper */

chunk_action_t rewrite_6502(
	const char *id,
	uint8_t *data,
	uint32_t size,
	char *newid,
	uint8_t **newdata,
	uint32_t *newsize);

/* Style precomputation (USTY chunk), implemented in bundle_sty.c. Call
 * bundle_sty_set_target() before rewrite_chunks() on the 8-bit targets;
 * "c64", "apple2" or "aambox". rewrite_6502_sty() is rewrite_6502() plus
 * the USTY chunk inserted right after LOOK. */

void bundle_sty_set_target(const char *target);

/* Call after rewrite_chunks() on a 6502 target: aborts if the interpreter
 * needs a USTY table and none was emitted. See bundle_sty.c.
 */
void bundle_sty_check(void);

/* Writes the rewritten story file, with no packaging around it, for the
 * aambox test platform. See bundle_6502.c.
 */
void bundle_aambox(char *filename);
chunk_action_t rewrite_6502_sty(
	const char *id,
	uint8_t *data,
	uint32_t size,
	char *newid,
	uint8_t **newdata,
	uint32_t *newsize);

/* main bundle routines */

uint8_t *unicode_to_utf8(const uint32_t unichar);
void warn_about_nonascii(uint8_t *dict, uint32_t dictsize, uint8_t *lang, uint32_t langsize);

void bundle_web(char *dirname);
void bundle_c64(char *dirname);
void bundle_apple2(char *dirname);
void bundle_web_story(char *filename);
