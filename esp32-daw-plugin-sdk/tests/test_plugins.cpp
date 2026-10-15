// SPDX-License-Identifier: MIT
// Every linked plugin passes the conformance checks; plugin-specific behaviour through the ABI.
#include <math.h>
#include <string.h>

#include <functional>
#include <semaphore>
#include <thread>
#include <string>
#include <vector>

#include "check.hpp"
#include "dawsdk/dsp.hpp"
#include "dawsdk_host/host.hpp"
#include "validate.hpp"

using namespace dawhost;

static double zeroCrossFreq(const std::vector<float> &x, size_t from, size_t to, double sr)
{
    double first = -1, last = -1;
    int n = 0;
    for (size_t i = from + 1; i < to; i++)
        if (x[i - 1] < 0 && x[i] >= 0) {
            double t = (i - 1) + (-x[i - 1]) / (x[i] - x[i - 1]);
            if (first < 0)
                first = t;
            last = t;
            n++;
        }
    return n > 1 ? (n - 1) * sr / (last - first) : 0;
}

static uint32_t paramId(const daw_plugin_descriptor_t *d, const char *key)
{
    for (uint32_t i = 0; i < d->param_count; i++)
        if (!strcmp(d->params[i].key, key))
            return d->params[i].id;
    fprintf(stderr, "no param %s in %s\n", key, d->id);
    return 0xFFFFFFFF;
}

static void testSine()
{
    auto *d = find("org.smhgzll.sine");
    CHECK(d != nullptr);
    if (!d)
        return;
    // pitch: A4 renders 440 Hz
    {
        Instance in(d);
        auto r = render(in, {{0.0, noteOn(69, 1.0f, 1)}}, 1.0);
        CHECK_NEAR(zeroCrossFreq(r.l, 4410, 44100, 44100), 440.0, 0.2);
    }
    // per-note pan: two notes, one hard left, one hard right, are independent
    {
        Instance in(d);
        daw_event_t a = noteOn(57, 1.0f, 1), b = noteOn(81, 1.0f, 2);
        a.u.note.pan = -1.0f;
        b.u.note.pan = 1.0f;
        auto r = render(in, {{0.0, a}, {0.0, b}}, 0.5);
        double fl = zeroCrossFreq(r.l, 4410, r.l.size(), 44100), fr = zeroCrossFreq(r.r, 4410, r.r.size(), 44100);
        CHECK_NEAR(fl, 220.0, 0.5);
        CHECK_NEAR(fr, 880.0, 1.0);
    }
    // per-note pitch expression moves only its note (+12 st on note 1)
    {
        Instance in(d);
        daw_event_t a = noteOn(57, 1.0f, 1), b = noteOn(81, 1.0f, 2);
        a.u.note.pan = -1.0f;
        b.u.note.pan = 1.0f;
        daw_event_t ex{};
        ex.type = DAW_EV_NOTE_EXPR;
        ex.u.expr.note_id = 1;
        ex.u.expr.key = 57;
        ex.u.expr.expr = DAW_EXPR_PITCH;
        ex.u.expr.value = 12.0f;
        auto r = render(in, {{0.0, a}, {0.0, b}, {0.1, ex}}, 0.5);
        CHECK_NEAR(zeroCrossFreq(r.l, 8820, r.l.size(), 44100), 440.0, 0.5);
        CHECK_NEAR(zeroCrossFreq(r.r, 8820, r.r.size(), 44100), 880.0, 1.0);
    }
    // polyphony limit and voice stealing
    {
        Instance in(d);
        in.setParam(paramId(d, "polyphony"), 4);
        std::vector<TimedEvent> ev;
        for (int i = 0; i < 6; i++)
            ev.push_back({0.01 * i, noteOn((uint8_t)(60 + i), 0.8f, i)});
        render(in, ev, 0.2);
        CHECK(in.voices() == 4);
        CHECK(in.host().reports >= 2);
        CHECK(in.host().lastReport == DAW_REPORT_VOICE_STEAL);
    }
    // velocity scales the level
    {
        Instance a(d), b(d);
        auto ra = render(a, {{0.0, noteOn(60, 1.0f, 1)}}, 0.3);
        auto rb = render(b, {{0.0, noteOn(60, 0.5f, 1)}}, 0.3);
        CHECK_NEAR(rms(rb.l) / rms(ra.l), 0.5, 0.01);
    }
}

// time until the signal stays below `thr` for good (seconds)
static double decayTime(const std::vector<float> &x, float thr, double sr)
{
    size_t last = 0;
    for (size_t i = 0; i < x.size(); i++)
        if (fabsf(x[i]) > thr)
            last = i;
    return last / sr;
}

static void testDrumLab()
{
    auto *d = find("org.smhgzll.drumlab");
    CHECK(d != nullptr);
    if (!d)
        return;
    // every mapped kit key sounds
    for (int key : {35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 50}) {
        Instance in(d);
        auto r = render(in, {{0.0, noteOn((uint8_t)key, 1.0f, 1)}}, 0.5);
        if (rms(r.l) < 1e-3)
            fprintf(stderr, "kit key %d is silent\n", key);
        CHECK(rms(r.l) > 1e-3);
    }
    // unmapped keys are silent in kit mode
    {
        Instance in(d);
        auto r = render(in, {{0.0, noteOn(80, 1.0f, 1)}}, 0.3);
        CHECK(peak(r.l) == 0.0f);
    }
    // the decay control changes the length (kick and snare: the upstream resonator bug made it a no-op)
    for (const char *piece : {"kick", "snare", "tom"}) {
        std::string key = std::string(piece) + "_decay";
        int note = !strcmp(piece, "kick") ? 36 : !strcmp(piece, "snare") ? 38 : 45;
        Instance s(d), l(d);
        s.setParam(paramId(d, key.c_str()), 0.1f);
        l.setParam(paramId(d, key.c_str()), 0.9f);
        auto rs = render(s, {{0.0, noteOn((uint8_t)note, 1.0f, 1)}}, 4.0);
        auto rl = render(l, {{0.0, noteOn((uint8_t)note, 1.0f, 1)}}, 4.0);
        double ts = decayTime(rs.l, 1e-3f, 44100), tl = decayTime(rl.l, 1e-3f, 44100);
        if (!(tl > ts * 1.5))
            fprintf(stderr, "%s decay: short %.3f s, long %.3f s\n", piece, ts, tl);
        CHECK(tl > ts * 1.5);
        CHECK(tl < 3.9);
    }
    // a closed hat chokes the open hat
    {
        Instance a(d), b(d);
        auto ra = render(a, {{0.0, noteOn(46, 1.0f, 1)}}, 1.0);
        auto rb = render(b, {{0.0, noteOn(46, 1.0f, 1)}, {0.1, noteOn(42, 0.01f, 2)}}, 1.0);
        std::vector<float> ta(ra.l.begin() + 13230, ra.l.end()), tb(rb.l.begin() + 13230, rb.l.end());
        CHECK(decayTime(ra.l, 1e-3f, 44100) > 0.25);  // an open hat rings
        CHECK(rms(tb) < rms(ta) * 0.1);
    }
    // single mode: any key plays the piece, transposed against C5
    {
        Instance in(d);
        in.setParam(paramId(d, "mode"), 1);  // Kick
        auto r60 = render(in, {{0.0, noteOn(60, 1.0f, 1)}}, 0.5);
        CHECK(rms(r60.l) > 1e-3);
        Instance hi(d);
        hi.setParam(paramId(d, "mode"), 1);
        auto r72 = render(hi, {{0.0, noteOn(72, 1.0f, 1)}}, 0.5);
        // an octave up: roughly twice the zero crossings in the body
        double f60 = zeroCrossFreq(r60.l, 2205, 13230, 44100), f72 = zeroCrossFreq(r72.l, 2205, 13230, 44100);
        CHECK(f72 > f60 * 1.6 && f72 < f60 * 2.5);
    }
    // velocity changes the level
    {
        Instance a(d), b(d);
        auto ra = render(a, {{0.0, noteOn(36, 1.0f, 1)}}, 0.5);
        auto rb = render(b, {{0.0, noteOn(36, 0.3f, 1)}}, 0.5);
        CHECK(rms(rb.l) < rms(ra.l) * 0.8);
    }
}

static void testThreeOsc()
{
    auto *d = find("org.smhgzll.3xosc");
    CHECK(d != nullptr);
    if (!d)
        return;
    auto setSine = [&](Instance &in) {  // one sine oscillator, open filter, no LFO
        in.setParam(paramId(d, "osc1_wave"), 0);
        in.setParam(paramId(d, "osc2_level"), 0);
        in.setParam(paramId(d, "osc3_level"), 0);
        in.setParam(paramId(d, "cutoff"), 20000);
        in.setParam(paramId(d, "flt_env"), 0);
        in.setParam(paramId(d, "flt_key"), 0);
        in.setParam(paramId(d, "resonance"), 0);
        in.setParam(paramId(d, "amp_attack"), 0.5f);
        in.setParam(paramId(d, "amp_sustain"), 1);
    };
    // pitch: A3 through oscillator 1 (octave / coarse / fine offsets)
    {
        Instance in(d);
        setSine(in);
        auto r = render(in, {{0.0, noteOn(57, 1.0f, 1)}}, 1.0);
        CHECK_NEAR(zeroCrossFreq(r.l, 4410, 44100, 44100), 220.0, 0.3);
        Instance o(d);
        setSine(o);
        o.setParam(paramId(d, "osc1_oct"), 1);
        o.setParam(paramId(d, "osc1_semi"), 7);
        auto r2 = render(o, {{0.0, noteOn(57, 1.0f, 1)}}, 1.0);
        CHECK_NEAR(zeroCrossFreq(r2.l, 4410, 44100, 44100), 220.0 * 2 * pow(2.0, 7 / 12.0), 0.8);
    }
    // slide: a SLIDE note glides the sounding voice to the new key without a second voice
    {
        Instance in(d);
        setSine(in);
        in.setParam(paramId(d, "glide"), 100);
        // the sequencer overlaps a slide note with the previous one (legato)
        auto r = render(in, {{0.0, noteOn(57, 1.0f, 1)}, {0.5, noteOn(69, 1.0f, 2, DAW_NOTE_FLAG_SLIDE)}, {0.55, noteOff(57, 1)}}, 1.2);
        CHECK(in.voices() == 1);
        CHECK_NEAR(zeroCrossFreq(r.l, 4410, 19845, 44100), 220.0, 0.5);    // before the slide
        CHECK_NEAR(zeroCrossFreq(r.l, 30870, 52920, 44100), 440.0, 1.0);   // after 100 ms of glide
        double mid = zeroCrossFreq(r.l, 22270, 24470, 44100);              // during the glide
        CHECK(mid > 230 && mid < 430);
    }
    // a slide note after a gap glides from the last key in a new voice
    {
        Instance in(d);
        setSine(in);
        in.setParam(paramId(d, "glide"), 100);
        auto r = render(in, {{0.0, noteOn(57, 1.0f, 1)}, {0.3, noteOff(57, 1)}, {0.5, noteOn(69, 1.0f, 2, DAW_NOTE_FLAG_SLIDE)}}, 1.2);
        double mid = zeroCrossFreq(r.r, 22270, 24470, 44100);
        CHECK(mid > 230 && mid < 430);
        CHECK_NEAR(zeroCrossFreq(r.l, 30870, 52920, 44100), 440.0, 1.0);
    }
    // portamento off: SLIDE flag ignored -> a second voice starts at the new pitch
    {
        Instance in(d);
        setSine(in);
        in.setParam(paramId(d, "porta"), 0);
        render(in, {{0.0, noteOn(57, 1.0f, 1)}, {0.2, noteOn(69, 1.0f, 2, DAW_NOTE_FLAG_SLIDE)}}, 0.4);
        CHECK(in.voices() == 2);
    }
    // the filter darkens: a saw through a 300 Hz low pass has far less high-frequency energy
    {
        Instance open(d), closed(d);
        for (Instance *in : {&open, &closed}) {
            in->setParam(paramId(d, "osc2_level"), 0);
            in->setParam(paramId(d, "osc3_level"), 0);
            in->setParam(paramId(d, "flt_env"), 0);
        }
        open.setParam(paramId(d, "cutoff"), 20000);
        closed.setParam(paramId(d, "cutoff"), 300);
        auto ro = render(open, {{0.0, noteOn(45, 1.0f, 1)}}, 0.5);
        auto rc = render(closed, {{0.0, noteOn(45, 1.0f, 1)}}, 0.5);
        auto hf = [](const std::vector<float> &x) {  // first difference ~ high-pass
            double e = 0;
            for (size_t i = 1; i < x.size(); i++)
                e += (x[i] - x[i - 1]) * (x[i] - x[i - 1]);
            return e;
        };
        CHECK(hf(rc.l) < hf(ro.l) * 0.05);
    }
    // polyphony limit
    {
        Instance in(d);
        in.setParam(paramId(d, "polyphony"), 3);
        std::vector<TimedEvent> ev;
        for (int i = 0; i < 5; i++)
            ev.push_back({0.01 * i, noteOn((uint8_t)(60 + 2 * i), 0.8f, i)});
        render(in, ev, 0.2);
        CHECK(in.voices() == 3);
    }
    // Oscillator tiles retain phase, noise ordering, stereo pan and event offsets
    // across different host block sizes, including simultaneous noise oscillators.
    for (int combo = 0; combo < 125; combo++) {
        Instance small(d), large(d);
        for (Instance *in : {&small, &large}) {
            in->setParam(paramId(d, "osc1_wave"), combo % 5);
            in->setParam(paramId(d, "osc2_wave"), combo / 5 % 5);
            in->setParam(paramId(d, "osc3_wave"), combo / 25);
            in->setParam(paramId(d, "amp_attack"), 0.5f);
            in->setParam(paramId(d, "amp_release"), 10);
            in->setParam(paramId(d, "polyphony"), 8);
        }
        auto a = noteOn(57, 0.7f, 1), b = noteOn(69, 0.6f, 2);
        a.u.note.pan = -0.7f;
        b.u.note.pan = 0.8f;
        const std::vector<TimedEvent> events = {
            {19.0 / 44100, a}, {37.0 / 44100, b},
            {101.0 / 44100, paramEv(paramId(d, "osc2_fine"), -37)},
            {0.07, noteOff(57, 1)}, {0.09, noteOff(69, 2)},
        };
        auto x = render(small, events, 0.15, 16), y = render(large, events, 0.15, 256);
        CHECK(x.l == y.l && x.r == y.r);
        CHECK(peak(x.l) > 0.001f && peak(x.r) > 0.001f);
        CHECK(small.host().rtViolations == 0 && large.host().rtViolations == 0);
    }
}

