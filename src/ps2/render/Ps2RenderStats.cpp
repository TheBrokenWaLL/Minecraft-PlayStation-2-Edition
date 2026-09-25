#ifdef PS2_PLATFORM

#include "ps2/render/Ps2RenderStats.h"

#include "platform/Log.h"
#include "ps2/render/Ps2GeometryCache.h"
#include "ps2/render/Ps2GsQueue.h"
#include "ps2/render/Ps2RenderBackend.h"
#include "ps2/render/Ps2TerrainRenderer.h"
#ifdef PS2_ENABLE_VU1_TERRAIN
#include "ps2/render/Ps2Vu1Terrain.h"
#endif

Ps2RenderStats& ps2_render_stats()
{
    static Ps2RenderStats stats;
    return stats;
}


extern "C" void ps2_dbg_draw_dump()
{
#ifdef PS2_RENDER_STATS
    Ps2RenderStats& stats = ps2_render_stats();
    Ps2GsQueueRenderStats queueStats;
    ps2_gs_queue_render_stats(queueStats, false);
    MC_LOG_DEBUG("render", "[PS2] 3D draw: calls=%ld  native=%ld/%ld  prims=%ld  clip=%ld  offscr=%ld  backface=%ld  qflush=%ld"
           "  qwait=%.1fms qpeak=%ldKB qdraw=%ldKB qover=%ld\n",
           stats.draw3dCalls, stats.nativeHits, stats.nativeAttempts,
           stats.draw3dPrims, stats.draw3dClipped, stats.draw3dOffscreen,
           stats.draw3dBackface, queueStats.flushes,
           (double)queueStats.flushCycles / 294000.0,
           queueStats.peakBytes / 1024, queueStats.maxGuardWriteBytes / 1024, queueStats.guardOverruns);

    Ps2TerrainClusterStats clusterStats;
    ps2_terrain_take_cluster_stats(clusterStats);
    // Packed opaque command path only. Times exclude validation, native
    // context preparation, target selection, stats gathering and submission.
    // Counts/totals cover the reporting interval, not a single frame.
    const double passCycles = 294912.0 *
        (clusterStats.opaquePasses > 0 ? clusterStats.opaquePasses : 1);
    MC_LOG_DEBUG("render", "[PS2] terrain CPU: passes=%ld sections=%ld clusters=%ld rejected=%ld"
           " classifyMs/pass=%.3f buildMs/pass=%.3f"
           " classifyMaxSectionMs=%.3f buildMaxSectionMs=%.3f\n",
           clusterStats.opaquePasses, clusterStats.commandSections,
           clusterStats.testedClusters, clusterStats.rejectedClusters,
           (double)clusterStats.classificationCycles / passCycles,
           (double)clusterStats.commandBuildCycles / passCycles,
           (double)clusterStats.classificationMaxCycles / 294912.0,
           (double)clusterStats.commandBuildMaxCycles / 294912.0);
    MC_LOG_DEBUG("render", "[PS2] terrain clusters: sections=%ld inside=%ld partial=%ld outside=%ld"
           " culledVerts=%ld guardSafePartial=%ld/%ld guardRisk=%ld/%ld"
           " vu1Eligible=%ld sideClip=%ld vu0Risk=%ld/%ld/%ld unavailable=%ld policy=%ld"
           " vu1Ranges=%ld vu1Verts=%ld vu0Batches=%ld vu0Verts=%ld\n",
           clusterStats.sections, clusterStats.insideClusters,
           clusterStats.partialClusters, clusterStats.outsideClusters,
           clusterStats.outsideVertices,
           clusterStats.guardSafePartialClusters,
           clusterStats.guardSafePartialVertices,
           clusterStats.guardNearRiskClusters,
           clusterStats.guardSideRiskClusters,
           clusterStats.vu1EligibleVertices,
           clusterStats.vu1SideClipVertices,
           clusterStats.vu0NearRiskVertices,
           clusterStats.vu0SideRiskVertices,
           clusterStats.vu0MixedRiskVertices,
           clusterStats.vu0UnavailableVertices,
           clusterStats.vu0PolicyVertices,
           clusterStats.vu1Ranges,
           clusterStats.vu1Vertices, clusterStats.vu0GatherBatches,
           clusterStats.vu0GatherVertices);

    const Ps2TranslucentStats& trans = stats.translucent;
    const double transPassCycles = 294912.0 * (trans.passes > 0 ? trans.passes : 1);
    MC_LOG_DEBUG("render", "[PS2] translucent: passes=%ld draws=%ld quads=%ld trivialRejectQuads=%ld"
        " clipInTris=%ld clipFanTris=%ld stripFlush=%ld triFlush=%ld\n",
        trans.passes, trans.draws, trans.quads, trans.rejectedQuads,
        trans.clippedInputTriangles, trans.clippedOutputTriangles,
        trans.stripFlushes, trans.triangleFlushes);
    // Partial CPU spans, not total pass time: project covers quad-strip
    // projection; emit covers strip packing (including fog) and batch flushes.
    // clip measures polygon clipping only, excluding fan projection/emission.
    MC_LOG_DEBUG("render", "[PS2] translucent CPU ms/pass: xform=%.3f stripProject=%.3f"
        " packFlush=%.3f clipPoly=%.3f\n",
        (double)trans.transformCycles / transPassCycles,
        (double)trans.projectCycles / transPassCycles,
        (double)trans.emitCycles / transPassCycles,
        (double)trans.clipCycles / transPassCycles);
    // Submit includes queue flush time: these columns must not be added.
    // Strip packing includes fog, but excludes clipped-triangle packing.
    // Queue flush measures submission plus any wait, not isolated GS busy time.
    MC_LOG_DEBUG("render", "[PS2] translucent submit ms/pass: stripPack=%.3f"
        " batchSubmit=%.3f queueFlushIncluded=%.3f queueFlushes=%ld\n",
        (double)trans.stripPackCycles / transPassCycles,
        (double)trans.batchSubmitCycles / transPassCycles,
        (double)trans.queueFlushCycles / transPassCycles, trans.queueFlushes);
    // Nested scopes, NOT an additive pass breakdown. drawTotal includes all
    // scopes and profiler overhead; quadTriangles includes clipping, projection,
    // attribute fetch, emission and any flush it triggers. stripPrepare covers
    // post-projection rejection, attribute fetch/software fog and atlas selection.
    // Work before ps2_draw_3d (e.g. raw terrain preparation) is outside drawTotal.
    MC_LOG_DEBUG("render", "[PS2] translucent coverage ms/pass: drawTotal=%.3f"
        " setup=%.3f quadClassify=%.3f stripPrepare=%.3f quadTriangles=%.3f\n",
        (double)trans.drawCycles / transPassCycles,
        (double)trans.setupCycles / transPassCycles,
        (double)trans.classifyCycles / transPassCycles,
        (double)trans.stripPrepareCycles / transPassCycles,
        (double)trans.quadTriangleCycles / transPassCycles);
    stats.translucent = Ps2TranslucentStats{};

    const long stripFlush = stats.stripFlush;
    MC_LOG_DEBUG("render", "[PS2] batch: stripFlush=%ld batchFlush=%ld quads=%ld quadsPerFlush=%ld"
           "  clampAsk=%ld clampSet=%ld vu0Quads=%ld | cyc xform=%.1fms project=%.1fms emit=%.1fms\n",
           stripFlush, stats.batchFlush, stats.stripQuads,
           stripFlush > 0 ? stats.stripQuads / stripFlush : 0,
           stats.clampAsk, stats.clampSet, stats.vu0Quads,
           (double)stats.cycleTransform / 294000.0,
           (double)stats.cycleProject / 294000.0,
           (double)stats.cycleEmit / 294000.0);

    // Model geometry is a lazily grown pool on a 32MB console, and
    // terrainMeshRamBreakdown covers terrain only. Report it here or the port
    // loses sight of it entirely.
    const Ps2GeometryCacheStats geometry = ps2GetGeometryCacheStats();
    MC_LOG_DEBUG("render", "[PS2] geometry cache: model=%dx/%uKB inventory=%dx/%uKB\n",
           geometry.modelEntries, (unsigned int)(geometry.modelBytes / 1024u),
           geometry.inventoryEntries, (unsigned int)(geometry.inventoryBytes / 1024u));
    stats.stripFlush = 0;
    stats.batchFlush = 0;
    stats.stripQuads = 0;
    stats.clampAsk = 0;
    stats.clampSet = 0;
    stats.vu0Quads = 0;
    stats.cycleTransform = 0;
    stats.cycleProject = 0;
    stats.cycleEmit = 0;
    stats.cycleStripPack = 0;
    stats.cycleBatchSubmit = 0;
    stats.cycleVu0QueueFlush = 0;
    stats.vu0QueueFlushes = 0;

#ifdef PS2_ENABLE_VU1_TERRAIN
    Ps2Vu1TerrainStats terrainVu1;
    ps2_vu1_terrain_take_stats(terrainVu1);
    MC_LOG_DEBUG("render", "[PS2] terrain VU1: pages=%ld batches=%ld vertices=%ld clipped=%ld/%ld"
           " probe=%ld/%ld retry=%ld xgkicks=%ld"
           " buffers=%ld/%ld qwords=%ld pagePeak=%ld waits=%ld wait=%.1fms transitions=%ld"
           " canary=%ld/%ld fail=%ld\n",
           terrainVu1.pages, terrainVu1.batches, terrainVu1.vertices,
           terrainVu1.clippedBatches, terrainVu1.clippedVertices,
           terrainVu1.clippedProbeBatches, terrainVu1.clippedProbeVertices,
           terrainVu1.clippedProbeRetries,
           terrainVu1.xgkicks, terrainVu1.buffer0Batches,
           terrainVu1.buffer1Batches, terrainVu1.qwords, terrainVu1.maxPageQwords,
           terrainVu1.waits, (double)terrainVu1.waitCycles / 294000.0,
           terrainVu1.pathTransitions, terrainVu1.canaryCompleted,
           terrainVu1.canarySubmitted, terrainVu1.canaryFailures);
#endif

    stats.draw3dCalls = 0;
    stats.draw3dPrims = 0;
    stats.draw3dClipped = 0;
    stats.draw3dOffscreen = 0;
    stats.draw3dBackface = 0;
    stats.nativeAttempts = 0;
    stats.nativeHits = 0;
    ps2_gs_queue_render_stats(queueStats, true);
#endif
}

