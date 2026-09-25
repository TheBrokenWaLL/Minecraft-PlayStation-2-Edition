#pragma once

#ifdef PS2_PLATFORM
#include <cmath>
#include <cstring>
#include <vector>

struct Ps2WaterMergeStats {
    unsigned input = 0, eligible = 0, rejectedShape = 0, rejectedMaterial = 0;
    unsigned boundaries = 0, pairs = 0, squares = 0, removed = 0;
};

// Merge only inside uninterrupted runs of coplanar, identically colored still
// water tops. Other transparent primitives are ordering barriers. A fixed local
// 16x16 lookup avoids persistent mesh metadata and heap allocations.
template<class Word, class IsStillWater>
Ps2WaterMergeStats ps2MergeWaterTops(std::vector<Word>& raw, int tile, IsStillWater isStillWater)
{
    static_assert(sizeof(Word) == 4, "PS2 capture requires 32-bit slots");
    constexpr unsigned stride = 6, quad = 24;
    Ps2WaterMergeStats stats;
    if (tile < 0 || tile > 255 || raw.size() % quad != 0) return stats;
    stats.input = unsigned(raw.size() / quad);
    const float u = float(tile & 15) / 16.0f, v = float(tile >> 4) / 16.0f;
    constexpr float span = 1.0f / 16.0f;
    auto read = [](const Word* p) { float f; std::memcpy(&f, p, 4); return f; };
    auto write = [](Word* p, float f) { std::memcpy(p, &f, 4); };
    auto classify = [&](const Word* p) {
        const float x = read(p), y = read(p+1), z = read(p+2);
        if (!(x >= 0 && x < 16 && y > 0 && y < 16 && z >= 0 && z < 16) ||
            x != std::floor(x) || z != std::floor(z) || y == std::floor(y)) return 1;
        const int dx[4] = {0,0,1,1}, dz[4] = {0,1,1,0};
        for (unsigned i = 0; i < 4; ++i) {
            const Word* a = p+i*stride;
            if (read(a) != x+dx[i] || read(a+1) != y || read(a+2) != z+dz[i] ||
                read(a+3) != u+dx[i]*span || read(a+4) != v+dz[i]*span ||
                a[5] != p[5]) return 1;
        }
        return isStillWater(int(x), int(std::floor(y)), int(z)) ? 0 : 2;
    };
    std::size_t out = 0, in = 0;
    while (in < raw.size()) {
        const int reason = classify(raw.data()+in);
        if (reason != 0) {
            if (reason == 1) ++stats.rejectedShape; else ++stats.rejectedMaterial;
            std::memmove(raw.data()+out, raw.data()+in, quad*sizeof(Word));
            out += quad; in += quad;
            continue;
        }
        short cells[256];
        for (short& cell : cells) cell = -1;
        bool consumed[256] = {};
        unsigned count = 0;
        const Word height = raw[in+1], color = raw[in+5];
        while (count < 256 && in+count*quad < raw.size()) {
            const Word* p = raw.data()+in+count*quad;
            // First face already classified. Rejected lookahead is handled by
            // the outer loop, so each face contributes to counters just once.
            if (count && (p[1] != height || p[5] != color || classify(p) != 0)) break;
            const unsigned cell = unsigned(read(p))+16u*unsigned(read(p+2));
            if (cells[cell] >= 0) break; // duplicate/overlapping surface: barrier
            cells[cell] = short(count++);
        }
        stats.eligible += count;
        if (in+count*quad < raw.size()) ++stats.boundaries;
        for (unsigned q = 0; q < count; ++q) {
            if (consumed[q]) continue;
            Word* p = raw.data()+in+q*quad;
            const int x = int(read(p)), z = int(read(p+2));
            auto available = [&](int cx, int cz) {
                if (cx >= 16 || cz >= 16) return -1;
                const int n = cells[cx+16*cz];
                return n > int(q) && !consumed[n] ? n : -1;
            };
            const int right = available(x+1,z), below = available(x,z+1);
            const int diagonal = available(x+1,z+1);
            int width = 1, depth = 1;
            if (right >= 0 && below >= 0 && diagonal >= 0) {
                consumed[right] = consumed[below] = consumed[diagonal] = true;
                width = depth = 2; ++stats.squares; stats.removed += 3;
            } else if (right >= 0) {
                consumed[right] = true; width = 2; ++stats.pairs; ++stats.removed;
            } else if (below >= 0) {
                consumed[below] = true; depth = 2; ++stats.pairs; ++stats.removed;
            }
            if (width == 2 || depth == 2) {
                const int dx[4] = {0,0,width,width}, dz[4] = {0,depth,depth,0};
                for (unsigned i = 0; i < 4; ++i) {
                    write(p+i*stride, float(x+dx[i]));
                    write(p+i*stride+2, float(z+dz[i]));
                    write(p+i*stride+3, u+dx[i]*span);
                    write(p+i*stride+4, v+dz[i]*span);
                }
            }
            std::memmove(raw.data()+out, p, quad*sizeof(Word));
            out += quad;
        }
        in += count*quad;
    }
    raw.resize(out);
    return stats;
}
#endif
