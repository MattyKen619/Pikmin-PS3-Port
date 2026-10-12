#!/usr/bin/env python3
"""
PS3 port: extract the game's files from your own GameCube disc image (ISO/GCM or CISO) into a folder
that the PS3 DVD layer reads. Also writes fst.txt, the disc's file table (entry number -> path), which
the game uses for DVDFastOpen / DVDConvertPathToEntrynum.

Usage:
  python3 extract_disc.py "Pikmin (USA) (Rev 1).iso" OUTPUT_DIR

Result:
  OUTPUT_DIR/files/...   every file on the disc, same folder structure
  OUTPUT_DIR/fst.txt     one line per FST entry: "<entrynum>\t<d|f>\t<path>"
  OUTPUT_DIR/disc.txt    game ID, revision and file count

Never share or upload the extracted files; they are Nintendo's copyrighted game data.
"""
import os
import struct
import sys


class DiscReader:
    """Reads plain ISO/GCM images and CISO (compact ISO) images."""

    def __init__(self, path):
        self.f = open(path, "rb")
        head = self.f.read(0x8000)
        self.ciso = head[:4] == b"CISO"
        if self.ciso:
            self.block_size = struct.unpack("<I", head[4:8])[0]
            self.index = {}
            n = 0
            for i, present in enumerate(head[8:0x8000]):
                if present:
                    self.index[i] = n
                    n += 1

    def read(self, offset, size):
        if not self.ciso:
            self.f.seek(offset)
            return self.f.read(size)
        out = bytearray()
        while size > 0:
            block, inner = divmod(offset, self.block_size)
            take = min(size, self.block_size - inner)
            if block in self.index:
                self.f.seek(0x8000 + self.index[block] * self.block_size + inner)
                out += self.f.read(take)
            else:
                out += b"\0" * take
            offset += take
            size -= take
        return bytes(out)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    disc = DiscReader(sys.argv[1])
    out_dir = sys.argv[2]
    header = disc.read(0, 0x440)
    if struct.unpack(">I", header[0x1C:0x20])[0] != 0xC2339F3D:
        sys.exit("Not a GameCube disc image (bad magic).")
    game_id = header[0:6].decode("ascii", "replace")
    revision = header[7]
    fst_offset, fst_size = struct.unpack(">II", header[0x424:0x42C])
    fst = disc.read(fst_offset, fst_size)
    count = struct.unpack(">I", fst[8:12])[0]
    strings = fst[count * 12:]

    def name_at(off):
        end = strings.index(b"\0", off)
        return strings[off:end].decode("shift_jis", "replace")

    files_root = os.path.join(out_dir, "files")
    os.makedirs(files_root, exist_ok=True)
    lines = ["0\td\t/"]
    dir_stack = [(count, "")]  # (end entry, path prefix)
    total_bytes = 0
    nfiles = 0
    for i in range(1, count):
        while dir_stack and i >= dir_stack[-1][0]:
            dir_stack.pop()
        prefix = dir_stack[-1][1] if dir_stack else ""
        word0, word1, word2 = struct.unpack(">III", fst[i * 12:i * 12 + 12])
        is_dir = (word0 >> 24) != 0
        name = name_at(word0 & 0xFFFFFF)
        path = f"{prefix}/{name}"
        lines.append(f"{i}\t{'d' if is_dir else 'f'}\t{path}")
        if is_dir:
            os.makedirs(os.path.join(files_root, path.lstrip("/")), exist_ok=True)
            dir_stack.append((word2, path))
        else:
            dest = os.path.join(files_root, path.lstrip("/"))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as out:
                remaining, pos = word2, word1
                while remaining:
                    chunk = disc.read(pos, min(remaining, 4 << 20))
                    out.write(chunk)
                    pos += len(chunk)
                    remaining -= len(chunk)
            total_bytes += word2
            nfiles += 1
            if nfiles % 200 == 0:
                print(f"  {nfiles} files, {total_bytes / 1e6:.0f} MB ...", flush=True)
    with open(os.path.join(out_dir, "fst.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    with open(os.path.join(out_dir, "disc.txt"), "w") as f:
        f.write(f"game_id={game_id}\nrevision={revision}\nfst_entries={count}\nfiles={nfiles}\nbytes={total_bytes}\n")
    print(f"Done: {game_id} rev {revision}, {nfiles} files, {total_bytes / 1e6:.1f} MB -> {out_dir}")


if __name__ == "__main__":
    main()
