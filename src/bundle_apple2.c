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
 * boot, so it should be the only one on the disk.
 *
 * STORY is read through the MLI a page at a time and never loaded whole,
 * so its file type does not matter.
 */

static char storyname[48];

static void writefile(char *dirname, char *name, const uint8_t *data, size_t size) {
	char *filename;
	FILE *f;

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
	fclose(f);

	printf("%-14s %7lu bytes\n", name, (unsigned long) size);

	free(filename);
}

static const char readme[] =
"Aa-machine for the Apple II\n"
"===========================\n"
"\n"
"This directory holds the two files that go on a ProDOS disk.\n"
"\n"
"  AAM.SYSTEM   the interpreter.  ProDOS file type SYS ($ff),\n"
"               auxiliary type (load address) $2000.\n"
"  STORY        the story file.  Any file type will do; BIN ($06)\n"
"               is a reasonable choice.  The name must be STORY,\n"
"               in the volume directory, because that is the\n"
"               pathname the interpreter opens.\n"
"\n"
"Building a bootable disk\n"
"------------------------\n"
"\n"
"Start from a bootable ProDOS volume.  The boot blocks and the PRODOS\n"
"kernel are Apple's, so they are not distributed here.  An 800 kB\n"
"volume is recommended; a 140 kB floppy cannot hold a large story.\n"
"\n"
"AAM.SYSTEM must be the only *.SYSTEM file on the volume, since ProDOS\n"
"runs the first one it finds.\n"
"\n"
"With AppleCommander:\n"
"\n"
"  java -jar AppleCommander.jar -d      disk.po AAM.SYSTEM\n"
"  java -jar AppleCommander.jar -d      disk.po BASIC.SYSTEM\n"
"  java -jar AppleCommander.jar -p      disk.po AAM.SYSTEM SYS 0x2000 <AAM.SYSTEM\n"
"  java -jar AppleCommander.jar -p      disk.po STORY BIN 0x0000 <STORY\n"
"\n"
"With Ciderpress II:\n"
"\n"
"  cp2 rm       disk.po BASIC.SYSTEM\n"
"  cp2 add      disk.po AAM.SYSTEM STORY\n"
"  cp2 set-attr disk.po AAM.SYSTEM type=SYS aux=0x2000\n"
"\n"
"Requirements\n"
"------------\n"
"\n"
"64 kB and ProDOS 8.  The interpreter identifies the machine at boot\n"
"and adapts to it.\n"
"\n"
"  Apple ][ / ][+       40 columns, upper case\n"
"  //e / //c / IIgs     80 columns when an 80-column card is present,\n"
"                       40 columns otherwise\n"
"  128 kB machines      undo, kept in auxiliary memory\n"
"\n"
"On a 128 kB machine the ProDOS RAM disk is disconnected at startup,\n"
"because the undo history lives where /RAM would.\n";

void bundle_apple2(char *dirname) {
	visit_chunks(storyname, sizeof(storyname), 0);
	trim_chunks(1);

	writefile(dirname, "AAM.SYSTEM", table_a2terp, sizeof(table_a2terp));
	writefile(dirname, "STORY", story, storysize);
	writefile(dirname, "readme.txt", (const uint8_t *) readme, strlen(readme));
	writefile(dirname, "interpreter_license.txt",
		table_a2license, sizeof(table_a2license));
}
