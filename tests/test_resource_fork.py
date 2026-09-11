"""Synthetic fixtures exercise signed IDs, byte order, and corrupt boundaries."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from resource_fork import read_resources


def fixture():
    data = struct.pack(">I", 3) + b"abc"
    types = struct.pack(">H4sHHhHII", 0, b"TEST", 0, 10, -7, 0, 0x04000000, 0)
    mapping = bytearray(28) + types + b"\x04Caf\x8e"
    struct.pack_into(">HH", mapping, 24, 28, 28 + len(types))
    header = struct.pack(">4I", 256, 256 + len(data), len(data), len(mapping))
    mapping[:16] = header
    return bytearray(header + bytes(240) + data + mapping)


class ResourceForkTests(unittest.TestCase):
    def test_signed_id_name_attributes_and_payload(self):
        resource, = read_resources(fixture())
        self.assertEqual((resource.kind, resource.id, resource.name, resource.attributes,
                          resource.data), (b"TEST", -7, "Café", 4, b"abc"))

    def test_all_truncations_rejected(self):
        data = fixture()
        for length in range(len(data)):
            with self.subTest(length=length), self.assertRaises(ValueError):
                read_resources(data[:length])

    def test_corrupt_offsets_and_lengths(self):
        map_offset = 263
        mutations = [
            (0, ">I", 0), (4, ">I", 256), (8, ">I", 0xffffffff),
            (256, ">I", 0xffffffff), (map_offset + 24, ">H", 0),
            (map_offset + 26, ">H", 65535),
            (map_offset + 28, ">H", 65534),
            (map_offset + 36, ">H", 0),
            (map_offset + 40, ">H", 65534),
            (map_offset + 42, ">I", 0x00ffffff),
        ]
        for offset, fmt, value in mutations:
            data = fixture()
            struct.pack_into(fmt, data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                read_resources(data)


if __name__ == "__main__":
    unittest.main()
