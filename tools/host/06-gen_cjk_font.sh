#!/bin/bash
# Generate an LVGL C font with Chinese coverage, sized for this board.
#
# Coverage = GB2312 (6763 hanzi + its symbol/punctuation rows) + ASCII +
# CJK punctuation + fullwidth forms.  GB2312 keeps the font ~1/3 the size of
# the whole CJK Unified Ideographs block while covering everyday Chinese.
set -e

NODE_DIR="$HOME/opt/node-v22.14.0-linux-x64"
export PATH="$NODE_DIR/bin:$PATH"
CONV="$HOME/opt/fonttool/node_modules/.bin/lv_font_conv"

TTF="${TTF:-$HOME/openvela/vendor/openvela/boards/vela/resource/font/MiSans-Normal.ttf}"
SIZE="${SIZE:-18}"
BPP="${BPP:-2}"
OUT_DIR="$HOME/openvela/packages/ai_agent/src/ui/fonts"
OUT="$OUT_DIR/lv_font_misans_18_cjk.c"

mkdir -p "$OUT_DIR"

# Build the GB2312 character list (every decodable GB2312 code point).
SYMS_FILE="$HOME/gb2312_symbols.txt"
python3 - "$SYMS_FILE" <<'PY'
import sys

chars = []
for b1 in range(0xA1, 0xF8):
    for b2 in range(0xA1, 0xFF):
        try:
            chars.append(bytes([b1, b2]).decode("gb2312"))
        except UnicodeDecodeError:
            pass

text = "".join(dict.fromkeys(chars))  # dedupe, keep order
with open(sys.argv[1], "w", encoding="utf-8") as fh:
    fh.write(text)
print("[font] gb2312 chars: %d" % len(text))
PY

SYMS=$(cat "$SYMS_FILE")

echo "[font] ttf=$TTF size=$SIZE bpp=$BPP"
"$CONV" \
  --font "$TTF" \
  --size "$SIZE" \
  --bpp "$BPP" \
  --format lvgl \
  --range '0x20-0x7F' \
  --range '0x2000-0x206F' \
  --range '0x3000-0x303F' \
  --range '0xFF00-0xFFEF' \
  --symbols "$SYMS°±×÷←→↑↓•▲▼■□●○★☆" \
  --lv-include 'lvgl/lvgl.h' \
  --force-fast-kern-format \
  -o "$OUT"

echo "[font] generated:"
ls -la "$OUT"
grep -n 'const lv_font_t lv_font_' "$OUT" | head -3
