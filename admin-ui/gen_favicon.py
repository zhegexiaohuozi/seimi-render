# -*- coding: utf-8 -*-
"""生成 seimi-render favicon.ico（16/32/48 多尺寸内嵌 PNG）。

设计：蓝色渐变方块底（圆角）+ 居中白色闪电。
每个尺寸直接用图元绘制（矢量级清晰），不用 qlmanage/外部工具。
闪电路径与 favicon.svg 一致：M72 18 L34 74 H60 L54 110 L94 50 H68 Z（128 坐标系）。

纯 Python 标准库，无外部依赖。ICO 结构 = ICONDIR + 每尺寸 ICONDIRENTRY + 各 PNG 原始字节。
"""
import struct, zlib, math, os

def make_png(pixels, w, h):
    """pixels: w*h 的 [(r,g,b,a)] 列表。返回 PNG 字节（标准库 zlib 压缩）。"""
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter type 0 (None)
        for x in range(w):
            r, g, b, a = pixels[y * w + x]
            raw += bytes([r, g, b, a])
    def chunk(typ, data):
        c = struct.pack('>I', len(data)) + typ + data
        crc = zlib.crc32(typ + data) & 0xffffffff
        return c + struct.pack('>I', crc)
    sig = b'\x89PNG\r\n\x1a\n'
    ihdr = struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)  # 8bit RGBA
    idat = zlib.compress(bytes(raw), 9)
    return sig + chunk(b'IHDR', ihdr) + chunk(b'IDAT', idat) + chunk(b'IEND', b'')

def render_icon(size):
    """渲染指定尺寸的图标，返回 [(r,g,b,a)]*size*size。"""
    # 128 坐标系 -> 缩放到 size
    s = 128.0 / size
    pixels = []
    # 渐变色（顶到底）：#3b82f6 -> #1d4ed8
    def bg_color(fy):
        t = fy  # 0=top,1=bottom
        r = int(0x3b + (0x1d - 0x3b) * t)
        g = int(0x82 + (0x4e - 0x82) * t)
        b = int(0xf6 + (0xd8 - 0xf6) * t)
        return (r, g, b, 255)

    # 圆角矩形判定（rx=28 in 128 space）
    rx = 28.0
    def in_rounded_rect(ix, iy):
        # ix,iy in 128 space (pixel center)
        if ix < rx and iy < rx:
            return (rx - ix) ** 2 + (rx - iy) ** 2 <= rx * rx
        if ix > 128 - rx and iy < rx:
            return (ix - (128 - rx)) ** 2 + (rx - iy) ** 2 <= rx * rx
        if ix < rx and iy > 128 - rx:
            return (rx - ix) ** 2 + (iy - (128 - rx)) ** 2 <= rx * rx
        if ix > 128 - rx and iy > 128 - rx:
            return (ix - (128 - rx)) ** 2 + (iy - (128 - rx)) ** 2 <= rx * rx
        return True

    # 闪电路径（128 坐标系，与 svg 一致）：M72 18 L34 74 H60 L54 110 L94 50 H68 Z
    # 顶点： (72,18) (34,74) (60,74) (54,110) (94,50) (68,50) 闭合回(72,18)
    bolt = [(72, 18), (34, 74), (60, 74), (54, 110), (94, 50), (68, 50)]
    def point_in_bolt(px, py):
        # 射线法（奇偶规则）判断点在多边形内
        inside = False
        n = len(bolt)
        j = n - 1
        for i in range(n):
            xi, yi = bolt[i]
            xj, yj = bolt[j]
            if ((yi > py) != (yj > py)) and (px < (xj - xi) * (py - yi) / (yj - yi + 1e-12) + xi):
                inside = not inside
            j = i
        return inside

    for y in range(size):
        for x in range(size):
            # 像素中心映射到 128 空间
            cx = (x + 0.5) * s
            cy = (y + 0.5) * s
            # 圆角矩形外 -> 透明
            if not in_rounded_rect(cx, cy):
                pixels.append((0, 0, 0, 0))
                continue
            # 闪电内 -> 白色；描边（距边界 <3）-> 深蓝
            if point_in_bolt(cx, cy):
                pixels.append((255, 255, 255, 255))
            else:
                pixels.append(bg_color(cy / 128.0))
    return pixels

def make_ico(sizes, outpath):
    """生成多尺寸 ICO。"""
    images = []
    for sz in sizes:
        px = render_icon(sz)
        png = make_png(px, sz, sz)
        images.append((sz, png))
    # ICONDIR
    header = struct.pack('<HHH', 0, 1, len(images))
    entries = b''
    offset = 6 + 16 * len(images)
    for sz, png in images:
        w = 0 if sz == 256 else sz
        h = 0 if sz == 256 else sz
        entries += struct.pack('<BBBBHHII', w, h, 0, 0, 1, 32, len(png), offset)
        offset += len(png)
    data = header + entries
    for _, png in images:
        data += png
    with open(outpath, 'wb') as f:
        f.write(data)
    print(f"wrote {outpath} ({len(data)} bytes, sizes={sizes})")

if __name__ == '__main__':
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, 'favicon.ico')
    make_ico([16, 32, 48], out)
