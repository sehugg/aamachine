#!/usr/bin/env python3
"""Build a bootable ProDOS disk image (.po) holding PRODOS plus the given files.

The boot blocks and the PRODOS kernel are lifted from an official ProDOS
release image, which is downloaded once and cached.  Everything else -- the
volume directory, the volume bitmap, and the seedling/sapling/tree files -- is
written from scratch, so the output contains only what was asked for.

Typical use (see makedsk.sh for the conventions this follows):

    ./mkprodos.py -o story.po example/dungeon/AAM.SYSTEM example/dungeon/STORY

A file's ProDOS type and aux type are inferred from its name: anything ending
in .SYSTEM becomes SYS with aux $2000, everything else BIN with aux $0000.
Override per file with the comma-suffix syntax, e.g.

    example/dungeon/STORY,name=STORY,type=BIN,aux=0

With the default size of "auto" the files are packed onto one 140k disk if
they fit.  If they do not, the build splits into two 140k disks: a bootable
AA.BOOT holding PRODOS and the .SYSTEM files, and an AA.STORY holding the
data files.  Use -s 800k to force a single 800k volume instead.
"""

import argparse
import datetime
import os
import struct
import sys
import urllib.request

PRODOS_URL = "http://releases.prodos8.com/ProDOS_2_4_3.po"

BLOCK_SIZE = 512
ENTRY_LENGTH = 0x27
ENTRIES_PER_BLOCK = 13
VOLUME_DIR_BLOCK = 2
VOLUME_DIR_BLOCKS = 4
BITMAP_BLOCK = 6
# blocks 0-1 boot, 2-5 volume directory, 6 volume bitmap
FIRST_DATA_BLOCK = 7

BLOCKS_140K = 280
BLOCKS_800K = 1600

BLOCKS_PER_TRACK = 8

# 0boot's two stages go where ProDOS puts its own.  The boot PROM reads
# physical sector 0 into $800 and hands over with $3d already at 1, so 0boot's
# inc asks for physical sector 2 -- which a ProDOS-order image holds at
# logical sector 1, the high half of block 0.
BOOT0_STAGE2_OFFSET = 0x0100

# 0boot reads whole tracks, so the interpreter has to start on a track
# boundary.  That is block 8, leaving block 7 unused.
BOOT0_FIRST_DATA = BLOCKS_PER_TRACK

# storage types (high nibble of the first entry byte)
ST_SEEDLING = 1
ST_SAPLING = 2
ST_TREE = 3
ST_VOLUME_HEADER = 0xF

ACCESS_UNLOCKED = 0xE3

FILE_TYPES = {
    "TXT": 0x04, "BIN": 0x06, "DIR": 0x0F, "AWP": 0x1A,
    "BAS": 0xFC, "VAR": 0xFD, "REL": 0xFE, "SYS": 0xFF,
}
TYPE_NAMES = {v: k for k, v in FILE_TYPES.items()}

POINTERS_PER_INDEX = 256


class DiskFull(Exception):
    pass


# ---------------------------------------------------------------- name/type

def prodos_name(name):
    """Normalise a host filename into a legal 15-character ProDOS name."""
    name = os.path.basename(name).upper()
    out = []
    for ch in name:
        if ch.isalnum() or ch == ".":
            out.append(ch)
        elif ch in "_- ":
            out.append(".")
    name = "".join(out).lstrip(".")
    if not name or not name[0].isalpha():
        name = "A" + name
    return name[:15]


def parse_type(text):
    text = text.strip().upper()
    if text in FILE_TYPES:
        return FILE_TYPES[text]
    return parse_int(text)


def parse_int(text):
    text = text.strip()
    if text.startswith("$"):
        return int(text[1:], 16)
    return int(text, 0)


def infer_type(name):
    """Default (file_type, aux_type) from the ProDOS name, per makedsk.sh."""
    if name.endswith(".SYSTEM"):
        return 0xFF, 0x2000
    return 0x06, 0x0000


