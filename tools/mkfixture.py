#!/usr/bin/env python3
"""mkfixture — 生成最小合成样本，用于冒烟测试 tools/ 里的解包工具。

不是游戏资产生成器，只造「结构最小但格式合法」的文件，让 opfs / psb / asb 在没有
真实游戏包的环境（CI、新机器）也能被验证：

    python tools/mkfixture.py psb <out.psb>   # PSB v2：对象树 + 1 个块
    python tools/mkfixture.py asb <out.asb>   # ASB：2 个标签 + 3 条标签行
    python tools/mkfixture.py pfs <out.pfs>   # pf6：2 个文件
    python tools/mkfixture.py save <out.dat>  # 存档域映射（saveg/system.dat）

格式依据（与引擎读取器同源）：
  * PSB — src/core/emote/psb_reader.cpp（v2 头 40 字节；数组 = 种类/计数/宽度标记/
          条目；名称表 = charset + namesData + nameIndexes 三点树；字符串表 =
          相对偏移数组 + NUL 结尾字符串）
  * ASB — src/core/runtime/runtime_iet.cpp §5（"ASB\\0" + flag + 条目数；标签/指令两种条目）
  * PFS — src/core/fs/physfs_fs.cpp（pf6：11 字节头 + 每文件
          u32 名字长度/名字/保留/偏移/大小）
  * 存档 — src/core/util/binary_stream.h（'OASB' + u32 版本 + 标签流；本脚本只造
          域映射这一种；编号存档 SaveData 见引擎自身的存档测试）
"""
import struct
import sys

# ---------------------------------------------------------------------------
# PSB
# ---------------------------------------------------------------------------
K_NUMBER1 = 0x05      # NumberN1：kind = 0x04 + width
K_STRING2 = 0x16      # StringN2：kind = 0x14 + width
K_RESOURCE1 = 0x19    # ResourceN1：kind = 0x18 + width
K_LIST = 0x20
K_OBJECTS = 0x21
A1 = 0x0D             # ArrayN1


def arr(values, width=None, count_width=None):
    """PSB 数组：[kind][count][width-marker][entries]；width-marker = 0x0C + 条目宽度。
    计数宽度与条目宽度都按需选（kind = ArrayN<count_width> = 0x0D + cw - 1）。"""
    if count_width is None:
        count_width = 1 if len(values) < 0x100 else (2 if len(values) < 0x10000 else 4)
    if width is None:
        top = max(values) if values else 0
        width = 1 if top < 0x100 else (2 if top < 0x10000 else 4)
    out = bytearray([A1 + count_width - 1])
    out += len(values).to_bytes(count_width, "little")
    out.append(0x0C + width)
    for v in values:
        out += int(v).to_bytes(width, "little")
    return bytes(out)


def points_at(blob, off, what):
    return blob[off:off + len(what)] == what