static double rmsOf(const std::vector<float> &x, size_t from, size_t to)
{
    double e = 0;
    for (size_t i = from; i < to && i < x.size(); i++)
        e += (double)x[i] * x[i];
    return to > from ? sqrt(e / (double)(to - from)) : 0;
}

static void testAcid303()
{
    auto *d = find("org.smhgzll.acid303");
    CHECK(d != nullptr);
    if (!d)
        return;
    auto plain = [&](Instance &in) {  // open filter, no resonance / envelope: the oscillator's pitch
        in.setParam(paramId(d, "resonance"), 0);
        in.setParam(paramId(d, "envmod"), 0);
        in.setParam(paramId(d, "cutoff"), 2394);
        in.setParam(paramId(d, "decay"), 2000);
        in.setParam(paramId(d, "sustain"), 1);
    };
    const size_t sr = 44100;
    // pitch: A2 = 110 Hz, saw and square
    for (float wave : {0.0f, 1.0f}) {
        Instance in(d);
        plain(in);
        in.setParam(paramId(d, "wave"), wave);
        auto r = render(in, {{0.0, noteOn(45, 0.7f, 1)}}, 1.0);
        CHECK_NEAR(zeroCrossFreq(r.l, sr / 10, sr, sr), 110.0, 0.5);
        CHECK(peak(r.l) > 0.05 && peak(r.l) < 1.0);
    }
    // slide: the SLIDE note glides the sounding note without retriggering; one voice
    {
        Instance in(d);
        plain(in);
        auto r = render(in, {{0.0, noteOn(45, 0.7f, 1)}, {0.5, noteOn(57, 0.7f, 2, DAW_NOTE_FLAG_SLIDE)}, {0.5, noteOff(45, 1)}}, 1.0);
        CHECK(in.voices() == 1);
        CHECK_NEAR(zeroCrossFreq(r.l, sr / 10, sr * 45 / 100, sr), 110.0, 0.5);
        CHECK_NEAR(zeroCrossFreq(r.l, sr * 7 / 10, sr, sr), 220.0, 1.0);
        double mid = zeroCrossFreq(r.l, sr / 2 + 200, sr / 2 + 1500, sr);  // 5..35 ms into the glide
        CHECK(mid > 115 && mid < 215);
        // no retrigger: the level right after the slide is close to the level before it
        const double before = rmsOf(r.l, sr * 45 / 100, sr / 2), after = rmsOf(r.l, sr / 2, sr / 2 + sr / 20);
        CHECK(after > before * 0.6 && after < before * 1.6);
    }
    // a plain note after silence retriggers (no glide from the previous key)
    {
        Instance in(d);
        plain(in);
        auto r = render(in, {{0.0, noteOn(45, 0.7f, 1)}, {0.2, noteOff(45, 1)}, {0.5, noteOn(57, 0.7f, 2, DAW_NOTE_FLAG_SLIDE)}}, 0.8);
        CHECK_NEAR(zeroCrossFreq(r.l, sr / 2 + 400, sr / 2 + 4400, sr), 220.0, 2.0);  // a slide needs a sounding note
    }
    // accent: louder and brighter than the same note without it
    {
        Instance a(d), n(d);
        auto ra = render(a, {{0.0, noteOn(45, 0.7f, 1, DAW_NOTE_FLAG_ACCENT)}}, 0.4);
        auto rn = render(n, {{0.0, noteOn(45, 0.7f, 1)}}, 0.4);
        CHECK(rmsOf(ra.l, 0, sr / 5) > rmsOf(rn.l, 0, sr / 5) * 1.3);
    }
    // the filter envelope: with env mod the attack is brighter than the tail
    {
        Instance in(d);
        in.setParam(paramId(d, "envmod"), 100);
        in.setParam(paramId(d, "decay"), 200);
        in.setParam(paramId(d, "sustain"), 1);
        auto r = render(in, {{0.0, noteOn(45, 0.7f, 1)}}, 1.0);
        auto hf = [&](size_t from, size_t to) {  // first-difference energy relative to the signal energy
            double e = 0, s = 0;
            for (size_t i = from + 1; i < to; i++) {
                e += (r.l[i] - r.l[i - 1]) * (r.l[i] - r.l[i - 1]);
                s += r.l[i] * r.l[i];
            }
            return s > 0 ? e / s : 0;
        };
        CHECK(hf(sr / 100, sr / 100 + sr / 20) > hf(sr * 8 / 10, sr * 8 / 10 + sr / 20) * 2.0);
    }
    // last-note priority: releasing the newer key slides back to the one still held
    {
        Instance in(d);
        plain(in);
        auto r = render(in, {{0.0, noteOn(45, 0.7f, 1)}, {0.3, noteOn(57, 0.7f, 2)}, {0.6, noteOff(57, 2)}}, 1.0);
        CHECK_NEAR(zeroCrossFreq(r.l, sr * 4 / 10, sr * 58 / 100, sr), 220.0, 1.0);
        CHECK_NEAR(zeroCrossFreq(r.l, sr * 75 / 100, sr, sr), 110.0, 0.5);
    }
    // goes to sleep after the release, and is silent
    {
        Instance in(d);
        auto r = render(in, {{0.0, noteOn(45, 0.7f, 1)}, {0.2, noteOff(45, 1)}}, 2.0);
        CHECK(in.voices() == 0);
        CHECK(peak(std::vector<float>(r.l.end() - 4410, r.l.end())) == 0.0f);
    }
    // stable at the extremes: full resonance, env mod and accent over five octaves
    {
        Instance in(d);
        in.setParam(paramId(d, "resonance"), 100);
        in.setParam(paramId(d, "envmod"), 100);
        in.setParam(paramId(d, "cutoff"), 2394);
        in.setParam(paramId(d, "accent"), 100);
        in.setParam(paramId(d, "wave"), 1);
        std::vector<TimedEvent> ev;
        for (int i = 0; i < 20; i++) {
            ev.push_back({0.1 * i, noteOn((uint8_t)(24 + 4 * i), 1.0f, i, i % 3 == 0 ? DAW_NOTE_FLAG_ACCENT : (i % 3 == 1 ? DAW_NOTE_FLAG_SLIDE : 0))});
            ev.push_back({0.1 * i + 0.09, noteOff((uint8_t)(24 + 4 * i), i)});
        }
        auto r = render(in, ev, 2.5);
        bool finiteAll = true;
        for (float x : r.l)
            finiteAll &= std::isfinite(x);
        CHECK(finiteAll);
        CHECK(peak(r.l) < 2.0);
    }
}

