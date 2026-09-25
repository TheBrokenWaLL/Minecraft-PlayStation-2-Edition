#pragma once

#ifdef PS2_PLATFORM

#ifdef PS2_RENDER_STATS
struct Ps2TranslucentStats
{
    unsigned long long transformCycles = 0, projectCycles = 0, emitCycles = 0, clipCycles = 0;
    unsigned long long stripPackCycles = 0, batchSubmitCycles = 0, queueFlushCycles = 0;
    unsigned long long drawCycles = 0, setupCycles = 0, classifyCycles = 0;
    unsigned long long stripPrepareCycles = 0, quadTriangleCycles = 0;
    long queueFlushes = 0;
    long passes = 0, draws = 0, quads = 0, rejectedQuads = 0;
    long clippedInputTriangles = 0, clippedOutputTriangles = 0;
    long stripFlushes = 0, triangleFlushes = 0;
};
#endif

struct Ps2RenderStats
{
    long depthClears = 0;
    long orthoZTested = 0;
    long orthoZPinned = 0;
    int orthoZLow = 0x7FFFFFFF;
    int orthoZHigh = -1;
    float orthoEyeZLow = 1.0e30f;
    float orthoEyeZHigh = -1.0e30f;
    float orthoNear = 0.0f;
    float orthoFar = 0.0f;
    long orthoSprites = 0;
    long orthoSpriteFallbacks = 0;
    long zWriteOn = 0;
    long zWriteOff = 0;

    long draw3dPrims = 0;
    long draw3dClipped = 0;
    long draw3dOffscreen = 0;
    long draw3dBackface = 0;
    long draw3dCalls = 0;
    long nativeAttempts = 0;
    long nativeHits = 0;

#if MC_LOG_LEVEL > 2
    long profile3DrawCalls = 0;
    long profile3Vertices = 0;
    long profile3Vu0Vertices = 0;
    long profile3Vu1Vertices = 0;
    long profile3Vu1DrawCalls = 0;
#endif

#ifdef PS2_RENDER_STATS
    Ps2TranslucentStats translucent;
    long stripFlush = 0;
    long batchFlush = 0;
    long stripQuads = 0;
    long clampAsk = 0;
    long clampSet = 0;
    long vu0Quads = 0;
    unsigned long cycleTransform = 0;
    unsigned long cycleProject = 0;
    unsigned long cycleEmit = 0;
    unsigned long cycleStripPack = 0, cycleBatchSubmit = 0, cycleVu0QueueFlush = 0;
    long vu0QueueFlushes = 0;
#endif
};

Ps2RenderStats& ps2_render_stats();

#ifdef PS2_RENDER_STATS
#define PS2_RENDER_STAT(expr) do { expr; } while (0)
#define PS2_DEPTH_STAT(expr) do { expr; } while (0)
#else
#define PS2_RENDER_STAT(expr) do { } while (0)
#define PS2_DEPTH_STAT(expr) do { } while (0)
#endif

extern "C" void ps2_dbg_draw_dump();
extern "C" void ps2_dbg_depth_dump();

#endif // PS2_PLATFORM
