#pragma once

/**
    Factory presets: real circuits, in one gesture.

    Each entry is *a link somebody actually built* rather than a set of slider
    positions that looked nice -- a broadband wireless compander, a two-band
    one, a tape format's luma channel, a satellite up-link -- because the
    controls here are the parts of a real system and a coherent look is a
    coherent story about which system this is.

    The values live in the same 0..1 host-facing space both builds expose, so
    ONE table drives the FFGL and the OFX plugin and a preset cannot mean
    different things in Resolume and Resolve. Plain data only; the machinery
    that applies it lives with each host's glue.

    Element 0 of the dropdown is "Custom" and is not in this table: it means
    "the sliders are the truth".

    --------------------------------------------------------------- the naming

    These are the well-known systems, named for what they are rather than for
    whose trademark they are. "Two-to-one VCA compander", "dual-band", "digital
    hybrid" and "video link pre-emphasis" are the terms the specifications use,
    they are what the rest of the industry calls these curves when it is not
    licensed to use the brand names, and they are what somebody looking for this
    behaviour will actually search for. This is not anybody's product and does
    not claim to be a measurement of one.

    ⚠️ **`High Density` is an approximation of a topology this plugin does not
    have.** The real two-band systems split the signal and compand the halves
    against different laws; this has one band. The preset gets near the
    character by leaning the emphasis network harder and slowing the detector,
    which is a fair impression and is not the same circuit. Said here because
    the preset name cannot say it.

    ------------------------------------------------- what a preset must not set

    **Not Mix.** The wet/dry every effect has.

    **Not Sidechain, and not any of the Audio controls.** Which envelope drives
    the circuit, and off which part of the spectrum, is a routing decision about
    the operator's show -- their audio, their mapping. A preset that reached in
    there would silently repatch a working rig.

    ------------------------------------------------------ three decimal places

    Every value below is an exact multiple of 0.001, and that is load-bearing
    rather than tidy. The host quantises what it echoes back, the "has the
    operator taken over?" test has to allow for that, and orrery is on record as
    the one plugin in the fleet whose preset values were not round -- which is
    how it found that the allowance and the edit-detection threshold are two
    different numbers. Staying on the grid sidesteps the whole question.
*/

namespace compander
{
namespace presets
{
/// The parameters a preset sets, in one fixed order. The FFGL build binds this
/// order to its ParamIDs and the OFX build to its param handles; both
/// static_assert against kParamCount so the three lists cannot drift silently.
enum Param
{
	kChroma,
	kCompress,
	kEmphasis,
	kLinkLevel,
	kNoise,
	kHeadroom,
	kExpand,
	kTilt,
	kTimeConstant,
	kPivot,
	kMaxGain,
	kParamCount
};

struct Preset
{
	const char* name;
	float v[ kParamCount ];
};

// Chroma is 0 for anything that carried one channel -- a radio mic, a tape
// format's luma head -- and 1 for anything that put a whole composite signal
// through one emphasis network, which is what the video links did.
inline constexpr Preset kPresets[] = {
	// The one almost everything was: broadband, two-to-one, a VCA and an RMS
	// detector, moderate pre-emphasis. Fast enough that the gain change hugs
	// the edges rather than trailing off them.
	{ "Two-to-One VCA",
	  { 0.000f, 0.333f, 0.500f, 0.500f, 0.200f, 0.257f, 0.333f, 0.500f, 0.357f, 0.823f, 0.500f } },

	// The quieter, more ambitious end of the same generation: a gentler ratio
	// carrying a much deeper emphasis network, and a slower detector so the
	// gain rides the shot rather than the edge. Approximated with one band --
	// see the warning above.
	{ "High Density",
	  { 0.000f, 0.267f, 0.750f, 0.500f, 0.133f, 0.161f, 0.267f, 0.500f, 0.442f, 0.732f, 0.375f } },

	// The modern one: a compander emulated in arithmetic rather than built out
	// of a VCA, so it tracks almost perfectly. Nearly transparent on purpose --
	// this preset is what the plugin looks like when everything is working, and
	// it is the one to start from before breaking something deliberately.
	{ "Digital Hybrid",
	  { 0.000f, 0.333f, 0.250f, 0.500f, 0.067f, 0.000f, 0.333f, 0.500f, 0.264f, 0.823f, 0.625f } },

	// A receiver on the wrong side of the room. Six dB down into the link with
	// the noise floor where it always was, so the expander restores the picture
	// and the hiss by different amounts and the shadows breathe.
	{ "Diversity Fade",
	  { 0.000f, 0.333f, 0.500f, 0.250f, 0.533f, 0.257f, 0.333f, 0.500f, 0.536f, 0.823f, 0.500f } },

	// Something from a market stall: over-compressed, running out of headroom,
	// and with the two ends set to ratios that do not match, so nothing ever
	// quite comes back. The detector is slow enough to smear the gain change
	// off the side of everything bright.
	{ "Cheap Handheld",
	  { 0.000f, 0.667f, 0.917f, 0.375f, 0.400f, 0.661f, 0.467f, 0.625f, 0.660f, 0.732f, 0.625f } },

	// Not a microphone at all: the luma channel of a consumer tape format,
	// which used the same trick. Very heavy pre-emphasis into a hard ceiling --
	// the white clip -- a fast detector because the channel was only a few MHz
	// wide, and Chroma at 0 because the colour genuinely did go somewhere else.
	{ "Tape Luma",
	  { 0.000f, 0.167f, 1.000f, 0.500f, 0.333f, 0.757f, 0.167f, 0.650f, 0.179f, 0.732f, 0.250f } },

	// A video up-link. Composite through one emphasis network, so Chroma is 1
	// and the colour moves with the picture; a long detector, because the thing
	// being protected is the whole frame's level and not its edges.
	{ "Satellite Link",
	  { 1.000f, 0.267f, 0.583f, 0.417f, 0.467f, 0.500f, 0.267f, 0.500f, 0.482f, 0.823f, 0.500f } },

	// The same idea in a truck, done worse: further down into the link, noisier,
	// and de-emphasising slightly harder than the transmitter emphasised, which
	// is the soft, slightly smeared look of a shot that has been up a mast.
	{ "ENG Microwave",
	  { 1.000f, 0.333f, 0.750f, 0.333f, 0.600f, 0.576f, 0.300f, 0.425f, 0.567f, 0.823f, 0.500f } },

	// Half the chain. Compressed and emphasised with nothing at the far end to
	// undo either: flat, lifted out of the shadows, edges ringing. Tilt at the
	// top of its travel is what removes the de-emphasis entirely.
	{ "Encode Only",
	  { 0.000f, 0.500f, 0.750f, 0.500f, 0.100f, 0.369f, 0.000f, 1.000f, 0.357f, 0.823f, 0.500f } },

	// The other half. A decoder handed material that was never encoded:
	// contrasty, blacks crushed under the floor, fine detail sucked out of
	// anything dim. The single most recognisable of the three.
	{ "Decode Only",
	  { 0.000f, 0.000f, 0.750f, 0.500f, 0.100f, 0.000f, 0.500f, 0.000f, 0.357f, 0.823f, 0.500f } },
};

inline constexpr int kCount = int( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );

} // namespace presets
} // namespace compander