static double toneAmp(const std::vector<float> &x, size_t from, size_t to, double f, double sr);
static void testFm6()
{
    auto *d = find("org.smhgzll.fm6");
    CHECK(d != nullptr);
    if (!d)
        return;
    const double sr = 44100;
    const size_t N = 44100;
    // algorithm 32 (all carriers) with only operator 1 sounding: a pure sine at the key's pitch
    auto sineOnly = [&](Instance &in) {
        in.setParam(paramId(d, "algorithm"), 32);
        in.setParam(paramId(d, "feedback"), 0);
        for (int o = 2; o <= 6; o++)
            in.setParam(paramId(d, ("op" + std::to_string(o) + "_level").c_str()), 0);
        in.setParam(paramId(d, "op1_level"), 99);
        in.setParam(paramId(d, "op1_sustain"), 1);
    };
    {
        Instance in(d);
        sineOnly(in);
        auto r = render(in, {{0.0, noteOn(57, 1.0f, 1)}}, 1.0);
        CHECK_NEAR(zeroCrossFreq(r.l, N / 10, N, sr), 220.0, 0.3);
        CHECK(toneAmp(r.l, N / 2, N, 440, sr) < toneAmp(r.l, N / 2, N, 220, sr) * 0.01);  // no harmonics
        // coarse 2 and transpose -12 cancel out
        Instance t(d);
        sineOnly(t);
        t.setParam(paramId(d, "op1_coarse"), 2);
        t.setParam(paramId(d, "transpose"), -12);
        auto rt = render(t, {{0.0, noteOn(57, 1.0f, 1)}}, 1.0);
        CHECK_NEAR(zeroCrossFreq(rt.l, N / 10, N, sr), 220.0, 0.3);
        // negative integer parameters round correctly (regression: -12 used to become -11)
        CHECK(t.getParam(paramId(d, "transpose")) == -12.0f);
        t.setParam(paramId(d, "op1_detune"), -3.4f);
        CHECK(t.getParam(paramId(d, "op1_detune")) == -3.0f);
        t.setParam(paramId(d, "op1_detune"), -3.6f);
        CHECK(t.getParam(paramId(d, "op1_detune")) == -4.0f);
    }
    // algorithm 1: operator 2 (ratio 1) modulates operator 1 -> harmonics appear with its level
    {
        auto secondHarmonic = [&](float modLevel) {
            Instance in(d);
            in.setParam(paramId(d, "algorithm"), 1);
            in.setParam(paramId(d, "feedback"), 0);
            for (int o = 3; o <= 6; o++)
                in.setParam(paramId(d, ("op" + std::to_string(o) + "_level").c_str()), 0);
            in.setParam(paramId(d, "op1_level"), 99);
            in.setParam(paramId(d, "op1_sustain"), 1);
            in.setParam(paramId(d, "op2_level"), modLevel);
            in.setParam(paramId(d, "op2_sustain"), 1);
            auto r = render(in, {{0.0, noteOn(57, 1.0f, 1)}}, 1.0);
            return toneAmp(r.l, N / 2, N, 440, sr) / toneAmp(r.l, N / 2, N, 220, sr);
        };
        CHECK(secondHarmonic(0) < 0.01);
        CHECK(secondHarmonic(85) > 0.1);
    }
    // feedback alone turns a sine into a brighter wave
    {
        Instance a(d), b(d);
        sineOnly(a);
        sineOnly(b);
        a.setParam(paramId(d, "algorithm"), 32);  // feedback on operator 6: give it level, mute op 1
        b.setParam(paramId(d, "algorithm"), 32);
        for (Instance *in : {&a, &b}) {
            in->setParam(paramId(d, "op1_level"), 0);
            in->setParam(paramId(d, "op6_level"), 99);
            in->setParam(paramId(d, "op6_sustain"), 1);
        }
        a.setParam(paramId(d, "feedback"), 0);
        b.setParam(paramId(d, "feedback"), 7);
        auto ra = render(a, {{0.0, noteOn(57, 1.0f, 1)}}, 1.0), rb = render(b, {{0.0, noteOn(57, 1.0f, 1)}}, 1.0);
        CHECK(toneAmp(ra.l, N / 2, N, 440, sr) < 0.005);
        CHECK(toneAmp(rb.l, N / 2, N, 440, sr) > 0.02);
    }
    // Operator tiles must survive partial blocks and routing changes at arbitrary sample
    // offsets. With vibrato disabled, different host block sizes produce identical floats.
    for (int algorithm = 1; algorithm <= 32; ++algorithm) {
        Instance whole(d), split(d);
        for (Instance *in : {&whole, &split}) {
            CHECK(in->loadPreset(1));
            in->setParam(paramId(d, "algorithm"), (float)algorithm);
            in->setParam(paramId(d, "feedback"), 7);
        }
        daw_event_t route{};
        route.type = DAW_EV_PARAM;
        route.u.param.id = paramId(d, "algorithm");
        route.u.param.value = (float)(algorithm % 32 + 1);
        std::vector<TimedEvent> events = {{5.0 / sr, noteOn(48, .8f, 1)},
            {23.0 / sr, noteOn(67, .6f, 2)}, {881.0 / sr, route},
            {2003.0 / sr, noteOff(48, 1)}, {3041.0 / sr, noteOff(67, 2)}};
        const auto a = render(whole, events, 1.6, 256), b = render(split, events, 1.6, 17);
        CHECK(a.l.size() == b.l.size() && !memcmp(a.l.data(), b.l.data(), a.l.size() * sizeof(float)) &&
              !memcmp(a.r.data(), b.r.data(), a.r.size() * sizeof(float)));
        CHECK(whole.voices() == 0 && split.voices() == 0);
    }
    // velocity sensitivity, polyphony limit, every algorithm stable, release frees the voices
    {
        Instance soft(d), loud(d);
        auto rs = render(soft, {{0.0, noteOn(60, 0.2f, 1)}}, 0.3), rl = render(loud, {{0.0, noteOn(60, 1.0f, 1)}}, 0.3);
        CHECK(rmsOf(rl.l, 0, (size_t)(0.3 * sr)) > rmsOf(rs.l, 0, (size_t)(0.3 * sr)) * 1.3);
        Instance poly(d);
        poly.setParam(paramId(d, "polyphony"), 3);
        std::vector<TimedEvent> ev;
        for (int i = 0; i < 5; i++)
            ev.push_back({0.01 * i, noteOn((uint8_t)(60 + 3 * i), 0.8f, i)});
        render(poly, ev, 0.2);
        CHECK(poly.voices() == 3);
        for (int algo = 1; algo <= 32; algo++) {
            Instance in(d);
            in.setParam(paramId(d, "algorithm"), (float)algo);
            in.setParam(paramId(d, "feedback"), 7);
            for (int o = 1; o <= 6; o++)
                in.setParam(paramId(d, ("op" + std::to_string(o) + "_level").c_str()), 99);
            auto r = render(in, {{0.0, noteOn(48, 1.0f, 1)}, {0.0, noteOn(84, 1.0f, 2)}, {0.3, noteOff(48, 1)}, {0.3, noteOff(84, 2)}}, 1.5);
            bool ok = true;
            for (float v : r.l)
                ok &= std::isfinite(v) && v < 4.0f && v > -4.0f;
            CHECK(ok);
            CHECK(in.voices() == 0);
        }
    }
}

// the strongest frequency in [lo, hi] (0.05 Hz steps, single-bin DFT)
static double peakFreq(const std::vector<float> &x, size_t from, size_t to, double lo, double hi, double sr)
{
    double best = lo, bestA = -1;
    for (double f = lo; f <= hi; f += 0.05) {
        const double a = toneAmp(x, from, to, f, sr);
        if (a > bestA) {
            bestA = a;
            best = f;
        }
    }
    return best;
}

static void testMacroSynth()
{
    auto *d = find("org.smhgzll.macrosynth");
    CHECK(d != nullptr);
    if (!d)
        return;
    const uint32_t model = paramId(d, "model");
    // pitch: A3 is 220 Hz at both host rates (Braids runs at 2 x the host rate, pitch-corrected)
    for (uint32_t rate : {44100u, 48000u})
        for (float m : {0.0f, 2.0f}) {
            Instance in(d, rate);
            in.setParam(model, m);
            in.setParam(paramId(d, "sustain"), 1);
            auto r = render(in, {{0.0, noteOn(57, 1.0f, 1)}}, 1.2);
            CHECK_NEAR(peakFreq(r.l, rate / 5, rate / 5 + rate, 200, 240, rate), 220.0, 0.5);
            Instance up(d, rate);
            up.setParam(model, m);
            up.setParam(paramId(d, "coarse"), 12);
            up.setParam(paramId(d, "sustain"), 1);
            auto ru = render(up, {{0.0, noteOn(57, 1.0f, 1)}}, 1.2);
            CHECK_NEAR(peakFreq(ru.l, rate / 5, rate / 5 + rate, 420, 460, rate), 440.0, 0.8);
        }
    const double sr = 44100;
    const size_t N = 44100;
    // every model: sounds, stays finite and bounded at both ends of the keyboard and of
    // timbre / color, and frees its voices after the release
    for (int m = 0; m < 48; m++)
        for (float tc : {0.0f, 1.0f}) {
            Instance in(d);
            in.setParam(model, (float)m);
            in.setParam(paramId(d, "timbre"), tc);
            in.setParam(paramId(d, "color"), 1.0f - tc);
            in.setParam(paramId(d, "sustain"), 1);
            in.setParam(paramId(d, "release"), 50);
            auto r = render(in, {{0.0, noteOn(36, 1.0f, 1)}, {0.0, noteOn(96, 1.0f, 2)}, {0.4, noteOff(36, 1)}, {0.4, noteOff(96, 2)}}, 1.0);
            bool ok = true;
            for (float v : r.l)
                ok &= std::isfinite(v) && v < 2.0f && v > -2.0f;
            if (!ok || rmsOf(r.l, 0, (size_t)(0.4 * sr)) < 1e-4)
                fprintf(stderr, "macrosynth model %d timbre %.0f: rms %g\n", m, tc, rmsOf(r.l, 0, (size_t)(0.4 * sr)));
            CHECK(ok);
            CHECK(rmsOf(r.l, 0, (size_t)(0.4 * sr)) > 1e-4);
            CHECK(in.voices() == 0);
        }
    // percussive models are struck by the note-on: they decay under a held envelope, and a new
    // note strikes again
    for (int m : {28, 33, 34}) {
        Instance in(d);
        in.setParam(model, (float)m);
        in.setParam(paramId(d, "sustain"), 1);
        in.setParam(paramId(d, "polyphony"), 1);
        auto r = render(in, {{0.0, noteOn(48, 1.0f, 1)}, {1.5, noteOn(48, 1.0f, 2)}}, 2.0);
        const double hit = rmsOf(r.l, 0, N / 5), tail = rmsOf(r.l, (size_t)(1.2 * sr), (size_t)(1.4 * sr));
        const double again = rmsOf(r.l, (size_t)(1.5 * sr), (size_t)(1.7 * sr));
        CHECK(tail < hit * (m == 28 ? 0.5 : 0.1));  // the plucked string rings longer
        CHECK(again > hit * 0.5);
    }
    // timbre opens the low-pass filter model: more upper harmonics
    {
        auto bright = [&](float t) {
            Instance in(d);
            in.setParam(model, 17);
            in.setParam(paramId(d, "timbre"), t);
            in.setParam(paramId(d, "color"), 0);
            in.setParam(paramId(d, "sustain"), 1);
            auto r = render(in, {{0.0, noteOn(45, 1.0f, 1)}}, 1.0);
            return toneAmp(r.l, N / 2, N, 110 * 8, sr) / toneAmp(r.l, N / 2, N, 110, sr);
        };
        CHECK(bright(0.9f) > bright(0.1f) * 4);
    }
    // the noise models render the same every time (the shared random generator is reseeded)
    {
        std::vector<float> first;
        for (int k = 0; k < 2; k++) {
            Instance in(d);
            in.setParam(model, 41);
            auto r = render(in, {{0.0, noteOn(60, 1.0f, 1)}}, 0.3);
            if (k == 0)
                first = r.l;
            else
                CHECK(r.l == first);
        }
    }
    // velocity and the polyphony limit
    {
        Instance soft(d), loud(d);
        auto rs = render(soft, {{0.0, noteOn(60, 0.2f, 1)}}, 0.3), rl = render(loud, {{0.0, noteOn(60, 1.0f, 1)}}, 0.3);
        CHECK(rmsOf(rl.l, 0, (size_t)(0.3 * sr)) > rmsOf(rs.l, 0, (size_t)(0.3 * sr)) * 1.3);
        Instance poly(d);
        std::vector<TimedEvent> ev;
        for (int i = 0; i < 4; i++)
            ev.push_back({0.01 * i, noteOn((uint8_t)(60 + 3 * i), 0.8f, i)});
        render(poly, ev, 0.2);
        CHECK(poly.voices() == 2);
    }
}

static AudioFile sineFile(double f, double seconds, uint32_t rate, int root, uint32_t channels = 1)
{
    AudioFile a;
    a.channels = channels;
    a.rate = rate;
    a.rootKey = root;
    for (uint32_t c = 0; c < channels; c++) {
        a.ch[c].resize((size_t)(seconds * rate));
        for (size_t i = 0; i < a.ch[c].size(); i++)
            a.ch[c][i] = (float)(0.5 * sin(2 * M_PI * f * (double)i / rate) * (c ? -1 : 1));
    }
    return a;
}

