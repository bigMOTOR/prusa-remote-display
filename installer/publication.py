#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Fetch one public immutable release and verify its ready-built Pages artifact.

No credentials, signing or device access. Deployment remains a separate job.
"""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import io
import json
from pathlib import Path, PurePosixPath
import re
import stat
import tempfile
from urllib.request import Request, urlopen
import zipfile

REPO = 'bigMOTOR/prusa-remote-display'
API = f'https://api.github.com/repos/{REPO}'
LIMIT = 20 * 1024 * 1024
SOURCE_SPEC = importlib.util.spec_from_file_location('prd_source',
    Path(__file__).resolve().with_name('accompaniment.py'))
source_gate = importlib.util.module_from_spec(SOURCE_SPEC)
SOURCE_SPEC.loader.exec_module(source_gate)


def verify_tag_commit(tag, commit):
    """Resolve public refs, including annotated tag objects, without target_commitish."""
    ref = load(fetch(f'{API}/git/ref/tags/{tag}', 1024 * 1024))
    if ref.get('ref') != f'refs/tags/{tag}':
        raise ValueError('public tag ref differs')
    obj = ref.get('object', {})
    seen = set()
    for _ in range(8):
        oid, kind = obj.get('sha', ''), obj.get('type')
        if re.fullmatch(r'[0-9a-f]{40}', oid) is None or oid in seen:
            raise ValueError('invalid or cyclic public tag object')
        seen.add(oid)
        if kind == 'commit':
            if oid != commit:
                raise ValueError('public tag commit differs from reviewed commit')
            return oid
        if kind != 'tag':
            raise ValueError('public tag does not resolve to commit')
        annotated = load(fetch(f'{API}/git/tags/{oid}', 1024 * 1024))
        if annotated.get('sha') != oid:
            raise ValueError('public annotated tag identity differs')
        obj = annotated.get('object', {})
    raise ValueError('public tag nesting exceeds bound')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate JSON member')
        result[key] = value
    return result


def load(data):
    return json.loads(data, object_pairs_hook=unique)


def fetch(url, maximum):
    request = Request(url, headers={'Accept': 'application/vnd.github+json',
                                   'X-GitHub-Api-Version': '2022-11-28',
                                   'User-Agent': 'Prusa-Remote-Display-release-check'})
    with urlopen(request, timeout=60) as response:
        data = response.read(maximum + 1)
    if len(data) > maximum:
        raise ValueError('download exceeds bound')
    return data


def asset(release, name):
    matches = [a for a in release['assets'] if a['name'] == name]
    if len(matches) != 1:
        raise ValueError(f'expected one release asset: {name}')
    item = matches[0]
    if (type(item['id']) is not int or item['id'] <= 0 or item['state'] != 'uploaded' or
            type(item['size']) is not int or item['size'] <= 0 or
            re.fullmatch(r'sha256:[0-9a-f]{64}', item.get('digest', '')) is None):
        raise ValueError('release asset lacks exact uploaded identity/digest')
    expected = f'https://github.com/{REPO}/releases/download/{release["tag_name"]}/{name}'
    if item['browser_download_url'] != expected:
        raise ValueError('unexpected download origin')
    return item


def download(item, maximum):
    if item['size'] > maximum:
        raise ValueError('asset exceeds download bound')
    data = fetch(item['browser_download_url'], maximum)
    if len(data) != item['size'] or 'sha256:' + sha(data) != item['digest']:
        raise ValueError('release asset bytes differ')
    return data


def unpack(data):
    files = {}
    total = 0
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        entries = archive.infolist()
        if len(entries) > 1000:
            raise ValueError('too many site members')
        for item in entries:
            name = item.filename
            path = PurePosixPath(name)
            if (item.is_dir() or name in files or str(path) != name or path.is_absolute() or
                    '..' in path.parts or '\\' in name or not name or
                    stat.S_IFMT(item.external_attr >> 16) not in (0, stat.S_IFREG)):
                raise ValueError('unsafe or duplicate site member')
            total += item.file_size
            if (item.file_size == 0 and name != '.nojekyll') or total > LIMIT:
                raise ValueError('site exceeds unpacked budget or has empty member')
            files[name] = archive.read(item)
    return files


def verify(release, release_id, tag, commit, site_hash, site_zip, app, manifest, sums, project_files):
    if (type(release_id) is not int or release_id <= 0 or
            re.fullmatch(r'v\d+\.\d+\.\d+', tag) is None or
            re.fullmatch(r'[0-9a-f]{40}', commit) is None or
            re.fullmatch(r'[0-9a-f]{64}', site_hash) is None):
        raise ValueError('invalid independently supplied identity')
    if (release['id'] != release_id or release['tag_name'] != tag or
            release.get('immutable') is not True or release.get('draft') is not False or
            release.get('prerelease') is not False or
            release['html_url'] != f'https://github.com/{REPO}/releases/tag/{tag}'):
        raise ValueError('not the intended public stable immutable release')
    source_name = source_gate.source_asset_name(tag)
    required = ['installer-site.zip', source_name,
                'prusa_remote_display.bin', 'manifest.json', 'SHA256SUMS']
    assets = {name: asset(release, name) for name in required}
    for name, data in [('installer-site.zip', site_zip), ('prusa_remote_display.bin', app),
                       ('manifest.json', manifest), ('SHA256SUMS', sums)]:
        if len(data) != assets[name]['size'] or 'sha256:' + sha(data) != assets[name]['digest']:
            raise ValueError('downloaded bytes do not match immutable asset')
    if sha(site_zip) != site_hash:
        raise ValueError('site archive differs from owner-reviewed hash')
    # Reuse the existing complete OTA publication gate, without changing trust policy.
    spec = importlib.util.spec_from_file_location('prd_gate',
        Path(__file__).resolve().parents[1] / 'firmware/tools/check-release-assets.py')
    gate = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gate)
    with tempfile.TemporaryDirectory(prefix='prd-publication-') as temp:
        directory = Path(temp)
        for name, data in [('manifest.json', manifest), ('prusa_remote_display.bin', app),
                           ('SHA256SUMS', sums)]:
            (directory / name).write_bytes(data)
        identity = gate.verify(directory, tag, commit)
    files = unpack(site_zip)
    for required in ['index.html', 'style.css', 'app.js', 'gate.mjs',
                     'vendor/install-button.js', 'third-party-notices.txt', 'LICENSE.txt']:
        if not files.get(required):
            raise ValueError('missing static installer member')
    site = load(files['release.json'])
    source = load(files['source.json'])
    merged = files['firmware/merged-firmware.bin']
    web_bytes = files['firmware/web-manifest.json']
    web = load(web_bytes)
    if (site.get('available') is not True or site['tag'] != tag or site['commit'] != commit or
            site['board'] != gate.BOARD_TARGET or site['imageSha256'] != sha(merged) or
            site['manifestSha256'] != sha(web_bytes) or
            site['publisherSpkiSha256'] != identity['publicKeySpkiSha256'] or
            site['manifest'] != 'firmware/web-manifest.json' or merged[0x20000:] != app or
            len(merged) > 16 * 1024 * 1024 or web.get('version') != tag[1:] or
            web.get('new_install_prompt_erase') is not False or
            web.get('new_install_improv_wait_time') != 0 or
            web.get('builds') != [{'chipFamily': 'ESP32-S3',
                                  'parts': [{'path': 'merged-firmware.bin', 'offset': 0}]}]):
        raise ValueError('site does not install the exact immutable release application')
    src = assets[source_name]
    if (source['tag'] != tag or source['commit'] != commit or
            source['url'] != src['browser_download_url'] or
            'sha256:' + source['sha256'] != src['digest']):
        raise ValueError('corresponding-source release identity differs')
    seen = set()
    for notice in source['notices']:
        name = notice['name']
        if re.fullmatch(r'[a-zA-Z0-9][a-zA-Z0-9._-]{0,120}', name) is None or name in seen:
            raise ValueError('unsafe or duplicate firmware notice')
        seen.add(name)
        data = files['firmware-notices/' + name]
        if not data or len(data) > 1024 * 1024 or sha(data) != notice['sha256']:
            raise ValueError('firmware notice differs')
    if not {'THIRD_PARTY_NOTICES.md', 'LICENSE.txt', 'LICENSING.md', 'build-dependencies.txt', 'notice-origins.json'} <= seen:
        raise ValueError('firmware accompaniment missing')
    for notice, path in [('THIRD_PARTY_NOTICES.md', 'THIRD_PARTY_NOTICES.md'),
                         ('LICENSING.md', 'LICENSING.md'), ('LICENSE.txt', 'LICENSE')]:
        if files['firmware-notices/' + notice] != project_files[path]:
            raise ValueError('project notice differs from exact Git commit')
    origins = load(files['firmware-notices/notice-origins.json'])
    upstream = {name for name in seen if name.startswith('upstream-')}
    if not isinstance(origins, list) or not origins or not upstream:
        raise ValueError('upstream notice origins missing')
    origin_files, original_paths = set(), set()
    for origin in origins:
        name, original = origin['file'], origin['original']
        path = PurePosixPath(original)
        if (name not in upstream or name in origin_files or original in original_paths or
                str(path) != original or path.is_absolute() or '..' in path.parts or
                not original or '\\' in original or
                name != 'upstream-' + sha(original.encode()) + '.txt' or
                sha(files['firmware-notices/' + name]) != origin['sha256']):
            raise ValueError('upstream notice origin bytes differ')
        origin_files.add(name)
        original_paths.add(original)
    if origin_files != upstream:
        raise ValueError('upstream notice origins incomplete')
    receipt = {'releaseId' : release_id, 'tag': tag, 'commit': commit,
               'siteArchiveSha256': sha(site_zip), 'mergedSha256': sha(merged),
               'applicationSha256': sha(app), 'sourceSha256': source['sha256'],
               'assets': {n: {'id': a['id'], 'digest': a['digest']} for n, a in assets.items()}}
    return files, receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--release-id', type=int, required=True)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--commit', required=True)
    parser.add_argument('--site-sha256', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if (args.release_id <= 0 or re.fullmatch(r'v\d+\.\d+\.\d+', args.tag) is None or
            re.fullmatch(r'[0-9a-f]{40}', args.commit) is None or
            re.fullmatch(r'[0-9a-f]{64}', args.site_sha256) is None):
        raise ValueError('invalid independently supplied release identity')
    if args.output.exists():
        raise ValueError('output must be new')
    release = load(fetch(f'{API}/releases/{args.release_id}', 1024 * 1024))
    # Refuse private repositories; no token is accepted or sent by this tool.
    if load(fetch(API, 1024 * 1024)).get('private') is not False:
        raise ValueError('repository is not public')
    if release.get('immutable') is not True or release.get('tag_name') != args.tag:
        raise ValueError('immutable release identity differs')
    resolved_commit = verify_tag_commit(args.tag, args.commit)
    project_files = source_gate.tracked_blobs(args.commit)
    content = {name: download(asset(release, name), maximum) for name, maximum in [
        ('installer-site.zip', LIMIT), ('prusa_remote_display.bin', 0x680000),
        ('manifest.json', 8192), ('SHA256SUMS', 65536)]}
    files, receipt = verify(release, args.release_id, args.tag, args.commit, args.site_sha256,
                           content['installer-site.zip'], content['prusa_remote_display.bin'],
                           content['manifest.json'], content['SHA256SUMS'], project_files)
    receipt['publicTagCommit'] = resolved_commit
    # Prove anonymous source download, not merely an uploaded asset record.
    source_name = source_gate.source_asset_name(args.tag)
    source_bytes = download(asset(release, source_name), 32 * 1024 * 1024)
    with tempfile.TemporaryDirectory(prefix='prd-source-binding-') as temp:
        source_path = Path(temp) / source_name
        source_path.write_bytes(source_bytes)
        receipt['projectSourceFilesVerified'] = source_gate.verify_project_source(
            source_path, args.commit, args.tag)
    args.output.mkdir(parents=True)
    for name, data in files.items():
        target = args.output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    (args.output.parent / 'deployment-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt, sort_keys=True))


if __name__ == '__main__':
    main()
