#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Browser tests of the DAW UI (Playwright, Chromium): real pointer / touch editing in every editor,
checked against the device's state (GET /api/project), plus screenshots for review.

  tests/browser/test_ui.py --url http://127.0.0.1:18190/ [--out tests/browser/out]
"""
import argparse
import json
import math
import os
import re
import sys
import tempfile
import time
import urllib.error
import urllib.request

from playwright.sync_api import sync_playwright

FAILS = []
ADMIN = os.environ.get("DAW_TEST_ADMIN", "browser-test")


def check(cond, what):
    print(("PASS  " if cond else "FAIL  ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def project(url):
    with urllib.request.urlopen(url.rstrip("/") + "/api/project", timeout=10) as r:
        return json.loads(r.read())


def wait_for(fn, timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            v = fn()
            if v:
                return v
        except Exception:
            pass
        time.sleep(0.1)
    return None


def chan(p, name):
    return next(c for c in p["channels"] if c["name"] == name)


def seq(p, pat_name, ch_name):
    """a channel's row in a pattern; the device leaves out empty rows and all-off steps"""
    pat = next(x for x in p["patterns"] if x["name"] == pat_name)
    cid = chan(p, ch_name)["id"]
    s = next((s for s in pat["seqs"] if s["channel"] == cid), None) or {"channel": cid, "stepCount": 16, "notes": []}
    if "steps" not in s:
        s["steps"] = [[0, 0.78, 0, 0, 0, 1] for _ in range(s.get("stepCount", 16))]
    return s