def prodos_datetime(when):
    date = ((when.year % 100) << 9) | (when.month << 5) | when.day
    time = (when.hour << 8) | when.minute
    return struct.pack("<HH", date, time)


# ---------------------------------------------------------------- file spec

class Entry(object):
    """One file destined for a disk image."""

    def __init__(self, name, data, file_type, aux_type, disk=None):
        self.name = name
        self.data = data
        self.file_type = file_type
        self.aux_type = aux_type
        self.disk = disk        # "boot", "story", or None to decide by type

    @property
    def blocks(self):
        return blocks_needed(len(self.data))

    def wants_boot_disk(self):
        if self.disk is not None:
            return self.disk == "boot"
        return self.file_type == 0xFF


def parse_spec(spec):
    """Parse PATH[,name=NAME][,type=SYS][,aux=$2000][,disk=boot|story]."""
    parts = spec.split(",")
    path = parts[0]
    with open(path, "rb") as f:
        data = f.read()

    name = prodos_name(path)
    file_type = aux_type = disk = None
    for opt in parts[1:]:
        if "=" not in opt:
            raise ValueError("bad option %r in %r (expected key=value)"
                             % (opt, spec))
        key, value = opt.split("=", 1)
        key = key.strip().lower()
        if key == "name":
            name = prodos_name(value)
        elif key == "type":
            file_type = parse_type(value)
        elif key == "aux":
            aux_type = parse_int(value)
        elif key == "disk":
            disk = value.strip().lower()
            if disk not in ("boot", "story"):
                raise ValueError("disk= must be boot or story, not %r" % disk)
        else:
            raise ValueError("unknown option %r in %r" % (key, spec))

    default_type, default_aux = infer_type(name)
    if file_type is None:
        file_type = default_type
    if aux_type is None:
        aux_type = default_aux
    return Entry(name, data, file_type, aux_type, disk)


