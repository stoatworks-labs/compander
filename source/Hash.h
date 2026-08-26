#pragma once

#include <cstdint>

/**
    A hash, for the scattered arrangement.

    Integer in, float out, deterministic, and cheap. It is not a good hash in
    any cryptographic sense and does not need to be -- the only property asked
    of it is that consecutive copy indices give uncorrelated positions, which
    is exactly what a plain LCG fails at and this passes.

    It runs on the **CPU only**. There is no GLSL twin, because there are at
    most 64 copies and their placement is solved once per frame rather than once
    per pixel. That is the same reasoning as orrery's motion, and it is the
    reason this file has no `//= mirrored` markers anywhere: there is one copy
    of the arithmetic, and the harness tests the real one.
*/
namespace flipbook
{
/// Thomas Wang's 32-bit integer hash. Every input bit reaches every output bit,
/// which is the whole requirement.
inline uint32_t HashU32( uint32_t x )
{
	x = ( x ^ 61u ) ^ ( x >> 16 );
	x = x + ( x << 3 );
	x = x ^ ( x >> 4 );
	x = x * 0x27d4eb2du;
	x = x ^ ( x >> 15 );
	return x;
}

inline uint32_t Hash2( uint32_t a, uint32_t b )
{
	return HashU32( a * 0x9e3779b9u ^ HashU32( b ) );
}

/// 0..1, from two integers.
inline float Hash01( uint32_t a, uint32_t b )
{
	return static_cast< float >( Hash2( a, b ) ) * ( 1.0f / 4294967296.0f );
}

/// -1..1, from two integers.
inline float Hash11( uint32_t a, uint32_t b )
{
	return Hash01( a, b ) * 2.0f - 1.0f;
}

} // namespace flipbook
