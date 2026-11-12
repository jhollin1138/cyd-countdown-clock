# PlatformIO post-build step: copy firmware.bin to firmware/countdown-clock-<board>-v<version>.bin
# The version is read from FW_VERSION in src/app.h. This step never fails the build.
Import("env")  # noqa: F821  (provided by PlatformIO)
import os
import re
import shutil


def _version():
    try:
        with open(os.path.join(env.subst("$PROJECT_DIR"), "src", "app.h"), encoding="utf-8") as f:
            m = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', f.read())
        return m.group(1) if m else "dev"
    except Exception:
        return "dev"


def copy_versioned(source, target, env):
    try:
        src = target[0].get_abspath()
        out_dir = os.path.join(env.subst("$PROJECT_DIR"), "firmware")
        os.makedirs(out_dir, exist_ok=True)
        name = "countdown-clock-%s-v%s.bin" % (env.subst("$PIOENV"), _version())
        shutil.copyfile(src, os.path.join(out_dir, name))
        print("Firmware for wireless update: firmware/%s" % name)
    except Exception as e:  # never break the build over a convenience copy
        print("Note: couldn't make the versioned firmware copy (%s)" % e)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_versioned)  # noqa: F821
