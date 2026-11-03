# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Bound mutation-test scratch growth without deleting failure evidence.

Only the wrapper's fresh marked run can be compacted, and only after the exact
expected assertion fires without a sanitizer failure. Preserve logs and hashes;
discard disposable copied objects/executable/frames, not ordinary render output.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import time

FW = Path(__file__).resolve().parents[1]
BUDGET = 4 * 1024**3
MARKER = '.prd-control-run.json'

def tree(path):
    rows = {}
    for base, dirs, files in os.walk(path, followlinks=False):
        for name in sorted(dirs + files):
            p = Path(base) / name
            s = p.lstat()
            if not (stat.S_ISREG(s.st_mode) or stat.S_ISDIR(s.st_mode)):
                raise ValueError('Refusing symlink/special scratch entry')
            rows[str(p.relative_to(path))] = (s.st_ino, s.st_size, s.st_mtime_ns,
                                             s.st_mode, s.st_blocks)
    return rows

def checksum(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for data in iter(lambda: stream.read(1024*1024), b''):
            h.update(data)
    return h.hexdigest()

def allocator(path):
    p = Path(path).absolute()
    if p != p.resolve() or p.parent != FW/'build' or p.name not in (
            'controller-tests-1', 'controller-tests-2'):
        raise ValueError('Not an allocator scratch root')
    return p

def run(path):
    p = Path(path).absolute()
    allocator(p.parent)
    if p != p.resolve() or not re.fullmatch(r'message-controls-[A-Za-z0-9]{6}', p.name):
        raise ValueError('Not a fresh mutation-control run')
    return p

def check(path):
    p = allocator(path)
    used = 0
    for child in p.glob('message-controls-*'):
        if not re.fullmatch(r'message-controls-[A-Za-z0-9]{6}', child.name):
            continue
        if child.is_symlink() or not child.is_dir():
            raise ValueError('Unexpected mutation scratch entry')
        used += sum(row[4]*512 for row in tree(child).values())
    if used > BUDGET:
        raise ValueError('Control evidence exceeds 4 GiB; preserve and review old runs before retrying')
    print(f'control_artifacts_bytes={used} budget_bytes={BUDGET}')

def begin(path):
    p = run(path)
    if list(p.iterdir()):
        raise ValueError('Run is not empty')
    with (p/MARKER).open('x') as f:
        json.dump({'schema':1,'started_ns':time.time_ns(),'run':str(p),
                   'wrapper_sha256':checksum(FW/'tools/test-message-controls.sh')}, f)

def validate_payload(rows):
    """Only known generated formats, including inside disposable folders."""
    directories = {'objects','frames','control.dSYM','control.dSYM/Contents',
                   'control.dSYM/Contents/Resources',
                   'control.dSYM/Contents/Resources/DWARF',
                   'control.dSYM/Contents/Resources/Relocations'}
    for name,s in rows.items():
        if stat.S_ISDIR(s[3]):
            allowed = name in directories or re.fullmatch(
                r'control\.dSYM/Contents/Resources/Relocations/[A-Za-z0-9_-]+',name)
        else:
            allowed = name in {'control','control.log','control.dSYM/Contents/Info.plist',
                              'control.dSYM/Contents/Resources/DWARF/control'} or any(
                re.fullmatch(pattern,name) for pattern in (
                    r'objects/[^/]+\.o',
                    r'frames/[A-Za-z0-9][A-Za-z0-9_.-]*\.rgb565',
                    r'control\.dSYM/Contents/Resources/Relocations/[A-Za-z0-9_-]+/control\.yml'))
        if not allowed:
            raise ValueError('Unknown nested artifact; all evidence retained: '+name)

def compact(path, expected):
    p = Path(path).absolute()
    parent = run(p.parent)
    if p != p.resolve() or not re.fullmatch(r'[a-z][a-z0-9-]*', p.name):
        raise ValueError('Invalid control path')
    marker = parent/MARKER
    if marker.is_symlink() or not marker.is_file():
        raise ValueError('Unmarked run')
    state = json.loads(marker.read_text())
    if state.get('schema') != 1 or state.get('run') != str(parent):
        raise ValueError('Run marker mismatch')
    before = tree(p)
    log_path = p/'control.log'
    if not log_path.is_file() or log_path.is_symlink():
        raise ValueError('Missing safe control log')
    log = log_path.read_text()
    if not expected or '\n' in expected or ('FAIL '+expected) not in log.splitlines():
        raise ValueError('Expected failure not proven; all evidence retained')
    if 'ERROR: AddressSanitizer' in log or 'runtime error:' in log:
        raise ValueError('Sanitizer failure; all evidence retained')
    disposable = {'objects','frames','control','control.dSYM'}
    names = set(x.name for x in p.iterdir())
    if not {'objects','frames','control','control.log'} <= names or not names <= disposable | {'control.log'}:
        raise ValueError('Unexpected or missing control artifact; retained')
    if not (p/'objects').is_dir() or not (p/'frames').is_dir() or not (p/'control').is_file():
        raise ValueError('Unexpected artifact type; retained')
    validate_payload(before)
    hashes = {name:checksum(p/name) for name,s in before.items() if stat.S_ISREG(s[3])}
    if tree(p) != before:
        raise ValueError('Scratch changed during hashing')
    evidence = {'schema':1,'expected_failure':expected,'status':'expected-control-fired',
                'files_sha256':hashes,'bytes_before':sum(s[4]*512 for s in before.values()),
                'retained':'control.log and this receipt; failed/unverified controls are never compacted'}
    receipt = p/'evidence.json'
    with receipt.open('x') as f:
        json.dump(evidence,f,indent=2)
        f.flush()
        os.fsync(f.fileno())
    descriptor = os.open(p,os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    s = receipt.stat()
    receipt_state = (s.st_ino,s.st_size,s.st_mtime_ns,s.st_mode,s.st_blocks)
    if tree(p) != before | {'evidence.json':receipt_state}:
        raise ValueError('Scratch changed before removal')
    for name,s in before.items():
        if Path(name).parts[0] in disposable and stat.S_ISREG(s[3]):
            (p/name).unlink()
    for name in sorted(before,key=lambda n:len(Path(n).parts),reverse=True):
        if Path(name).parts[0] in disposable and stat.S_ISDIR(before[name][3]):
            (p/name).rmdir()
    print(f'compacted_control={p.name} preserved_log_and_hashes=true')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=('check','begin','compact'))
    parser.add_argument('path')
    parser.add_argument('expected',nargs='?')
    args = parser.parse_args()
    try:
        if args.action == 'check':check(args.path)
        elif args.action == 'begin':begin(args.path)
        else:compact(args.path,args.expected)
    except (ValueError,OSError,json.JSONDecodeError) as error:
        parser.exit(1,f'Control artifact retention: {error}\n')
