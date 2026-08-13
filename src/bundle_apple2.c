#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aambundle.h"

#include "table_a20boot.h"
#include "table_a2license.h"
#include "table_a2terp.h"

/* Apple II bundling.
 *
 * The two pieces that go on the disk are emitted as plain files, for the
 * author to copy onto a ProDOS volume with whatever tool they prefer.
 *
 * 140k disks boot with 0boot, Peter Ferrie's track loader, which reads
 * AAM.SYSTEM straight off the raw tracks and enters it at $2003.  That needs
 * nothing from Apple, so those images are always built.  The volume itself is
 * still an ordinary ProDOS one -- only block 0 differs -- so ProRWTS2 can find
 * STORY by name and the files can be copied off with any ProDOS tool.
 *
 * 0boot only understands 5.25" tracks, so an 800k image still needs the real
 * thing: the ProDOS boot blocks and the PRODOS kernel, which are Apple's and
 * cannot be shipped here.  If a ProDOS release image turns up in one of the
 * usual places (see prodos_dirs below) we lift those two pieces out of it and
 * build the 800k disk as well; if not, we say where to get one and carry on.
 *
 * AAM.SYSTEM is a ProDOS system program, type SYS ($ff), load address
 * $2000.  ProDOS runs the first *.SYSTEM file in the volume directory at
 * boot, so it should be the only one on the disk. It has a stub to relocate
 * it to its final address of 0x800.
 *
 * STORY is read through the MLI a page at a time and never loaded whole,
 * so its file type should be $06 (binary)
 *
 * IMPORTANT:
 * The disk volume containing the story file must be named "AA.STORY"
 * but it need not be the same as the boot/interpreter volume.
 */

static char storyname[48];

/* ProDOS on-disk layout.  Blocks are 512 bytes and .po images store them
 * in plain ascending order, so a block number is just an offset. */
#define BLOCK_SIZE		512
#define ENTRY_LENGTH		0x27
#define ENTRIES_PER_BLOCK	13
#define VOLUME_DIR_BLOCK	2
#define VOLUME_DIR_BLOCKS	4
#define BITMAP_BLOCK		6
#define POINTERS_PER_INDEX	256
#define MAX_FILES		(VOLUME_DIR_BLOCKS * ENTRIES_PER_BLOCK - 1)

/* storage types, the high nibble of the first byte of a directory entry */
#define ST_SEEDLING		1
#define ST_SAPLING		2
#define ST_TREE			3
#define ST_VOLUME_HEADER	0xf

#define ACCESS_UNLOCKED		0xe3
#define ACCESS_VOLUME		0xc3

#define TYPE_BIN		0x06
#define TYPE_SYS		0xff

#define BLOCKS_140K		280
#define BLOCKS_800K		1600
#define BLOCKS_PER_TRACK	8

/* 0boot's two stages go where ProDOS puts its own.  The boot PROM reads
 * physical sector 0 into $800 and hands over with $3d already at 1, so 0boot's
 * inc asks for physical sector 2 -- which a ProDOS-order image holds at
 * logical sector 1, the high half of block 0.  So the blob drops in at offset
 * 0 as it stands, its second stage landing at $100 by itself.
 *
 * 0boot then reads whole tracks from track 1, which means AAM.SYSTEM has to
 * start on that boundary, at block 8.  Block 7 is the cost of the alignment. */
#define BOOT0_FIRST_DATA	BLOCKS_PER_TRACK

#define PRODOS_FILENAME		"ProDOS_2_4_3.po"
#define PRODOS_URL		"https://prodos8.com/"

/* Where to look for a ProDOS release image, and what it might be called.
 * The first entry is filled in with the output directory at run time. */
static char *prodos_dirs[16];
static int nprodos_dirs;

static const char *prodos_names[] = {
	"ProDOS_2_4_3.po",
	"prodos_2_4_3.po",
	"PRODOS_2_4_3.po",
	"PRODOS_2_4_3.PO",
	"ProDOS.po",
	"prodos.po",
	"PRODOS.PO",
	"PRODOS.po",
};

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

/* ---------------------------------------------------------------- reading */

/* Just enough of a ProDOS reader to pull the boot blocks and the kernel out
 * of a release image. */

static const uint8_t *src_block(const uint8_t *img, size_t imgsize, int num) {
	if(num < 0 || (size_t) (num + 1) * BLOCK_SIZE > imgsize) return 0;
	return img + (size_t) num * BLOCK_SIZE;
}

