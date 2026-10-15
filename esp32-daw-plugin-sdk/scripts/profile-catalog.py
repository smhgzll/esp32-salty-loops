#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Audible catalog/preset/model/polyphony profiling with reference WAVs.

Generate resources with ../esp32-s3-daw/scripts/performance.py generate first.
Timing is for this host; ESP32 measurements are collected by performance.py board.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tool', default=str(ROOT / 'build-host/bin/dawplug'))
    parser.add_argument('--out', required=True)
    parser.add_argument('--render-dir', required=True)
    parser.add_argument('--seconds', default='2')
    parser.add_argument('--resources', default=str(ROOT.parent / 'esp32-s3-daw/examples/performance'))
    a = parser.parse_args()
    dest = Path(a.render_dir); dest.mkdir(parents=True, exist_ok=True)
    result = []
    for line in subprocess.check_output([a.tool, 'list'], text=True).splitlines():
        pid = line.split()[0]
        if not pid.startswith('org.smhgzll.'): continue
        info = json.loads(subprocess.check_output([a.tool, 'info', pid], text=True))
        cases = [(f'preset.{i}', ['--preset', str(i)]) for i in range(len(info['presets']))]
        for par in info['parameters']:
            if par['type'] == 'enum' or par['key'] == 'algorithm':
                for n in range(int(par['min']), int(par['max']) + 1):
                    cases.append((f"{par['key']}.{n}", ['--param', f"{par['key']}={n}"]))
        if info['type'] == 'instrument':
            for n in sorted({1, min(4, info['maxVoices']), min(8, info['maxVoices']), info['maxVoices']}):
                cases.append((f'voices.{n}', ['--voices', str(n)]))
        resources = []
        if pid.endswith('.sampler'):
            resources = ['--resource', 'sample=' + str(Path(a.resources) / 'perf-tone.wav'), '--param', 'loop=1']
        if pid.endswith('.soundfont'):
            resources = ['--resource', 'soundfont=' + str(Path(a.resources) / 'perf-sine.sf2')]
        for name, args in cases:
            row = json.loads(subprocess.check_output([a.tool, 'bench', pid, '--json', '--seconds', a.seconds, *resources, *args], text=True))
            row['case'] = pid + '.' + name
            wav = dest / (row['case'] + '.wav')
            render_args = [v for i, v in enumerate(args) if v != '--voices' and (i == 0 or args[i - 1] != '--voices')]
            subprocess.run([a.tool, 'render', pid, '--seconds', a.seconds, '--out', str(wav), *resources, *render_args], check=True, stdout=subprocess.DEVNULL)
            row['wavSha256'] = hashlib.sha256(wav.read_bytes()).hexdigest()
            row['audible'] = row['rms'] > 1e-6
            result.append(row)
        print(pid, len(cases), 'cases', flush=True)
    Path(a.out).write_text(json.dumps({'target': 'host', 'cases': result}, indent=2) + '\n')
    assert all(r['rtViolations'] == 0 for r in result), 'real-time allocation/logging violations'
    print(len(result), 'cases; silent:', [r['case'] for r in result if not r['audible']], flush=True)

if __name__ == '__main__': main()
