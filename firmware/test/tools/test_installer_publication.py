# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Immutable public release -> actual merged image, with adversarial archives."""
import copy
import importlib.util
import io
import json
import tarfile
import tempfile
import subprocess
import sys
import os
import textwrap
from unittest.mock import patch
from types import SimpleNamespace
from pathlib import Path
import unittest
import zipfile
import test_release_assets

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location('publication', ROOT / 'installer/publication.py')
publication = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(publication)
ACCOMP_SPEC = importlib.util.spec_from_file_location('accompaniment', ROOT / 'installer/accompaniment.py')
accompaniment = importlib.util.module_from_spec(ACCOMP_SPEC)
ACCOMP_SPEC.loader.exec_module(accompaniment)


class PublicationTests(unittest.TestCase):
    def setUp(self):
        fixture = test_release_assets.ReleaseAssetsTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        self.tag, self.commit = fixture.TAG, fixture.COMMIT
        self.source_name = f'prusa-remote-display-{self.tag[1:]}-source.tar.gz'
        self.app = fixture.image.read_bytes()
        self.manifest = (fixture.directory / 'manifest.json').read_bytes()
        self.sums = (fixture.directory / 'SHA256SUMS').read_bytes()
        self.release = {'id': 123, 'tag_name': self.tag, 'immutable': True,
                        'draft': False, 'prerelease': False, 'assets': [],
                        'html_url': f'https://github.com/{publication.REPO}/releases/tag/{self.tag}'}
        merged = b'\xff' * 0x20000 + self.app
        web = json.dumps({'version': self.tag[1:], 'new_install_prompt_erase': False,
            'new_install_improv_wait_time': 0, 'builds': [{'chipFamily': 'ESP32-S3',
            'parts': [{'path': 'merged-firmware.bin', 'offset': 0}]}]}).encode()
        source = {'tag': self.tag, 'commit': self.commit, 'sha256': 'c' * 64,
            'url': f'https://github.com/{publication.REPO}/releases/download/{self.tag}/{self.source_name}',
            'notices': []}
        self.files = {'firmware/merged-firmware.bin': merged, 'firmware/web-manifest.json': web,
            '.nojekyll': b'', 'index.html': b'fixture only', 'style.css': b'fixture',
            'app.js': b'fixture', 'gate.mjs': b'fixture', 'vendor/install-button.js': b'fixture',
            'third-party-notices.txt': b'fixture', 'LICENSE.txt': b'fixture'}
        for name in ['THIRD_PARTY_NOTICES.md', 'LICENSE.txt', 'LICENSING.md', 'build-dependencies.txt']:
            data = ('fixture only ' + name).encode()
            self.files['firmware-notices/' + name] = data
            source['notices'].append({'name': name, 'sha256': publication.sha(data)})
        original = 'dependency/LICENSE'
        name = 'upstream-' + publication.sha(original.encode()) + '.txt'
        upstream = b'complete upstream fixture terms'
        origins = json.dumps([{'file': name, 'original': original,
                               'sha256': publication.sha(upstream)}]).encode()
        for name, data in [(name, upstream), ('notice-origins.json', origins)]:
            self.files['firmware-notices/' + name] = data
            source['notices'].append({'name': name, 'sha256': publication.sha(data)})
        self.project_files = {path: self.files['firmware-notices/' + name] for name, path in [
            ('THIRD_PARTY_NOTICES.md', 'THIRD_PARTY_NOTICES.md'),
            ('LICENSING.md', 'LICENSING.md'), ('LICENSE.txt', 'LICENSE')]}
        self.project_files['firmware/version.txt'] = (self.tag[1:] + '\n').encode()
        self.source_bytes = self.source_archive(self.project_files)
        source['sha256'] = publication.sha(self.source_bytes)
        self.files['source.json'] = json.dumps(source).encode()
        self.site = {'available': True, 'tag': self.tag, 'commit': self.commit,
            'board': 'waveshare_esp32_s3_lcd_2_8', 'imageSha256': publication.sha(merged),
            'manifestSha256': publication.sha(web), 'publisherSpkiSha256': 'b' * 64,
            'manifest': 'firmware/web-manifest.json'}
        self.files['release.json'] = json.dumps(self.site).encode()
        for name, data in [('manifest.json', self.manifest), ('SHA256SUMS', self.sums),
                ('prusa_remote_display.bin', self.app), (self.source_name, self.source_bytes)]:
            self.add_asset(name, data)


    @staticmethod
    def source_archive(files):
        out = io.BytesIO()
        with tarfile.open(fileobj=out, mode='w:gz') as archive:
            for name, data in files.items():
                item = tarfile.TarInfo(name)
                item.size = len(data)
                archive.addfile(item, io.BytesIO(data))
        return out.getvalue()

    def add_asset(self, name, data):
        self.release['assets'] = [a for a in self.release['assets'] if a['name'] != name]
        self.release['assets'].append({'id': 100 + len(self.release['assets']), 'name': name,
            'state': 'uploaded', 'size': len(data), 'digest': 'sha256:' + publication.sha(data),
            'browser_download_url': f'https://github.com/{publication.REPO}/releases/download/{self.tag}/{name}'})

    def archive(self):
        out = io.BytesIO()
        with zipfile.ZipFile(out, 'w') as archive:
            for name, data in self.files.items():
                archive.writestr(name, data)
        return out.getvalue()

    def verify(self):
        data = self.archive()
        self.add_asset('installer-site.zip', data)
        return publication.verify(self.release, 123, self.tag, self.commit,
                                  publication.sha(data), data, self.app, self.manifest, self.sums, self.project_files)

    def test_valid_binding_and_full_receipt(self):
        files, receipt = self.verify()
        self.assertEqual(files['firmware/merged-firmware.bin'][0x20000:], self.app)
        self.assertEqual(receipt['applicationSha256'], publication.sha(self.app))
        self.assertEqual(receipt['releaseId'], 123)

    def test_old_source_asset_name_and_url_fail(self):
        self.release['assets'] = [a for a in self.release['assets'] if a['name'] != self.source_name]
        self.add_asset('candidate-source.tar.gz', self.source_bytes)
        with self.assertRaisesRegex(ValueError, 'expected one release asset: ' + self.source_name):
            self.verify()
        self.add_asset(self.source_name, self.source_bytes)
        source = json.loads(self.files['source.json'])
        source['url'] = f'https://github.com/{publication.REPO}/releases/download/{self.tag}/candidate-source.tar.gz'
        self.files['source.json'] = json.dumps(source).encode()
        with self.assertRaisesRegex(ValueError, 'corresponding-source release identity differs'):
            self.verify()

    def test_mutable_draft_prerelease_wrong_id_fail(self):
        for key, bad in [('immutable', False), ('draft', True), ('prerelease', True),
                         ('id', 124), ('tag_name', 'v9.9.9'), ('html_url', 'https://example.invalid/')]:
            original = self.release[key]
            self.release[key] = bad
            with self.assertRaisesRegex(ValueError, 'intended public stable immutable'):
                self.verify()
            self.release[key] = original

    def test_recomputed_site_hash_cannot_substitute_application(self):
        merged = self.files['firmware/merged-firmware.bin'] + b'substitution'
        self.files['firmware/merged-firmware.bin'] = merged
        self.site['imageSha256'] = publication.sha(merged)
        self.files['release.json'] = json.dumps(self.site).encode()
        with self.assertRaisesRegex(ValueError, 'exact immutable'):
            self.verify()

    def test_source_and_notice_substitution_fail(self):
        source = self.release['assets'][-1]
        original = source['digest']
        source['digest'] = 'sha256:' + 'd' * 64
        with self.assertRaisesRegex(ValueError, 'corresponding-source'):
            self.verify()
        source['digest'] = original
        self.files['firmware-notices/LICENSE.txt'] = b'changed'
        with self.assertRaisesRegex(ValueError, 'notice differs'):
            self.verify()

    def test_wrong_reviewed_archive_hash_and_duplicate_asset_fail(self):
        data = self.archive()
        self.add_asset('installer-site.zip', data)
        with self.assertRaisesRegex(ValueError, 'owner-reviewed'):
            publication.verify(self.release, 123, self.tag, self.commit, 'f'*64,
                               data, self.app, self.manifest, self.sums, self.project_files)
        self.release['assets'].append(copy.deepcopy(self.release['assets'][0]))
        with self.assertRaisesRegex(ValueError, 'one release asset'):
            self.verify()

    def test_archive_traversal_and_symlink_fail(self):
        self.files['../escape'] = b'bad'
        with self.assertRaisesRegex(ValueError, 'unsafe or duplicate site member'):
            self.verify()
        out = io.BytesIO()
        with zipfile.ZipFile(out, 'w') as archive:
            entry = zipfile.ZipInfo('link')
            entry.external_attr = 0o120777 << 16
            archive.writestr(entry, b'../escape')
        with self.assertRaisesRegex(ValueError, 'unsafe or duplicate site member'):
            publication.unpack(out.getvalue())


    def test_project_notices_cannot_be_self_declared_by_zip(self):
        self.files['firmware-notices/LICENSE.txt'] = b'substitute'
        source = json.loads(self.files['source.json'])
        next(n for n in source['notices'] if n['name'] == 'LICENSE.txt')['sha256'] = publication.sha(b'substitute')
        self.files['source.json'] = json.dumps(source).encode()
        with self.assertRaisesRegex(ValueError, 'project notice differs from exact Git'):
            self.verify()

    def test_missing_origins_upstream_and_forged_origin_hash_fail(self):
        saved = self.files['source.json']
        for missing in ['notice-origins.json', 'upstream-']:
            source = json.loads(saved)
            source['notices'] = [n for n in source['notices'] if not n['name'].startswith(missing)]
            self.files['source.json'] = json.dumps(source).encode()
            with self.assertRaisesRegex(ValueError, 'accompaniment missing|origins missing|origin bytes differ'):
                self.verify()
        source = json.loads(saved)
        origins = json.loads(self.files['firmware-notices/notice-origins.json'])
        origins[0]['sha256'] = '0' * 64
        data = json.dumps(origins).encode()
        self.files['firmware-notices/notice-origins.json'] = data
        next(n for n in source['notices'] if n['name'] == 'notice-origins.json')['sha256'] = publication.sha(data)
        self.files['source.json'] = json.dumps(source).encode()
        with self.assertRaisesRegex(ValueError, 'origin bytes differ'):
            self.verify()

    def test_public_tag_lightweight_annotated_and_causal_mismatch(self):
        for annotated in [False, True]:
            ref = {'ref': 'refs/tags/' + self.tag, 'object': {
                'type': 'tag' if annotated else 'commit',
                'sha': 'd' * 40 if annotated else self.commit}}
            tag = {'sha': 'd' * 40, 'object': {'type': 'commit', 'sha': self.commit}}
            responses = [json.dumps(x).encode() for x in ([ref, tag] if annotated else [ref])]
            with patch.object(publication, 'fetch', side_effect=responses) as fetch:
                self.assertEqual(publication.verify_tag_commit(self.tag, self.commit), self.commit)
                self.assertEqual(fetch.call_args_list[0].args[0], f'{publication.API}/git/ref/tags/{self.tag}')
                if annotated:
                    self.assertEqual(fetch.call_args_list[1].args[0], f'{publication.API}/git/tags/' + 'd' * 40)
            tag['object']['sha'] = 'e' * 40
            ref['object']['sha'] = 'd' * 40 if annotated else 'e' * 40
            responses = [json.dumps(x).encode() for x in ([ref, tag] if annotated else [ref])]
            with patch.object(publication, 'fetch', side_effect=responses):
                with self.assertRaisesRegex(ValueError, 'public tag commit differs'):
                    publication.verify_tag_commit(self.tag, self.commit)

    def test_main_network_binding_output_receipt_and_negative_controls(self):
        self.verify()
        data = self.archive()
        self.add_asset('installer-site.zip', data)
        payload = {'installer-site.zip': data, 'prusa_remote_display.bin': self.app,
                   'manifest.json': self.manifest, 'SHA256SUMS': self.sums,
                   self.source_name: self.source_bytes}
        responses = {publication.API: json.dumps({'private': False}).encode(),
            f'{publication.API}/releases/123': json.dumps(self.release).encode(),
            f'{publication.API}/git/ref/tags/{self.tag}': json.dumps({'ref': 'refs/tags/' + self.tag,
                'object': {'type': 'commit', 'sha': self.commit}}).encode()}
        for name, content in payload.items():
            responses[f'https://github.com/{publication.REPO}/releases/download/{self.tag}/{name}'] = content
        with tempfile.TemporaryDirectory(prefix='prd-main-test-') as temp:
            def run(case, changed=None, message=None):
                output = Path(temp) / case / 'site'
                current = dict(responses)
                if changed:
                    current.update(changed)
                calls = []
                def fetch(url, maximum):
                    calls.append(url)
                    self.assertIn(url, current)  # Reject any unanticipated/network URL.
                    return current[url]
                args = ['publication.py', '--release-id', '123', '--tag', self.tag,
                    '--commit', self.commit, '--site-sha256', publication.sha(data), '--output', str(output)]
                with patch.object(sys, 'argv', args), patch.object(publication, 'fetch', side_effect=fetch), \
                        patch.object(publication.source_gate, 'tracked_blobs', return_value=self.project_files):
                    if message:
                        with self.assertRaisesRegex(ValueError, message):
                            publication.main()
                        self.assertFalse(output.exists())
                        self.assertFalse((output.parent / 'deployment-receipt.json').exists())
                    else:
                        publication.main()
                        receipt = json.loads((output.parent / 'deployment-receipt.json').read_text())
                        self.assertEqual(receipt['publicTagCommit'], self.commit)
                        self.assertEqual(receipt['projectSourceFilesVerified'], len(self.project_files))
                        self.assertEqual(receipt['sourceSha256'], publication.sha(self.source_bytes))
                        self.assertEqual(receipt['siteArchiveSha256'], publication.sha(data))
                        self.assertEqual((output / 'firmware/merged-firmware.bin').read_bytes()[0x20000:], self.app)
                        self.assertEqual(set(calls), set(current))
            for bad_flag, value in [('--release-id', '0'), ('--tag', 'bad'),
                                    ('--commit', 'short'), ('--site-sha256', 'bad')]:
                args = ['publication.py', '--release-id', '123', '--tag', self.tag,
                    '--commit', self.commit, '--site-sha256', publication.sha(data),
                    '--output', str(Path(temp) / 'invalid')]
                args[args.index(bad_flag) + 1] = value
                with patch.object(sys, 'argv', args), patch.object(publication, 'fetch') as fetch:
                    with self.assertRaisesRegex(ValueError, 'invalid independently supplied'):
                        publication.main()
                    fetch.assert_not_called()
            existing = Path(temp) / 'existing'
            existing.mkdir()
            args = ['publication.py', '--release-id', '123', '--tag', self.tag,
                '--commit', self.commit, '--site-sha256', publication.sha(data), '--output', str(existing)]
            with patch.object(sys, 'argv', args), patch.object(publication, 'fetch') as fetch:
                with self.assertRaisesRegex(ValueError, 'output must be new'):
                    publication.main()
                fetch.assert_not_called()
            run('valid')
            run('private', {publication.API: b'{"private":true}'}, 'repository is not public')
            bad = copy.deepcopy(self.release)
            for field, value in [('immutable', False), ('html_url', 'https://example.invalid/'), ('draft', True), ('prerelease', True), ('id', 124)]:
                bad = copy.deepcopy(self.release)
                bad[field] = value
                run(field, {f'{publication.API}/releases/123': json.dumps(bad).encode()},
                    'immutable release identity differs|intended public stable immutable')
            ref_url = f'{publication.API}/git/ref/tags/{self.tag}'
            run('wrong-tag-commit', {ref_url: json.dumps({'ref': 'refs/tags/' + self.tag,
                'object': {'type': 'commit', 'sha': 'e' * 40}}).encode()}, 'public tag commit differs')
            source_url = f'https://github.com/{publication.REPO}/releases/download/{self.tag}/{self.source_name}'
            run('source-download', {source_url: b'corrupt'}, 'release asset bytes differ')


    def test_environment_preflight_requires_disabled_admin_bypass(self):
        workflow = (ROOT / '.github/workflows/installer-pages.yml').read_text()
        code = textwrap.dedent(workflow.split("python3 -B - <<'PY'\n", 1)[1].split("          PY", 1)[0])
        environment = {'can_admins_bypass': False, 'protection_rules': [{
            'type': 'required_reviewers', 'reviewers': [{
                'type': 'User', 'reviewer': {'login': 'bigMOTOR'}}]}],
            'deployment_branch_policy': {'custom_branch_policies': True}}
        with tempfile.TemporaryDirectory(prefix='prd-env-test-') as temp:
            root = Path(temp)
            (root / 'pages-branches.json').write_text(json.dumps({'branch_policies': [
                {'name': 'main', 'type': 'branch'}]}))
            for bypass in [False, True, None]:
                current = dict(environment)
                if bypass is None:
                    current.pop('can_admins_bypass')
                else:
                    current['can_admins_bypass'] = bypass
                (root / 'pages-environment.json').write_text(json.dumps(current))
                with patch.dict(os.environ, {'RUNNER_TEMP': temp}):
                    if bypass is False:
                        exec(code, {})
                    else:
                        with self.assertRaisesRegex(AssertionError, 'Administrator bypass must be disabled'):
                            exec(code, {})