static int name_matches(const uint8_t *entry, const char *want) {
	int len = entry[0] & 0x0f, i;
	char a, b;

	if((int) strlen(want) != len) return 0;
	for(i = 0; i < len; i++) {
		a = (char) entry[1 + i];
		b = want[i];
		if(a >= 'a' && a <= 'z') a ^= 0x20;
		if(b >= 'a' && b <= 'z') b ^= 0x20;
		if(a != b) return 0;
	}
	return 1;
}

static const uint8_t *find_entry(const uint8_t *img, size_t imgsize, const char *want) {
	const uint8_t *block, *entry;
	int num = VOLUME_DIR_BLOCK, i, guard;

	for(guard = 0; num && guard < 64; guard++) {
		block = src_block(img, imgsize, num);
		if(!block) return 0;
		for(i = 0; i < ENTRIES_PER_BLOCK; i++) {
			entry = block + 4 + i * ENTRY_LENGTH;
			switch(entry[0] >> 4) {
			case ST_SEEDLING:
			case ST_SAPLING:
			case ST_TREE:
				if(name_matches(entry, want)) return entry;
			}
		}
		num = block[2] | (block[3] << 8);
	}
	return 0;
}

/* Copies the len bytes addressed by one index block into out. */
static int read_sapling(const uint8_t *img, size_t imgsize, int index_block, uint8_t *out, uint32_t len) {
	const uint8_t *index, *data;
	uint32_t pos = 0, chunk;
	int i, num;

	if(!index_block) {			/* sparse */
		memset(out, 0, len);
		return 0;
	}
	index = src_block(img, imgsize, index_block);
	if(!index) return -1;
	for(i = 0; i < POINTERS_PER_INDEX && pos < len; i++) {
		chunk = (len - pos > BLOCK_SIZE)? BLOCK_SIZE : len - pos;
		num = index[i] | (index[POINTERS_PER_INDEX + i] << 8);
		if(!num) {
			memset(out + pos, 0, chunk);
		} else {
			data = src_block(img, imgsize, num);
			if(!data) return -1;
			memcpy(out + pos, data, chunk);
		}
		pos += chunk;
	}
	return (pos == len)? 0 : -1;
}

/* Returns the file's contents, or 0 if the entry can't be followed. */
static uint8_t *read_entry(const uint8_t *img, size_t imgsize, const uint8_t *entry, uint32_t *sizeout) {
	const uint8_t *master, *data;
	uint8_t *out;
	uint32_t eof, pos = 0, chunk;
	int storage, key, i, num;

	storage = entry[0] >> 4;
	key = entry[0x11] | (entry[0x12] << 8);
	eof = entry[0x15] | (entry[0x16] << 8) | ((uint32_t) entry[0x17] << 16);

	out = calloc(eof? eof : 1, 1);
	if(!out) return 0;

	switch(storage) {
	case ST_SEEDLING:
		data = src_block(img, imgsize, key);
		if(!data) break;
		memcpy(out, data, (eof > BLOCK_SIZE)? BLOCK_SIZE : eof);
		*sizeout = eof;
		return out;
	case ST_SAPLING:
		if(read_sapling(img, imgsize, key, out, eof)) break;
		*sizeout = eof;
		return out;
	case ST_TREE:
		master = src_block(img, imgsize, key);
		if(!master) break;
		for(i = 0; i < POINTERS_PER_INDEX && pos < eof; i++) {
			chunk = POINTERS_PER_INDEX * BLOCK_SIZE;
			if(chunk > eof - pos) chunk = eof - pos;
			num = master[i] | (master[POINTERS_PER_INDEX + i] << 8);
			if(read_sapling(img, imgsize, num, out + pos, chunk)) {
				free(out);
				return 0;
			}
			pos += chunk;
		}
		if(pos != eof) break;
		*sizeout = eof;
		return out;
	}

	free(out);
	return 0;
}

static void add_prodos_dir(const char *dir) {
	if(nprodos_dirs < (int) (sizeof(prodos_dirs) / sizeof(*prodos_dirs))) {
		prodos_dirs[nprodos_dirs++] = strdup(dir);
	}
}

static void add_home_dir(const char *home, const char *rest) {
	char *path;

	if(!home || !*home) return;
	path = malloc(strlen(home) + strlen(rest) + 2);
	sprintf(path, "%s/%s", home, rest);
	add_prodos_dir(path);
	free(path);
}

