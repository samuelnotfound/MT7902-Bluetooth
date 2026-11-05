"""Bounds validation for the Linux btmtk 79xx firmware container."""
import struct

def validate_layout(data):
    if len(data) < 96:
        raise ValueError("truncated firmware header/global descriptor")
    count = struct.unpack_from('<I', data, 44)[0]
    if not 1 <= count <= 16 or 96 + count * 64 > len(data):
        raise ValueError("invalid/truncated section table")
    end = 96 + count * 64
    sections = []
    for i in range(count):
        base = 96 + i * 64
        offset = struct.unpack_from('<I', data, base + 4)[0]
        size = struct.unpack_from('<I', data, base + 16)[0]
        if size:
            if offset < end or offset > len(data) or size > len(data) - offset:
                raise ValueError("section outside payload")
            if any(s['size'] and offset < s['offset'] + s['size'] and s['offset'] < offset + size for s in sections):
                raise ValueError("overlapping sections")
        sections.append(dict(index=i, offset=offset, size=size))
    if not any(s['size'] for s in sections):
        raise ValueError("no downloadable sections")
    return sections
