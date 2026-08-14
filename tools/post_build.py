import os
import shutil
from SCons.Script import DefaultEnvironment

env = DefaultEnvironment()

FIRMWARE_ID = "mimios-base"


def copy_bin(source, target, env):
    chip = env.GetProjectOption("custom_chip", "esp32")
    build_dir = env.subst("$BUILD_DIR")
    src = os.path.join(build_dir, env.subst("$PROGNAME") + ".bin")
    if not os.path.exists(src):
        print(f"[firmware] bin not found: {src}")
        return
    dest = os.path.join(build_dir, f"{FIRMWARE_ID}-{chip}.bin")
    shutil.copy(src, dest)
    size = os.path.getsize(dest)
    print(f"[firmware] {os.path.basename(dest)} ({size} bytes)")

    fs_src = os.path.join(build_dir, "littlefs.bin")
    fs_dest = os.path.join(build_dir, f"{FIRMWARE_ID}-{chip}-fs.bin")
    if os.path.exists(fs_src):
        shutil.copy(fs_src, fs_dest)
        fs_size = os.path.getsize(fs_dest)
        print(f"[firmware] {os.path.basename(fs_dest)} ({fs_size} bytes)")
    else:
        print("[firmware] littlefs.bin not found (run `pio run -t buildfs`)")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_bin)
