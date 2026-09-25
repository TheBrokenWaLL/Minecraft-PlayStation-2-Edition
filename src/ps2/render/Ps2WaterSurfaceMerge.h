#pragma once

#ifdef PS2_PLATFORM
#include <cmath>
#include <cstring>
#include <vector>

// Compact only consecutive top-face pairs. Never sort translucent primitives
// or merge across build-step/section boundaries. The predicate checks material
// identity in the source world; atlas coordinates alone are not sufficient.
template<class Word, class IsStillWater>
unsigned ps2MergeWaterTopPairs(std::vector<Word>& raw, int tile, IsStillWater isStillWater)
{
    static_assert(sizeof(Word) == 4, "PS2 capture requires 32-bit slots");
    constexpr unsigned stride = 6, quad = 24;
    if (tile < 0 || tile > 255 || raw.size() % quad != 0) return 0;
    const float u = float(tile & 15) / 16.0f;
    const float v = float(tile >> 4) / 16.0f;
    const float span = 1.0f / 16.0f;
    auto read = [](const Word* p) { float f; std::memcpy(&f, p, 4); return f; };
    auto write = [](Word* p, float f) { std::memcpy(p, &f, 4); };
    auto eligible = [&](const Word* p) {
        const float x = read(p), y = read(p + 1), z = read(p + 2);
        // Local section coordinates only; fractional height excludes cube tops.
        if (!(x >= 0 && x < 16 && y > 0 && y < 16 && z >= 0 && z < 16) ||
            x != std::floor(x) || z != std::floor(z) || y == std::floor(y)) return false;
        const int dx[4] = {0,0,1,1}, dz[4] = {0,1,1,0};
        for (unsigned i = 0; i < 4; ++i) {
            const Word* a = p + i * stride;
            if (read(a) != x + dx[i] || read(a+1) != y || read(a+2) != z + dz[i] ||
                read(a+3) != u + dx[i]*span || read(a+4) != v + dz[i]*span ||
                a[5] != p[5]) return false;
        }
        return isStillWater(int(x), int(std::floor(y)), int(z));
    };
    unsigned merged = 0;
    std::size_t out = 0;
    for (std::size_t in = 0; in < raw.size();) {
        Word* a = raw.data() + in;
        bool join = false;
        if (in + 2*quad <= raw.size()) {
            const Word* b = a + quad;
            // Cheap adjacency/color check before material and full vertex checks.
            join = read(b) == read(a)+1 && read(b+1) == read(a+1) &&
                read(b+2) == read(a+2) && b[5] == a[5] && eligible(a) && eligible(b);
        }
        if (join) {
            // Keep the first face's position in the stream and UV origin. The
            // existing atlas REGION_REPEAT sampler tiles the extended U span.
            for (unsigned i : {2u,3u}) {
                write(a+i*stride, read(a+i*stride)+1.0f);
                write(a+i*stride+3, u+2*span);
            }
            ++merged;
        }
        std::memmove(raw.data()+out, a, quad*sizeof(Word));
        out += quad;
        in += join ? 2*quad : quad;
    }
    raw.resize(out);
    return merged;
}
#endif