def desktop(b, url, out):
    ctx = b.new_context(viewport={"width": 1366, "height": 768})
    page = ctx.new_page()
    errors = []
    page.on("pageerror", lambda e: errors.append(str(e)))
    page.goto(url)
    page.wait_for_selector(".shell")
    page.wait_for_timeout(500)

    # ---- channel rack: click a step, paint four, undo ------------------------------------------
    rows = page.locator(".rack-row")
    kick = rows.filter(has_text="Kick").first
    kick.locator(".step").nth(1).click()
    check(wait_for(lambda: seq(project(url), "Beat", "Kick")["steps"][1][0] == 1), "click: Kick step 2 is on the device")
    lead = rows.filter(has_text="Lead").first
    lead.scroll_into_view_if_needed()
    s0 = lead.locator(".step").nth(0).bounding_box()
    s3 = lead.locator(".step").nth(3).bounding_box()
    page.mouse.move(s0["x"] + 5, s0["y"] + 8)
    page.mouse.down()
    page.mouse.move(s3["x"] + 8, s3["y"] + 8, steps=8)
    page.mouse.up()
    check(wait_for(lambda: [st[0] for st in seq(project(url), "Beat", "Lead")["steps"][:4]] == [1, 1, 1, 1]), "drag: four Lead steps painted in one gesture")
    page.keyboard.press("Control+z")
    check(wait_for(lambda: [st[0] for st in seq(project(url), "Beat", "Lead")["steps"][:4]] == [0, 0, 0, 0]), "Ctrl+Z undoes the painted steps")
    page.keyboard.press("Control+y")
    check(wait_for(lambda: seq(project(url), "Beat", "Lead")["steps"][0][0] == 1), "Ctrl+Y redoes them")

    # ---- piano roll: Bass in Bassline. Draw, move, delete ----------------------------------------
    page.locator(".patsel .btn").nth(1).first.click()  # next pattern -> Bassline
    check(wait_for(lambda: project(url)["meta"]["currentPattern"] == next(x for x in project(url)["patterns"] if x["name"] == "Bassline")["id"]),
          "next-pattern button selects Bassline")
    roll = page.locator(".panel").filter(has_text="Piano roll")
    roll.locator("select").first.select_option(label="Bass")
    page.wait_for_timeout(300)
    canvas = roll.locator("canvas").first
    box = canvas.bounding_box()
    n0 = len(seq(project(url), "Bassline", "Bass")["notes"])
    # an empty spot: bar 2 (the bassline is one bar long), middle of the visible keys
    x, y = box["x"] + 62 + 3840 * 0.11 + 120 * 0.11 + 30, box["y"] + 22 + 14 * 10 + 5
    page.mouse.click(x, y)
    check(wait_for(lambda: len(seq(project(url), "Bassline", "Bass")["notes"]) == n0 + 1), "piano roll: a click draws a note")
    notes = seq(project(url), "Bassline", "Bass")["notes"]
    new = max(notes, key=lambda n: n[1])
    gx = box["x"] + 62 + new[1] * 0.11 + 5  # grab the body near the start (the right edge resizes)
    page.mouse.move(gx, y)
    page.mouse.down()
    page.mouse.move(gx + 960 * 0.11, y - 14 * 2, steps=6)
    page.mouse.up()
    check(wait_for(lambda: any(n[0] == new[0] and n[1] == new[1] + 960 and n[3] == new[3] + 2 for n in seq(project(url), "Bassline", "Bass")["notes"])),
          "piano roll: dragging moves the note a beat later and two keys up")
    page.screenshot(path=os.path.join(out, "desktop-pianoroll.png"))
    page.mouse.click(gx + 960 * 0.11, y - 14 * 2, button="right")
    check(wait_for(lambda: len(seq(project(url), "Bassline", "Bass")["notes"]) == n0), "piano roll: right-click deletes it")

    # ---- playlist: place a clip, then drag it -------------------------------------------------------
    pl = page.locator(".panel").filter(has_text="Playlist").first
    pc = pl.locator("canvas").first
    pb = pc.bounding_box()
    clips0 = len(project(url)["playlist"]["clips"])
    cx, cy = pb["x"] + 132 + 3840 * 10 * 0.018 + 5, pb["y"] + 24 + 38 * 3 + 15
    page.mouse.click(cx, cy)
    check(wait_for(lambda: len(project(url)["playlist"]["clips"]) == clips0 + 1), "playlist: a click places a clip")
    page.mouse.move(cx + 4, cy)
    page.mouse.down()
    page.mouse.move(cx + 4 + 3840 * 0.018 * 2, cy, steps=6)
    page.mouse.up()
    check(wait_for(lambda: any(c["start"] == 3840 * 12 and c["track"] == 3 for c in project(url)["playlist"]["clips"])),
          "playlist: dragging moves the clip two bars")

    # ---- mixer: fader --------------------------------------------------------------------------------
    page.keyboard.press("F9")
    page.wait_for_selector(".strip")
    v0 = project(url)["mixer"][1]["vol"]
    thumb = page.locator(".strip").nth(1).locator(".fader")
    fb = thumb.bounding_box()
    page.mouse.move(fb["x"] + 13, fb["y"] + fb["height"] / 2)
    page.mouse.down()
    page.mouse.move(fb["x"] + 13, fb["y"] + fb["height"] / 2 + 40, steps=5)
    page.mouse.up()
    check(wait_for(lambda: project(url)["mixer"][1]["vol"] < v0 - 0.05), "mixer: dragging insert 1's fader down lowers its volume")
    page.screenshot(path=os.path.join(out, "desktop-mixer.png"))

    # ---- plugin editor: a knob --------------------------------------------------------------------
    page.locator(".ch-name", has_text="Bass").first.click()
    page.wait_for_selector(".floating .pe")
    # A header drag must not capture clicks on the close button (including its SVG child).
    fb = page.locator(".floating").bounding_box()
    head = page.locator(".floating .modal-head span").first.bounding_box()
    page.mouse.move(head["x"] + 12, head["y"] + head["height"] / 2)
    page.mouse.down()
    page.mouse.move(head["x"] + 52, head["y"] + head["height"] / 2 + 25, steps=5)
    page.mouse.up()
    moved = page.locator(".floating").bounding_box()
    check(abs(moved["x"] - fb["x"] - 40) < 2 and abs(moved["y"] - fb["y"] - 25) < 2,
          "plugin window moves when its title is dragged")
    page.locator('.floating .modal-head button[aria-label="Close"] svg').click()
    check(wait_for(lambda: page.locator(".floating").count() == 0), "plugin window closes when its X is clicked")
    page.locator(".ch-name", has_text="Bass").first.click()
    page.wait_for_selector(".floating .pe")
    cut0 = chan(project(url), "Bass")["plugin"]["params"]["cutoff"]
    knob = page.locator(".floating .pe-ctl", has_text="Cutoff").locator("svg").first
    kb = knob.bounding_box()
    page.mouse.move(kb["x"] + kb["width"] / 2, kb["y"] + kb["height"] / 2)
    page.mouse.down()
    page.mouse.move(kb["x"] + kb["width"] / 2, kb["y"] + kb["height"] / 2 - 40, steps=5)
    page.mouse.up()
    check(wait_for(lambda: chan(project(url), "Bass")["plugin"]["params"]["cutoff"] > cut0 * 1.2), "plugin editor: dragging the cutoff knob up raises it")
    page.screenshot(path=os.path.join(out, "desktop-plugin.png"))

    # ---- a user preset: save the current sound on the device, it appears in the preset menu --------
    page.once("dialog", lambda dlg: dlg.accept("Bright Bass"))
    page.locator(".floating .pe-head .btn[title^='Save these settings']").click()
    check(wait_for(lambda: page.evaluate("window.__daw.userPresets.state.list.some(u => u.name === 'Bright Bass' && u.plugin === 'org.smhgzll.3xosc')")),
          "a user preset is saved on the device")
    check(page.locator(".floating select[aria-label='Preset'] optgroup[label^='Mine'] option", has_text="Bright Bass").count() == 1,
          "it is offered in the editor's preset menu")

    # ---- automation: create from the knob's menu, then edit its points --------------------------------
    autos0 = len(project(url)["automation"])
    page.locator(".floating .pe-ctl", has_text="Cutoff").first.click(button="right")
    page.locator(".menu-item", has_text="Create automation clip").click()
    check(wait_for(lambda: len(project(url)["automation"]) == autos0 + 1), "automation clip created from the knob's menu")
    page.wait_for_selector(".auto-cv")
    auto = lambda: max(project(url)["automation"], key=lambda x: x["id"])
    pts0 = auto()["points"]
    check(auto()["target"]["param"] == "cutoff" and len(pts0) == 2, "it targets the cutoff and starts with two points")
    d = page.evaluate("window.__daw.catalog.state.plugins.find(p => p.id === 'org.smhgzll.3xosc').params.find(p => p.key === 'cutoff')")
    cut = chan(project(url), "Bass")["plugin"]["params"]["cutoff"]
    want = math.log(cut / d["min"]) / math.log(d["max"] / d["min"]) if d["scale"] == "log" else (cut - d["min"]) / (d["max"] - d["min"])
    check(pts0[0][1] == pts0[1][1] and abs(pts0[0][1] - want) < 0.01, f"it starts flat at the knob's current value ({cut:.0f} Hz = {want:.3f}, got {pts0[0][1]:.3f})")
    ab = page.locator(".auto-cv").bounding_box()
    w, h = ab["width"] - 52 - 14, ab["height"] - 14 - 26
    length = auto()["length"]
    # an empty spot at 3/4 of the clip, value 0.25 (far from the line from (0, 0.2) to (half, 0.8) and beyond)
    px, py = ab["x"] + 52 + w * 0.75, ab["y"] + 14 + h * 0.75
    page.mouse.click(px, py)
    check(wait_for(lambda: len(auto()["points"]) == 3), "a click adds a point")
    pt = next((q for q in auto()["points"] if abs(q[0] - length * 0.75) <= 240), None)
    check(pt is not None and abs(pt[1] - 0.25) < 0.03, f"the point sits where it was clicked ({pt})")
    page.mouse.move(px, py)
    page.mouse.down()
    page.mouse.move(px, py - h * 0.5, steps=6)
    page.mouse.up()
    check(wait_for(lambda: any(abs(q[1] - 0.75) < 0.03 for q in auto()["points"])), "dragging the point up raises its value")
    page.screenshot(path=os.path.join(out, "desktop-automation.png"))
    page.mouse.click(px, py - h * 0.5, button="right")
    check(wait_for(lambda: len(auto()["points"]) == 2), "right-click deletes the point")
    page.keyboard.press("Control+z")
    check(wait_for(lambda: len(auto()["points"]) == 3), "Ctrl+Z brings it back")
    page.keyboard.press("Escape")
    page.wait_for_timeout(200)
    check(page.locator(".auto-cv").count() == 0, "Escape closes the editor")
    page.locator(".floating .modal-head .btn").click()

    # ---- sample library: upload a WAV, make a Sampler channel from it ------------------------------
    import struct, tempfile, math as _m
    frames = 22050
    pcm = b"".join(struct.pack("<h", int(12000 * _m.sin(2 * _m.pi * 440 * i / 44100))) for i in range(frames))
    wav = b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, 44100, 88200, 2, 16) + b"data" + struct.pack("<I", len(pcm)) + pcm
    tmp = os.path.join(tempfile.mkdtemp(), "UI Tone.wav")
    open(tmp, "wb").write(wav)
    page.locator(".side .tab[title='Samples']").click()
    page.locator("input[aria-label='Upload samples']").set_input_files(tmp)
    lib = lambda: json.loads(urllib.request.urlopen(url.rstrip("/") + "/api/samples").read())["list"]
    check(wait_for(lambda: any(f["name"] == "UI Tone.wav" for f in lib())), "a WAV uploaded from the browser is in the device's sample library")
    page.locator(".sample-item", has_text="UI Tone.wav").first.click()
    def sampler_ok():
        for c in project(url)["channels"]:
            if c["plugin"]["id"] == "org.smhgzll.sampler" and c["plugin"].get("resources", {}).get("sample") == "UI Tone.wav":
                return c
        return None
    smp = wait_for(sampler_ok)
    check(smp is not None, "clicking it creates a Sampler channel playing that file")
    if smp:
        page.wait_for_selector(".floating .pe")  # its editor opens by itself
        check(page.locator(".floating select[aria-label='Sample']").input_value() == "UI Tone.wav", "the Sampler's editor shows the file")
        page.screenshot(path=os.path.join(out, "desktop-sampler.png"))
        page.locator(".floating .modal-head .btn").click()
    # a SoundFont: its editor lists the presets inside the file and selects one by bank / program
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "web"))
    from sf2gen import make_sf2
    tmp2 = os.path.join(tempfile.mkdtemp(), "UI Font.sf2")
    open(tmp2, "wb").write(make_sf2(extra_presets=[("Bright Piano", 0, 1), ("Room Kit", 128, 8)]))
    page.locator("input[aria-label='Upload samples']").set_input_files(tmp2)
    check(wait_for(lambda: any(f["name"] == "UI Font.sf2" for f in lib())), "a SoundFont uploaded from the browser")
    page.locator(".sample-item", has_text="UI Font.sf2").first.click()
    def sf_chan():
        for c in project(url)["channels"]:
            if c["plugin"]["id"] == "org.smhgzll.soundfont" and c["plugin"].get("resources", {}).get("soundfont") == "UI Font.sf2":
                return c
        return None
    sfc = wait_for(sf_chan)
    check(sfc is not None, "clicking it creates a SoundFont channel")
    if sfc:
        sel = page.locator(".floating select[aria-label='SoundFont preset']")
        sel.wait_for(timeout=5000)
        labels = sel.locator("option").all_inner_texts()
        check(labels == ["000:000 Test Sine", "000:001 Bright Piano", "128:008 Room Kit"], f"the editor lists the font's presets ({labels})")
        sel.select_option("128:8")
        def picked():
            c = sf_chan()
            return c and c["plugin"]["params"].get("bank") == 128 and c["plugin"]["params"].get("program") == 8
        check(wait_for(picked), "choosing one sets the channel's bank and program")
        page.screenshot(path=os.path.join(out, "desktop-soundfont.png"))
        page.locator(".floating .modal-head .btn").click()
    page.locator(".side .tab[title='Plugins']").click()

    # ---- transport --------------------------------------------------------------------------------
    page.keyboard.press("Space")
    check(wait_for(lambda: page.evaluate("window.__daw.live.state.status?.play") == 1), "Space starts playback")
    t1 = page.evaluate("window.__daw.live.state.status.tick")
    page.wait_for_timeout(600)
    t2 = page.evaluate("window.__daw.live.state.status.tick")
    check(t2 != t1, "the position display advances while playing")
    page.screenshot(path=os.path.join(out, "desktop-playing.png"))
    page.locator(".topbar .btn[title='Stop (back to the start)']").click()
    check(wait_for(lambda: page.evaluate("window.__daw.live.state.status?.play") == 0), "Stop stops playback")
    # ---- MIDI recording from the computer keyboard ---------------------------------------------------
    lead = chan(project(url), "Lead")
    pat_id = project(url)["meta"]["currentPattern"]
    def lead_notes():
        pat = next(x for x in project(url)["patterns"] if x["id"] == pat_id)
        s = next((q for q in pat["seqs"] if q["channel"] == lead["id"]), None)
        return len(s["notes"]) if s and s.get("pianoRoll") else 0
    n0 = lead_notes()
    page.evaluate(f"window.__daw.ui.set({{ selectedChannel: {lead['id']} }})")
    page.locator(".topbar .btn.rec").click()
    check(wait_for(lambda: page.evaluate("!!window.__daw.live.state.status?.rec")), "the record button arms recording")
    page.mouse.click(5, 760)  # focus off any input
    page.keyboard.press("Space")
    wait_for(lambda: page.evaluate("window.__daw.live.state.status?.play") == 1)
    page.wait_for_timeout(300)
    page.keyboard.down("q")
    page.wait_for_timeout(400)
    page.keyboard.up("q")
    page.wait_for_timeout(300)
    page.locator(".topbar .btn[title='Stop (back to the start)']").click()
    check(wait_for(lambda: lead_notes() == n0 + 1), f"a note played on the computer keyboard is recorded into the pattern ({n0} -> {lead_notes()})")
    page.locator(".topbar .btn.rec").click()
    check(wait_for(lambda: not page.evaluate("!!window.__daw.live.state.status?.rec")), "and disarmed again")
    # ---- CPU guard: a simulated overload (daw-host --test-hooks) skips new notes; the meter says so -
    def cpu_load(permille):
        req = urllib.request.Request(url.rstrip("/") + "/api/test/cpu-load", data=str(permille).encode(), method="POST")
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                return r.status
        except urllib.error.HTTPError as e:  # no test hooks (the firmware): skipped
            return e.code
    meter = page.locator("[data-testid=cpu-meter]")
    if cpu_load(950) == 200:
        page.wait_for_timeout(1500)  # measured, smoothed and published
        page.mouse.click(5, 760)
        page.keyboard.down("q")
        page.wait_for_timeout(200)
        page.keyboard.up("q")
        check(wait_for(lambda: meter.inner_text().strip() == "CPU FULL"), "a note skipped by the CPU guard shows on the load meter")
        meter.screenshot(path=os.path.join(out, "desktop-cpu-guard.png"))
        cpu_load(0)
        page.wait_for_timeout(1500)
        page.keyboard.down("q")
        page.wait_for_timeout(200)
        page.keyboard.up("q")
        check(wait_for(lambda: "%" in meter.inner_text(), timeout=6), "the meter recovers when the load drops")
    # ---- device settings (admin login prompt answered by the test) ---------------------------------
    page.keyboard.press("Escape")
    page.evaluate("window.__daw.ui.set(s => ({ panels: { ...s.panels, settings: true } }))")
    page.once("dialog", lambda dlg: dlg.accept(ADMIN))
    page.locator(".btn", has_text="Device settings…").click()
    page.wait_for_selector("input[aria-label='Device name']")
    page.locator("input[aria-label='Device name']").fill("browser-daw")
    page.locator(".modal button[type=submit]").click()
    check(wait_for(lambda: page.locator(".btn", has_text="Restart now").count() == 1), "device settings saved from the browser")
    page.screenshot(path=os.path.join(out, "desktop-device-settings.png"))
    page.keyboard.press("Escape")
    check(not errors, f"no page errors ({'; '.join(errors)[:200]})")
    return ctx