static void build_prodos_dirs(char *dirname) {
	const char *home = getenv("HOME");

	add_prodos_dir(".");
	add_prodos_dir(dirname);
	add_home_dir(home, ".cache/aambundle");
	add_home_dir(home, ".cache");
	add_prodos_dir("/usr/local/share/aambundle");
	add_prodos_dir("/usr/share/aambundle");
}

static uint8_t *slurp(const char *filename, size_t *sizeout) {
	FILE *f;
	uint8_t *data;
	long size;

	f = fopen(filename, "rb");
	if(!f) return 0;
	if(fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
		fclose(f);
		return 0;
	}
	/* A ProDOS image is a whole number of blocks, and must at least hold
	 * the boot blocks, the volume directory and the bitmap. */
	if(size % BLOCK_SIZE || size < (BITMAP_BLOCK + 1) * BLOCK_SIZE) {
		fclose(f);
		return 0;
	}
	data = malloc(size);
	if(!data || 1 != fread(data, size, 1, f)) {
		free(data);
		fclose(f);
		return 0;
	}
	fclose(f);
	*sizeout = size;
	return data;
}

/* Looks for a ProDOS release image and returns it, or 0.  On success
 * *found is the path we used. */
static uint8_t *find_prodos_image(size_t *sizeout, char **found) {
	const char *env;
	char *path;
	uint8_t *img;
	size_t namelen;
	int i, j;

	env = getenv("AAM_PRODOS_IMAGE");
	if(env && *env) {
		img = slurp(env, sizeout);
		if(img) {
			*found = strdup(env);
			return img;
		}
		fprintf(stderr,
			"AAM_PRODOS_IMAGE is set to '%s', but that is not a "
			"readable ProDOS image.\n", env);
		return 0;
	}

	for(i = 0; i < nprodos_dirs; i++) {
		for(j = 0; j < (int) (sizeof(prodos_names) / sizeof(*prodos_names)); j++) {
			namelen = strlen(prodos_dirs[i]) + strlen(prodos_names[j]) + 2;
			path = malloc(namelen);
			snprintf(path, namelen, "%s/%s", prodos_dirs[i], prodos_names[j]);
			img = slurp(path, sizeout);
			if(img) {
				*found = path;
				return img;
			}
			free(path);
		}
	}
	return 0;
}

static void explain_missing_prodos(void) {
	int i;

	printf("\nNo 800k disk image was built: no ProDOS image found.\n");
	printf("(The 140k disks boot with 0boot and do not need one.)\n");
	printf("Download " PRODOS_FILENAME " from " PRODOS_URL " and put it in\n");
	printf("one of these directories, then run aambundle again:\n");
	for(i = 0; i < nprodos_dirs; i++) {
		printf("    %s\n", prodos_dirs[i]);
	}
	printf("Any of the names");
	for(i = 0; i < (int) (sizeof(prodos_names) / sizeof(*prodos_names)); i++) {
		printf("%s %s", i? "," : "", prodos_names[i]);
	}
	printf(" will do,\n");
	printf("or set AAM_PRODOS_IMAGE to the full path of an image.\n");
}

/* ---------------------------------------------------------------- writing */

/* A freshly formatted ProDOS volume, built up block by block.  Blocks 0-1
 * hold the boot code, 2-5 the volume directory and 6 the bitmap; ProDOS
 * reserves all of those whatever they contain, so the boot blocks are free
 * to carry, and a data disk gets them too in order to fail politely when
 * someone boots it by mistake. */
struct volume {
	uint8_t		*image;
	uint8_t		*isfree;
	int		total_blocks;
	int		bitmap_blocks;
	int		first_data;
	int		file_count;
	char		name[16];
};

struct diskfile {
	const char	*name;
	const uint8_t	*data;
	uint32_t	size;
	uint8_t		type;
	uint16_t	aux;
};

static uint32_t prodos_now(void) {
	time_t now = time(0);
	struct tm *tm = localtime(&now);
	uint16_t date, clock;

	date = ((tm->tm_year % 100) << 9) | ((tm->tm_mon + 1) << 5) | tm->tm_mday;
	clock = (tm->tm_hour << 8) | tm->tm_min;
	return ((uint32_t) clock << 16) | date;
}

