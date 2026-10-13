#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Solstice contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Refresh windows-dependencies.lock.json from the public KDE registry.

Usage: refresh-dependency-lock.py [--mirror-only] <current lock> <refreshed lock>

Resolves the newest transition.now/qt6 build of every package in the
current lock (checked against each build's metadata.json), records its
archive size, SHA-256, source revision and dependencies, and prints what
changed. A dependency outside the set is reported; add a placeholder entry
for it ({"name": ..., "version": "", "sourceRevision": "", "dependencies":
{}, "runtimeDependencies": {}}) and run again. The toolchain and Vulkan SDK
entries are copied unchanged. Reads only the public registry API; the
checksums are verified again by bootstrap-windows.py on download
(docs/agent/github-actions.md).

The lock also names its mirror release in udtren/solstice-deps (tag
deps-<newest build date>-<source revision>) and each package's mirror URL.
After a refresh, run that repository's "Mirror Solstice dependencies"
workflow with the Solstice ref holding the new lock, while KDE still has the
packages. --mirror-only only (re)writes the mirror fields of the current lock.
"""
import argparse
import collections
import datetime
import json
import time
import urllib.parse
import urllib.request

API = 'https://invent.kde.org/api/v4/projects/16406'
MIRROR_REPOSITORY = 'udtren/solstice-deps'
BRANCH = 'transition.now/qt6'
PREFIX = 'transition.now-qt6-'


def get(url):
    for attempt in range(4):
        try:
            request = urllib.request.Request(url, headers={'User-Agent': 'Solstice-lock-refresh'})
            with urllib.request.urlopen(request, timeout=60) as response:
                return json.load(response)
        except Exception:
            if attempt == 3:
                raise
            time.sleep(3 * (attempt + 1))


def newest(name):
    versions = []
    page = 1
    while True:
        query = urllib.parse.urlencode({'package_name': name, 'package_type': 'generic',
                                        'order_by': 'created_at', 'sort': 'desc',
                                        'per_page': 100, 'page': page})
        batch = get(f'{API}/packages?{query}')
        versions += [p for p in batch if p['name'] == name and p['version'].startswith(PREFIX)
                     and p['version'][len(PREFIX):].isdigit() and p['status'] == 'default']
        if len(batch) < 100:
            break
        page += 1
    if not versions:
        raise RuntimeError(f'{name}: no {PREFIX}* build')
    return max(versions, key=lambda p: int(p['version'][len(PREFIX):]))


def add_mirror(lock):
    """The mirror release of this package set and each package's URL in it"""
    newest = max(int(p['version'][len(PREFIX):]) for p in lock['packages'])
    day = datetime.datetime.fromtimestamp(newest, datetime.timezone.utc).strftime('%Y%m%d')
    revision = collections.Counter(p['sourceRevision'] for p in lock['packages']).most_common(1)[0][0]
    repository = lock.get('mirror', {}).get('repository', MIRROR_REPOSITORY)
    tag = f'deps-{day}-{revision[:7]}'
    lock['mirror'] = {'repository': repository, 'tag': tag}
    for package in lock['packages']:
        package['mirrorUrl'] = (f"https://github.com/{repository}/releases/download/{tag}/"
                                f"{package['name']}-{package['version']}.tar")


def write(lock, path):
    # the mirror entry follows the registry project, before the packages
    ordered = {key: lock[key] for key in ('schema', 'platform', 'registryProject', 'mirror') if key in lock}
    ordered.update({key: value for key, value in lock.items() if key not in ordered})
    with open(path, 'w', encoding='utf8', newline='\n') as output:
        json.dump(ordered, output, indent=2)
        output.write('\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mirror-only', action='store_true')
    parser.add_argument('current')
    parser.add_argument('refreshed')
    args = parser.parse_args()
    old = json.load(open(args.current, encoding='utf8'))
    if args.mirror_only:
        add_mirror(old)
        write(old, args.refreshed)
        print('mirror:', old['mirror'])
        return
    new_packages = []
    for item in old['packages']:
        name = item['name']
        package = newest(name)
        files = {f['file_name']: f for f in get(f"{API}/packages/{package['id']}/package_files")}
        archive = files['archive.tar']
        version = package['version']
        meta = get(f"{API}/packages/generic/{name}/{version}/metadata.json")
        if meta.get('branch') != BRANCH or meta.get('identifier') != name or meta.get('version') != version:
            raise RuntimeError(f'{name}: unexpected metadata {meta}')
        if not archive.get('file_sha256'):
            raise RuntimeError(f'{name}: registry gives no SHA-256')
        new_packages.append({
            'name': name,
            'version': version,
            'url': f"{API}/packages/generic/{name}/{version}/archive.tar",
            'sha256': archive['file_sha256'],
            'size': archive['size'],
            'sourceRevision': meta['gitRevision'],
            'dependencies': meta.get('dependencies', {}),
            'runtimeDependencies': meta.get('runtime-dependencies', {}),
        })
        changed = 'same' if version == item['version'] else 'new'
        rev = 'same source' if meta['gitRevision'] == item['sourceRevision'] else 'source changed'
        print(f"{name:32} {item['version'][len(PREFIX):]} -> {version[len(PREFIX):]} {changed}, {rev}", flush=True)

    names = {p['name'] for p in new_packages}
    missing = sorted({d for p in new_packages for d in list(p['dependencies']) + list(p['runtimeDependencies'])} - names)
    print('dependencies outside the set:', missing or 'none')
    lock = dict(old)
    lock['packages'] = new_packages
    add_mirror(lock)
    write(lock, args.refreshed)
    print('mirror:', lock['mirror'])


if __name__ == '__main__':
    main()
