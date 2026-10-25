# Third-party code and licences

This repository bundles five engines.  **Each keeps its own licence**; nothing
here relicences them.  The files added by this project (the `gal` manager,
`build-all.sh`, the documents, and `artemis/`) are covered by `LICENSE`.

| Directory | What it is | Licence | Upstream |
|---|---|---|---|
| `OnscripterYuri/` | ONScripter fork (NScripter-compatible engine) | **GPL-2.0** (see `OnscripterYuri/LICENSE`) | YuriSizuku/OnscripterYuri |
| `kirikiroid2-mac/` | KiriKiri2 / KiriKiri Z port (TJS2, XP3) | **BSD-style**, W.Dee and contributors (see `kirikiroid2-mac/LICENSE.upstream`) | zeas2/Kirikiroid2, W.Dee's Kirikiri |
| `rlvm/` | RealLive interpreter (Kanon/Air/CLANNAD/…) | **GPL-3.0-or-later** (see `rlvm/COPYING.TXT`) | eglaysher/rlvm |
| `openbgi/openbgi-src/` | OpenBGI, an Ethornell/BGI reimplementation | **GPL-2.0-only** (see `openbgi/openbgi-src/LICENSE`) | OpenBGI |
| `artemis/` | An Artemis-format runtime **written from scratch here** | same as this repository (`LICENSE`) | — (no upstream code used) |

## What that means in practice

- Redistributing this repository as a whole means shipping GPL-2.0 and
  GPL-3.0-or-later components: keep their licence files intact, and make the
  corresponding source available (it is, since this is the source tree).
- `gal` **runs** the engines as separate processes.  It does not link against
  them, so its own licence does not have to match theirs.
- The Artemis runtime is a clean implementation of a documented-by-observation
  file format.  It contains no code from the original Artemis engine, and it
  ships no game data.

## Game data is not included

No game assets, archives or scripts from any commercial title are part of this
repository.  See `docs/DATA.md` for where each test data set came from, how it
is checked, and how to obtain it yourself.

## Bundled vendor code inside the engines

The engines bring their own vendored dependencies, each with its own licence
text in place:

- `kirikiroid2-mac/` vendors libpng, zlib, lz4, libjpeg-turbo and others under
  `lib/` and `src/` — see the notices in those directories.
- `rlvm/vendor/` carries SDL_mixer, SDL_ttf, SDL_image, GLEW, guichan and
  zita-resampler sources for static building.
- `OnscripterYuri/` vendors bzip2 and its own SDL helpers.

## Screenshots

`docs/` and each engine's `screenshots/` contain frames captured from engines
running real games.  They exist to show what the ports render — engine
compatibility evidence — and are reproduced at low resolution for that purpose.
Copyright in the depicted artwork remains with the respective publishers:
CLANNAD © VisualArts/Key, 遥か碧の花嫁に © Lump of Sugar, KAG3 demo © W.Dee
and contributors.  If you are a rights holder and want one removed, open an
issue and it will be.
