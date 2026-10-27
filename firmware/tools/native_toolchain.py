# PlatformIO pre-script for the native env: on Windows, put PlatformIO's MinGW gcc on PATH.
# The package is pinned in platformio.ini (platform_packages). Other hosts use their own compiler.
import os
import sys

Import("env")  # noqa: F821  (provided by PlatformIO/SCons)

if sys.platform.startswith("win"):
    platform = env.PioPlatform()  # noqa: F821
    package_dir = platform.get_package_dir("toolchain-gccmingw32")
    if package_dir and os.path.isdir(os.path.join(package_dir, "bin")):
        env.PrependENVPath("PATH", os.path.join(package_dir, "bin"))  # noqa: F821
        # Link the runtime statically: otherwise the 32-bit exe may load a 64-bit
        # libstdc++/libgcc DLL found on PATH (e.g. Git's mingw64) and fail with 0xC000007B.
        env.Append(LINKFLAGS=["-static", "-static-libgcc", "-static-libstdc++"])  # noqa: F821
    else:
        print("native_toolchain.py: toolchain-gccmingw32 not installed; using PATH gcc")
