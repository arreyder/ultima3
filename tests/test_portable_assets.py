"""Verify the built bundle against original data, including representative assets."""
import hashlib
import json
from pathlib import Path
import plistlib
import sys
import unittest

from PIL import Image

SOURCE, BUNDLE = map(Path, sys.argv[1:3])
sys.argv[1:] = []
sys.path.insert(0, str(SOURCE / "scripts"))
from resource_fork import read_resources


class PortableAssetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = json.loads((BUNDLE / "manifest.json").read_text())

    def test_all_output_checksums(self):
        for category in ("resources", "strings", "images", "files"):
            for record in self.manifest[category]:
                with self.subTest(path=record["path"]):
                    data = (BUNDLE / record["path"]).read_bytes()
                    self.assertEqual(len(data), record["size"])
                    self.assertEqual(hashlib.sha256(data).hexdigest(), record["sha256"])

    def test_resource_bytes_and_world_map(self):
        original = read_resources((SOURCE / "Resources/English.lproj/MainResources.rsrc").read_bytes())
        self.assertEqual(len(original), len(self.manifest["resources"]))
        for resource, record in zip(original, self.manifest["resources"]):
            self.assertEqual((resource.kind.hex(), resource.id), (record["type_hex"], record["id"]))
            self.assertEqual(resource.data, (BUNDLE / record["path"]).read_bytes())
        # ResetSosaria copies template 420 into mutable save resource 419.
        world = next(r for r in original if r.kind == b"MAPS" and r.id == 420)
        self.assertEqual(world.data[0], 64)
        self.assertEqual(len(world.data), 1 + 64 * 64 + 4)
        self.assertGreater(len(set(world.data[1:4097])), 1)

    def test_all_string_tables(self):
        for record in self.manifest["strings"]:
            original = plistlib.loads((SOURCE / "Resources/English.lproj/Strings" /
                                      (record["name"] + ".plist")).read_bytes())
            restored = json.loads((BUNDLE / record["path"]).read_text())
            self.assertEqual(restored, original)
            self.assertEqual(len(restored), record["count"])

    def test_tile_sheet_and_font_pixels(self):
        for name in ("Standard-Tiles.png", "Standard-Font.gif"):
            relative = "Resources/Graphics/" + name
            record = next(r for r in self.manifest["images"] if r["source"] == relative)
            with Image.open(SOURCE / relative) as original:
                restored = Image.frombytes("RGBA", (record["width"], record["height"]),
                                           (BUNDLE / record["path"]).read_bytes())
                expected = original.convert("RGBA")
                self.assertEqual(restored.tobytes(), expected.tobytes())
                self.assertEqual(record["stride"], original.width * 4)


if __name__ == "__main__":
    unittest.main()