static void put_datetime(uint8_t *dest, uint32_t stamp) {
	dest[0] = stamp & 0xff;
	dest[1] = (stamp >> 8) & 0xff;
	dest[2] = (stamp >> 16) & 0xff;
	dest[3] = (stamp >> 24) & 0xff;
}

static void put_word(uint8_t *dest, uint16_t value) {
	dest[0] = value & 0xff;
	dest[1] = value >> 8;
}

/* Returns the address of directory slot n; slot 0 is the volume header. */
static uint8_t *dir_slot(struct volume *v, int slot) {
	int block = VOLUME_DIR_BLOCK + slot / ENTRIES_PER_BLOCK;

	return v->image
		+ (size_t) block * BLOCK_SIZE
		+ 4 + (slot % ENTRIES_PER_BLOCK) * ENTRY_LENGTH;
}

/* first_data is the first block files may use; 0 means the usual one, just
 * past the bitmap.  0boot volumes pass 8 to keep the interpreter on a track
 * boundary. */
static void vol_init(struct volume *v, const char *name, int total_blocks, const uint8_t *boot, size_t bootsize, int first_data) {
	uint8_t *entry;
	int i, num, prev, next;

	memset(v, 0, sizeof(*v));
	snprintf(v->name, sizeof(v->name), "%s", name);
	v->total_blocks = total_blocks;
	v->image = calloc(total_blocks, BLOCK_SIZE);
	v->isfree = malloc(total_blocks);
	if(!v->image || !v->isfree) {
		fprintf(stderr, "Out of memory\n");
		exit(1);
	}

	if(boot) memcpy(v->image, boot, bootsize);

	/* one bit per block, most significant bit first; one bitmap block
	 * covers 8 * 512 = 4096 blocks */
	v->bitmap_blocks = (total_blocks + 4095) / 4096;
	v->first_data = BITMAP_BLOCK + v->bitmap_blocks;
	if(first_data > v->first_data) v->first_data = first_data;
	memset(v->isfree, 1, total_blocks);
	for(i = 0; i < v->first_data; i++) v->isfree[i] = 0;

	entry = dir_slot(v, 0);
	entry[0] = (ST_VOLUME_HEADER << 4) | strlen(v->name);
	memcpy(entry + 1, v->name, strlen(v->name));
	put_datetime(entry + 0x18, prodos_now());
	entry[0x1e] = ACCESS_VOLUME;
	entry[0x1f] = ENTRY_LENGTH;
	entry[0x20] = ENTRIES_PER_BLOCK;
	put_word(entry + 0x23, BITMAP_BLOCK);
	put_word(entry + 0x25, total_blocks);

	/* link the directory blocks into a doubly linked chain */
	for(i = 0; i < VOLUME_DIR_BLOCKS; i++) {
		num = VOLUME_DIR_BLOCK + i;
		prev = i? num - 1 : 0;
		next = (i < VOLUME_DIR_BLOCKS - 1)? num + 1 : 0;
		put_word(v->image + (size_t) num * BLOCK_SIZE + 0, prev);
		put_word(v->image + (size_t) num * BLOCK_SIZE + 2, next);
	}
}

static void vol_free(struct volume *v) {
	free(v->image);
	free(v->isfree);
	v->image = 0;
	v->isfree = 0;
}

static int vol_blocks_free(struct volume *v) {
	int i, n = 0;

	for(i = 0; i < v->total_blocks; i++) if(v->isfree[i]) n++;
	return n;
}

/* Returns a fresh block number, or -1 when the volume is full. */
static int vol_alloc(struct volume *v) {
	int i;

	for(i = v->first_data; i < v->total_blocks; i++) {
		if(v->isfree[i]) {
			v->isfree[i] = 0;
			return i;
		}
	}
	return -1;
}

static void vol_put(struct volume *v, int num, const uint8_t *data, uint32_t size) {
	memcpy(v->image + (size_t) num * BLOCK_SIZE, data, size);
}

/* Stores up to 256 blocks under one index block.  Returns -1 if the volume
 * fills up, otherwise the index block number, with the number of blocks
 * consumed (index block included) in *used.
 *
 * The data goes down first and the index block after it, so that a file's
 * payload starts at the first free block on the volume.  ProDOS does not care
 * where the index block sits, and 0boot needs AAM.SYSTEM to begin exactly on
 * the track boundary. */
