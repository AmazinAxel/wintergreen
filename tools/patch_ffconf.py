"""
Pre-build script: patch ffconf.h in the PlatformIO ESP-IDF FatFs component
to enable exFAT support (FF_FS_EXFAT = 1).

Runs before every build. The patch is idempotent — if already applied it does
nothing. Survives PlatformIO cache clears but would need to re-run after a
full framework reinstall (which is rare and would be caught by the next build).
"""

import os

try:
    Import("env")  # noqa: F821  (PlatformIO SCons env)
except NameError:
    pass  # allow running standalone for manual patching

IDF_PACKAGES = os.path.join(
    os.path.expanduser("~"), ".platformio", "packages", "framework-espidf"
)
FFCONF = os.path.join(IDF_PACKAGES, "components", "fatfs", "src", "ffconf.h")

NEEDLE = "#define FF_FS_EXFAT\t\t0"
PATCHED = "#define FF_FS_EXFAT\t\t1  /* patched by tools/patch_ffconf.py */"

if not os.path.isfile(FFCONF):
    print(f"patch_ffconf: WARNING — ffconf.h not found at {FFCONF}, skipping.")
else:
    text = open(FFCONF, "r", encoding="utf-8").read()
    if NEEDLE in text:
        patched = text.replace(NEEDLE, PATCHED, 1)
        open(FFCONF, "w", encoding="utf-8").write(patched)
        print("patch_ffconf: enabled FF_FS_EXFAT = 1 in ffconf.h")
    elif PATCHED in text:
        print("patch_ffconf: FF_FS_EXFAT already enabled, nothing to do.")
    else:
        print(
            "patch_ffconf: WARNING — expected pattern not found in ffconf.h, "
            "check if the ESP-IDF version changed the define format."
        )