class AccompanimentTests(unittest.TestCase):
    def test_source_asset_name_uses_validated_stable_tag(self):
        for tag, expected in [('v1.0.0', 'prusa-remote-display-1.0.0-source.tar.gz'),
                              ('v2.3.4', 'prusa-remote-display-2.3.4-source.tar.gz')]:
            with self.subTest(tag=tag):
                self.assertEqual(accompaniment.source_asset_name(tag), expected)
        for tag in ['1.0.0', 'v1.0.0-rc1', 'v1.0.0/escape', 'v1.0.0\n']:
            with self.subTest(tag=tag), self.assertRaisesRegex(ValueError, 'expected exact stable tag'):
                accompaniment.source_asset_name(tag)

    def test_preserves_full_upstream_text_and_frozen_project_notices(self):
        with tempfile.TemporaryDirectory(prefix='prd-accompaniment-test-') as temp:
            root = Path(temp)
            source, inventory, archive = root/'source.tar.gz', root/'inventory.txt', root/'notices.tar.gz'
            with tarfile.open(source, 'w:gz') as tar:
                member = tarfile.TarInfo('README.md')
                member.size = 7
                tar.addfile(member, io.BytesIO(b'fixture'))
                member = tarfile.TarInfo('firmware/version.txt')
                member.size = 6
                tar.addfile(member, io.BytesIO(b'1.0.0\n'))
            inventory.write_bytes(b'fixture-only configuration and SDK inventory')
            data = b'Copyright fixture\nFull upstream terms\n'
            with tarfile.open(archive, 'w:gz') as tar:
                member = tarfile.TarInfo('dependency/LICENSE')
                member.size = len(data)
                tar.addfile(member, io.BytesIO(data))
            def frozen(command, **kwargs):
                path = command[-1].split(':', 1)[1]
                return SimpleNamespace(stdout=b'1.0.0\n' if path == 'firmware/version.txt'
                                       else ('frozen ' + path).encode())
            with patch.object(accompaniment.subprocess, 'run', side_effect=frozen), \
                    patch.object(accompaniment, 'tracked_blobs', return_value={'README.md': b'fixture', 'firmware/version.txt': b'1.0.0\n'}):
                result = accompaniment.prepare(source, archive, inventory, 'v1.0.0', 'a'*40, root/'out')
            self.assertEqual(result['url'],
                'https://github.com/bigMOTOR/prusa-remote-display/releases/download/v1.0.0/prusa-remote-display-1.0.0-source.tar.gz')
            self.assertEqual(result['sha256'], publication.sha(source.read_bytes()))
            upstream = next(n for n in result['notices'] if n['name'].startswith('upstream-'))
            self.assertEqual((root/'out'/upstream['name']).read_bytes(), data)
            self.assertEqual((root/'out'/'THIRD_PARTY_NOTICES.md').read_bytes(), b'frozen THIRD_PARTY_NOTICES.md')
            self.assertEqual(result['dependencyNoticesArchiveSha256'], publication.sha(archive.read_bytes()))
            with self.assertRaisesRegex(ValueError, 'expected exact stable tag'):
                accompaniment.prepare(source, archive, inventory, 'v1.0.0', 'a'*40, root/'out')


    def test_project_archive_rejects_old_changed_missing_extra_and_links(self):
        expected = {'firmware/version.txt': b'1.0.0\n', 'README.md': b'current source'}
        with tempfile.TemporaryDirectory(prefix='prd-source-integrity-') as temp:
            archive = Path(temp)/'source.tar.gz'
            def write(files, symlink=False):
                with tarfile.open(archive, 'w:gz') as tar:
                    for name, data in files.items():
                        item = tarfile.TarInfo(name)
                        if symlink:
                            item.type = tarfile.SYMTYPE
                            item.linkname = 'elsewhere'
                            tar.addfile(item)
                        else:
                            item.size = len(data)
                            tar.addfile(item, io.BytesIO(data))
            with patch.object(accompaniment, 'tracked_blobs', return_value=expected):
                write(expected)
                self.assertEqual(accompaniment.verify_project_source(archive, 'a'*40), 2)
                for bad in [dict(expected, **{'firmware/version.txt': b'0.1.1-dev\n'}),
                            dict(expected, **{'README.md': b'changed source'}),
                            {'README.md': b'current source'},
                            dict(expected, **{'extra': b'extra'}),
                            dict(expected, **{'../escape': b'bad'})]:
                    write(bad)
                    with self.assertRaisesRegex(ValueError, 'source bytes differ|missing project source|extra/non-regular|unsafe or duplicate'):
                        accompaniment.verify_project_source(archive, 'a'*40)
                write(expected, symlink=True)
                with self.assertRaisesRegex(ValueError, 'extra/non-regular'):
                    accompaniment.verify_project_source(archive, 'a'*40)


    def test_real_git_tracked_blobs_archive_and_same_bytes_tag_mismatch(self):
        with tempfile.TemporaryDirectory(prefix='prd-real-source-git-') as temp:
            root = Path(temp)
            (root / 'firmware').mkdir()
            expected = {'firmware/version.txt': b'1.0.0\n', 'README.md': b'current source\n',
                        'name with spaces.txt': b'blob with spaces\n', 'nul.bin': b'\0\xff\0'}
            for name, content in expected.items():
                (root / name).write_bytes(content)
            def git(*args):
                return subprocess.run(['git', *args], cwd=root, check=True, capture_output=True).stdout
            git('init', '-q')
            git('add', '.')
            git('-c', 'user.name=Disposable Fixture', '-c', 'user.email=fixture@example.invalid',
                'commit', '-qm', 'fixture only')
            commit = git('rev-parse', 'HEAD').decode().strip()
            archive = root / 'source.tar.gz'
            archive.write_bytes(git('archive', '--format=tar.gz', commit))
            with patch.object(accompaniment, 'ROOT', root):
                self.assertEqual(accompaniment.tracked_blobs(commit), expected)
                self.assertEqual(accompaniment.verify_project_source(archive, commit, 'v1.0.0'), 4)
                with self.assertRaisesRegex(ValueError, 'project source version differs from intended tag'):
                    accompaniment.verify_project_source(archive, commit, 'v1.0.1')


if __name__ == '__main__':
    unittest.main()
