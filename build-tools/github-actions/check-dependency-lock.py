#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Solstice contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that the Windows build can still fetch its pinned dependencies.

Checks every package of windows-dependencies.lock.json at its mirror URL
(udtren/solstice-deps; required) and at the KDE registry (informational:
KDE removes old builds, so the lock will need a refresh before the next
dependency update). Writes a Markdown report to --report and exits with 1
when a mirror file is missing or has the wrong size.
"""

import argparse
import json
import time
import urllib.error
import urllib.request
from pathlib import Path


def head(url):
    """(status, size) of url; status 0 for a connection failure"""
    for attempt in range(3):
        try:
            request = urllib.request.Request(url, method='HEAD', headers={'User-Agent': 'Solstice-lock-check'})
            with urllib.request.urlopen(request, timeout=60) as response:
                return response.status, int(response.headers.get('Content-Length') or -1)
        except urllib.error.HTTPError as error:
            if error.code == 404:
                return 404, -1
            failure = error.code
        except Exception:
            failure = 0
        time.sleep(5 * (attempt + 1))
    return failure, -1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lock', type=Path,
                        default=Path(__file__).with_name('windows-dependencies.lock.json'))
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    lock = json.loads(args.lock.read_text(encoding='utf8'))
    packages = lock['packages']

    mirror_problems = []
    kde_gone = []
    for package in packages:
        name = f"{package['name']} {package['version']}"
        url = package.get('mirrorUrl')
        if not url:
            mirror_problems.append(f'{name}: no mirror URL in the lock')
        else:
            status, size = head(url)
            if status != 200:
                mirror_problems.append(f'{name}: mirror HTTP {status}')
            elif size not in (-1, package['size']):
                mirror_problems.append(f"{name}: mirror size {size}, lock {package['size']}")
        status, _ = head(package['url'])
        if status != 200:
            kde_gone.append(f'{name}: KDE registry HTTP {status}')

    mirror = lock.get('mirror', {})
    lines = [
        '# Windows dependency lock check',
        '',
        f"Mirror: `{mirror.get('repository', '-')}` release `{mirror.get('tag', '-')}`; {len(packages)} packages.",
        '',
        f'- Mirror: {"all present" if not mirror_problems else f"{len(mirror_problems)} problem(s)"}',
        f'- KDE registry: {"all present" if not kde_gone else f"{len(kde_gone)} of {len(packages)} removed"}',
        '',
    ]
    if mirror_problems:
        lines += ['## Mirror problems (the build will fall back to KDE or fail)', '']
        lines += [f'- {line}' for line in mirror_problems] + ['']
        lines += ['Run the "Mirror Solstice dependencies" workflow in the mirror repository with the '
                  'ref that holds this lock, or refresh the lock '
                  '(`build-tools/github-actions/refresh-dependency-lock.py`).', '']
    if kde_gone:
        lines += ['## Removed from the KDE registry', '',
                  'Builds use the mirror. Refresh the lock before the next dependency update, '
                  'then mirror the new set while KDE still has it (docs/agent/github-actions.md).', '']
        lines += [f'- {line}' for line in kde_gone[:20]]
        if len(kde_gone) > 20:
            lines.append(f'- ... and {len(kde_gone) - 20} more')
        lines.append('')
    args.report.write_text('\n'.join(lines), encoding='utf8')
    print('\n'.join(lines))
    return 1 if mirror_problems else 0


if __name__ == '__main__':
    raise SystemExit(main())
