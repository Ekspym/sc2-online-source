# SC2 Online - source of the game program

The source of the game program (`soulcalibur2.exe`) of SC2 Online, under the [GPL-3.0](LICENSE). Its authors are
named in the files' notices and in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

**No code or data of the game is in this repository.** Each player builds the game program on their own PC from their
own Soulcalibur II (USA) Xbox disc: the launcher reads the disc's `Default.xbe`, translates it to C with
`tools/xboxrecomp` and compiles it together with this runtime.

## Why there is no link to the original project

The original project's releases contain the game's own program code, translated from the disc's `Default.xbe`.
SC2 Online distributes no code or data of the game. We link to the original project again once its releases no longer
contain the game's code. Its authors' copyright notices and the GPL-3.0 stay in this repository as the licence
requires.

## What SC2 Online changes

- Online play with rollback: GGPO (MIT, `runtime/ggpo`), the frame pacing of the two sides, lockstep outside the
  fight, saves of only the frames that can be gone back to, sound and drawing left out of the frames run again.
- Matches started by the launcher (picks, stage, rematch, leave) through a status file and a command file; the way
  back to the waiting game after a match without a restart.
- The game's own Practice started by the launcher, with frame data and the other training aids, and a match accepted
  in it.
- Shaders listed and made at the start; present without waiting for the display online.
- No outside online service: the launcher's lobby server meets the players.
- The runtime (`third_party/doa2u-re`) is included with these changes.

The SC2 Online launcher and its lobby server are separate programs and are not part of this repository.

## Building

The player's launcher builds it with `tools/builder/build_native.py` (Python, the lifter, the compiler and the prebuilt
runtime objects, which `tools/builder/make_builder.py` puts together; no code of the game), from the player's own disc
image, about 4 minutes once. By hand:


The player's launcher builds it with `tools/builder/build_native.py` (Python, the lifter, the compiler and the prebuilt
runtime objects, which `tools/builder/make_builder.py` puts together; no code of the game), from the player's own disc
image, about 4 minutes once. By hand:


Put your disc's `Default.xbe` in `private/disc/` (the retail one is taken with extract-xiso's media patch, see
`docs/bring-up.md`), then:

```
git submodule update --init
cd tools/xboxrecomp && git am ../../patches/xboxrecomp/*.patch && cd ../..
python tools/build_game.py lift
python tools/build_game.py build
```

`private/` (the disc file, the generated code, logs) is never committed.
