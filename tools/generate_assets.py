Import("env")
import hashlib
import os
import sys

_TOOLS_DIR = os.path.join(env.subst("$PROJECT_DIR"), "tools")
if _TOOLS_DIR not in sys.path:
    sys.path.insert(0, _TOOLS_DIR)
import build_assets  # noqa: E402

project_dir = env.subst("$PROJECT_DIR")
esp32_dir = os.path.join(project_dir, "platforms", "esp32")
bin_path = os.path.join(esp32_dir, "assets.bin")
asm_path = os.path.join(esp32_dir, "assets_embedded.S")

# WG_NO_EMBED_FONT drops the reader font (~290 KB) from the blob for iterative
# dev uploads. The device keeps using whatever is already in the font partition,
# so a board must have been flashed once with a full build first.
skip_font = any("WG_NO_EMBED_FONT" in str(f) for f in env.get("CPPDEFINES", []) + env.get("BUILD_FLAGS", []))
assets = [a for a in build_assets.DEFAULT_ASSETS if not (skip_font and a[0] == build_assets.FONT_ASSET)]
if skip_font:
    print(f"[generate_assets] WG_NO_EMBED_FONT: omitting {build_assets.FONT_ASSET} from the blob")

build_assets.build(project_dir, bin_path, assets)
print(f"[generate_assets] assets.bin written ({os.path.getsize(bin_path):,} bytes)")

# Embed an MD5 of assets.bin as a comment so CMake detects content changes
# and recompiles the .S file whenever assets.bin is updated.
with open(bin_path, "rb") as bf:
    blob_hash = hashlib.md5(bf.read()).hexdigest()

bin_path_escaped = bin_path.replace("\\", "/")
asm = f"""\
    /* assets_md5: {blob_hash} */
    .section .rodata
    .global _binary_assets_bin_start
    .global _binary_assets_bin_end
_binary_assets_bin_start:
    .incbin \"{bin_path_escaped}\"
_binary_assets_bin_end:
"""
with open(asm_path, "w") as f:
    f.write(asm)
print(f"[generate_assets] assets_embedded.S written (blob md5={blob_hash})")