static void testSampler()
{
    auto *d = find("org.smhgzll.sampler");
    CHECK(d != nullptr);
    if (!d)
        return;
    setDefaultResources(nullptr);
    const double sr = 44100;
    const size_t N = 44100;
    auto play = [&](AudioFile &a, uint8_t key, double seconds, std::vector<std::pair<const char *, float>> params = {},
                    double offAt = -1) {
        Instance in(d);
        for (auto &p : params)
            in.setParam(paramId(d, p.first), p.second);
        CHECK(in.setResource(0, a.resource()) == DAW_OK);
        std::vector<TimedEvent> ev{{0.0, noteOn(key, 1.0f, 1)}};
        if (offAt >= 0)
            ev.push_back({offAt, noteOff(key, 1)});
        auto r = render(in, ev, seconds);
        return std::make_pair(r, in.voices());
    };
    // the file's root key (smpl chunk): key 69 plays it as recorded, 81 an octave up
    {
        auto a = sineFile(440, 2.0, 44100, 69);
        auto r = play(a, 69, 1.0).first;
        CHECK_NEAR(zeroCrossFreq(r.l, N / 10, N, sr), 440.0, 0.5);
        auto up = play(a, 81, 0.5).first;
        CHECK_NEAR(zeroCrossFreq(up.l, N / 20, N / 2, sr), 880.0, 1.0);
        // the Root parameter when the file has none (or when told to ignore it)
        auto noRoot = sineFile(440, 2.0, 44100, -1);
        auto r60 = play(noRoot, 72, 0.5, {{"root", 60}}).first;
        CHECK_NEAR(zeroCrossFreq(r60.l, N / 20, N / 2, sr), 880.0, 1.0);
        auto ign = play(a, 69, 0.5, {{"file_root", 0}, {"root", 57}}).first;
        CHECK_NEAR(zeroCrossFreq(ign.l, N / 20, N / 2, sr), 880.0, 1.0);
    }
    // a file at another sample rate plays at its own pitch
    {
        auto a = sineFile(440, 2.0, 22050, 69);
        auto r = play(a, 69, 1.0).first;
        CHECK_NEAR(zeroCrossFreq(r.l, N / 10, N, sr), 440.0, 0.5);
    }
    // without a loop the voice ends with the sample (0.5 s file); end at 50 % halves it
    {
        auto a = sineFile(440, 0.5, 44100, 69);
        auto r = play(a, 69, 1.0);
        CHECK(r.second == 0);
        CHECK(peak(std::vector<float>(r.first.l.begin() + (long)(0.55 * sr), r.first.l.end())) == 0.0f);
        auto half = play(a, 69, 1.0, {{"end", 50}}).first;
        CHECK(peak(std::vector<float>(half.l.begin() + (long)(0.3 * sr), half.l.end())) == 0.0f);
        CHECK(rmsOf(half.l, N / 20, N / 5) > 0.2);
        // forward loop: still sounding after 1 s, at the same pitch
        auto lp = play(a, 69, 1.5, {{"loop", 1}});
        CHECK(lp.second == 1);
        CHECK_NEAR(zeroCrossFreq(lp.first.l, N, N * 3 / 2, sr), 440.0, 1.0);
        // one-shot ignores the note-off; normal mode releases
        auto os = play(a, 69, 0.45, {{"oneshot", 1}, {"release", 5}}, 0.1);
        CHECK(rmsOf(os.first.l, (size_t)(0.3 * sr), (size_t)(0.4 * sr)) > 0.2);
        auto rel = play(a, 69, 0.45, {{"release", 5}}, 0.1);
        CHECK(rmsOf(rel.first.l, (size_t)(0.3 * sr), (size_t)(0.4 * sr)) < 1e-3);
    }
    // reverse: a rising ramp plays falling
    {
        AudioFile ramp;
        ramp.channels = 1;
        ramp.rate = 44100;
        ramp.rootKey = 60;
        ramp.ch[0].resize(4410);
        for (size_t i = 0; i < ramp.ch[0].size(); i++)
            ramp.ch[0][i] = (float)i / 4410.0f;
        auto r = play(ramp, 60, 0.2, {{"reverse", 1}, {"velocity", 0}, {"level", 1}}).first;
        CHECK(r.l[200] > r.l[3000] && r.l[200] > 0.8f);
    }
    // stereo files keep their channels; no resource: silent and no voices
    {
        auto st = sineFile(440, 1.0, 44100, 69, 2);
        auto r = play(st, 69, 0.3).first;
        CHECK(r.l[1000] * r.r[1000] < 0);  // the right channel is the inverted left
        Instance none(d);
        auto rn = render(none, {{0.0, noteOn(60, 1.0f, 1)}}, 0.3);
        CHECK(peak(rn.l) == 0.0f && none.voices() == 0);
    }
}

static void testSoundFont()
{
    auto *d = find("org.smhgzll.soundfont");
    CHECK(d != nullptr);
    if (!d)
        return;
    setDefaultResources(nullptr);
    const double sr = 44100;
    const size_t N = 44100;
    FileResource font;
    font.bytes = makeTestSoundFont(261.6256, 60);
    font.name = "test.sf2";
    auto play = [&](std::vector<TimedEvent> ev, double seconds, std::vector<std::pair<const char *, float>> params = {}) {
        Instance in(d);
        CHECK(in.setResource(0, font.resource()) == DAW_OK);
        for (auto &p : params)
            in.setParam(paramId(d, p.first), p.second);
        auto r = render(in, ev, seconds);
        return std::make_pair(r, in.voices());
    };
    // pitch at the root key and an octave up; the loop sustains a held note past the sample's end.
    // The test font's sine has a whole number of samples per period (seamless loop): 44100 / 169 Hz
    const double f0 = 44100.0 / lrint(44100.0 / 261.6256);
    auto r60 = play({{0.0, noteOn(60, 1.0f, 1)}}, 1.5).first;
    CHECK_NEAR(zeroCrossFreq(r60.l, N / 10, N, sr), f0, 0.05);
    CHECK(rmsOf(r60.l, N, N * 3 / 2) > 0.05);
    auto r72 = play({{0.0, noteOn(72, 1.0f, 1)}}, 0.5).first;
    CHECK_NEAR(zeroCrossFreq(r72.l, N / 20, N / 2, sr), 2 * f0, 0.1);
    // transpose
    auto rt = play({{0.0, noteOn(60, 1.0f, 1)}}, 0.5, {{"transpose", 12}}).first;
    CHECK_NEAR(zeroCrossFreq(rt.l, N / 20, N / 2, sr), 2 * f0, 0.1);
    // a program the font does not have falls back to its first preset
    auto rp = play({{0.0, noteOn(60, 1.0f, 1)}}, 0.3, {{"program", 40}, {"bank", 128}}).first;
    CHECK(rmsOf(rp.l, N / 20, N * 3 / 10) > 0.05);
    // velocity, release, voices freed
    auto soft = play({{0.0, noteOn(60, 0.2f, 1)}}, 0.3).first, loud = play({{0.0, noteOn(60, 1.0f, 1)}}, 0.3).first;
    CHECK(rmsOf(loud.l, N / 20, N / 4) > rmsOf(soft.l, N / 20, N / 4) * 1.5);
    auto rel = play({{0.0, noteOn(60, 1.0f, 1)}, {0.2, noteOff(60, 1)}}, 2.0);
    CHECK(rel.second == 0);
    CHECK(peak(std::vector<float>(rel.first.l.end() - 4410, rel.first.l.end())) == 0.0f);
    // chords: several voices
    auto ch = play({{0.0, noteOn(60, 1.0f, 1)}, {0.0, noteOn(64, 1.0f, 2)}, {0.0, noteOn(67, 1.0f, 3)}}, 0.2);
    CHECK(ch.second == 3);
    // A shared, aligned font must not allocate a duplicate sample pool. An unaligned memory
    // resource takes the PCM16 copy fallback and sounds exactly the same; detach releases it.
    {
        std::vector<uint8_t> bytes(font.bytes.size() + 1);
        memcpy(bytes.data() + 1, font.bytes.data(), font.bytes.size());
        daw_resource_t unaligned = *font.resource();
        unaligned.data = bytes.data() + 1;
        Instance aligned(d), copied(d);
        CHECK(aligned.setResource(0, font.resource()) == DAW_OK);
        CHECK(copied.setResource(0, &unaligned) == DAW_OK);
        // (both hold the font's metadata and 24 voices; only the copy holds samples)
        const size_t pool = copied.host().largeBytes - aligned.host().largeBytes;
        CHECK(aligned.host().largeBytes < 16 * 1024);
        CHECK(pool > font.bytes.size() / 2);
        CHECK(pool < font.bytes.size() + 4096);
        auto a = render(aligned, {{0.0, noteOn(60, .8f, 1)}}, .25);
        auto b = render(copied, {{0.0, noteOn(60, .8f, 1)}}, .25);
        CHECK(hashPcm(a.l, a.r) == hashPcm(b.l, b.r));
        CHECK(copied.setResource(0, nullptr) == DAW_OK);
        CHECK(copied.host().largeBytes == 0);
        CHECK(copied.voices() == 0);
        CHECK(aligned.host().rtViolations == 0 && copied.host().rtViolations == 0);
    }
    // TinySoundFont's voice loop runs most samples in runs without its per-sample end / loop tests
    // (PROVENANCE.md). They must sound exactly like the per-sample loop: a held note looping many
    // times, notes pitched far up (runs end near every loop wrap), a pitch bend sweep, releases;
    // without and with the voice low-pass. The hashes were recorded with the per-sample loop.
    // Voices live in LARGE memory: a channel takes almost no internal RAM.
    {
        const uint64_t expected[2] = {0x873bb3540795ccb7ull, 0xf7d8ad88921f893dull};
        int k = 0;
        for (int filter : {0, 9000}) {
            FileResource f2;
            f2.bytes = makeTestSoundFont(261.6256, 60, 44100, 0.5, filter);
            f2.name = "runs.sf2";
            Instance in(d);
            CHECK(in.setResource(0, f2.resource()) == DAW_OK);
            std::vector<TimedEvent> ev{{0.0, noteOn(48, .9f, 1)}, {0.0, noteOn(96, .7f, 2)}, {0.05, noteOn(120, .6f, 3)}};
            for (int i = 0; i < 16; i++) {
                daw_event_t b{};
                b.type = DAW_EV_PITCH_BEND;
                b.u.midi.value = sinf(i * .7f);
                ev.push_back({0.3 + i * 0.04, b});
            }
            ev.push_back({1.2, noteOff(96, 2)});
            ev.push_back({1.3, noteOff(48, 1)});
            ev.push_back({1.3, noteOff(120, 3)});
            auto r = render(in, ev, 2.0);
            CHECK(hashPcm(r.l, r.r) == expected[k++]);
            CHECK(rmsOf(r.l, 0, N) > 0.05);
            CHECK(in.host().fastBytes < 1024);
        }
    }
    // Voices: TinySoundFont plays at most that many samples. A note that finds no free voice takes
    // the oldest one instead of being dropped: with one voice, a second held note an octave up
    // replaces the first (upstream TinySoundFont kept the first and dropped the second).
    {
        auto one = play({{0.0, noteOn(60, 1.0f, 1)}, {0.1, noteOn(72, 1.0f, 2)}}, 0.6, {{"polyphony", 1}});
        CHECK(one.second == 1);
        CHECK_NEAR(zeroCrossFreq(one.first.l, N / 5, N * 6 / 10, sr), 2 * f0, 0.2);
        auto four = play({{0.0, noteOn(48, 1.0f, 1)}, {0.0, noteOn(52, 1.0f, 2)}, {0.0, noteOn(55, 1.0f, 3)}, {0.0, noteOn(60, 1.0f, 4)},
                          {0.05, noteOn(64, 1.0f, 5)}, {0.05, noteOn(67, 1.0f, 6)}},
                         0.3, {{"polyphony", 4}});
        CHECK(four.second == 4);
        // fewer voices while notes sound: the ones beyond are silenced at once
        Instance in(d);
        CHECK(in.setResource(0, font.resource()) == DAW_OK);
        render(in, {{0.0, noteOn(60, 1.0f, 1)}, {0.0, noteOn(64, 1.0f, 2)}, {0.0, noteOn(67, 1.0f, 3)}}, 0.1);
        CHECK(in.voices() == 3);
        in.setParam(paramId(d, "polyphony"), 1);
        render(in, {}, 0.05);
        CHECK(in.voices() <= 1);
        CHECK(in.host().rtViolations == 0);
    }
    // refusals and an empty slot
    Instance in(d);
    FileResource junk;
    junk.bytes = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 's', 'f', 'b', 'k', 1, 2, 3};
    CHECK(in.setResource(0, junk.resource()) != DAW_OK);
    auto rn = render(in, {{0.0, noteOn(60, 1.0f, 1)}}, 0.2);
    CHECK(peak(rn.l) == 0.0f && in.voices() == 0);
}