extern "C" void ps2_dbg_depth_dump()
{
#ifdef PS2_RENDER_STATS
    Ps2RenderStats& stats = ps2_render_stats();
    MC_LOG_DEBUG("render", "[PS2] depth: zclr=%ld ztest=%ld zpin=%ld zwr=%ld/%ld zlo=%d zhi=%d span=%d (max=%d)\n",
           stats.depthClears, stats.orthoZTested, stats.orthoZPinned,
           stats.zWriteOn, stats.zWriteOn + stats.zWriteOff,
           stats.orthoZHigh < 0 ? -1 : stats.orthoZLow, stats.orthoZHigh,
           stats.orthoZHigh < 0 ? 0 : (stats.orthoZHigh - stats.orthoZLow),
           PS2_GS_Z_MAX);
    MC_LOG_DEBUG("render", "[PS2] depth: oz=%.1f..%.1f ortho near=%.1f far=%.1f (expect oz ~ -2000)\n",
           stats.orthoEyeZHigh < -1e29f ? 0.0f : stats.orthoEyeZLow,
           stats.orthoEyeZHigh < -1e29f ? 0.0f : stats.orthoEyeZHigh,
           stats.orthoNear, stats.orthoFar);
    MC_LOG_DEBUG("render", "[PS2] gui: sprites=%ld tris=%ld\n",
           stats.orthoSprites, stats.orthoSpriteFallbacks);

    stats.orthoSprites = 0;
    stats.orthoSpriteFallbacks = 0;
    stats.depthClears = 0;
    stats.orthoZTested = 0;
    stats.orthoZPinned = 0;
    stats.zWriteOn = 0;
    stats.zWriteOff = 0;
    stats.orthoZLow = 0x7FFFFFFF;
    stats.orthoZHigh = -1;
    stats.orthoEyeZLow = 1.0e30f;
    stats.orthoEyeZHigh = -1.0e30f;
#endif
}

#endif // PS2_PLATFORM
