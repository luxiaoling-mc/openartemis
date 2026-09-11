#!/usr/bin/env python3
"""pfs.py — read-only survey tool for Artemis PFS archives (pf2/pf6/pf8).

Reusable for development and QA of the C++ engine. Format facts (index
layouts, SHA1/XOR obfuscation for pf8, volume chains) derived from
black-box archive analysis and cross-checked against C++ opfs.

Usage:
  pfs.py info <archive>                 # header + dir/ext summary
  pfs.py find <archive> [substr]        # list entries, optional case-insensitive substring
  pfs.py get  <archive> <name> [out]    # write one file (out) or print head to stdout
  pfs.py dump <archive> <name> <out>    # alias of get with mandatory out
"""
import struct, sys, os, hashlib, collections, posixpath

ARCHIVE_MAGIC = b'pf'
HEADER_SIZE = 11  # magic(2) + version(1) + index_size(4) + file_count(4)


class Pfs:
    def __init__(self, path):
        self.path = path
        f = open(path, 'rb')
        magic = f.read(2)
        ver = f.read(1)
        if magic != ARCHIVE_MAGIC:
            raise ValueError(f"not a PFS archive (magic={magic!r})")
        self.version = ver.decode('ascii', 'replace')
        self.index_size, self.file_count = struct.unpack('<II', f.read(8))
        f.seek(HEADER_SIZE)
        self.entries = []
        for _ in range(self.file_count):
            (plen,) = struct.unpack('<I', f.read(4))
            raw = f.read(plen)
            reserved, off, size = struct.unpack('<III', f.read(12))
            self.entries.append((raw.decode('utf-8', 'replace'), reserved, off, size))
        f.close()
        # V8: XOR key = SHA1(index bytes at file offset 7, index_size bytes)
        self.xor_key = None
        if self.version == '8':
            f = open(path, 'rb')
            f.seek(HEADER_SIZE - 4)
            data = f.read(self.index_size)
            f.close()
            self.xor_key = hashlib.sha1(data).digest()

    def find(self, name):
        target = name.replace('\\', '/').lower()
        for p, reserved, off, size in self.entries:
            if p.replace('\\', '/').lower() == target:
                return p, reserved, off, size
        return None

    def read(self, off, size):
        f = open(self.path, 'rb')
        f.seek(off)
        data = f.read(size)
        f.close()
        if self.xor_key:
            data = bytes(b ^ self.xor_key[i % len(self.xor_key)] for i, b in enumerate(data))
        return data

    def get(self, name):
        ent = self.find(name)
        if not ent:
            return None
        return self.read(ent[2], ent[3])

    def dump_text(self, name, limit=20000):
        data = self.get(name)
        if data is None:
            return f"(not found: {name})"
        head = data[:limit]
        # treat as text if it decodes as UTF-8 with no replacement chars,
        # or if ASCII printable dominates
        try:
            text = head.decode('utf-8')
            if '\ufffd' not in text:
                return text
        except UnicodeDecodeError:
            pass
        printable = sum(1 for b in head if 32 <= b < 127 or b in (9, 10, 13))
        if head and printable / len(head) > 0.75:
            return head.decode('utf-8', 'replace')
        return f"<binary {len(data)} bytes>"


def cmd_info(path):
    p = Pfs(path)
    print(f"{os.path.basename(path)}: version=pf{p.version} index_size={p.index_size} "
          f"file_count={p.file_count} total={os.path.getsize(path)} bytes")
    ext = collections.Counter()
    top = collections.Counter()
    for name, reserved, off, size in p.entries:
        norm = name.replace('\\', '/')
        parts = norm.split('/')
        top[parts[0].lower() if len(parts) > 1 else '(root)'] += 1
        dot = norm.rfind('.')
        ext[norm[dot:].lower() if dot >= 0 else '(none)'] += 1
    print("\n== top-level dirs ==")
    for k, v in top.most_common(80):
        print(f"  {k:34s} {v}")
    print("\n== extensions ==")
    for k, v in ext.most_common(80):
        print(f"  {k:34s} {v}")


def cmd_find(path, substr=None):
    p = Pfs(path)
    for name, reserved, off, size in p.entries:
        if substr is None or substr.lower() in name.replace('\\', '/').lower():
            print(f"  {size:12d}  {name}")


def cmd_get(path, name, out=None, limit=20000):
    p = Pfs(path)
    ent = p.find(name)
    if not ent:
        print(f"not found: {name}", file=sys.stderr)
        sys.exit(1)
    data = p.read(ent[2], ent[3])
    if out:
        os.makedirs(os.path.dirname(out) or '.', exist_ok=True)
        with open(out, 'wb') as w:
            w.write(data)
        print(f"{name} ({len(data)} bytes) -> {out}")
    else:
        head = data[:limit]
        try:
            text = head.decode('utf-8')
            if '\ufffd' not in text:
                sys.stdout.buffer.write(text.encode('utf-8'))
                sys.stdout.buffer.write(b"\n")
                return
        except UnicodeDecodeError:
            pass
        printable = sum(1 for b in head if 32 <= b < 127 or b in (9, 10, 13))
        if printable / len(head) > 0.75:
            sys.stdout.buffer.write(head)
            sys.stdout.buffer.write(b"\n")
        else:
            print(f"<binary {len(data)} bytes; hexdump of first 512>")
            for j in range(0, min(len(head), 512), 16):
                chunk = head[j:j + 16]
                hexs = ' '.join(f'{b:02x}' for b in chunk)
                asc = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
                print(f'{ent[2] + j:08x}  {hexs:<48s}  {asc}')


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        sys.exit(2)
    cmd, path = argv[1], argv[2]
    if cmd == 'info':
        cmd_info(path)
    elif cmd == 'find':
        cmd_find(path, argv[3] if len(argv) > 3 else None)
    elif cmd == 'get':
        cmd_get(path, argv[3], argv[4] if len(argv) > 4 else None)
    elif cmd == 'dump':
        if len(argv) < 5:
            print("usage: pfs.py dump <archive> <name> <out>")
            sys.exit(2)
        cmd_get(path, argv[3], argv[4], limit=0x7fffffff)
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == '__main__':
    main(sys.argv)
