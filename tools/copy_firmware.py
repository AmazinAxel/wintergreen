Import("env")
import os
import shutil


def copy_firmware(source, target, env):
    src = str(target[0])
    dst = os.path.join(env.get("PROJECT_DIR"), "firmware.bin")
    shutil.copy(src, dst)
    print(f"[copy_firmware] firmware.bin -> {dst}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_firmware)