def sync(b, url):
    """two browsers: an edit in one appears in the other"""
    a = b.new_context(viewport={"width": 1366, "height": 768}).new_page()
    c = b.new_context(viewport={"width": 1366, "height": 768}).new_page()
    for p in (a, c):
        p.goto(url)
        p.wait_for_selector(".shell")
    a.locator(".patsel .name").first.click()
    a.locator(".menu-item", has_text="Beat").first.click()
    a.wait_for_timeout(300)
    on0 = c.evaluate("window.__daw.proj.state.project.patterns.find(p=>p.name==='Beat').seqs[0].steps[7][0]")
    a.locator(".rack-row").first.locator(".step").nth(7).click()
    check(wait_for(lambda: c.evaluate("window.__daw.proj.state.project.patterns.find(p=>p.name==='Beat').seqs[0].steps[7][0]") != on0),
          "sync: a step toggled in one browser appears in the other")


def playback_reload(b, url):
    """Opening and resizing editors while the device is already playing must stay responsive."""
    ctx = b.new_context(viewport={"width": 1366, "height": 768}, has_touch=True)
    page = ctx.new_page()
    errors = []
    page.on("pageerror", lambda e: errors.append(str(e)))
    page.goto(url)
    page.wait_for_selector(".ch-name")
    for mode in ("pattern", "song"):
        page.locator(".topbar .btn", has_text="PAT" if mode == "pattern" else "SONG").click()
        page.locator(".topbar .btn[title='Play / pause (Space)']").click()
        check(wait_for(lambda: page.evaluate("window.__daw.live.state.status?.play") == 1),
              f"reload test: {mode} playback starts")
        page.reload()
        page.wait_for_selector(".ch-name")
        # Force a narrow canvas too: the follow margin cannot fit even after layout.
        for title, width in (("Piano roll", 90), ("Playlist", 180)):
            page.locator(".panel").filter(has_text=title).locator(".cv-wrap").evaluate(
                "(el, width) => el.style.width = `${width}px`", width)
        page.wait_for_timeout(400)
        page.locator(".ch-name", has_text="Kick").first.click()
        page.wait_for_selector(".floating .pe")
        page.locator('.floating button[aria-label="Close"] svg').tap()
        check(wait_for(lambda: page.locator(".floating").count() == 0),
              f"{mode} playback: editor closes by touch after reload and narrow layout")
        page.locator(".topbar .btn[title='Stop (back to the start)']").click()
        page.wait_for_timeout(200)
    check(not errors, f"playing reload / narrow layout has no page errors ({'; '.join(errors)[:200]})")
    ctx.close()