def build_psb():
    names = ["name", "num", "list", "px", "spec"]
    strings = ["hello psb", "mini/first.iet"]

    # ---- 名称三点树
    # 读取器走法：cur = namesData[idx]; while (cur != 0) { code = namesData[cur];
    #   byte = cur - charset[code]; 记录 byte; cur = code; }
    # 即 nameIndexes[i] 存「终止标记节点」，它的父节点才是最后一个字符；
    # 节点字节 = 节点下标 - charset[父节点下标]（首字符的父 = 根 0）。
    children_base = {}   # parent -> 子节点基址
    parent_of = {}       # node -> parent
    next_base = 256

    def intern(parent, byte):
        nonlocal next_base
        base = children_base.get(parent)
        if base is None:
            base = next_base
            next_base += 256
            children_base[parent] = base
        idx = base + byte
        parent_of.setdefault(idx, parent)
        return idx

    name_indexes = []
    for nm in names:
        cur = 0
        for ch in nm.encode("ascii"):
            cur = intern(cur, ch)
        name_indexes.append(intern(cur, 0))   # 终止标记

    size = max(parent_of) + 1
    trie = [0] * size
    charset = [0] * size
    for idx, p in parent_of.items():
        trie[idx] = p
    for p, base in children_base.items():
        if p < size:
            charset[p] = base

    def enc_str(idx):
        return bytes([K_STRING2]) + struct.pack("<H", idx)

    def enc_num(v):
        return bytes([K_NUMBER1]) + struct.pack("<b", v)

    # ---- 值区
    num7, num9 = enc_num(7), enc_num(9)
    list_val = bytes([K_LIST]) + arr([0, len(num7)], width=1) + num7 + num9
    # 成员顺序与 names 一致：name(str) num(int) list(list) px(resource) spec(str)
    members = [enc_str(0), enc_num(42), bytes([K_LIST]) + b"\x00\x00\x00",
               bytes([K_RESOURCE1, 0]), enc_str(1)]

    # 对象里的 names 数组存的是「名称表下标」(0..N-1),不是三点树节点下标;
    # 三点树只用于名称表自身(charset + namesData + nameIndexes)。
    names_arr = arr(list(range(len(names))))
    offs_w = 2                             # 成员偏移一律 2 字节(文件 < 64 KiB)
    offs_arr_len = 3 + offs_w * len(members)
    member_area_abs = 40 + 1 + len(names_arr) + offs_arr_len   # 成员值区绝对起点
    # 读取器：成员值偏移 = (偏移数组之后) + 相对值，其中"偏移数组之后"是绝对偏移。
    def root_len():
        return 1 + len(names_arr) + offs_arr_len + sum(len(m) for m in members)

    offs = [0] * len(members)
    cur = 0
    for i, m in enumerate(members):
        offs[i] = cur
        cur += len(m)
    else_off = 40 + root_len()
    offs[2] = else_off - member_area_abs   # list 成员指向独立区块
    root = (bytes([K_OBJECTS]) + names_arr + arr(offs, width=offs_w) +
            b"".join(members))
    assert len(root) == root_len()

    # ---- 表区
    chunk_data = b"MINI-CHUNK-DATA!"       # 16 字节块
    chunk_offsets = arr([0], width=1)
    chunk_lengths = arr([len(chunk_data)], width=1)
    names_sec = arr(charset) + arr(trie) + arr(name_indexes)
    strings_data = b"\x00".join(s.encode("utf-8") for s in strings) + b"\x00"
    str_offs, off = [], 0
    for s in strings:
        str_offs.append(off)
        off += len(s.encode("utf-8")) + 1
    strings_sec = arr(str_offs, width=1)

    names_off = else_off + len(list_val)
    strings_off = names_off + len(names_sec)
    strings_data_off = strings_off + len(strings_sec)
    chunk_offsets_off = strings_data_off + len(strings_data)
    chunk_lengths_off = chunk_offsets_off + len(chunk_offsets)
    chunk_data_off = chunk_lengths_off + len(chunk_lengths)

    header = b"PSB\x00" + struct.pack("<HH", 2, 0) + struct.pack(
        "<IIIIIIII", 40, names_off, strings_off, strings_data_off,
        chunk_offsets_off, chunk_lengths_off, chunk_data_off, 40)
    blob = (header + root + list_val + names_sec + strings_sec + strings_data +
            chunk_offsets + chunk_lengths + chunk_data)
    assert points_at(blob, names_off, names_sec), "names offset"
    assert points_at(blob, strings_off, strings_sec), "strings offset"
    assert points_at(blob, strings_data_off, strings_data), "strings data offset"
    assert points_at(blob, 40, root), "entries offset"
    assert points_at(blob, else_off, list_val), "list offset"
    return blob