static void testEffects()
{
    const double sr = 44100;
    // limiter: +20 dB peaks never exceed the ceiling; quiet material passes unchanged
    {
        auto *d = find("org.smhgzll.fx.limiter");
        Instance in(d);
        in.setParam(paramId(d, "ceiling"), -1.0f);
        std::vector<float> l(44100), r(44100);
        for (size_t i = 0; i < l.size(); i++) {
            double t = i / sr;
            float burst = fmod(t, 0.25) < 0.05 ? 10.0f : 0.1f;
            l[i] = burst * (float)sin(2 * M_PI * 100 * t);
            r[i] = burst * (float)sin(2 * M_PI * 3000 * t);
        }
        auto o = render(in, {}, 1.0, 256, &l, &r);
        float ceil = powf(10.0f, -1.0f / 20.0f);
        CHECK(peak(o.l) <= ceil + 1e-6f);
        CHECK(peak(o.r) <= ceil + 1e-6f);
        CHECK(peak(o.l) > ceil * 0.9f);  // and it does reach it
        Instance q(d);
        std::vector<float> ql(44100), qr(44100);
        for (size_t i = 0; i < ql.size(); i++)
            ql[i] = qr[i] = 0.3f * (float)sin(2 * M_PI * 440 * i / sr);
        auto oq = render(q, {}, 1.0, 256, &ql, &qr);
        for (size_t i = 1000; i < ql.size(); i += 997)  // delayed by 64 samples, unity gain
            CHECK_NEAR(oq.l[i], ql[i - 64], 1e-5);
    }
    // delay: an impulse comes back after the set time; with sync 1/4 at 120 BPM after 0.5 s
    {
        auto *d = find("org.smhgzll.fx.delay");
        auto echoAt = [&](float timeMs, float sync) {
            Instance in(d);
            in.setParam(paramId(d, "sync"), sync);
            in.setParam(paramId(d, "time"), timeMs);
            in.setParam(paramId(d, "pingpong"), 0);
            in.setParam(paramId(d, "feedback"), 0);
            in.setParam(paramId(d, "mix"), 100);
            in.setParam(paramId(d, "damping"), 20000);
            std::vector<float> l(66150, 0.0f), r(66150, 0.0f);
            l[100] = r[100] = 1.0f;
            auto o = render(in, {}, 1.5, 256, &l, &r);
            size_t best = 0;
            for (size_t i = 0; i < o.l.size(); i++)
                if (fabsf(o.l[i]) > fabsf(o.l[best]))
                    best = i;
            return (double)best - 100;
        };
        CHECK_NEAR(echoAt(250, 0), 0.250 * sr, 2);
        CHECK_NEAR(echoAt(250, 8), 0.5 * sr, 2);  // 1/4 note at the host's 120 BPM
        // feedback: successive echoes decay; ping-pong alternates sides
        Instance in(d);
        in.setParam(paramId(d, "sync"), 0);
        in.setParam(paramId(d, "time"), 100);
        in.setParam(paramId(d, "feedback"), 50);
        in.setParam(paramId(d, "mix"), 100);
        in.setParam(paramId(d, "pingpong"), 1);
        std::vector<float> l(44100, 0.0f), r(44100, 0.0f);
        l[0] = r[0] = 0.5f;
        auto o = render(in, {}, 1.0, 256, &l, &r);
        float e1l = fabsf(o.l[4410]), e2r = fabsf(o.r[8820]), e3l = fabsf(o.l[13230]);
        CHECK(e1l > 0.1f && e2r > 0.01f && e2r < e1l && e3l < e2r);
        CHECK(fabsf(o.r[4410]) < 1e-4f);  // first echo only on the left

        // A delay that starts empty, then drains completely, must wake for later notes without
        // losing their timing or dropping the tail between the input and its echo.
        Instance wake(d);
        wake.setParam(paramId(d, "sync"), 0);
        wake.setParam(paramId(d, "time"), 100);
        wake.setParam(paramId(d, "feedback"), 0);
        wake.setParam(paramId(d, "mix"), 100);
        wake.setParam(paramId(d, "pingpong"), 0);
        std::vector<float> wl(7 * 44100, 0.0f), wr(wl.size(), 0.0f);
        wl[110250] = wl[286650] = 0.5f;  // 2.5 s and 6.5 s, longer than the 2 s lines
        const auto wo = render(wake, {}, 7.0, 256, &wl, &wr);
        CHECK(wo.l[114660] > 0.1f);  // echo at 2.6 s
        CHECK_NEAR(wo.l[291060], wo.l[114660], 1e-7);  // same echo after the second wake-up
        CHECK(peak(wo.r) == 0.0f);
    }
    // filter: low pass at 500 Hz passes 100 Hz and attenuates 5 kHz
    {
        auto *d = find("org.smhgzll.fx.filter");
        auto gainAt = [&](double hz) {
            Instance in(d);
            in.setParam(paramId(d, "cutoff"), 500);
            in.setParam(paramId(d, "resonance"), 0);
            std::vector<float> l(44100), r(44100);
            for (size_t i = 0; i < l.size(); i++)
                l[i] = r[i] = 0.5f * (float)sin(2 * M_PI * hz * i / sr);
            auto o = render(in, {}, 1.0, 256, &l, &r);
            std::vector<float> tail(o.l.begin() + 22050, o.l.end()), itail(l.begin() + 22050, l.end());
            return rms(tail) / rms(itail);
        };
        CHECK(gainAt(100) > 0.85);
        CHECK(gainAt(5000) < 0.02);
    }
    // utility: -6 dB and hard left pan
    {
        auto *d = find("org.smhgzll.fx.utility");
        Instance in(d);
        in.setParam(paramId(d, "gain"), -6.0206f);
        in.setParam(paramId(d, "pan"), -1.0f);
        std::vector<float> l(4410, 0.8f), r(4410, 0.8f);
        auto o = render(in, {}, 0.1, 256, &l, &r);
        CHECK_NEAR(o.l.back(), 0.4, 1e-3);
        CHECK_NEAR(o.r.back(), 0.0, 1e-4);
    }
}

// amplitude of frequency f in x[from, to) (Goertzel), as a sine amplitude
static double toneAmp(const std::vector<float> &x, size_t from, size_t to, double f, double sr)
{
    const double w = 2 * M_PI * f / sr, cw = 2 * cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = from; i < to; i++) {
        const double s0 = x[i] + cw * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * cos(w), im = s2 * sin(w);
    return 2 * sqrt(re * re + im * im) / (double)(to - from);
}

static std::vector<float> sineVec(double f, double amp, size_t n, double sr = 44100)
{
    std::vector<float> x(n);
    for (size_t i = 0; i < n; i++)
        x[i] = (float)(amp * sin(2 * M_PI * f * (double)i / sr));
    return x;
}

