"""Reader for Atomic Bomberman .ANI animation files (new code; no original material).

Format notes are in docs/reverse-engineering/file-formats.md.
"""
import struct
from dataclasses import dataclass, field

MAGIC = b"CHFILEANI "
CONTAINERS = (b"FRAM", b"SEQ ", b"STAT")


@dataclass
class Chunk:
    tag: bytes
    ident: int
    offset: int
    data: bytes
    children: list = field(default_factory=list)

    def find(self, tag):
        return [c for c in self.children if c.tag == tag]


@dataclass
class Frame:
    name: str
    width: int
    height: int
    hot_x: int
    hot_y: int
    flags: int
    key: int
    pixels: list  # width*height RGB555 values, row-major


def parse_chunks(data, start, end, nested=True):
    chunks = []
    off = start
    while off + 10 <= end:
        tag = data[off:off + 4]
        length, ident = struct.unpack_from("<IH", data, off + 4)
        body = data[off + 10:off + 10 + length]
        chunk = Chunk(tag, ident, off, body)
        # A FRAM chunk inside STAT is a 12-byte frame reference, not a container.
        if nested and tag in CONTAINERS and not (tag == b"FRAM" and length <= 12):
            chunk.children = parse_chunks(data, off + 10, off + 10 + length)
        chunks.append(chunk)
        off += 10 + length
    return chunks


def load(path):
    data = open(path, "rb").read()
    if data[:10] != MAGIC:
        raise ValueError(f"{path}: not a CHFILEANI file")
    length, _ = struct.unpack_from("<IH", data, 10)
    return parse_chunks(data, 16, min(len(data), 16 + length))


def unpack_rle16(src, out_bytes):
    """enctype 0x11: byte-controlled run-length coding of 16-bit units."""
    out = bytearray()
    i = 0
    while i < len(src) and len(out) < out_bytes:
        c = src[i]
        if c == 0xFF:
            break
        if c & 0x80:
            out += src[i + 1:i + 3] * ((c & 0x7F) + 1)
            i += 3
        else:
            n = (c + 1) * 2
            out += src[i + 1:i + 1 + n]
            i += 1 + n
    return bytes(out[:out_bytes])


def decode_cimg(body, name=""):
    kind, flags, off_data, _off2, w, h, hx, hy, key = struct.unpack_from("<HHIIHHHHI", body, 0)
    if kind & 7 != 4:
        raise ValueError(f"unsupported CIMG type {kind}")
    enctype, _pad, hdr_len, csize, usize = struct.unpack_from("<BBHII", body, off_data)
    payload = body[off_data + hdr_len:off_data + hdr_len + csize]
    if enctype == 0x00:
        raw = payload[:usize]
    elif enctype == 0x11:
        raw = unpack_rle16(payload, usize)
    else:
        raise ValueError(f"unsupported enctype 0x{enctype:02x}")
    count = w * h
    pixels = list(struct.unpack_from(f"<{count}H", raw.ljust(count * 2, b"\0")))
    return Frame(name, w, h, hx, hy, flags, key, pixels)


def frames(chunks):
    out = []
    for c in chunks:
        if c.tag != b"FRAM":
            continue
        fnam = c.find(b"FNAM")
        name = fnam[0].data.split(b"\0")[0].decode("latin1").strip() if fnam else ""
        cimg = c.find(b"CIMG")
        out.append(decode_cimg(cimg[0].data, name) if cimg else None)
    return out


def sequences(chunks):
    """Return [(name, [(frame_index, raw_stat_head, raw_frame_ref), ...]), ...]."""
    out = []
    for c in chunks:
        if c.tag != b"SEQ ":
            continue
        head = c.find(b"HEAD")
        name = head[0].data[:32].split(b"\0")[0].decode("latin1") if head else ""
        states = []
        for st in c.find(b"STAT"):
            sh = st.find(b"HEAD")
            fr = st.find(b"FRAM")
            ref = fr[0].data if fr else b""
            index = struct.unpack_from("<H", ref, 2)[0] if len(ref) >= 4 else -1
            states.append((index, sh[0].data if sh else b"", ref))
        out.append((name, states))
    return out
