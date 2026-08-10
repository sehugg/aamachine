#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aambundle.h"

#include "table_a2license.h"
#include "table_a2terp.h"

/* Apple II bundling.
 *
 * The two pieces that go on the disk are emitted as plain files, for the
 * author to copy onto a ProDOS volume with whatever tool they prefer.  A
 * bootable volume also needs the ProDOS boot blocks and the PRODOS kernel,
 * which are Apple's and cannot be shipped here, so building the volume is
 * left to the author.
 *
 * AAM.SYSTEM is a ProDOS system program, type SYS ($ff), load address
 * $2000.  ProDOS runs the first *.SYSTEM file in the volume directory at
 * boot, so it should be the only one on the disk. It has a stub to relocate
 * it to its final address of 0x800.
 *
 * STORY is read through the MLI a page at a time and never loaded whole,
 * so its file type should be $06 (binary)
 */

static char storyname[48];

/* Writes data, then pads the file with zeros up to a multiple of padto
 * bytes (padto = 1 means no padding). */
static void writefile_padded(char *dirname, char *name, const uint8_t *data, size_t size, size_t padto) {
	char *filename;
	FILE *f;
	size_t npad;

	filename = malloc(strlen(dirname) + strlen(name) + 2);
	sprintf(filename, "%s/%s", dirname, name);

	f = fopen(filename, "wb");
	if(!f) {
		fprintf(stderr, "%s: %s\n", filename, strerror(errno));
		exit(1);
	}
	if(size != fwrite(data, 1, size, f)) {
		fprintf(stderr, "%s: %s\n", filename, strerror(errno));
		exit(1);
	}
	npad = (padto - size % padto) % padto;
	while(npad--) {
		if(EOF == fputc(0, f)) {
			fprintf(stderr, "%s: %s\n", filename, strerror(errno));
			exit(1);
		}
		size++;
	}
	fclose(f);

	printf("%-14s %7lu bytes\n", name, (unsigned long) size);

	free(filename);
}

static void writefile(char *dirname, char *name, const uint8_t *data, size_t size) {
	writefile_padded(dirname, name, data, size, 1);
}

static const char readme[] =
"Aa-machine for the Apple II family\n"
"==================================\n"
"\n"
"This directory holds the two files that go on a ProDOS disk:\n"
"\n"
"  AAM.SYSTEM   the interpreter.  ProDOS file type SYS ($ff),\n"
"               auxiliary type (load address) $2000.\n"
"\n"
"  STORY        the story file.  ProDOS file type BIN ($06)\n"
"               is a reasonable choice.  The name must be STORY,\n"
"               in the volume directory, because that is the\n"
"               pathname the interpreter opens.\n"
"\n"
"System Requirements\n"
"-------------------\n"
"\n"
"64 kB and ProDOS 8.  The interpreter identifies the machine at boot\n"
"and adapts to it:\n"
"\n"
"  Apple ][+            40 columns, upper case only\n"
"  //e / //c / IIgs     80 columns when an 80-column card is present\n"
"  128 kB machines      undo support\n"
"\n"
"On a 128 kB machine the ProDOS RAM disk is disconnected at startup,\n"
"because the undo history lives where /RAM would.\n"
"\n"
"Building a bootable disk\n"
"------------------------\n"
"\n"
"We need to create a bootable ProDOS volume, and for that we first need\n"
"a bootable ProDOS disk image to copy from.\n"
"\n"
"* Install AppleCommander from https://applecommander.github.io/\n"
"* Get the disk image 'ProDOS_2_4_3.po' from https://prodos8.com/\n"
"\n"
"AAM.SYSTEM must be the only *.SYSTEM file on the volume, since ProDOS\n"
"runs the first one it finds.\n"
"\n"
"With AppleCommander:\n"
"\n"
"  acx create -d disk.po -f ProDOS_2_4_3.po -s 800k --prodos\n"
"  ac -p disk.po AAM.SYSTEM SYS 0x2000 <AAM.SYSTEM\n"
"  ac -p disk.po STORY BIN 0x0000 <STORY\n"
"\n"
;

void bundle_apple2(char *dirname) {
	visit_chunks(storyname, sizeof(storyname), 0);
	trim_chunks(1);

	writefile(dirname, "AAM.SYSTEM", table_a2terp, sizeof(table_a2terp));
	/* The interpreter reads STORY a page at a time, so round the file up
	 * to a whole number of 256-byte pages. */
	writefile_padded(dirname, "STORY", story, storysize, 256);
	writefile(dirname, "readme.txt", (const uint8_t *) readme, strlen(readme));
	writefile(dirname, "interpreter_license.txt",
		table_a2license, sizeof(table_a2license));
}