static int vol_write_sapling(struct volume *v, const uint8_t *data, uint32_t size, int *used) {
	uint32_t pos = 0, chunk;
	int index, num, i = 0;
	uint8_t pointers[BLOCK_SIZE];

	memset(pointers, 0, sizeof(pointers));
	*used = 1;
	while(pos < size) {
		chunk = (size - pos > BLOCK_SIZE)? BLOCK_SIZE : size - pos;
		num = vol_alloc(v);
		if(num < 0) return -1;
		vol_put(v, num, data + pos, chunk);
		pointers[i] = num & 0xff;
		pointers[POINTERS_PER_INDEX + i] = num >> 8;
		(*used)++;
		pos += chunk;
		i++;
	}

	index = vol_alloc(v);
	if(index < 0) return -1;
	vol_put(v, index, pointers, sizeof(pointers));
	return index;
}

/* Stores the payload as a seedling, sapling or tree, whichever fits. */
static int vol_write_data(struct volume *v, const uint8_t *data, uint32_t size, uint8_t *storage, int *key, int *used) {
	uint32_t nblocks, pos = 0, chunk;
	uint8_t pointers[BLOCK_SIZE];
	int master, index, sub, i = 0;

	nblocks = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;
	if(!nblocks) nblocks = 1;

	if(nblocks == 1) {
		*key = vol_alloc(v);
		if(*key < 0) return -1;
		vol_put(v, *key, data, size);
		*storage = ST_SEEDLING;
		*used = 1;
		return 0;
	}

	if(nblocks <= POINTERS_PER_INDEX) {
		index = vol_write_sapling(v, data, size, used);
		if(index < 0) return -1;
		*storage = ST_SAPLING;
		*key = index;
		return 0;
	}

	/* Same again for the master index block -- data first, pointers after. */
	memset(pointers, 0, sizeof(pointers));
	*used = 1;
	while(pos < size) {
		chunk = POINTERS_PER_INDEX * BLOCK_SIZE;
		if(chunk > size - pos) chunk = size - pos;
		index = vol_write_sapling(v, data + pos, chunk, &sub);
		if(index < 0) return -1;
		pointers[i] = index & 0xff;
		pointers[POINTERS_PER_INDEX + i] = index >> 8;
		*used += sub;
		pos += chunk;
		i++;
	}

	master = vol_alloc(v);
	if(master < 0) return -1;
	vol_put(v, master, pointers, sizeof(pointers));
	*storage = ST_TREE;
	*key = master;
	return 0;
}

/* Returns 0, or -1 if the file does not fit. */
static int vol_add(struct volume *v, const struct diskfile *f) {
	uint8_t *entry, storage = 0;
	uint32_t stamp;
	int key = 0, used = 0, slot, block;

	if(v->file_count >= MAX_FILES) return -1;
	if(vol_write_data(v, f->data, f->size, &storage, &key, &used)) return -1;

	slot = ++v->file_count;
	block = VOLUME_DIR_BLOCK + slot / ENTRIES_PER_BLOCK;
	stamp = prodos_now();

	entry = dir_slot(v, slot);
	entry[0] = (storage << 4) | strlen(f->name);
	memcpy(entry + 1, f->name, strlen(f->name));
	entry[0x10] = f->type;
	put_word(entry + 0x11, key);
	put_word(entry + 0x13, used);
	entry[0x15] = f->size & 0xff;
	entry[0x16] = (f->size >> 8) & 0xff;
	entry[0x17] = (f->size >> 16) & 0xff;
	put_datetime(entry + 0x18, stamp);
	entry[0x1e] = ACCESS_UNLOCKED;
	put_word(entry + 0x1f, f->aux);
	put_datetime(entry + 0x21, stamp);
	put_word(entry + 0x25, block);

	put_word(dir_slot(v, 0) + 0x21, v->file_count);

	return 0;
}

static void vol_write_bitmap(struct volume *v) {
	uint8_t *bits = v->image + (size_t) BITMAP_BLOCK * BLOCK_SIZE;
	int i;

	memset(bits, 0, (size_t) v->bitmap_blocks * BLOCK_SIZE);
	for(i = 0; i < v->total_blocks; i++) {
		if(v->isfree[i]) bits[i >> 3] |= 0x80 >> (i & 7);
	}
}

/* Builds a volume holding the given files and writes it out.  Returns 0, or
 * -1 if they do not all fit, in which case nothing is written. */