static void testMoreEffects()
{
    const double sr = 44100;
    const size_t N = 44100;
    auto fx = [&](const char *id) { return find(id); };
    auto run = [&](Instance &in, const std::vector<float> &l, const std::vector<float> &r) {
        return render(in, {}, (double)l.size() / sr, 256, &l, &r);
    };
    // every new effect at 0 % mix is the identity
    for (const char *id : {"org.smhgzll.fx.reverb", "org.smhgzll.fx.chorus", "org.smhgzll.fx.flanger", "org.smhgzll.fx.compressor", "org.smhgzll.fx.distortion"}) {
        auto *d = fx(id);
        CHECK(d != nullptr);
        if (!d)
            continue;
        Instance in(d);
        in.setParam(paramId(d, "mix"), 0);
        auto x = sineVec(440, 0.5, N);
        auto o = run(in, x, x);
        double err = 0;
        for (size_t i = 0; i < N; i++)
            err = fmax(err, fabs(o.l[i] - x[i]));
        CHECK(err < 1e-6);
    }
    // EQ: flat by default (exact), a +12 dB peak at 1 kHz, a low cut, a high shelf
    {
        auto *d = fx("org.smhgzll.fx.eq");
        Instance flat(d);
        auto x = sineVec(1000, 0.25, N);
        auto o = run(flat, x, x);
        double err = 0;
        for (size_t i = 0; i < N; i++)
            err = fmax(err, fabs(o.l[i] - x[i]));
        CHECK(err < 1e-6);
        auto gainDb = [&](Instance &in, double f) {
            auto s = sineVec(f, 0.1, N);
            auto r = run(in, s, s);
            return 20 * log10(toneAmp(r.l, N / 2, N, f, sr) / 0.1);
        };
        Instance pk(d);
        pk.setParam(paramId(d, "mid1_freq"), 1000);
        pk.setParam(paramId(d, "mid1_gain"), 12);
        pk.setParam(paramId(d, "mid1_q"), 1);
        CHECK_NEAR(gainDb(pk, 1000), 12.0, 0.3);
        CHECK_NEAR(gainDb(pk, 100), 0.0, 0.5);
        Instance lc(d);
        lc.setParam(paramId(d, "lowcut_on"), 1);
        lc.setParam(paramId(d, "lowcut"), 200);
        CHECK(gainDb(lc, 50) < -20.0);
        CHECK_NEAR(gainDb(lc, 2000), 0.0, 0.3);
        Instance hs(d);
        hs.setParam(paramId(d, "high_freq"), 4000);
        hs.setParam(paramId(d, "high_gain"), 6);
        CHECK_NEAR(gainDb(hs, 15000), 6.0, 0.7);
        CHECK_NEAR(gainDb(hs, 200), 0.0, 0.3);
    }
    // compressor: the static curve (hard knee): -6 dB in, -24 dB threshold, 4:1 -> -19.5 dB out
    {
        auto *d = fx("org.smhgzll.fx.compressor");
        auto outDb = [&](double inAmp, float makeup) {
            Instance in(d);
            in.setParam(paramId(d, "threshold"), -24);
            in.setParam(paramId(d, "ratio"), 4);
            in.setParam(paramId(d, "knee"), 0);
            in.setParam(paramId(d, "attack"), 1);
            in.setParam(paramId(d, "release"), 200);
            in.setParam(paramId(d, "makeup"), makeup);
            auto s = sineVec(1000, inAmp, N);
            auto r = run(in, s, s);
            return 20 * log10(toneAmp(r.l, N / 2, N, 1000, sr));
        };
        CHECK_NEAR(outDb(0.5, 0), -19.5, 1.0);
        CHECK_NEAR(outDb(0.5, 6), -13.5, 1.0);
        CHECK_NEAR(outDb(0.02, 0), 20 * log10(0.02), 0.1);  // -34 dB: below the threshold, untouched
    }
    // flanger without modulation or feedback, 50 % mix: a comb with its first notch at 1 / (2 D)
    {
        auto *d = fx("org.smhgzll.fx.flanger");
        auto gainDb = [&](double f) {
            Instance in(d);
            in.setParam(paramId(d, "depth"), 0);
            in.setParam(paramId(d, "delay"), 1.0f);  // D = 1 ms -> notch at 500 Hz, peak at 1 kHz
            in.setParam(paramId(d, "feedback"), 0);
            in.setParam(paramId(d, "mix"), 50);
            auto s = sineVec(f, 0.2, N);
            auto r = run(in, s, s);
            return 20 * log10(toneAmp(r.l, N / 2, N, f, sr) / 0.2);
        };
        CHECK(gainDb(500) < -25.0);
        CHECK_NEAR(gainDb(1000), 0.0, 0.3);
        // maximum feedback, both signs, on noise: stays finite and bounded
        for (float fb : {95.0f, -95.0f}) {
            Instance in(d);
            in.setParam(paramId(d, "feedback"), fb);
            in.setParam(paramId(d, "mix"), 100);
            std::vector<float> n(N);
            dawsdk::dsp::Rng rng;
            for (auto &v : n)
                v = 0.5f * rng.bipolar();
            auto r = run(in, n, n);
            bool ok = true;
            for (float v : r.l)
                ok &= std::isfinite(v);
            CHECK(ok && peak(r.l) < 8.0f);
        }
    }
    // chorus: changes the signal, keeps its level, decorrelates the channels
    {
        auto *d = fx("org.smhgzll.fx.chorus");
        Instance in(d);
        in.setParam(paramId(d, "mix"), 100);
        auto x = sineVec(440, 0.3, N);
        auto r = run(in, x, x);
        double diff = 0, lr = 0;
        for (size_t i = N / 4; i < N; i++) {
            diff += fabs(r.l[i] - x[i]);
            lr += fabs(r.l[i] - r.r[i]);
        }
        CHECK(diff / (N * 0.75) > 0.01);
        CHECK(lr / (N * 0.75) > 0.01);
        CHECK(rmsOf(r.l, N / 4, N) > 0.15 && rmsOf(r.l, N / 4, N) < 0.3);
    }
    // reverb: a longer tail for a bigger room; freeze holds it; width 0 is mono
    {
        auto *d = fx("org.smhgzll.fx.reverb");
        // Separate and aliased output buffers, including an event split and partial blocks,
        // must produce identical tails after a silent start. The ABI permits in-place effects.
        for (float quality : {0.0f, 1.0f}) {
            Instance separate(d), inplace(d);
            separate.setParam(paramId(d, "quality"), quality);
            inplace.setParam(paramId(d, "quality"), quality);
            float l[256], r[256], ol[256], oright[256], il[256], ir[256];
            bool same = true;
            for (int block = 0; block < 80; block++) {
                const uint32_t frames = block % 3 == 0 ? 73 : 256;
                for (uint32_t i = 0; i < frames; i++) {
                    l[i] = il[i] = block == 4 && i == 0 ? .7f : 0;
                    r[i] = ir[i] = block == 4 && i == 0 ? -.3f : 0;
                }
                std::vector<daw_event_t> ev;
                if (block == 20) {
                    auto e = paramEv(paramId(d, "width"), 30); e.offset = 31; ev.push_back(e);
                }
                separate.process(frames, ev, ol, oright, l, r);
                inplace.process(frames, ev, il, ir, il, ir);
                same &= memcmp(ol, il, frames * sizeof(float)) == 0 && memcmp(oright, ir, frames * sizeof(float)) == 0;
            }
            CHECK(same);
            CHECK(separate.host().rtViolations == 0 && inplace.host().rtViolations == 0);
        }
        auto tail = [&](float size, float freeze, float width, double from, double to, std::vector<float> *right = nullptr, float quality = 0) {
            Instance in(d);
            in.setParam(paramId(d, "quality"), quality);
            in.setParam(paramId(d, "size"), size);
            in.setParam(paramId(d, "freeze"), 0);
            in.setParam(paramId(d, "width"), width);
            in.setParam(paramId(d, "mix"), 100);
            in.setParam(paramId(d, "predelay"), 0);
            std::vector<float> l(3 * N, 0.0f);
            for (size_t i = 0; i < N / 10; i++)  // 100 ms burst
                l[i] = (float)sin(2 * M_PI * 300 * (double)i / sr) * 0.5f;
            std::vector<TimedEvent> ev;
            if (freeze > 0)
                ev.push_back({0.5, paramEv(paramId(d, "freeze"), 1.0f)});
            auto r = render(in, ev, 3.0, 256, &l, &l);
            if (right)
                *right = r.r;
            return rmsOf(r.l, (size_t)(from * sr), (size_t)(to * sr));
        };
        const double small = tail(20, 0, 100, 1.0, 1.5), big = tail(90, 0, 100, 1.0, 1.5);
        CHECK(big > small * 3.0);
        // Light quality (the network at half rate): the same room, within a dB or two
        const double smallL = tail(20, 0, 100, 1.0, 1.5, nullptr, 1), bigL = tail(90, 0, 100, 1.0, 1.5, nullptr, 1);
        CHECK(bigL > smallL * 3.0);
        CHECK(fabs(20 * log10(bigL / big)) < 2.0);
        CHECK(fabs(20 * log10(tail(60, 0, 100, 0.2, 0.7, nullptr, 1) / tail(60, 0, 100, 0.2, 0.7))) < 2.0);
        const double frozenL1 = tail(70, 1, 100, 1.0, 1.5, nullptr, 1), frozenL2 = tail(70, 1, 100, 2.4, 2.9, nullptr, 1);
        CHECK(frozenL1 > 1e-3 && frozenL2 > frozenL1 * 0.5);
        {
            // switching the quality while it rings: an empty room, then the new quality; never NaN
            Instance in(d);
            in.setParam(paramId(d, "mix"), 100);
            auto x = sineVec(300, 0.3, N);
            std::vector<TimedEvent> ev = {{0.3, paramEv(paramId(d, "quality"), 1)}, {0.6, paramEv(paramId(d, "quality"), 0)}};
            auto r = render(in, ev, 1.0, 256, &x, &x);
            bool ok = true;
            for (float v : r.l)
                ok &= std::isfinite(v) && fabs(v) < 4.0f;
            CHECK(ok);
            CHECK(rmsOf(r.l, (size_t)(0.8 * sr), (size_t)(0.95 * sr)) > 1e-3);
        }
        const double frozen1 = tail(70, 1, 100, 1.0, 1.5), frozen2 = tail(70, 1, 100, 2.4, 2.9);
        CHECK(frozen1 > 1e-3 && frozen2 > frozen1 * 0.5);
        std::vector<float> r0;
        tail(70, 0, 0, 0.2, 0.6, &r0);
        Instance mono(d);
        mono.setParam(paramId(d, "width"), 0);
        mono.setParam(paramId(d, "mix"), 100);
        auto x = sineVec(300, 0.3, N);
        auto rm = run(mono, x, x);
        double lr = 0;
        for (size_t i = 0; i < N; i++)
            lr = fmax(lr, fabs(rm.l[i] - rm.r[i]));
        CHECK(lr < 1e-6);
    }
    // distortion: near-linear at low level; hard clip bounded; ADAA keeps aliasing low
    {
        auto *d = fx("org.smhgzll.fx.distortion");
        Instance soft(d);
        soft.setParam(paramId(d, "drive"), 0);
        soft.setParam(paramId(d, "output"), 0);
        soft.setParam(paramId(d, "tone"), 20000);
        auto x = sineVec(500, 0.05, N);
        auto r = run(soft, x, x);
        CHECK_NEAR(toneAmp(r.l, N / 2, N, 500, sr), 0.05 * 1.5, 0.002);  // the soft clipper's slope at 0 is 1.5
        Instance hard(d);
        hard.setParam(paramId(d, "mode"), 1);
        hard.setParam(paramId(d, "drive"), 30);
        hard.setParam(paramId(d, "output"), 0);
        hard.setParam(paramId(d, "tone"), 20000);
        auto xs = sineVec(5000, 0.9, N);
        auto rh = run(hard, xs, xs);
        CHECK(peak(rh.l) <= 1.0f + 1e-4f);
        // a clipped 5 kHz sine: its 9th harmonic (45 kHz) folds to 900 Hz. A naive clipper puts it
        // about 19 dB below the fundamental; with ADAA it must be far lower
        const double fund = toneAmp(rh.l, N / 2, N, 5000, sr), alias = toneAmp(rh.l, N / 2, N, 900, sr);
        CHECK(20 * log10(alias / fund) < -30.0);
        Instance crush(d);
        crush.setParam(paramId(d, "mode"), 3);
        crush.setParam(paramId(d, "bits"), 3);
        crush.setParam(paramId(d, "downsample"), 8);
        auto rc = run(crush, x, x);
        bool ok = true;
        for (float v : rc.l)
            ok &= std::isfinite(v);
        CHECK(ok);
    }
}

// ---- Tracker (libxmp) -------------------------------------------------------------------------

static daw_event_t chaseOn(uint32_t mbeats, int32_t id)
{
    daw_event_t e = noteOn(60, 0.8f, id, DAW_NOTE_FLAG_CHASE);
    e.u.note.chase_mbeats = mbeats;
    return e;
}

// index after the last non-zero sample of either channel
static size_t endOfSound(const RenderResult &r)
{
    size_t e = r.l.size();
    while (e > 0 && r.l[e - 1] == 0.0f && r.r[e - 1] == 0.0f)
        e--;
    return e;
}

static double maxDiff(const std::vector<float> &a, size_t ai, const std::vector<float> &b, size_t bi, size_t n)
{
    double m = 0;
    for (size_t i = 0; i < n && ai + i < a.size() && bi + i < b.size(); i++)
        m = fmax(m, fabs((double)a[ai + i] - b[bi + i]));
    return m;
}

class ParallelTestWorker {
    std::binary_semaphore pending_{0}, finished_{0};
    void (*function_)(void *) = nullptr;
    void *argument_ = nullptr;
    std::thread thread_;
public:
    unsigned jobs = 0, refused = 0;
    bool decline = false;
    ParallelTestWorker() : thread_([this] {
        for (;;) { pending_.acquire(); if (!function_) return; function_(argument_); finished_.release(); }
    }) {}
    ~ParallelTestWorker() { function_ = nullptr; pending_.release(); thread_.join(); }
    int32_t run(void (*local)(void *), void *la, void (*remote)(void *), void *ra) {
        if (decline && (jobs + refused) % 3 == 0) { ++refused; return 0; }
        ++jobs; function_ = remote; argument_ = ra; pending_.release();
        local(la); finished_.acquire(); return 1;
    }
};
static ParallelTestWorker *parallelTestWorker;

static void testReverbParallel()
{
    const auto *d = find("org.smhgzll.fx.reverb");
    CHECK(d);
    if (!d) return;
    ParallelTestWorker worker;
    parallelTestWorker = &worker;
    for (int mode = 0; mode < 3; ++mode) {
        for (float quality : {0.0f, 1.0f}) {
            Instance serial(d), parallel(d);
            serial.setParam(paramId(d, "quality"), quality);
            parallel.setParam(paramId(d, "quality"), quality);
            parallel.host().api.parallel = [](void *, void (*local)(void *), void *la,
                                              void (*remote)(void *), void *ra) -> int32_t {
                return parallelTestWorker->run(local, la, remote, ra);
            };
            if (mode == 0) parallel.host().api.struct_size = offsetof(daw_host_api_t, parallel);
            worker.decline = mode == 2;
            const unsigned before = worker.jobs, refusedBefore = worker.refused;
            bool equal = true;
            float l[256], r[256], a[256], b[256], pl[256], pr[256];
            uint32_t seed = 42;
            for (int block = 0; block < 240; ++block) {
                const uint32_t n = block % 5 == 0 ? 1 : block % 3 == 0 ? 73 : 256;
                for (uint32_t i = 0; i < n; ++i) {
                    seed = seed * 1664525u + 1013904223u;
                    l[i] = pl[i] = block < 30 ? ((int32_t)seed / 2147483648.0f) * 0.3f : 0;
                    seed = seed * 1664525u + 1013904223u;
                    r[i] = pr[i] = block < 30 ? ((int32_t)seed / 2147483648.0f) * 0.2f : 0;
                }
                std::vector<daw_event_t> events;
                if (block == 40 || block == 60 || block == 80 || block == 120 || block == 160) {
                    const char *key = block == 40 ? "width" : block == 60 ? "predelay" : block == 160 ? "quality" : "freeze";
                    auto e = paramEv(paramId(d, key), block == 40 ? 30 : block == 60 ? 3.7f : block == 80 ? 1 : block == 120 ? 0 : 1 - quality);
                    e.offset = n / 2; events.push_back(e);
                }
                serial.process(n, events, a, b, l, r);
                parallel.process(n, events, pl, pr, pl, pr);
                equal &= !memcmp(a, pl, n * sizeof(float)) && !memcmp(b, pr, n * sizeof(float));
            }
            CHECK(equal);
            CHECK(mode == 0 ? worker.jobs == before : worker.jobs > before);
            CHECK(mode != 2 || worker.refused > refusedBefore);
            CHECK(serial.host().rtViolations == 0 && parallel.host().rtViolations == 0);
        }
    }
    parallelTestWorker = nullptr;
}

