#pragma once

#include "Compander.h"

namespace compander
{
/**
    The one place a slider position becomes a physical quantity.

    -------------------------------------------------------------------- why

    **A ranged `FF_TYPE_STANDARD` parameter cannot have a ranged default.** The
    SDK's `SetParamInfo` clamps a default into 0..1 *before* returning, and
    `SetParamRange` can only be called afterwards because it finds the parameter
    by id. There is no `SetParamDefault`. So a control declared in microseconds
    cannot declare a default in microseconds -- 5 silently becomes 1. Every
    standard parameter here therefore lives in 0..1 and is converted on the way
    through, which is this file.

    The second reason arrives with the OpenFX build: both hosts expose the same
    0..1 controls and the same factory presets, so a conversion living in each
    host's glue would be two copies of every curve, and a preset would mean
    something slightly different in Resolume and in Resolve.

    ------------------------------------------------------------- the curves

    Anything that is a **time** or a **level** converts exponentially, so half a
    slider is the geometric middle and equal distances either side are
    reciprocal factors. Time Constant spans five and a half decades -- 0.1 us to
    40 ms -- and there is no other way to make one control usable at both ends.

    Anything that is an **amount** converts linearly. Anything centred on "no
    error" puts that at 0.5, which is why Link Level and Tilt default there and
    not to zero.

    ⚠️ **Ratios are linear in the ratio, not in the slope.** 1:1 to 4:1 across
    the slider, which puts the canonical 2:1 -- the ratio essentially every
    analogue wireless system used -- at exactly one third. That is an awkward
    place for a detent and it was still the right choice: linear in the slope
    puts 2:1 at two thirds and makes the top half of the travel almost all
    ratios above 3:1, which nothing was ever built with.
*/
namespace controls
{
/// The controls exactly as the host holds them: every one 0..1, option
/// parameters holding their element index.
struct HostValues
{
	//Signal
	float sidechain = 0.0f;
	float chroma    = 0.0f;

	//Encode
	float compress = 0.333f;///< 2:1
	float emphasis = 0.5f;  ///< 6 dB

	//Link
	float linkLevel = 0.5f; ///< no error
	float noise     = 0.13f;
	float headroom  = 0.6f;

	//Decode
	float expand = 0.333f;  ///< 1:2, complementary
	float tilt   = 0.5f;    ///< matched

	//Detector
	float timeConstant = 0.55f;
	float pivot        = 0.7f;
	float maxGain      = 0.5f;

	//Audio
	float audioAmount = 0.7f;
	float audioBand   = 0.2f;
	float audioTilt   = 0.5f;

	//Output
	float mix = 1.0f;
};

/// Convert a full set of host values into physical units.
Settings toSettings( const HostValues& v );

//-- The individual conversions, exposed so the harness can print a control's
//-- physical value next to its slider position and so `sweep.py` has something
//-- to assert against.

/// 1:1 .. 4:1.
float ratioFromParam( float v );

/// 0 .. 12 dB.
float emphasisDbFromParam( float v );

/// -12 .. +12 dB, centred.
float linkLevelDbFromParam( float v );

/// 0 .. 0.15 linear amplitude on the encoded signal.
float noiseFromParam( float v );

/// 2.0 .. 0.5 linear, exponential, and DESCENDING with the slider.
///
/// ⚠️ Backwards on purpose, like ferric's Tape Speed. The physical quantity is
/// where the link clips, and *less* headroom is the more dramatic setting, so
/// the slider reads low-is-clean / high-is-slammed the way a drive control
/// should. Getting this the other way round is not subtle -- the control does
/// the opposite of its label -- but it is invisible to any test that only
/// checks the control is not dead.
float headroomFromParam( float v );

/// -1 .. +1, centred.
float tiltFromParam( float v );

/// 0.1 us .. 40 ms, exponential.
float timeConstantUsFromParam( float v );

/// 0.02 .. 1.0 signal level, exponential.
float pivotFromParam( float v );

/// 0 .. 48 dB.
float maxGainDbFromParam( float v );

} // namespace controls
} // namespace compander
