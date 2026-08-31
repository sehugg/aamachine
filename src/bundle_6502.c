#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "aambundle.h"

// Chunk policy shared by the two 8-bit targets. Neither can do anything with
// embedded resource files, so those go; everything else is carried over as
// it stands.
//
// This is where a chunk the 6502 engine would rather receive pre-digested
// gets replaced -- rewrite_chunks() handles the resulting size change, and
// re-derives the WRIT page alignment from the new offsets.

chunk_action_t rewrite_6502(
	const char *id,
	uint8_t *data,
	uint32_t size,
	char *newid,
	uint8_t **newdata,
	uint32_t *newsize)
{
	return strcmp(id, "FILE")? CHUNK_KEEP : CHUNK_DROP;
}

// The aambox target: no packaging at all, just the rewritten .aastory.
//
// aambox is the synthetic 6502 platform the test suite runs the engine on
// (aambox6502.c loads a frontend blob and a story file). It is not a
// delivery target, so there is nothing to bundle -- but without it the only
// stories that ever reach the 6502 engine with a USTY chunk are the ones
// packed into a .d64 or a ProDOS image, and the automated tests would
// exercise nothing but the CSS-parsing fallback path. Writing the rewritten
// story out lets test/*/Makefile run the same transcript through both.

void bundle_aambox(char *filename) {
	char storyname[256];
	FILE *outf;

	visit_chunks(storyname, sizeof(storyname), 0);
	bundle_sty_set_target("aambox");
	rewrite_chunks(rewrite_6502_sty, 1);

	outf = fopen(filename, "wb");
	if(!outf) {
		fprintf(stderr, "%s: %s\n", filename, strerror(errno));
		exit(1);
	}
	if(storysize != fwrite(story, 1, storysize, outf)) {
		fprintf(stderr, "%s: write error\n", filename);
		exit(1);
	}
	fclose(outf);
}