def blocks_needed(size):
    """Total blocks a file of this size occupies, index blocks included."""
    data_blocks = max(1, (size + BLOCK_SIZE - 1) // BLOCK_SIZE)
    if data_blocks == 1:
        return 1                                        # seedling
    if data_blocks <= POINTERS_PER_INDEX:
        return data_blocks + 1                          # sapling
    index_blocks = ((data_blocks + POINTERS_PER_INDEX - 1)
                    // POINTERS_PER_INDEX)
    return data_blocks + index_blocks + 1               # tree


# ------------------------------------------------------------ source volume

class ProdosReader(object):
    """Just enough of a ProDOS reader to pull files off a release image."""

    def __init__(self, image):
        self.image = image

    def block(self, num):
        off = num * BLOCK_SIZE
        if off + BLOCK_SIZE > len(self.image):
            raise ValueError("block %d is past the end of the image" % num)
        return self.image[off:off + BLOCK_SIZE]

    def volume_entries(self):
        """Yield every active entry in the volume directory."""
        num = VOLUME_DIR_BLOCK
        while num:
            block = self.block(num)
            for i in range(ENTRIES_PER_BLOCK):
                off = 4 + i * ENTRY_LENGTH
                entry = block[off:off + ENTRY_LENGTH]
                storage = entry[0] >> 4
                if storage in (ST_SEEDLING, ST_SAPLING, ST_TREE):
                    yield entry
            num = struct.unpack("<H", block[2:4])[0]

    def find(self, name):
        want = name.upper()
        for entry in self.volume_entries():
            length = entry[0] & 0x0F
            if entry[1:1 + length].decode("ascii", "replace").upper() == want:
                return entry
        return None

    def read_entry(self, entry):
        storage = entry[0] >> 4
        key = struct.unpack("<H", entry[0x11:0x13])[0]
        eof = entry[0x15] | (entry[0x16] << 8) | (entry[0x17] << 16)

        if storage == ST_SEEDLING:
            data = self.block(key)
        elif storage == ST_SAPLING:
            data = self._read_sapling(key, eof)
        elif storage == ST_TREE:
            data = bytearray()
            master = self.block(key)
            for i in range(POINTERS_PER_INDEX):
                if len(data) >= eof:
                    break
                index = master[i] | (master[POINTERS_PER_INDEX + i] << 8)
                chunk = POINTERS_PER_INDEX * BLOCK_SIZE
                data += self._read_sapling(index, min(chunk, eof - len(data)))
        else:
            raise ValueError("unsupported storage type %d" % storage)
        return bytes(data[:eof])

    def _read_sapling(self, index_block, eof):
        if index_block == 0:                            # sparse
            return bytes(eof)
        index = self.block(index_block)
        data = bytearray()
        for i in range(POINTERS_PER_INDEX):
            if len(data) >= eof:
                break
            num = index[i] | (index[POINTERS_PER_INDEX + i] << 8)
            data += bytes(BLOCK_SIZE) if num == 0 else self.block(num)
        return bytes(data[:eof])


def fetch_prodos(url, cache_dir, local=None):
    if local:
        with open(local, "rb") as f:
            return f.read()
    cache_dir = os.path.expanduser(cache_dir)
    path = os.path.join(cache_dir, os.path.basename(url))
    if not os.path.exists(path):
        os.makedirs(cache_dir, exist_ok=True)
        sys.stderr.write("fetching %s\n" % url)
        request = urllib.request.Request(url, headers={"User-Agent": "mkprodos"})
        with urllib.request.urlopen(request) as response:
            data = response.read()
        tmp = path + ".part"
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, path)
        sys.stderr.write("cached %d bytes in %s\n" % (len(data), path))
    with open(path, "rb") as f:
        return f.read()


# ------------------------------------------------------------ volume writer

class Volume(object):
    """A freshly formatted ProDOS volume, written block by block."""

    def __init__(self, name, total_blocks, boot_blocks=None, when=None,
                 first_data=None):
        if total_blocks > 65535:
            raise ValueError("volume too large: %d blocks" % total_blocks)
        self.name = prodos_name(name)
        self.total_blocks = total_blocks
        self.when = when or datetime.datetime.now()
        self.image = bytearray(total_blocks * BLOCK_SIZE)
        self.file_count = 0

        if boot_blocks:
            self.image[0:len(boot_blocks)] = boot_blocks

        # 1 bit per block, MSB first; one bitmap block covers 4096 blocks.
        self.bitmap_blocks = (total_blocks + 4095) // 4096
        self.first_data = first_data or (BITMAP_BLOCK + self.bitmap_blocks)
        self.free = [True] * total_blocks
        for num in range(self.first_data):
            self.free[num] = False

        self._write_volume_header()

    # -- blocks

    def put(self, num, data):
        off = num * BLOCK_SIZE
        self.image[off:off + BLOCK_SIZE] = data.ljust(BLOCK_SIZE, b"\0")

    def allocate(self):
        for num in range(self.first_data, self.total_blocks):
            if self.free[num]:
                self.free[num] = False
                return num
        raise DiskFull("no free blocks on %s" % self.name)

    @property
    def blocks_free(self):
        return sum(1 for f in self.free if f)

    def _write_bitmap(self):
        bits = bytearray(self.bitmap_blocks * BLOCK_SIZE)
        for num in range(self.total_blocks):
            if self.free[num]:
                bits[num >> 3] |= 0x80 >> (num & 7)
        for i in range(self.bitmap_blocks):
            self.put(BITMAP_BLOCK + i,
                     bytes(bits[i * BLOCK_SIZE:(i + 1) * BLOCK_SIZE]))

    # -- directory

    def _write_volume_header(self):
        entry = bytearray(ENTRY_LENGTH)
        entry[0] = (ST_VOLUME_HEADER << 4) | len(self.name)
        entry[1:1 + len(self.name)] = self.name.encode("ascii")
        entry[0x18:0x1C] = prodos_datetime(self.when)
        entry[0x1E] = 0xC3                      # volume access bits
        entry[0x1F] = ENTRY_LENGTH
        entry[0x20] = ENTRIES_PER_BLOCK
        struct.pack_into("<H", entry, 0x23, BITMAP_BLOCK)
        struct.pack_into("<H", entry, 0x25, self.total_blocks)
        self._put_entry(0, bytes(entry))

        # link the four directory blocks into a doubly linked chain
        for i in range(VOLUME_DIR_BLOCKS):
            num = VOLUME_DIR_BLOCK + i
            prev = num - 1 if i else 0
            nxt = num + 1 if i < VOLUME_DIR_BLOCKS - 1 else 0
            off = num * BLOCK_SIZE
            struct.pack_into("<HH", self.image, off, prev, nxt)

    def _put_entry(self, slot, entry):
        """Slot 0 is the volume header; file entries start at slot 1."""
        block = VOLUME_DIR_BLOCK + slot // ENTRIES_PER_BLOCK
        index = slot % ENTRIES_PER_BLOCK
        off = block * BLOCK_SIZE + 4 + index * ENTRY_LENGTH
        self.image[off:off + ENTRY_LENGTH] = entry
        return block

    @property
    def max_files(self):
        return VOLUME_DIR_BLOCKS * ENTRIES_PER_BLOCK - 1

    # -- files

    def add(self, entry):
        if self.file_count >= self.max_files:
            raise DiskFull("volume directory of %s is full" % self.name)
        storage, key, used = self._write_data(entry.data)

        self.file_count += 1
        slot = self.file_count
        block = VOLUME_DIR_BLOCK + slot // ENTRIES_PER_BLOCK
        record = bytearray(ENTRY_LENGTH)
        record[0] = (storage << 4) | len(entry.name)
        record[1:1 + len(entry.name)] = entry.name.encode("ascii")
        record[0x10] = entry.file_type
        struct.pack_into("<H", record, 0x11, key)
        struct.pack_into("<H", record, 0x13, used)
        size = len(entry.data)
        record[0x15] = size & 0xFF
        record[0x16] = (size >> 8) & 0xFF
        record[0x17] = (size >> 16) & 0xFF
        record[0x18:0x1C] = prodos_datetime(self.when)
        record[0x1E] = ACCESS_UNLOCKED
        struct.pack_into("<H", record, 0x1F, entry.aux_type)
        record[0x21:0x25] = prodos_datetime(self.when)
        struct.pack_into("<H", record, 0x25, block)

        self._put_entry(slot, bytes(record))
        struct.pack_into("<H", self.image,
                         VOLUME_DIR_BLOCK * BLOCK_SIZE + 4 + 0x21,
                         self.file_count)

    def _write_data(self, data):
        """Store the payload, returning (storage_type, key_block, blocks_used)."""
        chunks = [data[i:i + BLOCK_SIZE]
                  for i in range(0, len(data), BLOCK_SIZE)] or [b""]

        if len(chunks) == 1:
            key = self.allocate()
            self.put(key, chunks[0])
            return ST_SEEDLING, key, 1

        if len(chunks) <= POINTERS_PER_INDEX:
            key, used = self._write_sapling(chunks)
            return ST_SAPLING, key, used

        pointers = bytearray(BLOCK_SIZE)
        used = 1
        for i in range(0, len(chunks), POINTERS_PER_INDEX):
            index, sub = self._write_sapling(chunks[i:i + POINTERS_PER_INDEX])
            slot = i // POINTERS_PER_INDEX
            pointers[slot] = index & 0xFF
            pointers[POINTERS_PER_INDEX + slot] = index >> 8
            used += sub
        master = self.allocate()
        self.put(master, bytes(pointers))
        return ST_TREE, master, used

    def _write_sapling(self, chunks):
        # Data first, index block after, so that a file's payload starts at
        # the first free block on the volume.  ProDOS does not care where the
        # index block sits, and 0boot needs the interpreter to begin exactly
        # on the track boundary.
        pointers = bytearray(BLOCK_SIZE)
        for i, chunk in enumerate(chunks):
            num = self.allocate()
            self.put(num, chunk)
            pointers[i] = num & 0xFF
            pointers[POINTERS_PER_INDEX + i] = num >> 8
        index = self.allocate()
        self.put(index, bytes(pointers))
        return index, len(chunks) + 1

    def finish(self):
        self._write_bitmap()
        return bytes(self.image)


def build(path, volume_name, total_blocks, boot_blocks, entries, when,
          first_data=None):
    volume = Volume(volume_name, total_blocks, boot_blocks, when, first_data)
    for entry in entries:
        volume.add(entry)
    image = volume.finish()
    with open(path, "wb") as f:
        f.write(image)

    print("%s: %s, %d blocks (%dk), %d files, %d blocks free"
          % (path, "/" + volume.name, total_blocks,
             total_blocks * BLOCK_SIZE // 1024, volume.file_count,
             volume.blocks_free))
    for entry in entries:
        print("    %-15s %-3s $%04X %7d bytes  %3d blocks"
              % (entry.name, TYPE_NAMES.get(entry.file_type, "$%02X"
                                            % entry.file_type),
                 entry.aux_type, len(entry.data), entry.blocks))
    return volume


def boot0_blocks(path):
    """Lay 0boot's two stages out where the Disk II boot PROM will find them."""
    with open(path, "rb") as f:
        blob = f.read()
    if len(blob) <= BLOCK_SIZE // 2:
        raise ValueError("%s is too short to be a 0boot image" % path)
    stage2 = blob[BLOCK_SIZE // 2:]
    if len(stage2) > BLOCK_SIZE // 2:
        raise ValueError("%s has a %d byte second stage, at most %d fits"
                         % (path, len(stage2), BLOCK_SIZE // 2))

    blocks = bytearray(BLOCK_SIZE)
    blocks[0:BLOCK_SIZE // 2] = blob[:BLOCK_SIZE // 2]
    blocks[BOOT0_STAGE2_OFFSET:BOOT0_STAGE2_OFFSET + len(stage2)] = stage2
    return blocks


def check_boot0(volume, entry):
    """0boot reads whole tracks from track 1, so the interpreter has to
    start there and be contiguous."""
    start = BOOT0_FIRST_DATA * BLOCK_SIZE
    if volume.image[start:start + len(entry.data)] != entry.data:
        raise DiskFull("%s is not contiguous from block %d, so 0boot cannot "
                       "load it" % (entry.name, BOOT0_FIRST_DATA))
    tracks = (len(entry.data) + BLOCK_SIZE * BLOCKS_PER_TRACK - 1) \
        // (BLOCK_SIZE * BLOCKS_PER_TRACK)
    print("    0boot loads %d track%s from track 1"
          % (tracks, "" if tracks == 1 else "s"))


def split_name(path, tag):
    stem, ext = os.path.splitext(path)
    return "%s.%s%s" % (stem, tag, ext or ".po")


def main(argv):
    parser = argparse.ArgumentParser(
        description="Build a bootable ProDOS .po disk image.")
    parser.add_argument("files", nargs="+", metavar="FILE[,key=value...]",
                        help="files to add; keys: name, type, aux, disk")
    parser.add_argument("-o", "--output", default="story.po",
                        help="output image (default: story.po)")
    parser.add_argument("-v", "--volume", default="AA.STORY",
                        help="volume name for a single disk "
                             "(default: AA.STORY)")
    parser.add_argument("-s", "--size", default="auto",
                        choices=["auto", "140k", "800k"],
                        help="disk size; auto splits into two 140k disks "
                             "when the files do not fit (default: auto)")
    parser.add_argument("--split", action="store_true",
                        help="always build two disks, boot and story")
    parser.add_argument("--boot-volume", default="AA.BOOT",
                        help="volume name of the boot disk when splitting")
    parser.add_argument("--story-volume", default="AA.STORY",
                        help="volume name of the story disk when splitting")
    parser.add_argument("--url", default=PRODOS_URL,
                        help="ProDOS release image to copy PRODOS from")
    parser.add_argument("--prodos-image",
                        help="use this local .po instead of downloading")
    parser.add_argument("--cache-dir", default="~/.cache/prodos8",
                        help="where to cache the download "
                             "(default: ~/.cache/prodos8)")
    parser.add_argument("--0boot", dest="boot0", metavar="A2_0BOOT.BIN",
                        help="boot with 0boot instead of ProDOS; the first "
                             "file given is loaded from track 1 and entered "
                             "at $2003, and no PRODOS is needed")
    args = parser.parse_args(argv[1:])

    try:
        entries = [parse_spec(spec) for spec in args.files]
    except (OSError, ValueError) as err:
        sys.stderr.write("error: %s\n" % err)
        return 1

    when = datetime.datetime.now()

    if args.boot0:
        try:
            boot_blocks = boot0_blocks(args.boot0)
        except (OSError, ValueError) as err:
            sys.stderr.write("error: %s\n" % err)
            return 1
        # No PRODOS, and the interpreter has to be the first file so that it
        # lands on track 1 where 0boot reads from.
        all_entries = entries
        first_data = BOOT0_FIRST_DATA
    else:
        source = ProdosReader(fetch_prodos(args.url, args.cache_dir,
                                           args.prodos_image))
        found = source.find("PRODOS")
        if found is None:
            sys.stderr.write("error: no PRODOS file in the source image\n")
            return 1
        kernel = Entry("PRODOS", source.read_entry(found),
                       found[0x10], struct.unpack("<H", found[0x1F:0x21])[0],
                       disk="boot")
        boot_blocks = source.image[:2 * BLOCK_SIZE]
        # PRODOS first so the boot block finds it, then the rest in the order
        # given -- ProDOS launches the first .SYSTEM file in the directory.
        all_entries = [kernel] + entries
        first_data = FIRST_DATA_BLOCK

    capacity = BLOCKS_140K - first_data
    max_files = VOLUME_DIR_BLOCKS * ENTRIES_PER_BLOCK - 1
    needed = sum(e.blocks for e in all_entries)

    def one_disk(path, name, blocks):
        volume = build(path, name, blocks, boot_blocks, all_entries, when,
                       first_data)
        if args.boot0:
            check_boot0(volume, all_entries[0])

    if args.boot0 and args.size == "800k":
        sys.stderr.write("error: 0boot reads 5.25\" tracks, so it cannot boot "
                         "an 800k disk\n")
        return 1

    try:
        if args.size == "800k":
            one_disk(args.output, args.volume, BLOCKS_800K)
            return 0

        fits = needed <= capacity and len(all_entries) <= max_files
        if not args.split and fits:
            one_disk(args.output, args.volume, BLOCKS_140K)
            return 0
    except DiskFull as err:
        sys.stderr.write("error: %s\n" % err)
        return 1

    if args.size == "140k" and not args.split:
        sys.stderr.write("error: %d blocks needed, only %d available on a "
                         "140k disk; use -s 800k or --split\n"
                         % (needed, capacity))
        return 1

    boot_files = [e for e in all_entries if e.wants_boot_disk()]
    story_files = [e for e in all_entries if not e.wants_boot_disk()]
    if not story_files:
        sys.stderr.write("error: nothing to put on the story disk\n")
        return 1

    boot_path = args.output
    story_path = split_name(args.output, "story")
    try:
        volume = build(boot_path, args.boot_volume, BLOCKS_140K, boot_blocks,
                       boot_files, when, first_data)
        if args.boot0:
            check_boot0(volume, boot_files[0])
        # Under ProDOS the story disk keeps the boot blocks, so booting it by
        # mistake gives ProDOS's own "unable to load" message rather than a
        # hang.  0boot's would try to load an interpreter that is not there,
        # so the data disk gets none, and the block back.
        build(story_path, args.story_volume, BLOCKS_140K,
              None if args.boot0 else boot_blocks, story_files, when)
    except DiskFull as err:
        sys.stderr.write("error: %s\n" % err)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
