# Example MIDI files

Files to try the MIDI import with (File > Import MIDI file…, or the piano roll's import button;
docs/MIDI.md, "MIDI files"). The browser tests import them too.

| file | what | provenance and license |
|---|---|---|
| `groove-format1.mid` | "Salty Groove": 16 bars, drums / bass / keys / lead, a tempo change, three markers; format 1 (a track per part) | written by `tests/web/midigen.py --examples examples/midi`; an original composition of this project, GPL-3.0-or-later like the repository |
| `groove-format0.mid` | the same song as format 0 (one track, a MIDI channel per part) | as above |
| `joplin-the-entertainer.mid` | Scott Joplin, *The Entertainer* (1902), piano, two tracks | [Mutopia Project](https://www.mutopiaproject.org/cgibin/piece-info.cgi?id=263) Mutopia-2016/11/25-263, typeset by Chris Sawer; **Public Domain** (Creative Commons Public Domain Dedication) |
| `bach-invention-02.mid` | J. S. Bach, Invention No. 2 in C minor, BWV 773, two voices | [Mutopia Project](https://www.mutopiaproject.org/cgibin/piece-info.cgi?id=58) Mutopia-2008/06/15-58, typeset by Allen Garvin; **Public Domain** |
| `haydn-emperor-quartet-2.mid` | J. Haydn, String Quartet Op. 76 No. 3 "Emperor", 2nd movement (Poco adagio, cantabile); violin I, violin II, viola, cello | [Mutopia Project](https://www.mutopiaproject.org/cgibin/piece-info.cgi?id=385) Mutopia-2015/10/16-385, typeset by Maurizio Tomasi; **Public Domain**; `score-2.mid` from `op76-n3-mids.zip` |

The Mutopia files are unmodified (downloaded 2026-10-12); their licenses were checked on each
piece's Mutopia page. SHA-256:

```
33e4e81ee64ffb2edf90d1c6a1ddee7276507296bfb915c2bb775231a467f066  joplin-the-entertainer.mid
b61e1e4d4a42ca70efd0eb8a65de3ed0ca0ee9c31556354608f6caa360e78e98  bach-invention-02.mid
7dc2e9e4ba2aa0335df6df66a07d2e2c8315a1d09af6e2954c2e5f8577375456  haydn-emperor-quartet-2.mid
```

Do not add MIDI files of copyrighted music (game, film or pop soundtracks and the like) to this
directory, even for testing: keep those on your own computer.
