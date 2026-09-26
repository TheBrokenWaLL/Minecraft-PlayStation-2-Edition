#pragma once

// Bounded float arithmetic avoids per-column Java RNG/software-double sampling
// on the EE, and avoids non-integral jumps in repeated weather textures.
inline unsigned int ps2WeatherHash(unsigned int seed)
{
    seed ^= seed >> 16;
    seed *= 0x7feb352du;
    seed ^= seed >> 15;
    return seed;
}

inline float ps2RainOffset(int tick, float partialTick, unsigned int seed)
{
    // Two texture repeats per 32 ticks (five blocks/second), with a fixed
    // column phase. Wrapping the clock drops exactly sixteen whole repeats.
    return (static_cast<float>(tick & 255) + partialTick) / 16.0f +
        static_cast<float>(seed & 255u) / 256.0f;
}

inline float ps2WeatherOpacity(float distanceSquared, float strength, bool snow)
{
    const float opacity = snow ? 0.8f - 0.3f * distanceSquared
                               : 1.0f - 0.5f * distanceSquared;
    return (opacity > 0.0f ? opacity : 0.0f) * strength;
}