static int build_disk(char *dirname, const char *leaf, const char *volname, int total_blocks, const uint8_t *boot, size_t bootsize, int first_data, const struct diskfile *files, int nfiles) {
	struct volume vol;
	char *filename;
	size_t namelen;
	FILE *f;
	int i;

	vol_init(&vol, volname, total_blocks, boot, bootsize, first_data);
	for(i = 0; i < nfiles; i++) {
		if(vol_add(&vol, &files[i])) {
			vol_free(&vol);
			return -1;
		}
	}
	vol_write_bitmap(&vol);

	/* 0boot reads whole tracks, so the file it loads has to be one
	 * unbroken run from where it starts reading.  It always is, but the
	 * disk is unbootable if it ever is not, so check rather than trust. */
	if(first_data && memcmp(vol.image + (size_t) first_data * BLOCK_SIZE,
		files[0].data, files[0].size)) {
		fprintf(stderr, "%s: %s is not contiguous from block %d\n",
			leaf, files[0].name, first_data);
		exit(1);
	}

	namelen = strlen(dirname) + strlen(leaf) + 2;
	filename = malloc(namelen);
	snprintf(filename, namelen, "%s/%s", dirname, leaf);

	f = fopen(filename, "wb");
	if(!f) {
		fprintf(stderr, "%s: %s\n", filename, strerror(errno));
		exit(1);
	}
	if(1 != fwrite(vol.image, (size_t) total_blocks * BLOCK_SIZE, 1, f)) {
		fprintf(stderr, "%s: %s\n", filename, strerror(errno));
		exit(1);
	}
	fclose(f);

	printf("%-14s %4dk  /%-8s %d files, %d blocks free\n",
		leaf,
		total_blocks * BLOCK_SIZE / 1024,
		vol.name,
		vol.file_count,
		vol_blocks_free(&vol));

	free(filename);
	vol_free(&vol);
	return 0;
}

/* ---------------------------------------------------------------- readme */

enum {
	DISKS_SINGLE,
	DISKS_SPLIT,
	DISKS_BIGONLY
};

static const char readme_head[] =
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
"IMPORTANT:\n"
"The disk volume containg the story file must be named AA.STORY\n"
"and may be a different volume than the boot disk.\n"
"\n"
"System Requirements\n"
"-------------------\n"
"\n"
"Requires 64 kB and ProDOS 8.\n"
"The interpreter identifies the machine at boot and adapts to it:\n"
"\n"
"  Apple ][+            40 columns, upper case only\n"
"  //e / //c / IIgs     80 columns when an 80-column card is present\n"
"  128 kB machines      additional page cache, undo support\n"
"\n"
"On a 128 kB machine the ProDOS /RAM disk is disconnected at startup,\n"
"because the interpreter uses that auxilary memory.\n"
"\n"
;

/* One %s, the disk image name. */
static const char readme_single[] =
"Bootable disk\n"
"-------------\n"
"\n"
"  %s\n"
"       A 140 kB disk, volume /AA.STORY, holding AAM.SYSTEM and\n"
"       STORY.  Boot it and the story starts.\n"
"\n"
;

/* Two %s, the boot and story disk image names. */
static const char readme_split[] =
"Bootable disks\n"
"--------------\n"
"\n"
"This story does not fit on one 140 kB disk alongside the interpreter,\n"
"so it comes as a two-disk set:\n"
"\n"
"  %s\n"
"  %s\n"
"       Boot the first (volume /AA.BOOT, holding AAM.SYSTEM) and keep\n"
"       the second (volume /AA.STORY, holding STORY) in the other\n"
"       drive.\n"
"\n"
;

static const char readme_toobig[] =
"Bootable disk\n"
"-------------\n"
"\n"
"This story is too large for a 140 kB disk even with nothing else on\n"
"it, so it needs an 800 kB drive or a hard disk.\n"
"\n"
;

/* One %s, the 800k disk image name. */
static const char readme_800k[] =
"  %s\n"
"       The same thing on one 800 kB disk, volume /AA.STORY, for a\n"
"       3.5\" drive or a hard disk.  This one boots ProDOS in the\n"
"       ordinary way, so it also holds PRODOS.  The kernel and the\n"
"       boot blocks were copied from the ProDOS release image found\n"
"       at bundling time; ProDOS itself is redistributed under the\n"
"       Apple Software License Agreement.\n"
"\n"
;

