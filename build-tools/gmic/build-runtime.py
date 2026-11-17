#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Solstice contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the pinned CPU G'MIC runtime after installing kritaqmicinterface."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

HERE = Path(__file__).resolve().parent


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


def prepare(archive, work, lock):
    if hashlib.sha256(archive.read_bytes()).hexdigest() != lock["sha256"]:
        raise ValueError("G'MIC source checksum mismatch")
    source = work / lock["directory"]
    if not source.exists():
        with tarfile.open(archive) as tar:
            # Reject links, devices and paths outside the single expected root.
            for member in tar.getmembers():
                target = (work / member.name).resolve()
                if not target.is_relative_to(source.resolve()) or not (member.isfile() or member.isdir()):
                    raise ValueError(f"Unsafe archive member: {member.name}")
            tar.extractall(work, filter="data")
    patches = json.loads((HERE / "runtime-patches.json").read_text(encoding="utf-8"))
    for patch in patches:
        path = source / patch["file"]
        text = path.read_text(encoding="utf-8")
        old, new = patch["before"], patch["after"]
        if new in text:
            continue
        if text.count(old) != 1:
            raise ValueError(f"Patch context mismatch: {path}")
        path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--install", required=True, type=Path)
    parser.add_argument("--deps", required=True, type=Path)
    parser.add_argument("--compiler-bin", required=True, type=Path)
    parser.add_argument("--archive", type=Path, help="Verified local archive for offline builds")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    lock = json.loads((HERE / "runtime.lock.json").read_text())
    args.work = args.work.resolve()
    args.work.mkdir(parents=True, exist_ok=True)
    archive = args.archive or args.work / f'gmic-{lock["version"]}.tar.gz'
    if not archive.exists():
        urllib.request.urlretrieve(lock["url"], archive)
    source = prepare(archive, args.work, lock)
    build = args.work / "build"
    os.environ["PATH"] = os.pathsep.join([str(args.install / "bin"), str(args.deps / "bin"),
                                         str(args.compiler_bin), os.environ["PATH"]])
    run("cmake", "-S", source / "gmic-qt", "-B", build, "-G", "Ninja",
        f"-DCMAKE_C_COMPILER={args.compiler_bin / 'clang.exe'}",
        f"-DCMAKE_CXX_COMPILER={args.compiler_bin / 'clang++.exe'}",
        f"-DCMAKE_PREFIX_PATH={args.install};{args.deps}",
        f"-DCMAKE_INSTALL_PREFIX={args.install}", "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_WITH_QT6=ON", "-DGMIC_QT_HOST=krita-plugin", "-DENABLE_SYSTEM_GMIC=OFF",
        "-DENABLE_DYNAMIC_LINKING=OFF", "-DENABLE_CURL=OFF", "-DENABLE_FFTW3=ON",
        "-DENABLE_LTO=OFF", "-DKDE_INSTALL_USE_QT_SYS_PATHS=ON")
    cache = (build / "CMakeCache.txt").read_text()
    if "OpenMP_CXX_FLAGS:STRING=-fopenmp" not in cache:
        raise RuntimeError("CPU OpenMP support is required")
    run("cmake", "--build", build, "--parallel", max(1, args.jobs))
    run("cmake", "--install", build)
    # Ship the precise upstream source and Solstice modifications together.
    provenance = args.install / "share/gmic/source"
    provenance.mkdir(parents=True, exist_ok=True)
    shutil.copy2(archive, provenance / f'gmic-{lock["version"]}.tar.gz')
    for name in ("runtime.lock.json", "runtime-patches.json", "build-runtime.py", "README.md"):
        shutil.copy2(HERE / name, provenance / name)
    for name in lock["resources"]:
        if not (args.install / "share/gmic" / name).is_file():
            raise RuntimeError(f"Missing G'MIC resource: {name}")
    if not (args.install / "lib/kritaplugins/krita_gmic_qt.dll").is_file():
        raise RuntimeError("Missing G'MIC runtime plugin")


if __name__ == "__main__":
    main()
