#!/usr/bin/env python3
"""Export original assets to deterministic, inspectable Linux build artifacts."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib

from PIL import Image
from resource_fork import read_resources


def digest(data):
    return hashlib.sha256(data).hexdigest()


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode()


def export(source, output):
    source, output = Path(source), Path(output)
    # The manifest is the completion marker, never leave one after a failed export.
    (output / "manifest.json").unlink(missing_ok=True)
    manifest = {"version": 1, "resources": [], "strings": [], "images": [], "files": []}

    def write(relative, data):
        path = output / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return {"path": relative, "size": len(data), "sha256": digest(data)}

    fork = source / "Resources/English.lproj/MainResources.rsrc"
    manifest["resource_fork_sha256"] = digest(fork.read_bytes())
    for item in read_resources(fork.read_bytes()):
        # Hex type directories preserve every FourCC without trusting it as a path.
        record = write(f"resources/{item.kind.hex()}/{item.id}.bin", item.data)
        record.update(type=item.kind.decode("mac_roman"), type_hex=item.kind.hex(),
                      id=item.id, name=item.name, attributes=item.attributes)
        manifest["resources"].append(record)
    for path in sorted((source / "Resources/English.lproj/Strings").glob("*.plist")):
        values = plistlib.loads(path.read_bytes())
        if not isinstance(values, list) or not all(isinstance(value, str) for value in values):
            raise ValueError(f"expected a string array: {path}")
        record = write(f"strings/{path.stem}.json", json_bytes(values))
        record.update(name=path.stem, count=len(values), source_sha256=digest(path.read_bytes()))
        manifest["strings"].append(record)
    for root in ("Resources/Graphics", "Images", "Cursors"):
        for path in sorted((source / root).iterdir()):
            if path.suffix.lower() not in (".png", ".gif", ".jpg", ".jpeg"):
                continue
            with Image.open(path) as image:
                image.seek(0)
                # Preserve transparency for cursors and masks. The renderer can
                # composite into U3Bitmap's opaque RGBX8 surfaces at load time.
                pixels = image.convert("RGBA").tobytes()
                relative = path.relative_to(source).as_posix()
                record = write(f"images/{relative}.rgba", pixels)
                record.update(source=relative, source_sha256=digest(path.read_bytes()),
                              width=image.width, height=image.height, stride=image.width * 4,
                              format="RGBA8", frames=getattr(image, "n_frames", 1))
                manifest["images"].append(record)
    # Preserve modern audio, its license notices, and supplementary documents.
    for root in ("Resources/SoundsPCM", "Resources/MusicMIDI", "Images"):
        for path in sorted((source / root).iterdir()):
            if path.suffix.lower() not in (".wav", ".mid", ".sf2", ".txt", ".pdf"):
                continue
            if path.name == "FluidR3_GM.sf2":
                continue  # Optional local download must not change the baseline bundle.
            manifest["files"].append(write(path.relative_to(source).as_posix(), path.read_bytes()))
    # One effect only exists in the original sound directory.
    path = source / "Resources/Sounds/ExpLevelUp.mp3"
    manifest["files"].append(write(path.relative_to(source).as_posix(), path.read_bytes()))
    manifest["files"].append(write("LICENSE", (source / "LICENSE").read_bytes()))
    manifest["files"].append(write("README.md", (source / "README.md").read_bytes()))
    temporary = output / "manifest.json.tmp"
    temporary.write_bytes(json_bytes(manifest))
    temporary.replace(output / "manifest.json")
    print("Exported " + ", ".join(f"{len(manifest[k])} {k}" for k in
                                  ("resources", "strings", "images", "files")))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    export(args.source, args.output)
