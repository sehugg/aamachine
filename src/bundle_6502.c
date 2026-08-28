#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
