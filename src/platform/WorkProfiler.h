#pragma once

#include "platform/Log.h"
#include "platform/PlatformConfig.h"
#include "platform/Profiler.h"

class Entity;

enum class PlatformLoadWork
{
    RegionOpen,
    RegionRead,
    Inflate,
    Nbt,
    ChunkDecode,
    TextureLoad,
    Palette,
    Count
};

enum class PlatformMeshWork
{
    CacheSetup,
    Greedy,
    ScanSetup,
    BlockScan,
    Capture,
    FaceSort,
    Pack,
    Commit,
    Count
};

#if PLATFORM_PS2 && MC_LOG_LEVEL >= 2
void platformProfileLoadWork(std::uint32_t start, PlatformLoadWork work);
void platformProfileEntityDraw(std::uint32_t start, Entity *entity);
void platformProfileEntityTickWork(std::uint32_t start, Entity *entity);
void platformProfileMeshWork(std::uint32_t start, PlatformMeshWork work);
// Attributed by NAME POINTER, not by an enum, so the caller's stage list stays
// the single definition of what the stages are. Callers must pass a string
// literal or other stable address: slots are matched by pointer identity, the
// way entity slots are matched by type_info identity.
//
// The "deco" figure in the pop line is one number covering roughly thirty
// generator passes, which is enough to know decoration dominates populate
// (measured 28.0 ms of a 44.3 ms average) and not enough to know which pass to
// look at. This splits it.
void platformProfileDecorWork(std::uint32_t start, const char *stage);
// Population block writes are counted rather than timed: a clock read on each
// side of a single chunk-array store would cost more than the store. The count
// is what the decoration timings need to be read against -- it turns "deco is
// 28 ms" into a cost per block.
void platformProfilePopulationBlockWrite();
void platformLogWorkProfileAndReset(int frame);
#else
inline void platformProfileLoadWork(std::uint32_t, PlatformLoadWork) {}
inline void platformProfileEntityDraw(std::uint32_t, Entity *) {}
inline void platformProfileEntityTickWork(std::uint32_t, Entity *) {}
inline void platformProfileMeshWork(std::uint32_t, PlatformMeshWork) {}
inline void platformProfileDecorWork(std::uint32_t, const char *) {}
inline void platformProfilePopulationBlockWrite() {}
inline void platformLogWorkProfileAndReset(int) {}
#endif

// Counts attempts, including early returns and exceptions; nested spans overlap.
class PlatformLoadWorkScope
{
public:
    explicit PlatformLoadWorkScope(PlatformLoadWork work)
#if PLATFORM_PS2 && MC_LOG_LEVEL >= 2
        : work_(work), start_(platformProfileRenderPhaseBegin())
#endif
    {
        (void)work;
    }

    ~PlatformLoadWorkScope()
    {
#if PLATFORM_PS2 && MC_LOG_LEVEL >= 2
        platformProfileLoadWork(start_, work_);
#endif
    }

    PlatformLoadWorkScope(const PlatformLoadWorkScope &) = delete;
    PlatformLoadWorkScope &operator=(const PlatformLoadWorkScope &) = delete;

private:
#if PLATFORM_PS2 && MC_LOG_LEVEL >= 2
    PlatformLoadWork work_;
    std::uint32_t start_;
#endif
};
