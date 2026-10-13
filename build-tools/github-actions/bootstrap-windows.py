#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Solstice contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch pinned public Windows dependencies into an isolated CI directory."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import time
import urllib.request
import zipfile


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def download_from(url, sha256, destination):
    for attempt in range(3):
        try:
            request = urllib.request.Request(url, headers={
                'User-Agent': 'Solstice-Windows-CI', 'Accept-Encoding': 'identity'})
            with urllib.request.urlopen(request, timeout=180) as response:
                with destination.open('wb') as output:
                    shutil.copyfileobj(response, output, 1024 * 1024)
            if digest(destination) != sha256:
                raise ValueError('SHA-256 mismatch: ' + url)
            return
        except Exception:
            destination.unlink(missing_ok=True)
            if attempt == 2:
                raise
            time.sleep(5 * (attempt + 1))


def download(item, destination):
    # The Solstice mirror first (the KDE registry removes old builds), then
    # the original; both must match the locked SHA-256
    # (docs/agent/github-actions.md, "Dependency mirror")
    sources = [url for url in (item.get('mirrorUrl'), item['url']) if url]
    for index, url in enumerate(sources):
        try:
            download_from(url, item['sha256'], destination)
            return
        except Exception as error:
            if index == len(sources) - 1:
                raise
            print(f'  {url} unavailable ({error}); trying {sources[index + 1]}', flush=True)


def extract_tar(archive, destination):
    with tarfile.open(archive) as contents:
        # data_filter refuses traversal and links outside the dependency root.
        # Debug symbols are unnecessary for this Release-only trial build.
        members = [entry for entry in contents if '.debug' not in Path(entry.name).parts]
        contents.extractall(destination, members=members, filter='data')


def extract_zip(archive, destination):
    with zipfile.ZipFile(archive) as contents:
        for name in contents.namelist():
            if not (destination / name).resolve().is_relative_to(destination.resolve()):
                raise ValueError('ZIP path escapes destination: ' + name)
        contents.extractall(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    lock_path = Path(__file__).with_name('windows-dependencies.lock.json')
    lock = json.loads(lock_path.read_text())
    lock_hash = digest(lock_path)
    marker = root / 'dependencies-ready.json'
    if marker.exists() and json.loads(marker.read_text())['lock_sha256'] == lock_hash:
        print('Restored matching dependency cache', flush=True)
        return
    if (root / 'deps').exists():
        raise RuntimeError('Incomplete/different dependency set; use a fresh CI root')
    free = shutil.disk_usage(root).free
    print(f'Initial free disk space: {free / 2**30:.1f} GiB', flush=True)
    if free < 7 * 2**30:
        raise RuntimeError('At least 7 GiB free is required before bootstrapping')
    deps = root / 'deps'
    deps.mkdir()
    for index, package in enumerate(lock['packages'], 1):
        print(f'[{index}/{len(lock["packages"])}] {package["name"]} {package["version"]}', flush=True)
        archive = root / 'dependency.tar'
        download(package, archive)
        extract_tar(archive, deps)
        archive.unlink()
    archive = root / 'toolchain.zip'
    print('Installing pinned LLVM-MinGW toolchain', flush=True)
    download(lock['toolchain'], archive)
    extract_zip(archive, root)
    archive.unlink()
    installer = root / 'vulkan-installer.exe'
    print('Installing pinned Vulkan SDK (copy only)', flush=True)
    download(lock['vulkan'], installer)
    subprocess.run([str(installer), '--root', str(root / 'vulkan'),
                    '--accept-licenses', '--default-answer', '--confirm-command',
                    'install', 'copy_only=1'], check=True, timeout=900)
    installer.unlink()
    for required in ('deps/bin/Qt6Core.dll', 'vulkan/Bin/glslc.exe',
                     'llvm-mingw-20251118-ucrt-x86_64/bin/clang++.exe'):
        if not (root / required).is_file():
            raise RuntimeError('Missing dependency: ' + required)
    # Write the cache marker only after every checksum and installation succeeds.
    marker.write_text(json.dumps({'lock_sha256': lock_hash}, indent=2) + '\n')
    print(f'Dependencies ready; free disk: {shutil.disk_usage(root).free / 2**30:.1f} GiB', flush=True)


if __name__ == '__main__':
    main()
