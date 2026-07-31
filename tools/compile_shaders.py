#!/usr/bin/env python3
"""Compiles the project's GLSL to SPIR-V, into the folder Gradle packages.

There is no shader compilation in the app. Every module is built here, ahead of
time, and loaded from the APK as finished SPIR-V -- which is why this script has
to run before a build that changed a shader.

    assets/shaders/*.vert|frag|comp   ->   app/src/main/assets/shaders/*.spv

Built for RenderDoc, deliberately: -g and -O0. See BUILD_FLAGS below for what
each one buys and what it costs.

Usage:
    python tools/compile_shaders.py            # only what changed
    python tools/compile_shaders.py --force    # everything
    python tools/compile_shaders.py --clean    # delete the output and stop
    python tools/compile_shaders.py --release  # optimised, no debug info
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SOURCE_DIR = REPO_ROOT / "assets" / "shaders"
OUTPUT_DIR = REPO_ROOT / "app" / "src" / "main" / "assets" / "shaders"
# Depfiles go under build/ rather than next to the .spv, because everything in
# the assets folder is packaged into the APK -- glslc's default of writing them
# beside the output was quietly shipping them to the device.
DEPS_DIR = REPO_ROOT / "app" / "build" / "shader-deps"

# Extensions glslc infers a stage from. Anything else in the source folder is
# treated as an include and is never compiled on its own.
SHADER_SUFFIXES = {".vert", ".frag", ".comp", ".geom", ".tesc", ".tese"}

BUILD_FLAGS = [
    # Must match the device the renderer asks for. Without it glslc targets
    # SPIR-V 1.0, which silently rejects anything from a later version rather
    # than telling you the target is the problem.
    "--target-env=vulkan1.3",
]

DEBUG_FLAGS = [
    # Embeds the GLSL source (OpSource), the line mapping (OpLine) and the
    # original identifiers (OpName). This is what makes RenderDoc show this file
    # instead of decompiled SPIR-V, and what lets its debugger step through it.
    "-g",
    # Optimisation would fold and rename exactly what -g just recorded, so the
    # line mapping stops pointing anywhere useful. The two flags belong together.
    "-O0",
]

RELEASE_FLAGS = ["-O"]


def pinned_ndk_version() -> str | None:
    """The ndkVersion app/build.gradle.kts pins, if it pins one."""
    gradle = REPO_ROOT / "app" / "build.gradle.kts"
    if not gradle.is_file():
        return None
    match = re.search(r'ndkVersion\s*=\s*"([^"]+)"', gradle.read_text(encoding="utf-8"))
    return match.group(1) if match else None


def sdk_roots() -> list[Path]:
    roots = []
    for variable in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        value = os.environ.get(variable)
        if value:
            roots.append(Path(value))
    # Default install locations, so this works on a fresh clone with nothing set.
    roots.append(Path.home() / "AppData" / "Local" / "Android" / "Sdk")
    roots.append(Path.home() / "Android" / "Sdk")
    roots.append(Path.home() / "Library" / "Android" / "sdk")
    return roots


def glslc_in(ndk_root: Path) -> Path | None:
    for found in sorted(ndk_root.glob("shader-tools/*/glslc*")):
        if found.is_file() and os.access(found, os.X_OK):
            return found
    return None


def version_key(name: str) -> tuple:
    """Numeric ordering for an NDK folder name, so 9.x does not outrank 10.x."""
    return tuple(int(part) if part.isdigit() else -1 for part in name.split("."))


def find_glslc() -> Path:
    """glslc, from the NDK this project actually builds with.

    The order matters and the top of it is not the obvious one. ANDROID_NDK_HOME
    is a machine-wide setting that drifts: it is routinely left pointing at
    whatever NDK was installed first, which on this project's own machine was
    three major versions behind the pinned one. Shaders built by a compiler years
    apart from the toolchain that builds the renderer is a difference nobody goes
    looking for, so the version app/build.gradle.kts pins wins over the
    environment. An explicit GLSLC still wins over everything, because someone
    setting that means it.
    """
    override = os.environ.get("GLSLC")
    if override:
        candidate = Path(override)
        if candidate.is_file():
            return candidate
        sys.exit(f"GLSLC is set to '{override}', which is not a file")

    pinned = pinned_ndk_version()
    if pinned:
        for sdk_root in sdk_roots():
            found = glslc_in(sdk_root / "ndk" / pinned)
            if found:
                return found
        print(f"warning: build.gradle.kts pins NDK {pinned} but it is not installed; "
              f"falling back to whatever is", file=sys.stderr)

    for variable in ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT"):
        value = os.environ.get(variable)
        if value:
            found = glslc_in(Path(value))
            if found:
                return found

    installed: list[Path] = []
    for sdk_root in sdk_roots():
        ndk_dir = sdk_root / "ndk"
        if ndk_dir.is_dir():
            installed.extend(ndk_dir.iterdir())
    for ndk_root in sorted(installed, key=lambda p: version_key(p.name), reverse=True):
        found = glslc_in(ndk_root)
        if found:
            return found

    from_path = shutil.which("glslc")
    if from_path:
        return Path(from_path)

    sys.exit(
        "glslc not found. It ships with the Android NDK under shader-tools/.\n"
        "Set GLSLC to its full path, or ANDROID_NDK_HOME to an NDK that has it."
    )


def dependencies_of(depfile: Path) -> list[Path]:
    """The sources named in a glslc -MD depfile, includes and all.

    Without this an edit to an included .glsl would not rebuild the shaders that
    include it -- the kind of staleness that costs an hour because the symptom is
    a shader that behaves like the version before the fix.
    """
    if not depfile.is_file():
        return []
    text = depfile.read_text(encoding="utf-8").replace("\\\n", " ")
    _, _, right = text.partition(":")
    return [Path(token) for token in right.split() if token]


def needs_rebuild(source: Path, output: Path, depfile: Path) -> bool:
    if not output.is_file():
        return True
    output_time = output.stat().st_mtime
    for dependency in [source, *dependencies_of(depfile)]:
        if dependency.is_file() and dependency.stat().st_mtime > output_time:
            return True
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--force", action="store_true", help="rebuild everything")
    parser.add_argument("--clean", action="store_true", help="delete the output and stop")
    parser.add_argument("--release", action="store_true",
                        help="optimise, drop debug info (RenderDoc will show decompiled SPIR-V)")
    parser.add_argument("--verbose", action="store_true", help="print each command")
    args = parser.parse_args()

    if args.clean:
        removed = False
        for directory in (OUTPUT_DIR, DEPS_DIR):
            if directory.is_dir():
                shutil.rmtree(directory)
                print(f"removed {directory}")
                removed = True
        if not removed:
            print("nothing to clean")
        return 0

    if not SOURCE_DIR.is_dir():
        sys.exit(f"no shader sources at {SOURCE_DIR}")

    glslc = find_glslc()
    flags = BUILD_FLAGS + (RELEASE_FLAGS if args.release else DEBUG_FLAGS)
    print(f"glslc: {glslc}")
    print(f"mode:  {'release' if args.release else 'debug (RenderDoc-friendly)'}")

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    sources = sorted(p for p in SOURCE_DIR.rglob("*") if p.suffix in SHADER_SUFFIXES)
    if not sources:
        print(f"no shaders in {SOURCE_DIR}")
        return 0

    compiled = 0
    skipped = 0
    failed: list[str] = []

    for source in sources:
        relative = source.relative_to(SOURCE_DIR)
        # "mesh_flat.vert" -> "mesh_flat.vert.spv": the stage stays in the name,
        # so a vertex and a fragment shader of the same material cannot collide
        # and the runtime path reads as what it is.
        output = OUTPUT_DIR / relative.parent / (relative.name + ".spv")
        depfile = DEPS_DIR / relative.parent / (relative.name + ".d")
        output.parent.mkdir(parents=True, exist_ok=True)
        depfile.parent.mkdir(parents=True, exist_ok=True)

        if not args.force and not needs_rebuild(source, output, depfile):
            skipped += 1
            continue

        command = [
            str(glslc), *flags,
            # So an #include resolves against the shader folder rather than the
            # working directory this happened to be run from.
            "-I", str(SOURCE_DIR),
            "-MD", "-MF", str(depfile),
            "-o", str(output),
            str(source),
        ]
        if args.verbose:
            print("  " + " ".join(command))

        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0:
            failed.append(str(relative))
            print(f"FAILED {relative}", file=sys.stderr)
            print(result.stderr.rstrip(), file=sys.stderr)
            continue
        if result.stderr.strip():
            # glslc reports warnings here and still succeeds. Worth seeing: a
            # warning in a shader is usually a layout mismatch waiting to happen.
            print(f"  {relative}: {result.stderr.strip()}")

        compiled += 1
        print(f"  {relative} -> {output.relative_to(REPO_ROOT)} "
              f"({output.stat().st_size} bytes)")

    print(f"\n{compiled} compiled, {skipped} up to date, {len(failed)} failed")
    if failed:
        # Non-zero so a build wired to this stops instead of packaging the last
        # good .spv and behaving like the edit never happened.
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
