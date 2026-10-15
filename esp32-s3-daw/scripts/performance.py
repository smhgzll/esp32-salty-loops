#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reproducible audible 4/6/8-track projects and complete plugin hardware profiling.

Generate: scripts/performance.py generate
Measure: DAW_ADMIN_PASSWORD=... scripts/performance.py board --host esp32daw.local --out /tmp/report.json
The board runner backs up/restores the current project and transport in a finally block.
"""
import argparse
import copy
import json
import math
import os
import signal
from pathlib import Path
import statistics
import struct
import sys
import time
from urllib.parse import quote

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests/web'))
from wsclient import WS, http_req
from sf2gen import make_sf2

INSTRUMENTS = ['drumlab', 'sine', '3xosc', 'fm6', 'acid303', 'macrosynth', 'sampler', 'soundfont']
EFFECTS = ['filter', 'eq', 'compressor', 'distortion', 'utility', 'chorus', 'flanger', 'delay', 'reverb', 'limiter']
COUNTERS = ['overruns', 'underruns', 'cpuGuardDrops', 'noteDrops', 'eventDrops', 'nanResets',
            'fxFailures', 'cmdOverflow', 'retireOverflow', 'audioLate']
DEST = ROOT / 'examples/performance'

def plugin(name, params=None):
    p = {'id': 'org.smhgzll.' + name, 'version': '0.1.0', 'params': params or {}}
    if name == 'sampler':
        p['resources'] = {'sample': 'perf-tone.wav'}
        p['params'] = {'loop': 1, **p['params']}
    elif name == 'soundfont':
        p['resources'] = {'soundfont': 'perf-sine.sf2'}
    return p

def project(names, heavy=False, params=None, effect=None, voices=1):
    p = {'format': 'esp32-daw-project', 'schema': 1,
         'app': {'name': 'esp32-s3-daw', 'version': '0.1.0'},
         'meta': {'name': f'Performance {len(names)} tracks' if heavy else names[0], 'author': 'Performance suite',
                  'bpm': 120, 'timeSig': [4, 4], 'loop': {'on': True, 'start': 0, 'end': 15360},
                  'metronome': False, 'countIn': 0, 'currentPattern': 100, 'ppq': 960},
         'nextId': 1000, 'channels': [], 'patterns': [], 'mixer': [], 'automation': [], 'samples': [],
         'playlist': {'tracks': [{'name': 'Arrangement', 'mute': False, 'solo': False}],
                      'clips': [{'id': 101, 'kind': 'pattern', 'track': 0, 'start': 0, 'len': 15360, 'offset': 0, 'ref': 100}],
                      'markers': []}}
    pattern = {'id': 100, 'name': 'Four bar groove', 'color': '#40c4aa', 'length': 15360, 'swing': 0, 'seqs': []}
    p['patterns'].append(pattern)
    for i, name in enumerate(names, 1):
        par = params if len(names) == 1 else {}
        if heavy:
            par = {'3xosc': {'polyphony': 2}, 'fm6': {'polyphony': 2}, 'macrosynth': {'polyphony': 1},
                   'drumlab': {'mode': 2, 'kick2_decay': .22}, 'sampler': {'loop': 1}}.get(name, {})
        p['channels'].append({'id': i, 'name': name, 'color': '#5fb3ff', 'icon': 'wave-saw', 'vol': .6,
                              'pan': (i % 3 - 1) * .15, 'mute': False, 'solo': False, 'track': i,
                              'root': 36 if name == 'drumlab' else 60, 'plugin': plugin(name, par)})
        notes = []
        for step in range(64):
            if name == 'drumlab':
                if step % 4: continue
                keys, length = [36], 120
            elif name in ('sine', 'acid303'):
                if step % 2: continue
                keys, length = [[36, 36, 43, 39][(step // 4) % 4] + 7 * v % 24 for v in range(voices)], 360
            elif name in ('3xosc', 'fm6', 'soundfont'):
                if step % 8: continue
                root = [60, 56, 58, 55][step // 16]
                keys, length = [root + k for k in ([0, 7] if heavy else [7 * v % 24 for v in range(voices)])], 1440
            else:
                if step % 4: continue
                keys, length = [[60, 67, 63, 70][(step // 4) % 4] + 7 * v % 24 for v in range(voices)], 600
            for key in keys:
                notes.append([len(notes) + 1, step * 240, length, key, .72, 0, 0, .5, 1, 0])
        pattern['seqs'].append({'channel': i, 'stepCount': 64, 'pianoRoll': True,
                                'nextNoteId': len(notes) + 1, 'steps': [], 'notes': notes})
    for i in range(17):
        fx = []
        if heavy and 0 < i <= len(names):
            fx = [{'id': 200 + i * 4, 'enabled': True, 'mix': 1, 'plugin': plugin('fx.filter', {'cutoff': 5000, 'resonance': .18})},
                  {'id': 201 + i * 4, 'enabled': True, 'mix': 1, 'plugin': plugin('fx.eq', {'low_gain': 1, 'mid1_gain': -2})}]
        if i == 0:
            use = [effect] if effect else (['compressor', 'limiter'] if heavy else [])
            fx = [{'id': 300 + k, 'enabled': True, 'mix': 1, 'plugin': plugin('fx.' + e, params if effect else None)} for k, e in enumerate(use)]
        if heavy and i == 9:
            fx = [{'id': 350, 'enabled': True, 'mix': 1, 'plugin': plugin('fx.delay', {'mix': 100, 'feedback': 22, 'time': 180})}]
        if heavy and i == 10:
            fx = [{'id': 351, 'enabled': True, 'mix': 1, 'plugin': plugin('fx.reverb', {'mix': 100, 'size': .55})}]
        p['mixer'].append({'index': i, 'name': 'Master' if i == 0 else f'Insert {i}', 'color': '#8a8f98',
                           'vol': .65 if i == 0 else .8, 'pan': 0, 'mute': False, 'solo': False, 'route': 0,
                           'sends': [{'target': 9, 'level': .1}, {'target': 10, 'level': .05}] if heavy and 0 < i <= len(names) else [], 'fx': fx})
    return p

def generate():
    DEST.mkdir(parents=True, exist_ok=True)
    frames = 22050
    pcm = b''.join(struct.pack('<h', int(8000 * (math.sin(2 * math.pi * 261.6256 * i / 44100) + .3 * math.sin(2 * math.pi * 523.2512 * i / 44100)))) for i in range(frames))
    wav = b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 1, 44100, 88200, 2, 16) + b'data' + struct.pack('<I', len(pcm)) + pcm
    (DEST / 'perf-tone.wav').write_bytes(wav)
    (DEST / 'perf-sine.sf2').write_bytes(make_sf2())
    for n in (4, 6, 8):
        (DEST / f'{n}-tracks.json').write_text(json.dumps(project(INSTRUMENTS[:n], True), indent=2) + '\n')
    for n in (4, 6, 8):
        p = project(INSTRUMENTS[:n], True)
        p['meta']['name'] = f'Performance {n} tracks shared buses'
        # Identical linear filter/EQ chains commute with summing. Share them on a tone bus
        # instead of spending the same coefficients and filter work on every insert.
        for t in p['mixer']:
            if 0 < t['index'] <= n:
                t['route'] = 11
                t['fx'] = []
        p['mixer'][11]['name'] = 'Shared tone bus'
        p['mixer'][11]['vol'] = 1
        p['mixer'][11]['fx'] = [
            {'id': 390, 'enabled': True, 'mix': 1, 'plugin': plugin('fx.filter', {'cutoff': 5000, 'resonance': .18})},
            {'id': 391, 'enabled': True, 'mix': 1, 'plugin': plugin('fx.eq', {'low_gain': 1, 'mid1_gain': -2})}]
        # Sends after the shared tone chain retain its coloration.
        p['mixer'][11]['sends'] = [{'target': 9, 'level': .1}, {'target': 10, 'level': .05}]
        for t in p['mixer']:
            if 0 < t['index'] <= n: t['sends'] = []
        (DEST / f'{n}-tracks-shared.json').write_text(json.dumps(p, indent=2) + '\n')
        music = copy.deepcopy(p)
        music['meta']['name'] = f'{n} track live groove'
        # Keep all instruments live, but arrange the expensive leads as a call/response.
        # This is a music example with a CPU budget, distinct from the dense stress fixture.
        music['mixer'][10]['fx'] = []
        music['mixer'][11]['sends'] = [{'target': 9, 'level': .13}]
        music['mixer'][0]['fx'].append({'id': 392, 'enabled': True, 'mix': 1,
                                      'plugin': plugin('fx.utility')})
        lead_names = [x for x in INSTRUMENTS[:n] if x in ('3xosc', 'fm6', 'acid303', 'macrosynth')]
        for channel, seq in zip(music['channels'], music['patterns'][0]['seqs']):
            name = channel['name']
            par = {'polyphony': 1} if name not in ('drumlab', 'acid303', 'soundfont') else {}
            par.update({'drumlab': {'mode': 2, 'kick2_decay': .12}, 'sine': {'release': 30},
                        '3xosc': {'amp_release': 40, 'flt_release': 40},
                        'fm6': {f'op{k}_release': 40 for k in range(1, 7)},
                        'macrosynth': {'release': 40}, 'sampler': {'loop': 1, 'release': 40}}.get(name, {}))
            channel['plugin']['params'] = par
            notes = []
            for beat in range(16):
                root = [60, 56, 58, 55][beat // 4]
                if name == 'drumlab':
                    tick, length, key = beat * 960, 120, 36
                elif name == 'sine':
                    tick, length, key = beat * 960, 600, root - 24
                elif name in lead_names:
                    if beat % len(lead_names) != lead_names.index(name): continue
                    tick, length, key = beat * 960 + 480, 240, root + (7 if beat % 2 else 0)
                elif name == 'sampler':
                    if beat % 2 == 0: continue
                    tick, length, key = beat * 960 + 240, 240, root + 12
                else:
                    if beat % 4: continue
                    tick, length, key = beat * 960 + 240, 480, root + 7
                notes.append([len(notes) + 1, tick, length, key, .72, 0, 0, .5, 1, 0])
            seq['notes'] = notes
            seq['nextNoteId'] = len(notes) + 1
        (DEST / f'{n}-tracks-live.json').write_text(json.dumps(music, indent=2) + '\n')
    print('Generated 4/6/8-track dense stress and shared-bus music projects with actual WAV/SF2 resources.')

def board(a):
    def request(method, path, body=None, auth=False):
        headers = {'Origin': f'http://{a.host}', 'Content-Type': 'application/json'}
        if auth: headers['Authorization'] = 'Bearer ' + token
        code, data = http_req(a.host, 80, method, path, body=body, headers=headers, timeout=40)
        if code != 200: raise RuntimeError(f'{method} {path}: {code}: {data[:200]!r}')
        return json.loads(data)
    original = request('GET', '/api/project')
    status = request('GET', '/api/status')
    token = request('POST', '/api/login', json.dumps({'password': os.environ['DAW_ADMIN_PASSWORD']}))['token']
    out = Path(a.out)
    out.with_suffix('.project-backup.json').write_text(json.dumps(original, indent=2))
    ws = WS(a.host, 80, origin=f'http://{a.host}:80')
    ws.hello()
    def command(message):
        response = ws.request(message)
        if response['t'] != 'ack': raise RuntimeError(str(response))
        return response
    original_state = ws.wait(lambda m: m.get('t') == 'st')
    report = {'before': status, 'cases': [], 'restored': False}
    cases = [(name, project([name])) for name in INSTRUMENTS]
    cases += [('fx.' + name, project(['sine'], effect=name)) for name in EFFECTS]
    if a.extended:
        cases += [(f'drumlab.mode.{n}', project(['drumlab'], params={'mode': n})) for n in range(9)]
        # Kit mode also exercises the GM mapping of every individual drum, not only kick.
        kit = project(['drumlab'], params={'mode': 0})
        notes = kit['patterns'][0]['seqs'][0]['notes']
        keys = [35, 36, 38, 40, 39, 42, 46, 45]
        for i, note in enumerate(notes): note[3] = keys[i % len(keys)]
        cases.append(('drumlab.kit.all-pieces', kit))
        cases += [(f'macrosynth.model.{n}', project(['macrosynth'], params={'model': n})) for n in range(48)]
        cases += [(f'fm6.algorithm.{n}', project(['fm6'], params={'algorithm': n, **{f'op{k}_level': 85 for k in range(1, 7)}})) for n in range(1, 33)]
        cases += [(f'3xosc.wave.{n}', project(['3xosc'], params={f'osc{k}_wave': n for k in range(1, 4)})) for n in range(5)]
        cases += [(f'{name}.voices.{n}', project([name], voices=n, params={'polyphony': n}))
                  for name in INSTRUMENTS if name not in ('acid303', 'drumlab')
                  for n in ((4,) if name == 'macrosynth' else (4, 8))]
    if a.extended:
        for name in ('sine', 'sampler', 'soundfont'):
            n = 24 if name == 'soundfont' else 16
            cases.append((f'{name}.voices.{n}', project([name], voices=n, params={'polyphony': n})))
    cases += [(f'{n}-tracks', json.loads((DEST / f'{n}-tracks.json').read_text())) for n in (4, 6, 8)]
    cases += [(f'{n}-tracks-shared', json.loads((DEST / f'{n}-tracks-shared.json').read_text())) for n in (4, 6, 8)]
    cases += [(f'{n}-tracks-live', json.loads((DEST / f'{n}-tracks-live.json').read_text())) for n in (4, 6, 8)]
    if a.project:
        cases = [(Path(file).stem, json.loads(Path(file).read_text())) for file in a.project]
    if a.only: cases = [(name, p) for name, p in cases if a.only in name]
    try:
        command({'t': 'stop'}); command({'t': 'panic'})
        if not a.no_upload_resources:
            for name in ('perf-tone.wav', 'perf-sine.sf2'):
                request('POST', '/api/samples?name=' + quote(name), (DEST / name).read_bytes())
        for name, p in cases:
            command({'t': 'stop'}); command({'t': 'panic'})
            request('POST', '/api/project', json.dumps(p), True)
            ws.inbox.clear()
            command({'t': 'mode', 'mode': a.mode})
            initial = ws.wait(lambda m: m.get('t') == 'diag')
            command({'t': 'play'})
            diags, states = [], []
            end = time.monotonic() + a.seconds + 2
            while time.monotonic() < end:
                m = ws.recv(timeout=5)
                if m.get('t') == 'diag': diags.append(m)
                if m.get('t') == 'st' and time.monotonic() >= end - a.seconds: states.append(m)
            ds = diags[2:]
            if len(ds) < 2: raise RuntimeError('missing diagnostics: ' + name)
            if not ds[-1]['plugins']: raise RuntimeError('No plugins instantiated; silent project cannot be benchmarked: ' + name)
            row = {'case': name, 'cpuMean': round(statistics.mean(d['cpu'] for d in ds), 2),
                   'cpuMax': max(d['cpu'] for d in ds), 'renderUsAvg': round(statistics.mean(d['renderUsAvg'] for d in ds)),
                   'budgetUs': ds[-1]['budgetUs'], 'heapInternal': ds[-1]['heapInternal'], 'psramFree': ds[-1]['psramFree'],
                   'workerWait': round(statistics.mean(d.get('workerWait', 0) for d in ds), 2),
                   'workerBusy': round(statistics.mean(d.get('workerBusy', 0) for d in ds), 2),
                   'counterDeltas': {k: ds[-1][k] - ds[0][k] for k in COUNTERS},
                   'startupCounterDeltas': {k: ds[0][k] - initial[k] for k in COUNTERS},
                   'channelPeaks': [max(s['cm'][i] for s in states) for i in range(len(p['channels']))],
                   'plugins': ds[-1]['plugins'], 'seconds': (ds[-1]['uptimeMs'] - ds[0]['uptimeMs']) / 1000}
            for unit in row['plugins']:
                observations = [x for d in ds for x in d['plugins'] if (x['where'], x['id']) == (unit['where'], unit['id'])]
                unit['cpuMean'] = round(statistics.mean(x['cpu'] for x in observations), 2)
                unit['cpuMax'] = max(x['cpu'] for x in observations)
                unit['voicesMax'] = max(x['voices'] for x in observations)
                sources = p['channels'] if unit['where'] == 'channel' else [f for t in p['mixer'] for f in t['fx']]
                unit['pluginId'] = next(x['plugin']['id'] for x in sources if x['id'] == unit['id'])
            pattern_ids = ({p['meta']['currentPattern']} if a.mode == 'pattern' else
                           {clip['ref'] for clip in p['playlist']['clips'] if clip['kind'] == 'pattern'})
            scheduled = {seq['channel'] for pat in p['patterns'] if pat['id'] in pattern_ids
                         for seq in pat['seqs'] if seq.get('notes') or any(step[0] for step in seq.get('steps', []))}
            row['scheduledChannels'] = sorted(scheduled)
            row['mode'] = a.mode
            row['expectedPlugins'] = len(p['channels']) + sum(len(t['fx']) for t in p['mixer'])
            row['loadedPlugins'] = len(ds[-1]['plugins'])
            row['complete'] = (row['loadedPlugins'] == row['expectedPlugins']
                               and all(v == 0 for v in row['counterDeltas'].values())
                               and all(v == 0 for v in row['startupCounterDeltas'].values())
                               and all(row['channelPeaks'][i] > 0 for i, channel in enumerate(p['channels'])
                                       if channel['id'] in scheduled))
            report['cases'].append(row)
            out.write_text(json.dumps(report, indent=2))
            print(name, row['cpuMean'], 'max', row['cpuMax'], 'wait', row['workerWait'], 'worker', row['workerBusy'], 'RAM', row['heapInternal'], 'audible', row['channelPeaks'], 'drops', row['counterDeltas'], flush=True)
    finally:
        command({'t': 'stop'}); command({'t': 'panic'})
        request('POST', '/api/project', json.dumps(original), True)
        command({'t': 'mode', 'mode': original_state['mode']})
        command({'t': 'seek', 'tick': original_state['tick']})
        if original_state['play']:
            command({'t': 'play'})
            if original_state['play'] == 2: command({'t': 'pause'})
        report['restored'] = request('GET', '/api/project') == original
        report['after'] = request('GET', '/api/status')
        out.write_text(json.dumps(report, indent=2))
        ws.close()
        print('Original project restored:', report['restored'], flush=True)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('command', choices=['generate', 'board'])
    p.add_argument('--host', default='esp32daw.local')
    p.add_argument('--out', default='/tmp/esp32-performance.json')
    p.add_argument('--seconds', type=int, default=8)
    p.add_argument('--extended', action='store_true')
    p.add_argument('--only')
    p.add_argument('--mode', choices=['pattern', 'song'], default='pattern', help='transport used for every benchmark')
    p.add_argument('--no-upload-resources', action='store_true', help='reuse existing samples; avoid unnecessary flash writes')
    p.add_argument('--project', action='append', help='profile only this project JSON (repeatable); referenced samples must already be uploaded')
    a = p.parse_args()
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    generate() if a.command == 'generate' else board(a)

if __name__ == '__main__': main()