static void testTracker()
{
    auto *d = find("org.smhgzll.tracker");
    CHECK(d != nullptr);
    if (!d)
        return;
    CHECK((d->capabilities & DAW_CAP_NOTE_CHASE) != 0);
    setDefaultResources(nullptr);
    const double sr = 44100;
    // the test song: 64 rows of 6 ticks at 125 BPM (7.68 s), then 33 rows at 150 BPM (3.3 s)
    const double song = 10.98, row16 = 1.92;
    const char *formats[] = {"mod", "xm", "s3m", "it"};
    std::vector<FileResource> mods(4);
    for (int f = 0; f < 4; f++) {
        mods[f].bytes = makeTestModule(formats[f]);
        mods[f].name = std::string("test.") + formats[f];
        Instance in(d);
        CHECK(in.setResource(0, mods[f].resource()) == DAW_OK);
        CHECK_NEAR(in.getParam(paramId(d, "length")), song, 0.001);
        CHECK(in.getParam(paramId(d, "channels")) == 4);
        CHECK(in.getParam(paramId(d, "format")) == f + 1);
        CHECK_NEAR(in.getParam(paramId(d, "song_tempo")), 125, 1e-3);
        CHECK(in.getParam(paramId(d, "memory")) > 0);
        // a read-only value cannot be set
        in.setParam(paramId(d, "length"), 3);
        CHECK_NEAR(in.getParam(paramId(d, "length")), song, 0.001);
        // the whole song at its own speed (song tempo = the host's 120 BPM), then silence
        in.setParam(paramId(d, "tempo"), 120);
        auto r = render(in, {{0.0, noteOn(60, .8f, 1)}}, 12.0);
        CHECK(in.host().rtViolations == 0);
        CHECK(rmsOf(r.l, 0, 44100) > 0.01);
        CHECK(rmsOf(r.r, 0, 44100) > 0.01);
        CHECK_NEAR(endOfSound(r) / sr, song, 0.025);
        CHECK(in.voices() == 0);
        float pk = 0;
        for (size_t i = 0; i < r.l.size(); i++)
            pk = fmaxf(pk, fmaxf(fabsf(r.l[i]), fabsf(r.r[i])));
        CHECK(pk < 1.0f && pk == pk);
        // detaching frees everything libxmp allocated
        CHECK(in.setResource(0, nullptr) == DAW_OK);
        CHECK(in.host().largeBytes == 0);
        CHECK(in.getParam(paramId(d, "length")) == 0);
    }

    auto play = [&](std::vector<TimedEvent> ev, double seconds, double hostTempo = 120,
                    std::vector<std::pair<const char *, float>> params = {}, int f = 0) {
        Instance in(d);
        CHECK(in.setResource(0, mods[f].resource()) == DAW_OK);
        in.setParam(paramId(d, "tempo"), 120);
        for (auto &p : params)
            in.setParam(paramId(d, p.first), p.second);
        in.setTempo(hostTempo);
        auto r = render(in, ev, seconds);
        CHECK(in.host().rtViolations == 0);
        return r;
    };
    const uint32_t at16 = (uint32_t)lround(row16 * 2 * 1000);  // 120 BPM: 2 beats per second
    for (int f = 0; f < 4; f++) {
        // chase onto a row start plays from there at once; inside the row before, it waits for it
        auto a = play({{0.0, chaseOn(at16, 1)}}, 2.5, 120, {}, f);
        auto b = play({{0.0, chaseOn(at16 - 140, 1)}}, 2.5, 120, {}, f);  // 70 ms earlier
        const size_t w = (size_t)lround(0.070 * sr);
        CHECK(rmsOf(a.l, 0, 4410) > 0.01);
        CHECK(peak(std::vector<float>(b.l.begin(), b.l.begin() + w)) == 0.0f);
        CHECK(maxDiff(a.l, 0, b.l, w, 2 * 44100) == 0.0);
        CHECK(maxDiff(a.r, 0, b.r, w, 2 * 44100) == 0.0);
        // and matches the song played from the start once its channels have new notes (row 19)
        auto whole = play({{0.0, noteOn(60, .8f, 1)}}, 4.0, 120, {}, f);
        const size_t from = (size_t)lround((2.28 - row16) * sr);
        CHECK(maxDiff(whole.l, (size_t)lround(2.28 * sr), a.l, from, (size_t)(0.15 * sr)) < 1e-6);
        // twice the song tempo: twice as fast (half the length), chases land on the same rows
        auto fast = play({{0.0, noteOn(60, .8f, 1)}}, 7.0, 240, {}, f);
        CHECK_NEAR(endOfSound(fast) / sr, song / 2, 0.02);
        auto fa = play({{0.0, chaseOn(at16, 1)}}, 1.0, 240, {}, f);
        auto fb = play({{0.0, chaseOn(at16 - 140, 1)}}, 1.0, 240, {}, f);
        const size_t fw = (size_t)(0.035 * sr + 0.5);
        CHECK(maxDiff(fa.l, 0, fb.l, fw, 30000) == 0.0);
    }
    for (int f = 0; f < 4; f++) {
        // after the tempo change: pattern 1's rows are 100 ms apart from 7.68 s (the row map follows
        // libxmp's scan through speed and tempo changes)
        const uint32_t at5 = (uint32_t)lround((7.68 + 0.5) * 2 * 1000);
        auto a = play({{0.0, chaseOn(at5, 1)}}, 1.0, 120, {}, f);
        auto b = play({{0.0, chaseOn(at5 - 60, 1)}}, 1.0, 120, {}, f);  // 30 ms before row 5
        const size_t w = (size_t)lround(0.030 * sr);
        CHECK(rmsOf(a.l, (size_t)(0.3 * sr), (size_t)(0.4 * sr)) > 0.01);  // row 8's note
        CHECK(peak(std::vector<float>(b.l.begin(), b.l.begin() + w)) == 0.0f);
        CHECK(maxDiff(a.l, 0, b.l, w, 30000) == 0.0);
        auto whole = play({{0.0, noteOn(60, .8f, 1)}}, 9.0, 120, {}, f);
        const size_t from = (size_t)lround(0.65 * sr);  // pattern 1, row 11: both channels have new notes
        CHECK(maxDiff(whole.l, (size_t)lround(8.18 * sr) + from, a.l, from, (size_t)(0.15 * sr)) < 1e-6);
    }
    {
        // a tick that is not a whole number of samples (125 BPM song at 121 BPM: 911.16 samples):
        // the fraction carries over, so the song ends exactly where its length says
        auto r = play({{0.0, noteOn(60, .8f, 1)}}, 12.0, 121);
        const double exact = song * 120.0 / 121.0 * sr;
        CHECK_NEAR((double)endOfSound(r), exact, 4.0);
    }
    // the note's end ends the song (a 5 ms fade)
    auto off = play({{0.0, noteOn(60, .8f, 7)}, {1.0, noteOff(60, 7)}, {1.2, noteOff(60, 99)}}, 2.0);
    CHECK(rmsOf(off.l, 22050, 44100) > 0.01);
    CHECK(peak(std::vector<float>(off.l.begin() + 44100 + 300, off.l.end())) == 0.0f);
    // another note's end does not
    auto other = play({{0.0, noteOn(60, .8f, 7)}, {1.0, noteOff(60, 8)}}, 2.0);
    CHECK(rmsOf(other.l, 66150, 88200) > 0.01);
    // past the end: silent; with Loop song it wraps around
    const uint32_t past = (uint32_t)lround((song + row16) * 2 * 1000);
    auto silent = play({{0.0, chaseOn(past, 1)}}, 0.5);
    CHECK(peak(silent.l) == 0.0f);
    auto wrap = play({{0.0, chaseOn(past, 1)}}, 2.0, 120, {{"loop", 1}});
    auto a16 = play({{0.0, chaseOn(at16, 1)}}, 2.0);
    CHECK(rmsOf(wrap.l, 0, 4410) > 0.01);
    CHECK(maxDiff(wrap.l, 0, a16.l, 0, 30000) < 1e-6);
    auto looped = play({{0.0, noteOn(60, .8f, 1)}}, 13.0, 120, {{"loop", 1}});
    CHECK(rmsOf(looped.l, (size_t)(11.5 * sr), (size_t)(12.5 * sr)) > 0.01);
    // channel range: channels 3-4 carry no notes; channel 1 alone is quieter than all
    auto none = play({{0.0, noteOn(60, .8f, 1)}}, 1.0, 120, {{"first", 3}, {"last", 4}});
    CHECK(peak(none.l) < 1e-6f);
    auto one = play({{0.0, noteOn(60, .8f, 1)}}, 1.0, 120, {{"first", 1}, {"last", 1}});
    auto all = play({{0.0, noteOn(60, .8f, 1)}}, 1.0);
    CHECK(rmsOf(one.l, 4410, 44100) > 0.005);
    CHECK(maxDiff(one.l, 0, all.l, 0, 44100) > 1e-3);
    // the channel range set before the module loads (a project being opened) applies too
    {
        Instance in(d);
        in.setParam(paramId(d, "first"), 3);
        in.setParam(paramId(d, "last"), 4);
        in.setParam(paramId(d, "tempo"), 120);
        CHECK(in.setResource(0, mods[0].resource()) == DAW_OK);
        auto r = render(in, {{0.0, noteOn(60, .8f, 1)}}, 1.0);
        CHECK(peak(r.l) < 1e-6f);
    }
    // no stereo separation: both sides equal
    auto mono = play({{0.0, noteOn(60, .8f, 1)}}, 1.0, 120, {{"stereo", 0}});
    CHECK(maxDiff(mono.l, 0, mono.r, 0, 44100) < 1e-6);
    CHECK(maxDiff(all.l, 0, all.r, 0, 44100) > 1e-3);
    // what is not a module is refused, and leaves nothing allocated
    {
        Instance in(d);
        FileResource junk;
        junk.bytes.assign(4096, 0x5A);
        CHECK(in.setResource(0, junk.resource()) == DAW_ERR_ARG);
        FileResource cut;
        cut.bytes.assign(mods[1].bytes.begin(), mods[1].bytes.begin() + 300);
        CHECK(in.setResource(0, cut.resource()) == DAW_ERR_ARG);
        CHECK(in.host().largeBytes == 0);
        auto r = render(in, {{0.0, noteOn(60, .8f, 1)}}, 0.2);
        CHECK(peak(r.l) == 0.0f);
        // truncated sample data still loads (libxmp pads it) and plays
        for (int f = 0; f < 4; f++) {
            FileResource part;
            part.bytes.assign(mods[f].bytes.begin(), mods[f].bytes.end() - 16);
            const int32_t rc = in.setResource(0, part.resource());
            CHECK(rc == DAW_OK || rc == DAW_ERR_ARG);
            render(in, {{0.0, noteOn(60, .8f, 1)}}, 0.2);
        }
        CHECK(in.setResource(0, nullptr) == DAW_OK);
        CHECK(in.host().largeBytes == 0);
        CHECK(in.host().rtViolations == 0);
    }
}