def mobile(b, url, out):
    ctx = b.new_context(viewport={"width": 390, "height": 844}, has_touch=True, is_mobile=True)
    page = ctx.new_page()
    page.goto(url)
    page.wait_for_selector(".mshell")
    page.locator(".mtab", has_text="Patterns").tap()
    page.wait_for_selector(".rack-row")
    clap = page.locator(".rack-row").filter(has_text="Clap").first
    st0 = seq(project(url), next(x["name"] for x in project(url)["patterns"] if x["id"] == project(url)["meta"]["currentPattern"]), "Clap")
    clap.locator(".step").nth(2).tap()
    check(wait_for(lambda: seq(project(url), next(x["name"] for x in project(url)["patterns"] if x["id"] == project(url)["meta"]["currentPattern"]), "Clap")["steps"][2][0] != (st0["steps"][2][0] if st0 else 0)),
          "phone: tapping a step pad toggles it on the device")
    for tab in ("Song", "Piano", "Mixer", "Sound", "More"):
        page.locator(".mtab", has_text=tab).tap()
        page.wait_for_timeout(400)
        page.screenshot(path=os.path.join(out, f"phone-{tab.lower()}.png"))
    check(page.locator(".mrow", has_text="Device").count() == 1, "phone: the More tab lists the device page")
    # the piano tab: tap draws a note
    page.locator(".mtab", has_text="Piano").tap()
    page.wait_for_timeout(300)
    pr = page.locator(".mshell canvas").first.bounding_box()
    before = sum(len(s["notes"]) for p in project(url)["patterns"] for s in p["seqs"])
    page.touchscreen.tap(pr["x"] + 44 + 200, pr["y"] + 22 + 18 * 6 + 4)
    check(wait_for(lambda: sum(len(s["notes"]) for p in project(url)["patterns"] for s in p["seqs"]) == before + 1), "phone: a tap in the piano roll adds a note")
    # landscape
    page.set_viewport_size({"width": 844, "height": 390})
    page.wait_for_timeout(400)
    page.screenshot(path=os.path.join(out, "phone-landscape-piano.png"))
    ctx.close()


