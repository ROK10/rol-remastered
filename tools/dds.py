"""Minimal DDS helpers for Rise of Legends textures (DXT1/DXT5/A8R8G8B8 read, A8R8G8B8 write with mips)."""
import struct

import numpy as np
import texture2ddecoder

DDSD_CAPS, DDSD_HEIGHT, DDSD_WIDTH, DDSD_PITCH = 0x1, 0x2, 0x4, 0x8
DDSD_PIXELFORMAT, DDSD_MIPMAPCOUNT, DDSD_LINEARSIZE = 0x1000, 0x20000, 0x80000
DDPF_ALPHAPIXELS, DDPF_FOURCC, DDPF_RGB = 0x1, 0x4, 0x40
DDSCAPS_COMPLEX, DDSCAPS_TEXTURE, DDSCAPS_MIPMAP = 0x8, 0x1000, 0x400000


def info(data):
    if data[:4] != b'DDS ':
        raise ValueError('not a DDS')
    h, w, pitch, depth, mips = struct.unpack_from('<5I', data, 12)
    pf_flags, fourcc, bpp, rmask, gmask, bmask, amask = struct.unpack_from('<I4s5I', data, 80)
    if pf_flags & DDPF_FOURCC:
        fmt = fourcc.decode('latin1')
    else:
        fmt = f'RGB{bpp}' + ('A' if pf_flags & DDPF_ALPHAPIXELS else '')
    return dict(width=w, height=h, mips=max(mips, 1), fmt=fmt, masks=(rmask, gmask, bmask, amask), bpp=bpp)


def decode_top(data):
    """Decode mip 0 to an RGBA uint8 array (H, W, 4)."""
    i = info(data)
    w, h, fmt = i['width'], i['height'], i['fmt']
    body = data[128:]
    if fmt == 'DXT1':
        raw = texture2ddecoder.decode_bc1(body[:max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 8], w, h)
    elif fmt in ('DXT5', 'DXT4'):
        raw = texture2ddecoder.decode_bc3(body[:max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 16], w, h)
    elif fmt in ('DXT3', 'DXT2'):
        raise NotImplementedError('DXT3 not used by RoL')
    elif fmt.startswith('RGB32'):
        arr = np.frombuffer(body[:w * h * 4], np.uint8).reshape(h, w, 4)
        out = np.empty_like(arr)
        out[..., 0], out[..., 1], out[..., 2], out[..., 3] = arr[..., 2], arr[..., 1], arr[..., 0], arr[..., 3]
        if fmt == 'RGB32':
            out[..., 3] = 255
        return out
    elif fmt == 'RGB24':
        arr = np.frombuffer(body[:w * h * 3], np.uint8).reshape(h, w, 3)
        out = np.full((h, w, 4), 255, np.uint8)
        out[..., 0], out[..., 1], out[..., 2] = arr[..., 2], arr[..., 1], arr[..., 0]
        return out
    else:
        raise NotImplementedError(fmt)
    # texture2ddecoder returns BGRA
    arr = np.frombuffer(raw, np.uint8).reshape(h, w, 4)
    return arr[..., [2, 1, 0, 3]].copy()


def write_argb8888(levels):
    """levels: list of RGBA uint8 arrays (mip chain, largest first). Returns DDS bytes (A8R8G8B8)."""
    h, w = levels[0].shape[:2]
    flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT
    caps = DDSCAPS_TEXTURE
    if len(levels) > 1:
        flags |= DDSD_MIPMAPCOUNT
        caps |= DDSCAPS_COMPLEX | DDSCAPS_MIPMAP
    hdr = bytearray(128)
    hdr[0:4] = b'DDS '
    struct.pack_into('<7I', hdr, 4, 124, flags, h, w, w * 4, 0, len(levels) if len(levels) > 1 else 0)
    struct.pack_into('<2I4s5I', hdr, 76, 32, DDPF_RGB | DDPF_ALPHAPIXELS, b'\0\0\0\0', 32,
                     0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    struct.pack_into('<I', hdr, 108, caps)
    out = bytearray(hdr)
    for lv in levels:
        out += np.ascontiguousarray(lv[..., [2, 1, 0, 3]]).tobytes()
    return bytes(out)
