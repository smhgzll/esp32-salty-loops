#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only Chromium UI profile. Does not change project data or transport.

Measures this browser's task time, heap, drawing and observer lifecycle while the connected
device runs. Timings are browser measurements, not ESP32 audio CPU measurements.
"""
import argparse
import json
import time
from pathlib import Path
from playwright.sync_api import sync_playwright

INSTRUMENT = """(() => {
  const stats = window.__uiProfile = {created: 0, disconnected: 0, active: 0,
    callbacks: 0, raf: 0, canvasClears: 0};
  const RO = window.ResizeObserver;
  window.ResizeObserver = class extends RO {
    constructor(cb) { super((entries, observer) => { stats.callbacks++; cb(entries, observer); });
      stats.created++; this.targets = new Set(); }
    observe(el, options) { if (!this.targets.size) stats.active++; this.targets.add(el); super.observe(el, options); }
    unobserve(el) { const removed = this.targets.delete(el); if (removed && !this.targets.size) stats.active--; super.unobserve(el); }
    disconnect() { if (this.targets.size) stats.active--; this.targets.clear(); stats.disconnected++; super.disconnect(); }
  };
  const raf = window.requestAnimationFrame;
  window.requestAnimationFrame = cb => { stats.raf++; return raf.call(window, cb); };
  const clear = CanvasRenderingContext2D.prototype.clearRect;
  CanvasRenderingContext2D.prototype.clearRect = function(...args) { stats.canvasClears++; return clear.apply(this, args); };
})();"""

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', required=True)
    parser.add_argument('--seconds', type=int, default=20)
    parser.add_argument('--out', default='/tmp/esp32-ui-profile.json')
    a = parser.parse_args()
    with sync_playwright() as p:
        browser = p.chromium.launch(args=['--no-sandbox'])
        ctx = browser.new_context(viewport={'width': 1440, 'height': 900}, has_touch=True)
        page = ctx.new_page()
        errors = []
        page.on('pageerror', lambda e: errors.append(str(e)))
        page.add_init_script(INSTRUMENT)
        page.goto(a.url)
        page.wait_for_selector('.ch-name')
        page.wait_for_timeout(2000)
        cdp = ctx.new_cdp_session(page)
        cdp.send('Performance.enable')
        def metrics():
            return {x['name']: x['value'] for x in cdp.send('Performance.getMetrics')['metrics']}
        before = metrics()
        playing_before = page.evaluate('window.__daw.live.state.status?.play ?? 0')
        initial = page.evaluate('({...window.__uiProfile})')
        start = time.monotonic()
        page.wait_for_timeout(a.seconds * 1000)
        elapsed = time.monotonic() - start
        after = metrics()
        playing_after = page.evaluate('window.__daw.live.state.status?.play ?? 0')
        measured = page.evaluate('({...window.__uiProfile})')
        # Editors repeatedly mounted/unmounted must release their observers.
        for key in ('F7', 'F5'):
            for _ in range(4):
                page.keyboard.press(key); page.wait_for_timeout(80)
                page.keyboard.press(key); page.wait_for_timeout(80)
        page.wait_for_timeout(300)
        lifecycle = page.evaluate('({...window.__uiProfile})')
        page.locator('.ch-name').first.click()
        page.wait_for_selector('.floating .pe')
        page.locator('.floating button[aria-label="Close"] svg').click()
        page.wait_for_selector('.floating', state='hidden')
        page.locator('.ch-name').first.click()
        page.wait_for_selector('.floating .pe')
        page.locator('.floating button[aria-label="Close"] svg').tap()
        page.wait_for_selector('.floating', state='hidden')
        result = {'url': a.url, 'seconds': round(elapsed, 2),
                  'playingBefore': playing_before, 'playingAfter': playing_after,
                  'browserTaskPercent': round(100 * (after['TaskDuration'] - before['TaskDuration']) / elapsed, 2),
                  'jsHeapBefore': before['JSHeapUsedSize'], 'jsHeapAfter': after['JSHeapUsedSize'],
                  'domNodesBefore': before['Nodes'], 'domNodesAfter': after['Nodes'],
                  'initial': initial, 'measured': measured, 'afterEditorToggles': lifecycle,
                  'observerLeak': lifecycle['active'] > initial['active'],
                  'closeMouseAndTouch': True, 'errors': errors,
                  'assets': page.locator('script[src]').evaluate_all('(ss)=>ss.map(s=>s.src)')}
        Path(a.out).write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2))
        assert not errors and not result['observerLeak']
        ctx.close(); browser.close()

if __name__ == '__main__': main()