def midi_import(b, url, out):
    """MIDI files (docs/MIDI.md): a format 1 file as a new project that plays exactly the file's
    notes, a part of a format 0 file into the piano roll (one undo step), a public-domain piano piece
    (fits a project slot), and a file that is not MIDI"""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "web"))
    from midigen import DIV, groove
    from wsclient import WS

    tmp = tempfile.mkdtemp()
    f1, f0, bad = (os.path.join(tmp, n) for n in ("groove-format1.mid", "groove-format0.mid", "notes.mid"))
    data1, notes = groove(1)
    open(f1, "wb").write(data1)
    open(f0, "wb").write(groove(0)[0])
    open(bad, "wb").write(b"these are not MIDI bytes")
    ctx = b.new_context(viewport={"width": 1366, "height": 768})
    page = ctx.new_page()
    errors = []
    page.on("pageerror", lambda e: errors.append(str(e)))
    page.goto(url)
    page.wait_for_selector(".shell")
    page.wait_for_timeout(300)

    def import_project(path):
        page.locator(".topbar .btn", has_text="File").click()
        with page.expect_file_chooser() as fc:
            page.locator(".menu-item", has_text="Import MIDI file").click()
        fc.value.set_files(path)

    # ---- as a project ---------------------------------------------------------------------------
    import_project(f1)
    dlg = page.locator(".modal[aria-label='Import a MIDI file as a project']")
    dlg.wait_for(timeout=5000)
    plan = dlg.locator("[data-testid=midi-plan]").inner_text()
    check("4 channels" in plan, f"MIDI: the dialog plans a channel per part ({plan})")
    load = dlg.locator("[data-testid=midi-load]").inner_text()
    check("Estimated DSP load" in load and "skip notes" not in load, f"MIDI: the dialog estimates the DSP load ({load})")
    page.screenshot(path=os.path.join(out, "desktop-midi-import.png"))
    page.once("dialog", lambda d: d.accept())
    dlg.locator(".buttons .btn", has_text="Import").click()
    check(wait_for(lambda: project(url)["meta"]["name"] == "Salty Groove", 30), "MIDI: the file becomes a new project named after the song")
    check(wait_for(lambda: page.locator(".modal[aria-label='Import a MIDI file as a project']").count() == 0, 30), "MIDI: the dialog closes when done")
    p = project(url)
    names = [c["name"] for c in p["channels"]]
    check(names == ["Drums", "Bass", "Keys", "Lead"], f"MIDI: a channel per part ({names})")
    plugins = {c["name"]: c["plugin"]["id"] for c in p["channels"]}
    check(plugins.get("Drums") == "org.smhgzll.drumlab" and plugins.get("Keys") == "org.smhgzll.fm6" and plugins.get("Bass") == "org.smhgzll.3xosc",
          f"MIDI: GM programs pick built-in instruments ({plugins})")
    poly = {c["name"]: c["plugin"]["params"].get("polyphony") for c in p["channels"]}
    check(poly.get("Bass") == 1 and poly.get("Keys") == 3 and poly.get("Lead") == 1, f"MIDI: each channel gets the voices its part plays at once ({poly})")
    check(abs(p["meta"]["bpm"] - 112) < 0.01 and p["meta"]["timeSig"] == [4, 4], "MIDI: tempo and time signature from the file")
    check([m["name"] for m in p["playlist"]["markers"]] == ["Intro", "Groove", "Lead"], "MIDI: markers on the playlist")
    check(any(a["target"]["kind"] == "tempo" for a in p["automation"]), "MIDI: the tempo change as tempo automation")
    pats = {x["id"]: x for x in p["patterns"]}
    ch_name = {c["id"]: c["name"] for c in p["channels"]}
    played = {n: [] for n in names}
    for c in p["playlist"]["clips"]:
        if c["kind"] == "pattern":
            for s in pats[c["ref"]]["seqs"]:
                for n in s["notes"]:
                    played[ch_name[s["channel"]]].append((c["start"] + n[1], n[2], n[3]))
    k = 960 // DIV
    same = all(sorted(played[n]) == sorted((st * k, ln * k, 46 if key == 49 else key) for st, ln, key, _ in notes[n]) for n in names)
    check(same, "MIDI: the playlist plays exactly the file's notes (the crash on DrumLab's open hat)")
    stored = sum(len(s["notes"]) for x in p["patterns"] for s in x["seqs"])
    total = sum(len(v) for v in notes.values())
    check(stored < total * 0.6, f"MIDI: repeated bars share patterns ({stored} notes stored for {total})")
    check(wait_for(lambda: page.evaluate("window.__daw.live.state.status?.mode") == "song"), "MIDI: the project opens in Song mode")
    page.mouse.click(5, 760)
    page.keyboard.press("Space")
    t1 = wait_for(lambda: page.evaluate("window.__daw.live.state.status?.play") == 1 and page.evaluate("window.__daw.live.state.status.tick"))
    page.wait_for_timeout(500)
    check(t1 is not None and page.evaluate("window.__daw.live.state.status.tick") > t1, "MIDI: the imported song plays")
    page.locator(".topbar .btn[title='Stop (back to the start)']").click()
    wait_for(lambda: page.evaluate("window.__daw.live.state.status?.play") == 0)
    page.screenshot(path=os.path.join(out, "desktop-midi-project.png"))

    # ---- one part into the piano roll ---------------------------------------------------------------
    keys = next(c for c in p["channels"] if c["name"] == "Keys")
    cur = p["meta"]["currentPattern"]
    def keys_notes():
        pat = next(x for x in project(url)["patterns"] if x["id"] == cur)
        s = next((q for q in pat["seqs"] if q["channel"] == keys["id"]), None)
        return sorted((n[1], n[2], n[3]) for n in s["notes"]) if s else []
    before = keys_notes()
    page.evaluate(f"window.__daw.ui.set({{ pianoChannel: {keys['id']}, selectedChannel: {keys['id']} }})")
    with page.expect_file_chooser() as fc:
        page.locator(".btn[title='Import a MIDI file into this channel']").click()
    fc.value.set_files(f0)
    dlg = page.locator(".modal[aria-label='Import MIDI into Keys']")
    dlg.wait_for(timeout=5000)
    check(dlg.locator("input[aria-label='Import Electric Bass (finger)']").is_checked(), "MIDI: the piano roll import offers the first melodic part")
    dlg.locator("input[aria-label='To bar']").fill("4")
    plan = dlg.locator("[data-testid=midi-plan]").inner_text()
    check("32 notes over 4 bars" in plan, f"MIDI: four bars of the bass ({plan})")
    page.screenshot(path=os.path.join(out, "desktop-midi-piano.png"))
    dlg.locator(".buttons .btn", has_text="Import").click()
    want = sorted((st * k, ln * k, key) for st, ln, key, _ in notes["Bass"] if st < 4 * 4 * DIV)
    check(wait_for(lambda: keys_notes() == want), "MIDI: the notes are in the Keys row of the current pattern")
    page.mouse.click(5, 760)
    page.keyboard.press("Control+z")
    check(wait_for(lambda: keys_notes() == before), "MIDI: one Ctrl+Z takes the import back")

    # ---- a public-domain piece: fits a project slot; the size estimate is close ------------------------
    ent = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "examples", "midi", "joplin-the-entertainer.mid")
    import_project(ent)
    dlg = page.locator(".modal[aria-label='Import a MIDI file as a project']")
    dlg.wait_for(timeout=5000)
    plan = dlg.locator("[data-testid=midi-plan]").inner_text()
    page.once("dialog", lambda d: d.accept())
    dlg.locator(".buttons .btn", has_text="Import").click()
    check(wait_for(lambda: project(url)["meta"]["name"] == "The Entertainer", 60), "MIDI: The Entertainer imported")
    wait_for(lambda: page.locator(".modal[aria-label='Import a MIDI file as a project']").count() == 0, 30)
    with urllib.request.urlopen(url.rstrip("/") + "/api/project", timeout=10) as r:
        real = len(r.read())
    est = int(re.search(r"about (\d+) KB", plan).group(1)) * 1024
    check(0.7 < est / real < 1.4, f"MIDI: the size estimate is close ({est // 1024} KB estimated, {real // 1024} KB real)")
    host, port = url.split("//")[1].strip("/").split(":")
    ws = WS(host, int(port))
    ws.hello()
    a = ws.request({"t": "project.save", "slot": 6, "name": "The Entertainer"})
    ws.close()
    check(a.get("t") == "ack", f"MIDI: the imported piece fits a project slot ({a.get('bytes') or a.get('msg')})")

    # ---- GM distortion guitars: a plucked string through Distortion on the channel's insert -------------
    from midigen import guitars
    fg = os.path.join(tmp, "riff.mid")
    open(fg, "wb").write(guitars()[0])
    import_project(fg)
    dlg = page.locator(".modal[aria-label='Import a MIDI file as a project']")
    dlg.wait_for(timeout=5000)
    label = dlg.locator("select[aria-label='Instrument for Rhythm'] option").first.inner_text()
    check("Distortion" in label, f"MIDI: a GM distortion guitar gets a Distortion effect ({label})")
    page.once("dialog", lambda d: d.accept())
    dlg.locator(".buttons .btn", has_text="Import").click()
    check(wait_for(lambda: project(url)["meta"]["name"] == "Riff", 30), "MIDI: the guitar file imported")
    wait_for(lambda: page.locator(".modal[aria-label='Import a MIDI file as a project']").count() == 0, 30)
    pg = project(url)
    def insert_fx(name):
        c = next(x for x in pg["channels"] if x["name"] == name)
        t = next(x for x in pg["mixer"] if x["index"] == c["track"])
        return [(f["plugin"]["id"], f["plugin"]["params"].get("mode"), f["plugin"]["params"].get("drive")) for f in t["fx"]]
    check(insert_fx("Rhythm") == [("org.smhgzll.fx.distortion", 1, 24)] and insert_fx("Lead") == [("org.smhgzll.fx.distortion", 0, 14)],
          f"MIDI: the guitars' inserts hold their Distortion settings ({insert_fx('Rhythm')}, {insert_fx('Lead')})")

    # ---- not a MIDI file ------------------------------------------------------------------------------
    import_project(bad)
    check(wait_for(lambda: page.locator(".toast.error", has_text="not a MIDI file").count() > 0), "MIDI: a file that is not MIDI is refused with the reason")
    check(not errors, f"MIDI: no page errors ({errors[:2]})")
    ctx.close()


