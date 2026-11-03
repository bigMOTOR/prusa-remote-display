#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Prepare notices/source metadata for an exact release (no signing or publication).

Inputs are the reviewed ordinary source archive, full upstream notice archive and
that build's dependency/configuration inventory. This preserves their bytes; it
does not perform a new legal or linker audit or certify archive completeness.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def source_asset_name(tag):
    if re.fullmatch(r'v\d+\.\d+\.\d+', tag) is None:
        raise ValueError('expected exact stable tag')
    return f'prusa-remote-display-{tag[1:]}-source.tar.gz'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def tracked_blobs(commit):
    listing = subprocess.run(['git', 'ls-tree', '-rz', '--full-tree', commit], cwd=ROOT,
                             check=True, capture_output=True).stdout
    entries = []
    for record in listing.split(b'\0'):
        if not record:
            continue
        metadata, path = record.split(b'\t', 1)
        mode, kind, oid = metadata.split()
        if mode not in (b'100644', b'100755') or kind != b'blob':
            raise ValueError('source tree contains unsupported non-regular member')
        entries.append((path.decode('utf-8'), oid))
    if not entries or len(entries) > 2000:
        raise ValueError('source tree exceeds bounded file count')
    payload = subprocess.run(['git', 'cat-file', '--batch'], cwd=ROOT, check=True,
        input=b''.join(oid + b'\n' for _, oid in entries), capture_output=True).stdout
    if len(payload) > 32 * 1024 * 1024:
        raise ValueError('source tree exceeds 32 MiB')
    files, position = {}, 0
    for name, oid in entries:
        end = payload.index(b'\n', position)
        actual_oid, kind, size = payload[position:end].split()
        length = int(size)
        if actual_oid != oid or kind != b'blob':
            raise ValueError('Git blob response differs')
        position = end + 1
        files[name] = payload[position:position + length]
        position += length + 1
    if position != len(payload):
        raise ValueError('unexpected Git blob response')
    return files


def verify_project_source(source, commit, tag=None):
    expected = tracked_blobs(commit)
    if tag is not None and expected.get('firmware/version.txt', b'').decode().strip() != tag[1:]:
        raise ValueError('project source version differs from intended tag')
    directories = {str(parent) for name in expected for parent in PurePosixPath(name).parents
                   if str(parent) != '.'}
    seen = set()
    total = 0
    with tarfile.open(source, 'r:gz') as archive:
        count = 0
        for item in archive:
            count += 1
            if count > 4000:
                raise ValueError('too many project archive members')
            name = item.name.rstrip('/') if item.isdir() else item.name
            path = PurePosixPath(name)
            if (path.is_absolute() or '..' in path.parts or str(path) != name or
                    '\\' in name or name in seen):
                raise ValueError('unsafe or duplicate project source member')
            seen.add(name)
            if item.isdir():
                if name not in directories:
                    raise ValueError('extra project source directory')
                continue
            total += item.size
            if not item.isfile() or total > 32 * 1024 * 1024 or name not in expected:
                raise ValueError('extra/non-regular/oversized project source member')
            if item.size != len(expected[name]) or archive.extractfile(item).read() != expected[name]:
                raise ValueError('project source bytes differ from exact frozen commit')
    if not expected.keys() <= seen:
        raise ValueError('missing project source member')
    return len(expected)


def prepare(source, dependencies, inventory, tag, commit, output):
    if (re.fullmatch(r'v\d+\.\d+\.\d+', tag) is None or
            re.fullmatch(r'[0-9a-f]{40}', commit) is None or output.exists()):
        raise ValueError('expected exact stable tag, full commit and new output')
    if source.is_symlink() or not source.is_file() or not 0 < source.stat().st_size <= 32*1024*1024:
        raise ValueError('source archive must be regular and within 32 MiB')
    def frozen(path):
        return subprocess.run(['git', 'show', f'{commit}:{path}'], cwd=ROOT,
                              check=True, capture_output=True).stdout
    if frozen('firmware/version.txt').decode().strip() != tag[1:]:
        raise ValueError('frozen source version differs from intended tag')
    source_file_count = verify_project_source(source, commit, tag)
    files = {name: frozen(path) for name, path in [
        ('THIRD_PARTY_NOTICES.md', 'THIRD_PARTY_NOTICES.md'),
        ('LICENSING.md', 'LICENSING.md'), ('LICENSE.txt', 'LICENSE')]}
    files['build-dependencies.txt'] = inventory.read_bytes()
    origins = []
    with tarfile.open(dependencies, 'r:gz') as archive:
        members = archive.getmembers()
        if not 1 <= len(members) <= 190 or sum(m.size for m in members) > 4*1024*1024:
            raise ValueError('dependency notices exceed bounded contract')
        for item in members:
            path = PurePosixPath(item.name)
            if (not item.isfile() or path.is_absolute() or '..' in path.parts or
                    str(path) != item.name or '\\' in item.name or not 0 < item.size <= 1024*1024):
                raise ValueError('unsafe notice member')
            data = archive.extractfile(item).read()
            # Flat content names preserve all text bytes and avoid path collisions.
            name = 'upstream-' + digest(item.name.encode()) + '.txt'
            if name in files:
                raise ValueError('duplicate notice member')
            files[name] = data
            origins.append({'file': name, 'original': item.name, 'sha256': digest(data)})
    files['notice-origins.json'] = (json.dumps(origins, indent=2) + '\n').encode()
    if any(not data or len(data) > 1024*1024 for data in files.values()) or sum(map(len, files.values())) > 4*1024*1024:
        raise ValueError('empty/oversized accompaniment')
    source_hash = hashlib.sha256()
    with source.open('rb') as handle:
        for chunk in iter(lambda: handle.read(1024*1024), b''):
            source_hash.update(chunk)
    record = {'tag': tag, 'commit': commit, 'sha256': source_hash.hexdigest(),
        'url': f'https://github.com/bigMOTOR/prusa-remote-display/releases/download/{tag}/{source_asset_name(tag)}',
        'dependencyNoticesArchiveSha256': digest(dependencies.read_bytes()),
        'projectSourceFilesVerified': source_file_count,
        'notices': [{'name': name, 'sha256': digest(data)} for name, data in sorted(files.items())]}
    output.mkdir(parents=True)
    for name, data in files.items():
        (output / name).write_bytes(data)
    (output / 'source.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['source-archive', 'dependency-notices', 'build-inventory', 'output']:
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--commit', required=True)
    args = parser.parse_args()
    result = prepare(args.source_archive, args.dependency_notices, args.build_inventory,
                     args.tag, args.commit, args.output)
    print(json.dumps({'sourceSha256': result['sha256'], 'noticeFiles': len(result['notices'])}))
