#!/usr/bin/env python3
"""Compiler/linker wrapper that emulates a Haiku x86_64 toolchain with clang + Haiku headers.

Compiles are real (for CMake checks) or syntax-only (for the project); links just create the output."""
import os, subprocess, sys

HERE = os.path.dirname(os.path.realpath(__file__))
WORK = os.environ.get("HCHECK_WORK", os.path.join(HERE, "work"))
H = os.path.join(WORK, "haiku", "headers")          # Haiku system headers (haiku/haiku repo)
S = os.path.join(WORK, "sysroot")                   # dependency headers + stub libraries
LOG = os.path.join(WORK, "errors.log")              # every failing/warning compile is appended here
CLANG_RES = os.environ.get("HCHECK_CLANG_RESOURCE_DIR",
                           subprocess.run(["clang", "-print-resource-dir"], capture_output=True,
                                          text=True).stdout.strip())
LIBSTDCXX = os.environ.get("HCHECK_LIBSTDCXX", "/usr/include/c++/13")
LIBSTDCXX_ARCH = os.environ.get("HCHECK_LIBSTDCXX_ARCH", "/usr/include/x86_64-linux-gnu/c++/13")

def haiku_includes(cxx):
    inc = ["-isystem", CLANG_RES + "/include"]
    if cxx:
        # Haiku's libstdc++ isn't available here; the host's works once a few glibc-isms are papered over.
        inc = ["-nostdinc++", "-isystem", LIBSTDCXX, "-isystem", LIBSTDCXX_ARCH,
               "-isystem", os.path.join(HERE, "shim"), "-D__GLIBC_PREREQ(a,b)=0", "-D__locale_t=locale_t"] + inc
    # Same order as INCLUDE_DEFAULTS in Haiku's GCC (buildtools gcc/gcc/config/haiku.h); the installed
    # /boot/system/develop/headers/{bsd,gnu} come from headers/compatibility/ in the source tree. The order
    # matters: headers/bsd/features.h must win over headers/posix/features.h to enable _DEFAULT_SOURCE.
    osdirs = ["", "app", "device", "drivers", "game", "interface", "kernel", "locale", "mail", "media", "midi",
              "midi2", "net", "opengl", "storage", "support", "translation", "add-ons/graphics",
              "add-ons/input_server", "add-ons/mail_daemon", "add-ons/registrar", "add-ons/screen_saver",
              "add-ons/tracker", "be_apps/Deskbar", "be_apps/NetPositive", "be_apps/Tracker"]
    for d in osdirs:
        p = H + "/os" + ("/" + d if d else "")
        if os.path.isdir(p):
            inc += ["-isystem", p]
    for d in [S + "/boot/system/develop/headers/3rdparty", H + "/compatibility/bsd", H + "/glibc",
              H + "/compatibility/gnu", H + "/posix", H]:
        inc += ["-isystem", d]
    inc += ["-isystem", S + "/boot/system/develop/headers"]
    return inc

def main():
    cxx = os.path.basename(sys.argv[0]).endswith("++")
    args = sys.argv[1:]
    out = None
    if "-o" in args:
        out = args[args.index("-o") + 1]
    compiling = "-c" in args or "-E" in args or "-S" in args
    if not compiling and "--version" not in args and "-v" not in args:
        # link step: no Haiku libraries here, so produce a placeholder. For a single real object (CMake's
        # compiler/ABI probes), copy it so CMake can read the INFO strings it embeds.
        if out:
            os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
            objs = [a for a in args if a.endswith((".o", ".obj")) and os.path.isfile(a) and os.path.getsize(a) > 0]
            if len(objs) == 1:
                import shutil
                shutil.copyfile(objs[0], out)
            else:
                open(out, "wb").close()
        return 0

    base = ["clang++" if cxx else "clang", "--target=x86_64-unknown-haiku", "-nostdinc"] + haiku_includes(cxx)
    where = os.getcwd() + "/" + (out or "")
    config_check = any(k in where for k in ("CMakeScratch", "CompilerId", "CMakeTmp", "/CMakeFiles/3."))
    if config_check or out is None or "-E" in args:
        return subprocess.call(base + args)

    # syntax-only compile of a project source
    filtered = []
    skip = False
    depfile = None
    for i, a in enumerate(args):
        if skip:
            skip = False
            continue
        if a == "-o":
            skip = True
            continue
        if a == "-MF":
            depfile = args[i + 1]
        filtered.append(a)
    r = subprocess.run(base + filtered + ["-fsyntax-only"], capture_output=True, text=True)
    if r.returncode != 0 or "warning:" in r.stderr:
        with open(LOG, "a") as f:
            f.write("=== %s (exit %d)\n%s\n" % (out, r.returncode, r.stderr))
    sys.stderr.write(r.stderr)
    if r.returncode == 0:
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        open(out, "wb").close()
        if depfile and not os.path.exists(depfile):
            with open(depfile, "w") as f:
                f.write("%s:\n" % out)
    return r.returncode

sys.exit(main())
