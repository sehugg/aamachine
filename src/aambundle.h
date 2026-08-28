#if (defined(_WIN32) || defined(__WIN32__))
#define mkdir(Path, Mode) mkdir(Path)
#endif

typedef void (*chunk_visitor_t)(char *head, char *dirname, uint8_t *chunk, uint32_t size);

extern uint8_t *story;
extern uint32_t storysize;

void visit_chunks(char *storyname, int storynamesize, file_visitor_t file_visitor);

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
	CHUNK_REPLACE
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
extern int keyboard_warning_level;
extern int nwarning;

void warning(const char *fmt, ...);

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

/* main bundle routines */
>>>>>>> 0acaf1e (aambundle: refactor chunk rewriting, added codepoint warnings)

uint8_t *unicode_to_utf8(const uint32_t unichar);
void warn_about_nonascii(uint8_t *dict, uint32_t dictsize, uint8_t *lang, uint32_t langsize);

void bundle_web(char *dirname);
void bundle_c64(char *dirname);
void bundle_apple2(char *dirname);
void bundle_web_story(char *filename);
