#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "aambundle.h"

// Default rewrite policy for 6502 (called by rewrite_6502_sty)

chunk_action_t rewrite_6502(
	const char *id,
	uint8_t *data,
	uint32_t size,
	char *newid,
	uint8_t **newdata,
	uint32_t *newsize)
{
	// drop FILE chunks
	return strcmp(id, "FILE")? CHUNK_KEEP : CHUNK_DROP;
}

// The aambox target: no packaging at all, just the rewritten .aastory.

void bundle_aambox(char *filename) {
	char storyname[256];
	FILE *outf;

	visit_chunks(storyname, sizeof(storyname), 0);
	gen_usty_set_target("aambox");
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
