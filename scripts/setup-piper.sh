#!/usr/bin/env bash
set -euo pipefail

DEST_DIR="$(cd "$(dirname "$0")/.." && pwd)/tools/piper"
mkdir -p "$DEST_DIR"

echo "=== Ultima III: Setting up Piper Neural TTS ==="
echo "Target directory: $DEST_DIR"

# 1. Download Piper standalone binary for Linux x86-64
if [ ! -f "$DEST_DIR/piper" ]; then
    echo "Downloading Piper standalone engine (v1.2.0)..."
    curl -L -s https://github.com/rhasspy/piper/releases/download/2023.11.14-2/piper_linux_x86_64.tar.gz | tar -xz -C "$DEST_DIR" --strip-components=1
fi

# 2. Download Alan voice (British Male - ideal for Lord British & narrator)
if [ ! -f "$DEST_DIR/en_GB-alan-medium.onnx" ]; then
    echo "Downloading en_GB-alan-medium neural voice..."
    curl -L -s -o "$DEST_DIR/en_GB-alan-medium.onnx" https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/en/en_GB/alan/medium/en_GB-alan-medium.onnx
    curl -L -s -o "$DEST_DIR/en_GB-alan-medium.onnx.json" https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/en/en_GB/alan/medium/en_GB-alan-medium.onnx.json
fi

# 3. Download Alba voice (British Female - ideal for female NPCs & companions)
if [ ! -f "$DEST_DIR/en_GB-alba-medium.onnx" ]; then
    echo "Downloading en_GB-alba-medium neural voice..."
    curl -L -s -o "$DEST_DIR/en_GB-alba-medium.onnx" https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/en/en_GB/alba/medium/en_GB-alba-medium.onnx
    curl -L -s -o "$DEST_DIR/en_GB-alba-medium.onnx.json" https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/en/en_GB/alba/medium/en_GB-alba-medium.onnx.json
fi

chmod +x "$DEST_DIR/piper"
echo "=== Piper Neural TTS ready at: $DEST_DIR/piper ==="
