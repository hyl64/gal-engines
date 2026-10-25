# demo-game: an end-to-end revival test with zero copyright entanglements

A tiny, entirely original Kepago scenario that exercises the full pipeline:

    demo.ke --(rlc)--> SEEN.TXT --(rlvm)--> a window on your screen.

## Compiling

The compiler is rlc-python, a byte-for-byte Python port of rldev's rlc:

    git clone https://github.com/WorkingWarrior/rlc-python
    cd rlc-python && python3 -m venv .venv && ./.venv/bin/pip install -e .

    PYTHONPATH=src ./.venv/bin/python -c \
      "import sys; sys.path.insert(0,'src'); from rlc.main import main; sys.exit(main())" \
      demo.ke -e UTF-8 -f 1.4.0.5 -g -o SEEN -d .

## Running

    build/rlvm.app/Contents/MacOS/rlvm test/demo-game

The checked-in `Gameexe.ini` is the minimum the engine needs: a REGNAME (for
the save directory), a window, and a screen size.

A font is discovered from system paths (see src/utilities/find_font_file.cc);
no font file needs to ship with the game.

## Why this matters

Commercial RealLive games are the oracle for *behaviour*, but they carry
copyright. This scenario is the oracle for *the pipeline itself*: compiler,
archive reader, bytecode framing, text system, input, and termination, all
exercised with content nobody has to own.
