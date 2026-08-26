# Attributions

## Resolume FFGL SDK

`external/ffgl` is the Resolume FreeFrame GL SDK, included as a git submodule
pinned to `b1afaf9` — the revision the rest of this fleet pins.

- Source: https://github.com/resolume/ffgl
- Licence: see `external/ffgl/LICENSE`

## OpenFX

`external/openfx` is a subset of the OpenFX image effect API — the C headers and
the official C++ Support library — vendored for the OpenFX build.

- Source: https://github.com/AcademySoftwareFoundation/openfx
- Licence: BSD-3-Clause, see `external/openfx/LICENSE.md`

## zlib

The offline harness writes PNGs using zlib, which ships with macOS. No vendored
copy.

## Everything else

Written for this repo and MIT licensed. See `LICENSE`.
