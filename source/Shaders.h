#pragma once

namespace compander::shaders
{
/**
    The GLSL.

    ------------------------------------------------------ the chain, as passes

    The signal chain, in order, with the two detectors sitting beside it:

      Luma     RGB -> one channel. Everything from here to the last pass is a
               single signal, because that is what a link carried.
      Blur     Two horizontal blurs of it, giving the band split the emphasis
               networks need. Horizontal, always -- see `Chain.h`.
      Reduce   Luma to quarter width, taking a MAX. The detector's input.
      Scan     The recursive-doubling envelope. Run `plan.passes.size()` times,
               each pass reaching twice as far back as the last.
      Encode   Pre-emphasis, the compressor, and the link's level, ceiling and
               noise. Writes the encoded signal.
      Reduce   } The second detector. It sees a different signal from the first
      Scan     } -- compressed, levelled and noisy -- and that difference is
               } where a real compander's mistracking comes from. Not a
               } duplicate: running one detector and reusing it would delete
               } the effect's whole reason for existing.
      Decode   The expander. Writes the decoded signal.
      Blur     The two blurs again, this time of the DECODED signal, because
               de-emphasis is the last thing in the chain and acts on what the
               expander produced.
      Output   De-emphasis, the colour re-applied, wet/dry. Draws to the host.

    Nine passes plus twice the doubling count. The doubling passes are quarter
    width and one channel, which is where the cost went and why it is bearable.

    ⚠️ **Two sets of blurs, of two different signals** -- the input and the
    decoded output. There is deliberately no band split of the ENCODED signal,
    because nothing needs one: de-emphasis is after the expander, so it acts on
    what the expander produced. Computing the split once and reusing it
    everywhere would model a circuit in which pre-emphasis, the compander and
    de-emphasis all read the transmitter's input, which is not a circuit but a
    diagram with the arrows rubbed out.

    ------------------------------------------------------------- the mirror

    `Chain.cpp` carries the same per-pixel arithmetic as `Chain.cpp`'s GLSL twin
    below. Every mirrored block is marked `//= mirrored` in both, and
    `cmtest --chain` renders the GPU's answer and compares it with the C++ one.

    **The gain laws are NOT mirrored.** They arrive as two 128-point uniform
    arrays filled by `fillGainTables`, so `compressGain` and `expandGain` exist
    exactly once, in `Compander.cpp`. A curve that existed twice is a curve a
    preset could disagree with itself about.

    ---------------------------------------------------------------- the traps

    `layout`, `flat`, `active`, `filter`, `input`, `output`, `sample` and
    `common` are GLSL keywords. A shader that uses one fails to compile at
    RUNTIME, and the way that presents is "the effect does nothing" with the
    real message only in the diagnostics log.

    Randomness is an integer hash, never `fract( sin(x) * 43758.5453 )`. The
    noise is part of the model rather than decoration, and a model that differs
    per driver is not one.
*/

/// Pass-through, with the host's MaxUV folded into the coordinate.
extern const char* const kVertex;

/// The mirrored per-pixel functions plus the gain-table lookup. Concatenated
/// into every fragment shader that needs any of it, so the tests exercise the
/// same string the plugin runs.
extern const char* const kCommon;

extern const char* const kLumaFragment;
extern const char* const kBlurFragment;
extern const char* const kReduceFragment;
extern const char* const kScanFragment;
extern const char* const kFrameFragment;
extern const char* const kEncodeFragment;
extern const char* const kDecodeFragment;
extern const char* const kOutputFragment;

/// The fragment shaders that need `kCommon`, assembled. Call these rather than
/// concatenating by hand at each site.
///
/// ⚠️ Any line number in a compiler error refers to the ASSEMBLED string, not
/// to the file the line was written in. `Diag` logs the assembled source
/// alongside the error for that reason.
const char* EncodeFragment();
const char* DecodeFragment();
const char* OutputFragment();

} // namespace compander::shaders
