"""Read resource-fork bytes without Resource Manager or host byte-order assumptions."""
from dataclasses import dataclass
import struct


def region(data, offset, length):
    if offset < 0 or length < 0 or offset > len(data) - length:
        raise ValueError("resource fork range outside its container")
    return data[offset:offset + length]


def unpack(fmt, data, offset):
    return struct.unpack(fmt, region(data, offset, struct.calcsize(fmt)))


@dataclass(frozen=True)
class Resource:
    kind: bytes
    id: int
    name: str | None
    attributes: int
    data: bytes


def read_resources(data):
    data_offset, map_offset, data_length, map_length = unpack(">4I", data, 0)
    if min(data_offset, map_offset) < 16:
        raise ValueError("resource section overlaps header")
    if max(data_offset, map_offset) < min(data_offset + data_length, map_offset + map_length):
        raise ValueError("overlapping resource data and map")
    payload = region(data, data_offset, data_length)
    mapping = region(data, map_offset, map_length)
    type_offset, name_offset = unpack(">HH", mapping, 24)
    if type_offset < 28 or name_offset < type_offset + 2:
        raise ValueError("invalid resource map offsets")
    types = region(mapping, type_offset, name_offset - type_offset)
    names = region(mapping, name_offset, len(mapping) - name_offset)
    count_minus_one, = unpack(">H", types, 0)
    type_count = 0 if count_minus_one == 0xffff else count_minus_one + 1
    region(types, 2, type_count * 8)
    result, seen, seen_types = [], set(), set()
    for index in range(type_count):
        kind, count_minus_one, refs = unpack(">4sHH", types, 2 + index * 8)
        if kind in seen_types:
            raise ValueError("duplicate resource type")
        seen_types.add(kind)
        count = 0 if count_minus_one == 0xffff else count_minus_one + 1
        if refs < 2 + type_count * 8:
            raise ValueError("resource references overlap type table")
        region(types, refs, count * 12)
        for entry in range(count):
            rid, name, packed, _handle = unpack(">hHII", types, refs + entry * 12)
            if (kind, rid) in seen:
                raise ValueError("duplicate resource ID")
            seen.add((kind, rid))
            offset = packed & 0xffffff
            size, = unpack(">I", payload, offset)
            body = region(payload, offset + 4, size)
            label = None
            if name != 0xffff:
                length = region(names, name, 1)[0]
                label = region(names, name + 1, length).decode("mac_roman")
            result.append(Resource(kind, rid, label, packed >> 24, body))
    return sorted(result, key=lambda item: (item.kind, item.id))
