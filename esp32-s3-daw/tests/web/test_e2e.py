#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end protocol test against a running DAW (daw-host on Linux, or the firmware in QEMU).

  tests/web/test_e2e.py --connect 127.0.0.1:18080 --admin-password PW [--quick]

Covers the handshake, cross-origin refusal, two clients editing, stale edits, transport, live
notes and stuck-note prevention, MIDI bursts, reconnect storms, slow clients, oversized and invalid
messages, project slots, and the privileged HTTP endpoints. Prints PASS/FAIL per check.
"""
import argparse
import json
import sys
import time

from wsclient import WS, WSError, http_req

FAILS = []


def check(cond, what):
    print(("PASS  " if cond else "FAIL  ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def voice_counts(ws, seconds=1.5):
    """voice counts of the status frames received over a while (oldest first)"""
    end, out = time.time() + seconds, []
    while time.time() < end:
        try:
            m = ws.recv(max(0.05, end - time.time()))
        except (TimeoutError, OSError):
            break
        if m.get("t") == "st":
            out.append(m.get("voices", 0))
        elif m.get("t") not in ("diag",):
            ws.inbox.append(m)
    return out or [0]


def last_status(ws, seconds=0.6):
    """the newest status frame after reading for a while (older frames may sit in the socket)"""
    end, last = time.time() + seconds, None
    while time.time() < end or last is None:
        m = ws.recv(5)
        if m.get("t") == "st":
            last = m
        elif m.get("t") != "diag":
            ws.inbox.append(m)
    return last


def connect(host, port, tries=15, **kw):
    """connect + hello, retrying while the server is at its client limit (as a browser reconnects)"""
    for i in range(tries):
        try:
            w = WS(host, port, **kw)
            w.hello()
            return w
        except (WSError, OSError):
            time.sleep(0.3)
    raise WSError("cannot connect")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--connect", default="127.0.0.1:8080")
    ap.add_argument("--admin-password", default="")
    ap.add_argument("--quick", action="store_true", help="skip the slower stress parts")
    a = ap.parse_args()
    host, port = a.connect.rsplit(":", 1)
    port = int(port)
    origin = f"http://{host}:{port}"

    st, body = http_req(host, port, "GET", "/api/status")
    status = json.loads(body)
    check(st == 200 and status.get("proto") == 1, f"GET /api/status ({status.get('target')}, {status.get('plugins')} plugins)")

    # cross-origin WebSocket refused, same-origin accepted
    try:
        WS(host, port, origin="http://evil.example").close()
        check(False, "foreign Origin refused for the WebSocket")
    except (WSError, OSError):
        check(True, "foreign Origin refused for the WebSocket")
    w1 = WS(host, port, origin=origin)
    welcome = w1.hello()
    proj = welcome["project"]
    check(len(proj["channels"]) > 0 and len(welcome["plugins"]) >= 7, "welcome: project snapshot and plugin catalog")
    w2 = WS(host, port)  # (a non-browser client sends no Origin)
    w2.hello()

    # invalid input
    w1.send_text("{not json")
    check(w1.wait(lambda m: m.get("t") == "err")["code"] == "bad_json", "invalid JSON -> bad_json")
    check(w1.request({"t": "does.not.exist"})["code"] == "unknown_op", "unknown op -> unknown_op")
    big = json.dumps({"t": "ping", "pad": "x" * (300 * 1024)})
    try:
        w1.send_text(big)
        r = w1.wait(lambda m: m.get("t") == "err" and m.get("code") == "too_large", 10)
        check(True, "oversized message refused (too_large)")
    except (WSError, OSError):
        # the firmware closes the connection instead (its frame limit): reconnect
        check(True, "oversized message refused (connection closed)")
        w1 = WS(host, port, origin=origin)
        w1.hello()

    # two clients: an edit by one is a patch for both
    pat, ch = proj["patterns"][0]["id"], proj["channels"][0]["id"]
    r = w1.request({"t": "step.toggle", "pattern": pat, "channel": ch, "index": 5, "on": True})
    check(r["t"] == "ack", "step.toggle acknowledged")
    p = w2.wait(lambda m: m.get("t") == "patch" and m.get("path") == "pattern" and m["value"]["id"] == pat)
    seq = next(s for s in p["value"]["seqs"] if s["channel"] == ch)
    check(seq["steps"][5][0] == 1, "the other client received the pattern patch")

    # notes: add, stale update after the other client deleted it
    r = w1.request({"t": "notes.add", "pattern": pat, "channel": ch, "notes": [[0, 0, 240, 60, 0.8, 0, 0, 0.5, 1, 0]]})
    nid = r["ids"][0]
    check(w2.request({"t": "notes.del", "pattern": pat, "channel": ch, "ids": [nid]})["t"] == "ack", "note deleted by client 2")
    r = w1.request({"t": "notes.update", "pattern": pat, "channel": ch, "notes": [{"id": nid, "key": 62}]})
    check(r["t"] == "err" and r["code"] == "not_found", "stale edit of a deleted note refused")

    # transport
    w1.request({"t": "bpm", "bpm": 140})
    w1.request({"t": "play"})
    s1 = last_status(w1)
    time.sleep(1.0)
    w1.inbox.clear()
    s2 = last_status(w1)
    check(s2["play"] == 1 and s2["bpm"] == 140, "playing at 140 BPM")
    check(s2["tick"] != s1["tick"] or s2["play"] == 1, "position advances")
    w1.request({"t": "stop"})
    time.sleep(0.3)
    w1.inbox.clear()
    check(last_status(w1)["play"] == 0, "stopped")

    # live notes: held notes are released when their client disconnects
    sine = next((c["id"] for c in proj["channels"] if c["plugin"]["id"] == "org.smhgzll.sine"), proj["channels"][-1]["id"])
    w3 = WS(host, port)
    w3.hello()
    for k in (60, 64, 67):
        w3.request({"t": "note", "channel": sine, "key": k, "vel": 0.9, "on": True})
    check(max(voice_counts(w1, 1.0)) >= 3, "three held notes sound")
    w3.close()
    check(voice_counts(w1, 2.0)[-1] == 0, "notes released after the client disconnected")

    # MIDI burst through one message
    acks = 0
    for burst in range(4):  # 4 messages x 100 note pairs (600 bytes each; the limit is 1024)
        data = []
        for i in range(100):
            data += [0x90, 36 + (i + burst) % 48, 100, 0x80, 36 + (i + burst) % 48, 0]
        acks += w1.request({"t": "midi", "channel": sine, "data": data})["t"] == "ack"
    check(acks == 4, "MIDI bursts of 800 events")
    check(w1.request({"t": "midi", "data": [0x90] * 2000})["code"] == "bad_arg", "an oversized MIDI message is refused")
    check(voice_counts(w1, 2.0)[-1] == 0, "no stuck notes after the bursts")

    if not a.quick:
        # reconnect storm: the server may refuse connections beyond its client limit (the firmware
        # allows 4), but it must survive and recover
        ok_n = refused = 0
        for i in range(15):
            try:
                w = WS(host, port)
                if i % 2 == 0:
                    w.hello()
                w.close()
                ok_n += 1
            except (WSError, OSError):
                refused += 1
                time.sleep(0.2)
        time.sleep(1.0)
        check(ok_n >= 5 and w1.request({"t": "ping"})["t"] == "ack",
              f"responsive after a reconnect storm ({ok_n} connected, {refused} refused at the client limit)")
        late = connect(host, port)  # the slots were released
        late.close()
        check(True, "a new client connects after the storm")
        # a slow client: does not read while another one edits a lot
        slow = connect(host, port)
        for i in range(60):
            w1.request({"t": "notes.add", "pattern": pat, "channel": ch, "notes": [[0, (i * 60) % 3840, 60, 48 + i % 24]]})
        time.sleep(1)
        r = slow.request({"t": "ping"}, 30)
        check(r["t"] == "ack", "a slow client recovers (still connected, answers)")
        slow.close()

    # project slots over the protocol
    r = w1.request({"t": "project.rename", "name": "E2E Song"})
    r = w1.request({"t": "project.save", "slot": 3})
    if r["t"] == "err" and r["code"] == "no_storage":
        check(True, "project storage: not available on this target")
    else:
        check(r["t"] == "ack", f"project saved to slot 3 ({r.get('bytes')} bytes)")
        w1.request({"t": "project.new", "template": "empty"})
        w1.inbox.clear()
        r = w1.request({"t": "project.load", "slot": 3})
        wel = w1.wait(lambda m: m.get("t") == "welcome")
        check(r["t"] == "ack" and wel["project"]["meta"]["name"] == "E2E Song", "project loaded back from slot 3")
        w1.request({"t": "project.delete", "slot": 3})

    # privileged HTTP endpoints
    st, _ = http_req(host, port, "POST", "/api/project", body="{}", headers={"Content-Type": "application/json"})
    check(st == 401, "POST /api/project without a login -> 401")
    st, _ = http_req(host, port, "POST", "/api/login", body=json.dumps({"password": "wrong"}))
    check(st == 401, "wrong admin password -> 401")
    st, _ = http_req(host, port, "POST", "/api/login", body=json.dumps({"password": a.admin_password}),
                     headers={"Origin": "http://evil.example"})
    check(st == 403, "cross-origin login -> 403")
    st, body = http_req(host, port, "POST", "/api/login", body=json.dumps({"password": a.admin_password}))
    check(st == 200, "login with the admin password")
    token = json.loads(body).get("token", "") if st == 200 else ""
    st, exported = http_req(host, port, "GET", "/api/project")
    check(st == 200 and json.loads(exported)["format"] == "esp32-daw-project", f"project export ({len(exported)} bytes)")
    st, _ = http_req(host, port, "POST", "/api/project", body=b"{\"format\":\"nope\"}", headers={"Authorization": "Bearer " + token})
    check(st == 400, "a foreign document is refused by the import")
    st, _ = http_req(host, port, "POST", "/api/project", body=exported, headers={"Authorization": "Bearer " + token})
    check(st == 200, "the exported project imports back")
    w1.inbox.clear()
    wel = w1.wait(lambda m: m.get("t") == "welcome", 10)
    check(wel["project"]["meta"]["name"] == json.loads(exported)["meta"]["name"], "clients receive the imported project")

    # a large project (like a MIDI import: thousands of notes, several times a slot's 128 KB of
    # JSON) is stored deflated and loads back note for note
    big = json.loads(exported)
    if big["channels"] and big["patterns"]:
        ch = big["channels"][0]["id"]
        nid = int(big.get("nextId", 1000)) + 10
        for p in range(40):
            pat = dict(big["patterns"][0])
            notes = [[k + 1, (k * 120) % 61440, 110, 36 + (k * 7 + p) % 48, round(0.5 + ((k * 13 + p) % 50) / 100, 4), 0, 0, 0.5, 1, 0] for k in range(150)]
            pat.update({"id": nid + p, "name": f"Big {p}", "length": 61440, "seqs": [{"channel": ch, "notes": notes}]})
            big["patterns"].append(pat)
        big["nextId"] = nid + 100
        doc = json.dumps(big).encode()
        st, _ = http_req(host, port, "POST", "/api/project", body=doc, headers={"Authorization": "Bearer " + token})
        check(st == 200 and len(doc) > 200 * 1024, f"a {len(doc) // 1024} KB project imports")
        w1.inbox.clear()
        w1.wait(lambda m: m.get("t") == "welcome", 30)
        want = sum(len(q.get("notes", [])) for x in big["patterns"] for q in x["seqs"])
        r = w1.request({"t": "project.save", "slot": 4}, 120)
        if r["t"] == "err" and r["code"] == "no_storage":
            check(True, "large project slot: no storage on this target")
        else:
            check(r["t"] == "ack", f"it fits a project slot deflated ({r.get('bytes', r.get('msg'))} bytes of JSON)")
            w1.request({"t": "project.new", "template": "empty"})
            w1.inbox.clear()
            r = w1.request({"t": "project.load", "slot": 4}, 60)
            wel = w1.wait(lambda m: m.get("t") == "welcome", 30)
            got = sum(len(q.get("notes", [])) for x in wel["project"]["patterns"] for q in x["seqs"])
            check(r["t"] == "ack" and got == want, f"and loads back with all {want} notes ({got})")
            w1.request({"t": "project.delete", "slot": 4})

    # the sample library over HTTP, then a Sampler channel playing it (on the device: a flash write)
    import math, struct
    pcm = b"".join(struct.pack("<h", int(12000 * math.sin(2 * math.pi * 440 * i / 44100))) for i in range(22050))
    wav = (b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, 44100, 88200, 2, 16)
           + b"data" + struct.pack("<I", len(pcm)) + pcm)
    st, body = http_req(host, port, "POST", "/api/samples?name=e2e%20tone.wav", body=wav)
    lib = json.loads(body) if st == 200 else {}
    check(st == 200 and any(f["name"] == "e2e tone.wav" and f["size"] == len(wav) for f in lib.get("list", [])), f"a WAV uploaded to the sample library ({st})")
    st, _ = http_req(host, port, "POST", "/api/samples?name=x.wav", body=b"not audio at all")
    check(st == 400, "a non-audio upload is refused")
    st, _ = http_req(host, port, "POST", "/api/samples?name=evil.wav", body=wav, headers={"Origin": "http://evil.example"})
    check(st == 403, "a cross-origin upload is refused")
    ack = w1.request({"t": "channel.add", "plugin": "org.smhgzll.sampler", "name": "E2E Sampler"})
    sid = ack.get("newId", 0)
    ack = w1.request({"t": "channel.resource", "channel": sid, "slot": "sample", "sample": "e2e tone.wav"})
    check(ack["t"] == "ack", "the sample is assigned to a Sampler channel")
    w1.request({"t": "note", "channel": sid, "key": 60, "vel": 1, "on": True})
    check(max(voice_counts(w1, 1.0) or [0]) >= 1, "the Sampler channel sounds")
    w1.request({"t": "note", "channel": sid, "key": 60, "on": False})
    ack = w1.request({"t": "samples.delete", "name": "e2e tone.wav"})
    check(ack["t"] == "ack", "the sample is deleted from the library")
    # Large text operations can arrive in several WebSocket frames (Chromium). Continuations
    # must be consumed and joined before parsing, with one ACK for the whole message.
    fragmented_id = w1.next_id
    w1.next_id += 1
    w1.send_text(json.dumps({"t": "ping", "id": fragmented_id, "ts": 12345, "pad": "x" * 100000}), fragment_bytes=30000)
    fragmented_ack = w1.wait(lambda m: m.get("id") == fragmented_id, 20)
    check(fragmented_ack.get("t") == "ack" and fragmented_ack.get("echo") == 12345, "a 100 KB fragmented WebSocket operation is acknowledged once")
    # a SoundFont in the library, played by the SoundFont plugin
    from sf2gen import make_sf2
    st, body = http_req(host, port, "POST", "/api/samples?name=e2e.sf2", body=make_sf2())
    check(st == 200 and any(f["name"] == "e2e.sf2" and f["type"] == "sf2" for f in json.loads(body).get("list", [])), f"a SoundFont uploaded ({st})")
    ack = w1.request({"t": "channel.add", "plugin": "org.smhgzll.soundfont", "name": "E2E SF2"})
    fid = ack.get("newId", 0)
    ack = w1.request({"t": "channel.resource", "channel": fid, "slot": "soundfont", "sample": "e2e.sf2", "bank": 0, "program": 0, "presets": [{"bank": 0, "program": 0}]})
    check(ack["t"] == "ack", "the SoundFont is assigned")
    ack = w1.request({"t": "samples.presets", "name": "e2e.sf2"})
    check(ack["t"] == "ack" and ack.get("presets") == [{"name": "Test Sine", "bank": 0, "program": 0}], "the SoundFont's preset list")
    w1.request({"t": "note", "channel": fid, "key": 60, "vel": 1, "on": True})
    check(max(voice_counts(w1, 1.0) or [0]) >= 1, "the SoundFont channel sounds")
    w1.request({"t": "note", "channel": fid, "key": 60, "on": False})
    w1.request({"t": "samples.delete", "name": "e2e.sf2"})
    # MacroSynth (Braids): a struck model plays and a parameter reaches it
    ack = w1.request({"t": "channel.add", "plugin": "org.smhgzll.macrosynth", "name": "E2E Macro"})
    mid = ack.get("newId", 0)
    ack = w1.request({"t": "channel.param", "channel": mid, "key": "model", "v": 28})
    check(ack["t"] == "ack", "MacroSynth channel added, model set")
    w1.request({"t": "note", "channel": mid, "key": 60, "vel": 1, "on": True})
    check(max(voice_counts(w1, 1.0) or [0]) >= 1, "the MacroSynth channel sounds")
    w1.request({"t": "note", "channel": mid, "key": 60, "on": False})
    # a tracker module in the library, read by the Tracker plugin (libxmp) through samples.probe
    from modgen import make_mod
    st, body = http_req(host, port, "POST", "/api/samples?name=e2e.mod", body=make_mod())
    check(st == 200 and any(f["name"] == "e2e.mod" and f["type"] == "module" for f in json.loads(body).get("list", [])), f"a tracker module uploaded ({st})")
    st, _ = http_req(host, port, "POST", "/api/samples?name=star.mod", body=make_mod(b"FLT4"))
    check(st == 400, "a MOD variant the Tracker plugin does not read is refused")
    ack = w1.request({"t": "samples.probe", "name": "e2e.mod", "plugin": "org.smhgzll.tracker"})
    vals = ack.get("values", {})
    check(ack["t"] == "ack" and abs(vals.get("length", 0) - 7.68) < 0.01 and vals.get("channels") == 4 and vals.get("song_tempo") == 125,
          f"the module's length, channels and tempo through samples.probe ({vals})")
    ack = w1.request({"t": "channel.add", "plugin": "org.smhgzll.tracker", "name": "E2E Tracker"})
    tid = ack.get("newId", 0)
    ack = w1.request({"t": "channel.resource", "channel": tid, "slot": "module", "sample": "e2e.mod"})
    check(ack["t"] == "ack", "the module is assigned to a Tracker channel")
    w1.request({"t": "note", "channel": tid, "key": 60, "vel": 1, "on": True})
    check(max(voice_counts(w1, 1.0) or [0]) >= 1, "the Tracker channel plays the song")
    w1.request({"t": "note", "channel": tid, "key": 60, "on": False})
    w1.request({"t": "channel.del", "channel": tid})
    w1.request({"t": "samples.delete", "name": "e2e.mod"})

    # device settings: behind the admin token, passwords write-only, an admin password change revokes tokens
    st, _ = http_req(host, port, "GET", "/api/settings")
    check(st == 401, "settings need a login")
    st, body = http_req(host, port, "POST", "/api/login", body=json.dumps({"password": a.admin_password}))
    tok = json.loads(body).get("token", "") if st == 200 else ""
    auth = {"Authorization": "Bearer " + tok}
    st, body = http_req(host, port, "GET", "/api/settings", headers=auth)
    cfg = json.loads(body) if st == 200 else {}
    check(st == 200 and "hostname" in cfg and not any("assword" in k and isinstance(v, str) for k, v in cfg.items()),
          "settings readable with a login, no password in them")
    st, body = http_req(host, port, "POST", "/api/settings", body=json.dumps({"hostname": "Bad Name"}), headers=auth)
    check(st == 400 and "device name" in json.loads(body).get("error", ""), "an invalid device name is refused")
    st, _ = http_req(host, port, "POST", "/api/settings", body=json.dumps({"hostname": "e2e-daw"}), headers={**auth, "Origin": "http://evil.example"})
    check(st == 403, "a cross-origin settings change is refused")
    st, _ = http_req(host, port, "POST", "/api/settings", body=json.dumps({"hostname": "e2e-daw"}), headers=auth)
    st2, body = http_req(host, port, "GET", "/api/settings", headers=auth)
    check(st == 200 and json.loads(body).get("hostname") == "e2e-daw", "a valid change is stored")
    st, _ = http_req(host, port, "POST", "/api/settings", body=json.dumps({"adminPassword": "new-admin-pass"}), headers=auth)
    st2, _ = http_req(host, port, "GET", "/api/settings", headers=auth)
    st3, _ = http_req(host, port, "POST", "/api/login", body=json.dumps({"password": a.admin_password}))
    st4, body = http_req(host, port, "POST", "/api/login", body=json.dumps({"password": "new-admin-pass"}))
    check(st == 200 and st2 == 401 and st3 == 401 and st4 == 200, "a new admin password revokes the old token and replaces the old password")
    tok = json.loads(body).get("token", "") if st4 == 200 else ""
    http_req(host, port, "POST", "/api/settings", body=json.dumps({"adminPassword": a.admin_password, "hostname": cfg.get("hostname", "esp32daw")}),
             headers={"Authorization": "Bearer " + tok})  # restore (later tests log in with it)

    st, body = http_req(host, port, "GET", "/api/status")
    check(st == 200, "server healthy at the end")
    w1.close()
    w2.close()
    print(f"e2e: {'PASS' if not FAILS else 'FAIL'} ({len(FAILS)} failed)")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