static const char readme_no800k[] =
"No 800 kB disk was built, because that needs a real ProDOS to boot\n"
"from and no ProDOS release image was found.  To get one:\n"
"\n"
"* Get the disk image '" PRODOS_FILENAME "' from " PRODOS_URL "\n"
"* Put it next to the story file and run aambundle again.\n"
"\n"
"Or build the disk by hand, with AppleCommander from\n"
"https://applecommander.github.io/ .  AAM.SYSTEM must be the only\n"
"*.SYSTEM file on the volume, since ProDOS runs the first one it finds:\n"
"\n"
"  acx create -d disk.po -n AA.STORY -f " PRODOS_FILENAME " -s 800k --prodos\n"
"  ac -p disk.po AAM.SYSTEM SYS 0x2000 <AAM.SYSTEM\n"
"  ac -p disk.po STORY BIN 0x0000 <STORY\n"
"\n"
;

static const char readme_boot0[] =
"The 140 kB disks boot with 0boot, Peter Ferrie's track loader\n"
"(https://github.com/peterferrie/0boot), which reads the interpreter\n"
"off the raw tracks rather than going through ProDOS.  That saves the\n"
"17 kB the ProDOS kernel would take up, and boots faster.  The volume\n"
"is an ordinary ProDOS one otherwise, so the files can be copied off\n"
"with any ProDOS tool.\n"
"\n"
;

static void write_readme(char *dirname, int mode, const char *single, const char *bootdisk, const char *storydisk, const char *bigdisk) {
	char *text;
	size_t size;

	size = sizeof(readme_head) + sizeof(readme_single) + sizeof(readme_split)
		+ sizeof(readme_toobig) + sizeof(readme_800k)
		+ sizeof(readme_no800k) + sizeof(readme_boot0);
	if(single) size += strlen(single);
	if(bootdisk) size += strlen(bootdisk) + strlen(storydisk);
	if(bigdisk) size += strlen(bigdisk);
	text = malloc(size);
	if(!text) {
		fprintf(stderr, "Out of memory\n");
		exit(1);
	}

	strcpy(text, readme_head);
	switch(mode) {
	case DISKS_SPLIT:
		sprintf(text + strlen(text), readme_split, bootdisk, storydisk);
		break;
	case DISKS_BIGONLY:
		strcat(text, readme_toobig);
		break;
	default:
		sprintf(text + strlen(text), readme_single, single);
		break;
	}
	if(bigdisk) {
		sprintf(text + strlen(text), readme_800k, bigdisk);
	}
	if(mode != DISKS_BIGONLY) strcat(text, readme_boot0);
	if(!bigdisk) strcat(text, readme_no800k);

	writefile(dirname, "readme.txt", (const uint8_t *) text, strlen(text));
	free(text);
}

/* ---------------------------------------------------------------- bundle */

static char *disk_name(const char *suffix) {
	char *name;
	size_t size;

	size = strlen(storyname) + strlen(suffix) + 8;
	name = malloc(size);
	snprintf(name, size, "%s%s.po", storyname, suffix);
	return name;
}

/* Builds the 800k image, which is the one that still boots ProDOS.  Sets
 * *bigdisk and returns 0 on success; returns -1, having said why, when there
 * is no ProDOS to build it from. */
static int build_800k(char *dirname, const struct diskfile *aam, const struct diskfile *storyfile, char **bigdisk) {
	struct diskfile files[3];
	uint8_t *img, *kernel;
	char *path = 0;
	uint32_t kernelsize;
	size_t imgsize;
	const uint8_t *entry;

	img = find_prodos_image(&imgsize, &path);
	if(!img) {
		explain_missing_prodos();
		return -1;
	}

	entry = find_entry(img, imgsize, "PRODOS");
	if(!entry) {
		printf("\n%s holds no PRODOS file; no 800k disk built.\n", path);
		free(img);
		free(path);
		return -1;
	}
	kernel = read_entry(img, imgsize, entry, &kernelsize);
	if(!kernel) {
		printf("\nCould not read PRODOS from %s; no 800k disk built.\n", path);
		free(img);
		free(path);
		return -1;
	}
	printf("\nProDOS taken from %s\n", path);

	/* PRODOS goes in first so the boot block finds it, and AAM.SYSTEM
	 * before STORY so it is the first *.SYSTEM in the directory. */
	files[0].name = "PRODOS";
	files[0].data = kernel;
	files[0].size = kernelsize;
	files[0].type = entry[0x10];
	files[0].aux = entry[0x1f] | (entry[0x20] << 8);
	files[1] = *aam;
	files[2] = *storyfile;

	*bigdisk = disk_name("-800k");
	if(build_disk(dirname, *bigdisk, "AA.STORY", BLOCKS_800K,
		img, 2 * BLOCK_SIZE, 0, files, 3)) {
		fprintf(stderr, "%s: story too large for an 800k disk\n", *bigdisk);
		exit(1);
	}

	free(kernel);
	free(img);
	free(path);
	return 0;
}

