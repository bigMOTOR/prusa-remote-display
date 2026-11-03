#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Causal mutations in disposable objects; never edit the production sources or host binary."""
import json
from pathlib import Path
import shlex
import subprocess
import tempfile

firmware = Path(__file__).resolve().parent.parent
build = firmware / 'build/host-tests-asan'
commands = json.loads((build / 'compile_commands.json').read_text())
link = shlex.split((build / 'CMakeFiles/prd_host_tests.dir/link.txt').read_text())
controls = (
    ('upper-angle', 'operational_form.cpp', 'PRD_OLD_UPPER_ANGLE', 'FormMotionRegression.RoundedRadius*:FormMotionRegression.FirstOpening*'),
    ('stroke-anchor', 'operational_form.cpp', 'PRD_OLD_STROKE_ANCHOR', 'FormMotionRegression.RestingAnchor*'),
    ('paced-clock', 'printing_timeline_flow.cpp', None, 'PrintingPacedFlow.*'),
)
with tempfile.TemporaryDirectory(prefix='repair-controls-', dir=build) as temporary:
    work = Path(temporary)
    for name, source_name, macro, selection in controls:
        entry, = [c for c in commands if Path(c['file']).name == source_name]
        compile_args = shlex.split(entry['command'])
        original_object = compile_args[compile_args.index('-o')+1]
        control_object = str(work / f'{name}.o')
        compile_args[compile_args.index('-o')+1] = control_object
        if macro:
            compile_args.append(f'-D{macro}=1')
        else:
            original = Path(entry['file']).read_text()
            needle = 'std::max(now, store.state().last_event_at)'
            assert original.count(needle) == 1
            replacement = 'now + std::chrono::duration_cast<app_model::Timestamp>(std::chrono::milliseconds{step.offset_ms})'
            dispatch = 'emit_status(store, status_timeline_step_at(g_index), now);'
            assert original.count(dispatch) == 1
            mutated = work / source_name
            mutated.write_text(original.replace(needle,replacement).replace(dispatch,
                'emit_status(store, status_timeline_step_at(g_index), g_base);'))
            compile_args[compile_args.index(entry['file'])] = str(mutated)
        subprocess.run(compile_args,cwd=entry['directory'],check=True)
        control_link = [control_object if arg == original_object else arg for arg in link]
        assert control_object in control_link
        binary = str(work / name)
        control_link[control_link.index('-o')+1] = binary
        subprocess.run(control_link,cwd=build,check=True)
        run = subprocess.run([binary,f'--gtest_filter={selection}'],capture_output=True,text=True)
        report = run.stdout + run.stderr
        (build / f'control-{name}.log').write_text(report)
        assert run.returncode != 0 and '[  FAILED  ]' in report, name
        assert 'ERROR: AddressSanitizer' not in report and 'runtime error:' not in report, name
        print(f'{name}: original defect restored; regression failed as required')
