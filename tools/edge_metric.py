#!/usr/bin/env python3
"""描边环连续性度量（现象 J+，research/126）——对 fontscan 冻结帧 PNG 逐像素。

口径（与 tools/fontedge.cpp 同一判据，作用在真实帧上）：
  ink   = 与 outlinecolor 逐字节相等的像素（描边 pass 用 color_mod=outlinecolor
          画描边层贴图，不透明处必然精确等于该色）。
  cand  = "被描边三面包住"的文本色像素：文本色像素中，4 个轴向里至少有 3 个在
          reach px 内命中 ink。消息窗底色/立绘白块不可能三面被自己的字环包住
          ⇒ 该判据把字形从底色里分出来（底色只会一到两面邻环）。
  对每个 cand 的 4 个方向：reach px 内无 ink ⇒ open（该方向缺环）。
  由于 cand 入场要求 ≥3 面有环，open 计数 = "第四面缺环的字形像素数"，即
  环在字形边界的**开口/断裂**量。open_clusters = open 像素的 8 连通簇数。
  ring_offset = 首个 ink 的距离（字形 AA 边 + 环内沿）；ring_thickness = 连续 ink 长。

用法: edge_metric.py <png> <outlinecolor_hex> [textcolor_hex] [--reach N]
"""
import sys
from collections import deque

from PIL import Image


def hexc(s):
    s = s.strip().lstrip('#')
    if s.lower().startswith('0x'):
        s = s[2:]
    v = int(s, 16)
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255, 255)


def main():
    png = sys.argv[1]
    oc = hexc(sys.argv[2])
    tc = (255, 255, 255, 255)
    if len(sys.argv) > 3 and not sys.argv[3].startswith('--'):
        tc = hexc(sys.argv[3])
    reach = 3
    if '--reach' in sys.argv:
        reach = int(sys.argv[sys.argv.index('--reach') + 1])
    im = Image.open(png).convert('RGBA')
    w, h = im.size
    px = im.load()
    ink = [[False] * w for _ in range(h)]
    col = [[False] * w for _ in range(h)]
    n_ink = n_col = 0
    for y in range(h):
        for x in range(w):
            p = px[x, y]
            if p[3] < 250:
                continue
            if p[:3] == oc[:3]:
                ink[y][x] = True
                n_ink += 1
            if p[:3] == tc[:3]:
                col[y][x] = True
                n_col += 1
    dirs = ((1, 0), (-1, 0), (0, 1), (0, -1))

    def first_ink(x, y, dx, dy):
        for k in range(1, reach + 1):
            xx, yy = x + dx * k, y + dy * k
            if not (0 <= xx < w and 0 <= yy < h):
                return 0
            if ink[yy][xx]:
                return k
        return 0

    cand = [[False] * w for _ in range(h)]
    n_cand = 0
    for y in range(h):
        for x in range(w):
            if not col[y][x]:
                continue
            # 候选 = 文本色像素且 ≥2 个轴向邻居不是文本色（细笔画/拐角/笔画端）。
            # 消息窗底色像素沿环只有 1 面非文本色 ⇒ 被排除（抗底色污染）。
            off = sum(1 for dx, dy in dirs
                      if not (0 <= x + dx < w and 0 <= y + dy < h)
                      or not col[y + dy][x + dx])
            if off >= 2:
                cand[y][x] = True
                n_cand += 1
    open_px = []
    thicks = []
    offs = []
    for y in range(h):
        for x in range(w):
            if not cand[y][x]:
                continue
            for dx, dy in dirs:
                nx, ny = x + dx, y + dy
                if 0 <= nx < w and 0 <= ny < h and col[ny][nx]:
                    continue  # 内向（仍是字形）
                k = first_ink(x, y, dx, dy)
                if k == 0:
                    open_px.append((nx, ny))
                    continue
                offs.append(k)
                ln = 0
                while True:
                    xx, yy = x + dx * (k + ln), y + dy * (k + ln)
                    if not (0 <= xx < w and 0 <= yy < h) or not ink[yy][xx]:
                        break
                    ln += 1
                thicks.append(ln)
    oset = set(open_px)
    seen = set()
    clusters = 0
    for p in open_px:
        if p in seen:
            continue
        clusters += 1
        dq = deque([p])
        seen.add(p)
        while dq:
            cx, cy = dq.popleft()
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    q = (cx + dx, cy + dy)
                    if q in oset and q not in seen:
                        seen.add(q)
                        dq.append(q)
    tmin = min(thicks) if thicks else 0
    tmax = max(thicks) if thicks else 0
    tmean = sum(thicks) / len(thicks) if thicks else 0.0
    omean = sum(offs) / len(offs) if offs else 0.0
    print(f"{png}\n  size={w}x{h} ink={n_ink} textcolor_px={n_col} "
          f"cand={n_cand} ink/cand={n_ink / max(1, n_cand):.3f}\n"
          f"  open_dirs={len(open_px)} open_clusters={clusters} "
          f"(三面有环的字形像素中第四面缺环)\n"
          f"  ring_offset_mean={omean:.2f} ring_thickness min={tmin} max={tmax} "
          f"mean={tmean:.2f} reach={reach}")


if __name__ == '__main__':
    main()