/* Builds the bootable images.  Returns which set of 140k disks we made. */
static int build_disks(char *dirname, uint8_t *padded, uint32_t paddedsize, char **single, char **bootdisk, char **storydisk, char **bigdisk) {
	struct diskfile files[2];

	files[0].name = "AAM.SYSTEM";
	files[0].data = table_a2terp;
	files[0].size = sizeof(table_a2terp);
	files[0].type = TYPE_SYS;
	files[0].aux = 0x2000;

	files[1].name = "STORY";
	files[1].data = padded;
	files[1].size = paddedsize;
	files[1].type = TYPE_BIN;
	files[1].aux = 0;

	*single = disk_name("");
	if(!build_disk(dirname, *single, "AA.STORY", BLOCKS_140K,
		table_a20boot, sizeof(table_a20boot), BOOT0_FIRST_DATA,
		files, 2)) {
		return DISKS_SINGLE;
	}

	/* Too big for one floppy: a two-disk set, plus an 800k disk for
	 * anyone with a 3.5" drive, if we can build one.
	 *
	 * The story disk goes first because it is the one that can fail, and
	 * failing before anything is written keeps us from leaving half a set
	 * behind.  It carries no boot code -- under ProDOS it would get the
	 * boot blocks so that booting it by mistake gives ProDOS's own "unable
	 * to load" message, but 0boot's would try to load an interpreter that
	 * is not there, so it gets none, and the block back. */
	free(*single);
	*single = 0;
	*storydisk = disk_name("-story");

	if(build_disk(dirname, *storydisk, "AA.STORY", BLOCKS_140K,
		0, 0, 0, files + 1, 1)) {
		/* Too big even for a floppy of its own, so 5.25" is out
		 * altogether and the 800k disk is the only way to ship it. */
		free(*storydisk);
		*storydisk = 0;
		build_800k(dirname, &files[0], &files[1], bigdisk);
		return DISKS_BIGONLY;
	}

	*bootdisk = disk_name("-boot");
	if(build_disk(dirname, *bootdisk, "AA.BOOT", BLOCKS_140K,
		table_a20boot, sizeof(table_a20boot), BOOT0_FIRST_DATA,
		files, 1)) {
		fprintf(stderr, "The interpreter does not fit on a 140k disk!\n");
		exit(1);
	}

	build_800k(dirname, &files[0], &files[1], bigdisk);
	return DISKS_SPLIT;
}

void bundle_apple2(char *dirname) {
	char *single = 0, *bootdisk = 0, *storydisk = 0, *bigdisk = 0;
	uint8_t *padded;
	uint32_t paddedsize;
	int mode;

	visit_chunks(storyname, sizeof(storyname), 0);
	trim_chunks(1);

	build_prodos_dirs(dirname);

	writefile(dirname, "AAM.SYSTEM", table_a2terp, sizeof(table_a2terp));
	/* The interpreter reads STORY a page at a time, so round the file up
	 * to a whole number of 256-byte pages. */
	writefile_padded(dirname, "STORY", story, storysize, 256);

	paddedsize = (storysize + 0xff) & ~0xffu;
	padded = calloc(paddedsize? paddedsize : 1, 1);
	if(!padded) {
		fprintf(stderr, "Out of memory\n");
		exit(1);
	}
	memcpy(padded, story, storysize);

	mode = build_disks(dirname, padded, paddedsize, &single, &bootdisk, &storydisk, &bigdisk);

	write_readme(dirname, mode, single, bootdisk, storydisk, bigdisk);
	writefile(dirname, "interpreter_license.txt",
		table_a2license, sizeof(table_a2license));

	free(padded);
	free(single);
	free(bootdisk);
	free(storydisk);
	free(bigdisk);
}
