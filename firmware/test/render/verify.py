# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Pixel preservation and bounded-memory checks against real LVGL output, not a mock."""
from pathlib import Path
import re
import sys

def read(path):
    frames, operational = {}, {}
    for line in path.read_text().splitlines():
        if line.startswith('F '):
            _, origin, tick, *fields = line.split()
            frames[origin, int(tick)] = dict(v.split('=',1) for v in fields)
        elif line.startswith('OPERATIONAL '):
            _, index, digest = line.split()
            operational[int(index)] = digest[3:]
    return frames, operational

pool_path, clib_path = map(Path, sys.argv[1:])
pool, operational = read(pool_path)
clib, clib_operational = read(clib_path)
assert len(pool) == len(clib) == 942
assert operational == clib_operational
for key, row in pool.items():
    assert row['dg'] == clib[key]['dg'], key
    x1,y1,x2,y2 = map(int,re.findall(r'-?\d+',row['ink']))
    assert 12 <= x1 <= x2 <= 307 and 12 <= y1 <= y2 <= 227, key
    assert int(row['pts']) <= 64
    assert int(row['hi']) < 65536
for line in Path(__file__).with_name('preserved-pixels.txt').read_text().splitlines():
    name, tick, digest = line.split()
    got = pool[name,int(tick)]['dg']
    assert got == digest, (name,tick)
# New operational baseline, explicitly separate from the unchanged R2/B2 preservation pins.
pins = {}
for line in Path(__file__).with_name('operational-pixels.txt').read_text().splitlines():
    if line.startswith('#'):
        continue
    index, digest = line.split()
    assert int(index) not in pins
    pins[int(index)] = digest
assert len(pins) == 16 and operational == pins, (operational, pins)
assert pool['Completed',400]['ft'] == pool['Completed',416]['ft']
assert [int(pool['Printing',t]['ext']) for t in (32,48)] == [352,346]
assert [int(pool['Completed',t]['fw']) for t in (1040,1056,1072)] == [232,237,240]
assert (int(pool['Completed',1168]['ft']) - int(pool['Completed',1152]['ft'])) == 1
print('942 allocator pairs;12 preserved loss endpoints;157 preserved Ready-B2 frames;16 active operational pins;zero inset breaches.')
print(next(line for line in pool_path.read_text().splitlines() if line.startswith('HIWATER ')))

# Actual-controller animation frames, not just direct geometry samples.
native = pool_path.parent / 'controller-frames'
redzone = clib_path.parent / 'controller-frames'
names = sorted(p.name for p in native.glob('*.rgb565'))
edge_names = {f'{label}-{t}.rgb565'
              for label in ('Near-Ready-Loss', 'Offset-Ready-Loss', 'Zero-Appearance-Loss')
              for t in range(0, 2600, 32)}
assert len(edge_names) == 246 and edge_names.issubset(names)
age_names = {f'Age-{subject}-{direction}-{t}.rgb565'
             for subject in ('Ready', 'Preparing', 'Printing', 'Paused', 'Completed', 'Stopped', 'Unknown')
             for direction in ('entry', 'recovery') for t in range(0, 1105, 16)}
assert len(age_names) == 980 and age_names.issubset(names)
record_entry_names = {f'{edge}-{"0000" if t == 0 else t}.rgb565'
                      for edge in ('Preparing-Printing0', 'Paused-Printing79')
                      for t in range(0, 1105, 16)}
assert len(record_entry_names) == 140 and record_entry_names.issubset(names)
closed_recovery_names = {f'Printing-Recovery0-{t}.rgb565' for t in range(0, 1121, 16)}
assert len(closed_recovery_names) == 71 and closed_recovery_names.issubset(names)
error_names = {f'Error-{subject}-{direction}-{t}.rgb565'
               for subject in ('Ready', 'Preparing', 'Printing', 'Paused', 'Completed', 'Stopped')
               for direction in ('entry', 'recovery', 'aged', 'age-recovery') for t in range(0, 1121, 16)}
error_names.add('Error-Acquiring-settled.rgb565')
assert len(error_names) == 1705 and error_names.issubset(names)
attention_names = {name.replace('Error-', 'Attention-', 1) for name in error_names}
assert len(attention_names) == 1705 and attention_names.issubset(names)
assert len(set(names) - edge_names - age_names - record_entry_names - closed_recovery_names - error_names - attention_names) == 866, len(names)
assert names == sorted(p.name for p in redzone.glob('*.rgb565'))
for name in names:
    raw = (native / name).read_bytes()
    assert len(raw) == 320 * 240 * 2 and raw == (redzone / name).read_bytes(), name
    background = raw[:2]
    for y in range(240):
        spans = ((0, 320),) if y < 12 or y > 227 else ((0, 12), (308, 320))
        for left, right in spans:
            border = raw[(y * 320 + left) * 2:(y * 320 + right) * 2]
            assert border == background * (right - left), (name, y, left)
print(f'{len(names)} actual-controller frame pairs byte-identical; sampled canvas borders clear.')
for path in (pool_path, clib_path):
    log = (path.parent / 'controller.log').read_text()
    assert 'RECOVERY 144 origin/destination/phase cases' in log
    assert 'HANDOFF 30 interrupted losses; 24 visible/tail/blank recoveries' in log
    assert 'REACHABLE 3 Store/projection recoveries; Off clock; interrupted appearance pixels' in log
    assert 'RECORD_ENTRY Preparing-Printing0' in log
    assert 'RECORD_ENTRY Paused-Printing79' in log
    assert 'PASS 30 directed controller edges; interruption; wake; unknown;' in log
    assert log.count('EDGECASE ') == 3
    print(next(line for line in log.splitlines() if line.startswith('PASS ')))

# The complete flagged instrument, not direct helper calls. Its clock advances normally;
# both allocator variants must produce the same 4 entry/recovery sequences and post-run frame.
review_names = {f'{kind}-{chapter}-{t}.rgb565'
                for kind, end in (('loss', 2400), ('recovery', 1200))
                for chapter in range(1, 5) for t in range(0, end + 1, 80)}
review_names |= {f'unaccepted-degraded-{i}.rgb565' for i in range(1, 5)} | {'final-ready.rgb565'}
assert len(review_names) == 193
for path in (pool_path, clib_path):
    frames_dir = path.parent / 'review-frames'
    assert review_names == {p.name for p in frames_dir.glob('*.rgb565')}
    log = (path.parent / 'review.log').read_text()
    assert 'INVALID' not in log and 'INCOMPLETE' not in log
    assert 'event=complete-final-ready at_ms=617300 recovered=4 manual=0' in log
    assert 'PASS actual instrument 4 losses/4 recoveries, post-run=600000 ms' in log
    for i, delay in enumerate((3500, 400, 20600, 26000), 1):
        assert re.search(rf'^RECOVERY {i} at=\d+ delta={delay}$', log, re.M)
    assert len(re.findall(r'^UNACCEPTED_DEGRADED ', log, re.M)) == 4
    controls = (path.parent / 'review-controls.log').read_text()
    assert 'PASS rapid actual-flow controls;' in controls
    assert 'event=INCOMPLETE-final-ready' in controls
    assert 'event=complete-final-ready' not in controls and 'INVALID' not in controls
for name in sorted(review_names):
    raw = (pool_path.parent / 'review-frames' / name).read_bytes()
    assert len(raw) == 320 * 240 * 2
    assert raw == (clib_path.parent / 'review-frames' / name).read_bytes(), name
print('193 complete-instrument frame pairs identical; 4 recoveries; 10-minute final hold.')