// The Tracker's two-core mixing (the ABI 1.3 parallel service): a part's voices split by sample
// between this thread and a real second one give the same output as one core, also when the host
// declines every third call or offers no service; chased into a tick and at another tempo.
static void testTrackerParallel()
{
    auto *d = find("org.smhgzll.tracker");
    CHECK(d != nullptr);
    if (!d)
        return;
    ParallelTestWorker worker;
    parallelTestWorker = &worker;
    FileResource dense;
    dense.bytes = makeTestModule("it-dense");
    dense.name = "dense.it";
    auto run = [&](int mode, std::vector<float> &l, std::vector<float> &r) {
        Instance in(d);
        in.host().api.parallel = [](void *, void (*local)(void *), void *la, void (*remote)(void *),
                                    void *ra) -> int32_t { return parallelTestWorker->run(local, la, remote, ra); };
        if (mode == 0)
            in.host().api.parallel = nullptr;
        if (mode == 3)  // an older host: the table ends before the service
            in.host().api.struct_size = offsetof(daw_host_api_t, parallel);
        worker.decline = mode == 2;
        CHECK(in.setResource(0, dense.resource()) == DAW_OK);
        CHECK(in.getParam(paramId(d, "channels")) == 16);
        CHECK_NEAR(in.getParam(paramId(d, "length")), 3.84, 0.001);
        in.setParam(paramId(d, "tempo"), 125);
        in.setParam(paramId(d, "loop"), 1);
        in.setTempo(125);
        float L[256], R[256];
        for (int block = 0; block < 1400; ++block) {
            const uint32_t n = block % 7 == 0 ? 1 : block % 5 == 0 ? 73 : block % 3 == 0 ? 200 : 256;
            std::vector<daw_event_t> ev;
            if (block == 0)
                ev.push_back(noteOn(60, .8f, 1));
            if (block == 500) {  // 1.584 s into the song: inside a row and a tick
                auto e = chaseOn(3300, 2);
                e.offset = n / 2;
                ev.push_back(e);
            }
            if (block == 900)
                in.setTempo(150);
            in.process(n, ev, L, R);
            l.insert(l.end(), L, L + n);
            r.insert(r.end(), R, R + n);
        }
        CHECK(in.host().rtViolations == 0);
        CHECK(in.setResource(0, nullptr) == DAW_OK);
        CHECK(in.host().largeBytes == 0);
    };
    std::vector<float> l[4], r[4];
    unsigned jobs[4], refused[4];
    for (int mode = 0; mode < 4; mode++) {
        const unsigned j0 = worker.jobs, r0 = worker.refused;
        run(mode, l[mode], r[mode]);
        jobs[mode] = worker.jobs - j0;
        refused[mode] = worker.refused - r0;
    }
    CHECK(rmsOf(l[0], 0, l[0].size()) > 0.01 && rmsOf(r[0], 0, r[0].size()) > 0.01);
    CHECK(jobs[0] == 0 && jobs[3] == 0 && refused[3] == 0);
    CHECK(jobs[1] > 500 && refused[1] == 0);
    CHECK(jobs[2] > 300 && refused[2] > 100);
    for (int mode = 1; mode < 4; mode++)
        CHECK(l[mode] == l[0] && r[mode] == r[0]);
    parallelTestWorker = nullptr;
}

static daw_event_t imported(daw_event_t e) { e.flags |= DAW_NOTE_FLAG_MIDI; return e; }
static daw_event_t midiCC(int cc, int value) {
    daw_event_t e{}; e.type = DAW_EV_CC; e.flags = DAW_NOTE_FLAG_MIDI;
    e.u.midi.number = (uint8_t)cc; e.u.midi.value = value / 127.f; return e;
}
static daw_event_t midiReset() { daw_event_t e{}; e.type = DAW_EV_MIDI_RESET; e.flags = DAW_NOTE_FLAG_MIDI; return e; }
static daw_event_t midiBend(float v) { daw_event_t e{}; e.type = DAW_EV_PITCH_BEND; e.flags = DAW_NOTE_FLAG_MIDI; e.u.midi.value = v; return e; }
static std::vector<uint8_t> midiTestFont() {
    auto src = makeTestSoundFont();
    using B = std::vector<uint8_t>;
    auto get = [](const B &b, size_t p) { return uint32_t(b[p]) | uint32_t(b[p+1]) << 8 | uint32_t(b[p+2]) << 16 | uint32_t(b[p+3]) << 24; };
    auto put16 = [](B &b, size_t p, int v) { b[p] = v; b[p+1] = v >> 8; };
    auto put32 = [](B &b, size_t p, uint32_t v) { for (int i=0;i<4;++i) b[p+i] = v >> (8*i); };
    std::function<B(const B &)> rewrite = [&](const B &input) {
        B out;
        for (size_t p=0;p+8<=input.size();) {
            const uint32_t n = get(input,p+4); B body(input.begin()+p+8,input.begin()+p+8+n);
            if (!memcmp(input.data()+p,"RIFF",4) || !memcmp(input.data()+p,"LIST",4)) {
                B children(body.begin()+4,body.end()); children=rewrite(children); body.resize(4); body.insert(body.end(),children.begin(),children.end());
            } else if (!memcmp(input.data()+p,"phdr",4)) {
                B a(body.begin(),body.begin()+38), terminal(body.begin()+38,body.end());
                put16(a,20,1); put16(a,24,1); put16(terminal,24,2);
                body.resize(38); body.insert(body.end(),a.begin(),a.end()); body.insert(body.end(),terminal.begin(),terminal.end());
            } else if (!memcmp(input.data()+p,"pbag",4)) {
                body.resize(12); put16(body,0,0); put16(body,4,1); put16(body,8,3);
            } else if (!memcmp(input.data()+p,"pgen",4)) {
                body.resize(16); put16(body,0,41); put16(body,2,0); put16(body,4,51); put16(body,6,12);
                put16(body,8,41); put16(body,10,0); put16(body,12,0); put16(body,14,0);
            }
            const size_t at=out.size(); out.insert(out.end(),input.begin()+p,input.begin()+p+8); put32(out,at+4,body.size());
            out.insert(out.end(),body.begin(),body.end()); if(body.size()&1)out.push_back(0); p+=8+n+(n&1);
        }
        return out;
    };
    return rewrite(src);
}
static void testMidiMessages() {
    auto *d = find("org.smhgzll.soundfont"); CHECK(d != nullptr); if (!d) return;
    FileResource font; font.bytes = midiTestFont(); font.name = "midi-test.sf2";
    auto play = [&](std::vector<TimedEvent> ev, double seconds) {
        Instance in(d); CHECK(in.setResource(0,font.resource()) == DAW_OK);
        const auto large=in.host().largeBytes, fast=in.host().fastBytes;
        ev.insert(ev.begin(), {0,midiReset()}); auto result=render(in,ev,seconds);
        CHECK(in.host().rtViolations == 0); CHECK(in.host().largeBytes == large && in.host().fastBytes == fast);
        return result;
    };
    const double f0=44100.0/lrint(44100.0/261.6256);
    daw_event_t program{}; program.type=DAW_EV_PROGRAM; program.flags=DAW_NOTE_FLAG_MIDI; program.u.midi.index=1;
    auto switched=play({{.02, imported(noteOn(60,1,1))},{.3,program},{.4,imported(noteOff(60,1))},{.5,imported(noteOn(60,1,2))}},.9);
    CHECK_NEAR(zeroCrossFreq(switched.l,4410,13230,44100),f0,.2); // patch change keeps the held voice
    CHECK_NEAR(zeroCrossFreq(switched.l,26460,37485,44100),2*f0,.3);
    auto volume=play({{.01,imported(noteOn(60,1,1))},{.2,midiCC(7,0)},{.4,midiCC(7,100)},{.6,midiCC(11,0)}},.8);
    CHECK(rmsOf(volume.l,4410,8000)>.02); CHECK(rmsOf(volume.l,11025,15435)<1e-5);
    CHECK(rmsOf(volume.l,22050,25000)>.02); CHECK(rmsOf(volume.l,30000,34000)<1e-5);
    auto range=play({{0,midiCC(101,0)},{0,midiCC(100,0)},{0,midiCC(6,12)},{0,midiCC(38,0)},{0,midiBend(.5f)},{.01,imported(noteOn(60,1,1))}},.4);
    CHECK_NEAR(zeroCrossFreq(range.l,4410,15435,44100),f0*sqrt(2.),.5);
    auto tune=play({{0,midiCC(101,0)},{0,midiCC(100,1)},{0,midiCC(6,96)},{0,midiCC(38,0)},
        {0,midiCC(100,2)},{0,midiCC(6,65)},{.01,imported(noteOn(60,1,1))}},.4);
    CHECK_NEAR(zeroCrossFreq(tune.l,4410,15435,44100),f0*pow(2.,1.5/12.),.5);
    daw_event_t down{}; down.type=DAW_EV_SUSTAIN; down.flags=DAW_NOTE_FLAG_MIDI; down.u.midi.value=1;
    auto up=down; up.u.midi.value=0;
    auto sustain=play({{0,down},{.01,imported(noteOn(60,1,1))},{.1,imported(noteOff(60,1))},{.4,up}},.7);
    CHECK(rmsOf(sustain.l,8820,15435)>.02); CHECK(rmsOf(sustain.l,22050,26460)<1e-5);
    auto sostenuto=play({{.01,imported(noteOn(60,1,1))},{.1,midiCC(66,127)},{.15,imported(noteOn(64,1,2))},
        {.2,imported(noteOff(60,1))},{.25,imported(noteOff(64,2))},{.5,midiCC(66,0)}},.8);
    CHECK_NEAR(zeroCrossFreq(sostenuto.l,15435,19845,44100),f0,.5);
    CHECK(rmsOf(sostenuto.l,26460,30870)<1e-5);
    // Old note-offs must not stop a later same-key note after All Sound Off or voice stealing.
    auto ids=play({{.01,imported(noteOn(60,1,1))},{.1,midiCC(120,0)},{.2,imported(noteOn(60,1,2))},{.3,imported(noteOff(60,1))}},.5);
    CHECK(rmsOf(ids.l,15435,19845)>.02);
    daw_event_t pressure{}; pressure.type=DAW_EV_POLY_PRESSURE; pressure.flags=DAW_NOTE_FLAG_MIDI;
    pressure.u.midi.number=60; pressure.u.midi.value=1;
    auto plain=play({{.01,imported(noteOn(60,1,1))}},.6);
    auto mod=play({{.01,imported(noteOn(60,1,1))},{.1,pressure},{.2,midiCC(1,127)}},.6);
    CHECK(hashPcm(plain.l,plain.r)!=hashPcm(mod.l,mod.r));
    // A universal master-volume packet split over SMF F0/F7 events applies only when complete.
    const uint8_t start[]={0xf0,0x7f,0x7f,4,1}, finish[]={0xf7,0,0,0xf7};
    daw_event_t a{}; a.type=DAW_EV_MIDI_BYTES; a.flags=DAW_NOTE_FLAG_MIDI; a.u.bytes={start,sizeof start};
    daw_event_t b=a; b.u.bytes={finish,sizeof finish};
    auto muted=play({{.01,imported(noteOn(60,1,1))},{.2,a},{.3,b}},.5);
    CHECK(rmsOf(muted.l,11025,12500)>.02); CHECK(rmsOf(muted.l,15435,19845)<1e-5);
    { // RPN sensitivity on a fixed synth must not abuse the normalized bend ABI with values >1.
        Instance in(find("org.smhgzll.sine"));
        auto output=render(in,{{0,midiReset()},{0,midiCC(101,0)},{0,midiCC(100,0)},{0,midiCC(6,12)},
            {0,midiBend(.5f)},{.01,imported(noteOn(60,1,1))}},.4);
        CHECK_NEAR(zeroCrossFreq(output.l,4410,15435,44100),261.6256*sqrt(2.),.6);
        CHECK(in.host().rtViolations==0);
    }
    // Generic synth adapter affects held audio, while unflagged legacy notes keep their render.
    for(const char *id:{"org.smhgzll.sine","org.smhgzll.3xosc"}) {
        auto *synth=find(id); CHECK(synth!=nullptr); if(!synth)continue;
        Instance in(synth); auto output=render(in,{{0,midiReset()},{.01,imported(noteOn(60,1,1))},{.2,midiCC(11,0)}},.4);
        CHECK(rmsOf(output.l,4410,8000)>.001); CHECK(rmsOf(output.l,11025,15435)<1e-6); CHECK(in.host().rtViolations==0);
    }
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "midi")) { testMidiMessages(); TEST_MAIN_END("test_midi"); }
    if (argc == 2 && !strcmp(argv[1], "tracker")) {
        testTracker();
        testTrackerParallel();
        TEST_MAIN_END("test_tracker");
    }
    if (argc == 2 && !strcmp(argv[1], "effects")) {
        testEffects();
        testMoreEffects();
        testReverbParallel();
        TEST_MAIN_END("test_effects");
    }
    CHECK(!registry().empty());
    for (auto *d : registry()) {
        auto res = validatePlugin(d);
        for (auto &f : res.failures)
            fprintf(stderr, "  %s: %s\n", d->id, f.c_str());
        CHECK(res.failures.empty());
    }
    testSine();
    testThreeOsc();
    testDrumLab();
    testAcid303();
    testFm6();
    testMacroSynth();
    testSampler();
    testSoundFont();
    testMidiMessages();
    testTracker();
    testTrackerParallel();
    testEffects();
    testMoreEffects();
    testReverbParallel();
    TEST_MAIN_END("test_plugins");
}