def module_import(b, url, out):
    """Tracker modules (docs/TRACKER.md): a MOD file through File > Import tracker module: uploaded,
    read by the device (samples.probe), one Tracker channel with the song as one note, then played
    from its middle (note chase: channel meters move)"""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "web"))
    from modgen import make_mod

    tmp = tempfile.mkdtemp()
    path = os.path.join(tmp, "song.mod")
    open(path, "wb").write(make_mod())
    ctx = b.new_context(viewport={"width": 1366, "height": 768})
    page = ctx.new_page()
    errors = []
    page.on("pageerror", lambda e: errors.append(str(e)))
    page.goto(url)
    page.wait_for_selector(".shell")
    page.wait_for_timeout(300)
    page.locator(".topbar .btn", has_text="File").click()
    with page.expect_file_chooser() as fc:
        page.locator(".menu-item", has_text="Import tracker module").click()
    fc.value.set_files(path)
    dlg = page.locator(".modal[aria-label='Import a tracker module']")
    dlg.wait_for(timeout=5000)
    info = dlg.locator("[data-testid=module-info]")
    info.wait_for(timeout=20000)
    text = info.inner_text()
    check("0:08" in text and "4 channels" in text and "125 BPM" in text, f"module: the device reads its length, channels and tempo ({text})")
    plan = dlg.locator("[data-testid=module-plan]").inner_text()
    check("1 Tracker channel" in plan and "4 bars" in plan, f"module: the whole song on one channel, one 4-bar pattern ({plan})")
    dlg.locator("select[aria-label='Channels']").select_option("each")
    plan = dlg.locator("[data-testid=module-plan]").inner_text()
    check("4 Tracker channels" in plan, f"module: or one channel per tracker channel ({plan})")
    check(dlg.locator("[data-testid=module-split-warning]").count() == 1, "module: splitting shows its measured CPU cost")
    dlg.locator("select[aria-label='Channels']").select_option("one")
    page.screenshot(path=os.path.join(out, "desktop-module-import.png"))
    page.once("dialog", lambda d: d.accept())
    dlg.locator(".buttons .btn", has_text="Import").click()
    check(wait_for(lambda: project(url)["meta"]["name"] == "fixture", 30), "module: a new project named after the song")
    check(wait_for(lambda: page.locator(".modal[aria-label='Import a tracker module']").count() == 0, 30), "module: the dialog closes when done")
    p = project(url)
    chans = p["channels"]
    check(len(chans) == 1 and chans[0]["plugin"]["id"] == "org.smhgzll.tracker" and chans[0]["plugin"].get("resources", {}).get("module") == "song.mod",
          f"module: one Tracker channel playing song.mod ({[(c['name'], c['plugin']['id']) for c in chans]})")
    check(abs(p["meta"]["bpm"] - 125) < 0.01 and abs(chans[0]["plugin"]["params"].get("tempo", 0) - 125) < 0.01, "module: project and song tempo 125 BPM")
    pat = p["patterns"][0]
    notes = [n for s in pat["seqs"] for n in s["notes"]]
    clips = [c for c in p["playlist"]["clips"] if c["kind"] == "pattern"]
    check(pat["length"] == 15360 and len(notes) == 1 and notes[0][1] == 0 and notes[0][2] == 15360 and len(clips) == 1 and clips[0]["len"] == 15360,
          "module: the song is one note in one 4-bar pattern, one clip")
    # play from the middle of the song: the Tracker channel sounds at once (note chase)
    from wsclient import WS
    host, port = url.split("//")[1].strip("/").split(":")
    w = WS(host, int(port))
    w.hello()
    w.request({"t": "seek", "tick": 7680})
    w.request({"t": "play"})
    loud = wait_for(lambda: max(page.evaluate("window.__daw.live.state.status?.cm") or [0]) > 0.01, 5)
    check(bool(loud), "module: played from its middle, the channel sounds (note chase)")
    w.request({"t": "stop"})
    w.close()
    check(not errors, f"module: no page errors ({errors[:2]})")
    ctx.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:18190/")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "out"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    with sync_playwright() as p:
        b = p.chromium.launch()
        desktop(b, a.url, a.out).close()
        playback_reload(b, a.url)
        sync(b, a.url)
        mobile(b, a.url, a.out)
        midi_import(b, a.url, a.out)
        module_import(b, a.url, a.out)
        b.close()
    print(f"ui: {'PASS' if not FAILS else 'FAIL'} ({len(FAILS)} failed)")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