# ---------------------------------------------------------------------------
# ASB
# ---------------------------------------------------------------------------
def build_asb():
    out = bytearray(b"ASB\x00")
    out += bytes([0])                      # flag
    entries = [("label", "main", None),
               ("instr", "lyc", {"id": "1", "file": "bg.png"}),
               ("instr", "wait", {"time": "600"}),
               ("label", "end", None),
               ("instr", "stop", {})]
    out += struct.pack("<I", len(entries))
    for kind, name, params in entries:
        nb = name.encode("utf-8")
        out += struct.pack("<II", 1 if kind == "label" else 0, len(nb))
        out += nb + b"\x00"
        if kind == "instr":
            out += struct.pack("<II", 0, len(params))
            for k, v in params.items():
                kb, vb = k.encode("utf-8"), v.encode("utf-8")
                out += struct.pack("<I", len(kb)) + kb + b"\x00"
                out += struct.pack("<I", len(vb)) + vb + b"\x00"
    return bytes(out)


# ---------------------------------------------------------------------------
# PFS (pf6)
# ---------------------------------------------------------------------------
def build_pfs():
    files = [("system.ini", b"[WINDOWS]\nWIDTH = 1280\nHEIGHT = 720\n"),
             ("system/first.iet", b"*main\n[wait time=\"600\"]\n[stop]\n")]
    header_len = 11
    index_size = 0
    index, payload = bytearray(), bytearray()
    for _ in range(3):                     # 索引大小影响载荷偏移 -> 迭代到稳定
        index, payload = bytearray(), bytearray()
        base = header_len + index_size
        for name, data in files:
            nb = name.encode("utf-8")
            off = base + len(payload)
            index += struct.pack("<I", len(nb)) + nb
            index += struct.pack("<III", 0, off, len(data))
            payload += data
        index_size = len(index)
    out = bytearray(b"pf6")
    out += struct.pack("<II", len(index), len(files))
    out += index
    out += payload
    return bytes(out)


# ---------------------------------------------------------------------------
# Save domain map (saveg.dat / system.dat) — oa::util tagged stream
# (research/64; src/core/util/binary_stream.h): header 'OASB' + u32 version,
# then a map of string keys -> values. Tag table: fixmap 0x80|n, fixstr
# 0xa0|len, positive fixint 0x00..0x7f, negative fixint 0xe0..0xff,
# true/false = 0xc2/0xc3, nil = 0xc0, float64 = 0xcb + 8 bytes LE.
# ---------------------------------------------------------------------------
def _bstr(s):
    b = s.encode("utf-8")
    if len(b) <= 31:
        return bytes([0xA0 | len(b)]) + b
    return bytes([0xC4, len(b)]) + b


def _bint(v):
    if 0 <= v <= 0x7F:
        return bytes([v])
    if -32 <= v < 0:
        return bytes([0x100 + v & 0xFF])
    if 0 <= v <= 0xFF:
        return bytes([0xCC, v])
    if 0 <= v <= 0xFFFF:
        return bytes([0xCD]) + struct.pack("<H", v)
    return bytes([0xCE]) + struct.pack("<I", v & 0xFFFFFFFF)


def build_save():
    vars_ = [("bgmfile", "bgm01.ogg"),
             ("bgmvol", 800),
             ("chapter", 3),
             ("cleared", True),
             ("heroine", "aya"),
             ("lastsave", "save/01.dat")]
    out = bytearray(b"OASB")
    out += struct.pack("<I", 1)             # kFormatVersion
    out += bytes([0x80 | len(vars_)])       # fixmap
    for k, v in vars_:
        out += _bstr(k)
        if isinstance(v, bool):
            out += bytes([0xC2 if v else 0xC3])
        elif isinstance(v, int):
            out += _bint(v)
        else:
            out += _bstr(v)
    return bytes(out)


# ---------------------------------------------------------------------------
def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    kind, path = argv[1], argv[2]
    builders = {"psb": build_psb, "asb": build_asb, "pfs": build_pfs,
                "save": build_save}
    if kind not in builders:
        print(f"unknown kind '{kind}' (psb|asb|pfs|save)")
        return 2
    blob = builders[kind]()
    with open(path, "wb") as f:
        f.write(blob)
    print(f"{kind}: {path} ({len(blob)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
