#include "ps2/render/Ps2TerrainCommands.h"

#ifdef PS2_PLATFORM

#include <algorithm>
#include <cstddef>

#include "platform/PlatformTuning.h"
#include "ps2/render/Ps2RenderBackend.h"
#include "ps2/render/Ps2TerrainCulling.h"
#include "ps2/render/Ps2TerrainEligibility.h"
#include "ps2/render/Ps2TerrainMesh.h"
#include "ps2/render/Ps2TerrainMeshView.h"
#include "ps2/render/Ps2TerrainRuntime.h"
#include "ps2/render/Ps2Vu1Terrain.h"

#ifdef PS2_RENDER_STATS
#define PS2_TERRAIN_COMMAND_STAT(expr) do { expr; } while (0)
#else
#define PS2_TERRAIN_COMMAND_STAT(expr) do { } while (0)
#endif

namespace
{
constexpr int kMaxVu1Slices = 8;
constexpr int kMaxVu0Slices = 32;

bool faceVisible(const Ps2TerrainSectionView& section, int group)
{
#if PLATFORM_FACE_BUCKET_CULL
    if (group != PS2_FACE_OTHER)
    {
        const float eye[3] = {
            section.eyeLocalX,
            section.eyeLocalY,
            section.eyeLocalZ
        };
        const int axis = group >> 1;
        const bool positive = (group & 1) == 0;
        const float margin = (float)PLATFORM_FACE_CULL_EYE_MARGIN;
        return positive
            ? eye[axis] + margin > section.faceGroups->planeMin[group]
            : eye[axis] - margin < section.faceGroups->planeMax[group];
    }
#else
    (void)section;
    (void)group;
#endif
    return true;
}

void appendVu1Slices(Ps2TerrainCommandBuffer& commands,
                     int sectionIndex,
                     const Ps2Vu1TerrainSlice* slices,
                     int sliceCount,
                     int totalVertices,
                     int tileX,
                     int tileY)
{
    if (slices == nullptr || sliceCount <= 0 || totalVertices <= 0)
        return;

    Ps2TerrainVu1Command command = {};
    command.sectionIndex = sectionIndex;
    command.sliceOffset = (int)commands.vu1Slices.size();
    command.sliceCount = sliceCount;
    command.totalVertices = totalVertices;
    command.tileX = (unsigned char)tileX;
    command.tileY = (unsigned char)tileY;
    command.kind = Ps2TerrainVu1Command::Slices;
    for (int i = 0; i < sliceCount; ++i)
        commands.vu1Slices.push_back(slices[i]);
    commands.vu1Commands.push_back(command);
}

void appendVu1Range(Ps2TerrainCommandBuffer& commands,
                    int sectionIndex,
                    int firstVertex,
                    int vertexCount,
                    bool fullyInside)
{
    if (firstVertex < 0 || vertexCount <= 0)
        return;

    Ps2TerrainVu1Command command = {};
    command.sectionIndex = sectionIndex;
    command.firstVertex = firstVertex;
    command.vertexCount = vertexCount;
    command.totalVertices = vertexCount;
    command.fullyInside = fullyInside;
    command.kind = Ps2TerrainVu1Command::Range;
    commands.vu1Commands.push_back(command);
}

void appendVu0Command(std::vector<Ps2TerrainVu0Command>& commands,
                      std::vector<Ps2NativeSlice>& sliceStorage,
                      int sectionIndex,
                      const Ps2NativeSlice* slices,
                      int sliceCount,
                      int totalVertices,
                      bool fullyInside)
{
    if (slices == nullptr || sliceCount <= 0 || totalVertices <= 0)
        return;

    Ps2TerrainVu0Command command = {};
    command.sectionIndex = sectionIndex;
    command.sliceOffset = (int)sliceStorage.size();
    command.sliceCount = sliceCount;
    command.totalVertices = totalVertices;
    command.fullyInside = fullyInside;
    for (int i = 0; i < sliceCount; ++i)
        sliceStorage.push_back(slices[i]);
    commands.push_back(command);
}

void buildVu0Commands(Ps2TerrainCommandBuffer& commands,
                      const Ps2QueuedTerrainSection& queued,
                      int sectionIndex,
                      const Ps2TerrainClusterTarget* clusterTarget,
                      const bool* clusterClipSafe,
                      const bool* faceVisibility)
{
    const Ps2TerrainSectionView& section = queued.section;
    const std::vector<Ps2MeshRange>& ranges = section.faceGroups->ranges;
    const int maxBatchVertices = ps2_terrain_max_batch_vertices();

    for (int pass = 0; pass < 2; ++pass)
    {
        const bool wantClipSafe = pass == 0;
        Ps2NativeSlice normalSlices[kMaxVu0Slices];
        int normalSliceCount = 0;
        int normalVertices = 0;

        auto flushNormal = [&]()
        {
            appendVu0Command(commands.vu0Commands, commands.vu0Slices,
                             sectionIndex, normalSlices, normalSliceCount,
                             normalVertices, wantClipSafe);
            normalSliceCount = 0;
            normalVertices = 0;
        };
        auto appendSlice = [&](Ps2NativeSlice* slices,
                               int& sliceCount,
                               int& totalVertices,
                               int firstVertex,
                               int vertexCount,
                               const auto& flush)
        {
            while (vertexCount > 0)
            {
                const bool canMerge = sliceCount > 0 &&
                    slices[sliceCount - 1].firstVertex +
                        slices[sliceCount - 1].vertexCount == firstVertex;
                if (!canMerge && sliceCount >= kMaxVu0Slices)
                {
                    flush();
                    continue;
                }

                int take = maxBatchVertices - totalVertices;
                if (take > vertexCount)
                    take = vertexCount;
                take -= take & 3;
                if (take <= 0)
                {
                    flush();
                    continue;
                }

                if (canMerge)
                    slices[sliceCount - 1].vertexCount += take;
                else
                {
                    slices[sliceCount].firstVertex = firstVertex;
                    slices[sliceCount].vertexCount = take;
                    ++sliceCount;
                }
                totalVertices += take;
                firstVertex += take;
                vertexCount -= take;
            }
        };

        for (std::size_t i = 0; i < ranges.size(); ++i)
        {
            const Ps2MeshRange& range = ranges[i];
            const Ps2TerrainClusterTarget target = clusterTarget[range.cluster()];
            const bool clipSafe = clusterClipSafe[range.cluster()];
            if (target == PS2_TERRAIN_CLUSTER_OUTSIDE || clipSafe != wantClipSafe ||
                !faceVisibility[range.faceGroup()])
            {
                continue;
            }

            if (target != PS2_TERRAIN_CLUSTER_VU1)
            {
                appendSlice(normalSlices, normalSliceCount, normalVertices,
                            range.firstVertex, range.vertexCount(), flushNormal);
            }
        }
        flushNormal();
    }
}

void buildSideClippedVu1Commands(Ps2TerrainCommandBuffer& commands,
                                     const Ps2QueuedTerrainSection& queued,
                                     int sectionIndex,
                                     const Ps2TerrainClusterTarget* clusterTarget,
                                     const bool* clusterClipSafe,
                                     const bool* faceVisibility)
{
#if PS2_VU1_SIDE_CLIPPED_PARTIALS && !PS2_VU1_CLIPPED_PARTIALS
    const std::vector<Ps2MeshRange>& ranges = queued.section.faceGroups->ranges;
    int runStart = -1;
    int runCount = 0;

    auto flush = [&]()
    {
        if (runStart >= 0 && runCount > 0)
            appendVu1Range(commands, sectionIndex, runStart, runCount, false);
        runStart = -1;
        runCount = 0;
    };

    for (std::size_t i = 0; i < ranges.size(); ++i)
    {
        const Ps2MeshRange& range = ranges[i];
        const int cluster = range.cluster();
        const bool keep =
            clusterTarget[cluster] == PS2_TERRAIN_CLUSTER_VU1 &&
            !clusterClipSafe[cluster] &&
            faceVisibility[range.faceGroup()];
        if (!keep)
        {
            flush();
            continue;
        }

        if (runStart >= 0 && runStart + runCount == range.firstVertex)
        {
            runCount += range.vertexCount();
        }
        else
        {
            flush();
            runStart = range.firstVertex;
            runCount = range.vertexCount();
        }
    }
    flush();
#else
    (void)commands;
    (void)queued;
    (void)sectionIndex;
    (void)clusterTarget;
    (void)clusterClipSafe;
    (void)faceVisibility;
#endif
}

void buildVu1Commands(Ps2TerrainCommandBuffer& commands,
                      const Ps2QueuedTerrainSection& queued,
                      int sectionIndex,
                      const Ps2TerrainClusterTarget* clusterTarget,
                      const bool* clusterClipSafe,
                      const bool* faceVisibility)
{
    const Ps2TerrainSectionView& section = queued.section;
    const std::vector<Ps2MeshRange>& ranges = section.faceGroups->ranges;

#if !PS2_VU1_CLIPPED_PARTIALS
    const std::vector<Ps2TerrainTileRun>& tileRuns = section.opaqueMesh->runs();
    std::size_t rangeIndex = 0;
    for (std::size_t tr = 0; tr < tileRuns.size(); ++tr)
    {
        const Ps2TerrainTileRun& tile = tileRuns[tr];
        const int tileBegin = tile.firstVertex;
        const int tileEnd = tileBegin + tile.vertexCount;
        while (rangeIndex < ranges.size() &&
               ranges[rangeIndex].firstVertex + ranges[rangeIndex].vertexCount() <= tileBegin)
        {
            ++rangeIndex;
        }

        Ps2Vu1TerrainSlice slices[kMaxVu1Slices];
        int sliceCount = 0;
        int totalVertices = 0;

        auto flush = [&]()
        {
            appendVu1Slices(commands, sectionIndex, slices, sliceCount,
                            totalVertices, tile.tileX, tile.tileY);
            sliceCount = 0;
            totalVertices = 0;
        };

        for (std::size_t ri = rangeIndex; ri < ranges.size(); ++ri)
        {
            const Ps2MeshRange& range = ranges[ri];
            const int rangeBegin = range.firstVertex;
            const int rangeEnd = rangeBegin + range.vertexCount();
            if (rangeBegin >= tileEnd)
                break;
            if (rangeEnd <= tileBegin)
                continue;

            const Ps2TerrainClusterTarget target = clusterTarget[range.cluster()];
            if (target != PS2_TERRAIN_CLUSTER_VU1 ||
                !clusterClipSafe[range.cluster()] ||
                !faceVisibility[range.faceGroup()])
            {
                continue;
            }

            int cursor = std::max(rangeBegin, tileBegin);
            const int endVertex = std::min(rangeEnd, tileEnd);
            while (cursor < endVertex)
            {
                if (sliceCount >= kMaxVu1Slices ||
                    totalVertices >= PS2_VU1_TERRAIN_MAX_VERTICES)
                {
                    flush();
                }

                int take = endVertex - cursor;
                const int room = PS2_VU1_TERRAIN_MAX_VERTICES - totalVertices;
                if (take > room)
                    take = room;
                take &= ~3;
                if (take <= 0)
                {
                    flush();
                    continue;
                }

                if (sliceCount > 0 &&
                    slices[sliceCount - 1].firstVertex +
                        slices[sliceCount - 1].vertexCount == cursor)
                {
                    slices[sliceCount - 1].vertexCount += take;
                }
                else
                {
                    if (sliceCount >= kMaxVu1Slices)
                    {
                        flush();
                        continue;
                    }
                    slices[sliceCount].firstVertex = cursor;
                    slices[sliceCount].vertexCount = take;
                    ++sliceCount;
                }
                totalVertices += take;
                cursor += take;
            }
        }
        flush();
    }

    buildSideClippedVu1Commands(commands, queued, sectionIndex,
                                clusterTarget, clusterClipSafe, faceVisibility);
#else
    int runStart = -1;
    int runCount = 0;
    bool runFullyInside = false;
    for (std::size_t i = 0; i < ranges.size(); ++i)
    {
        const Ps2MeshRange& range = ranges[i];
        const bool fullyInside = clusterClipSafe[range.cluster()];
        const bool keep = clusterTarget[range.cluster()] == PS2_TERRAIN_CLUSTER_VU1 &&
            faceVisibility[range.faceGroup()];
        if (keep)
        {
            if (runStart >= 0 && runFullyInside == fullyInside &&
                runStart + runCount == range.firstVertex)
            {
                runCount += range.vertexCount();
            }
            else
            {
                if (runStart >= 0)
                    appendVu1Range(commands, sectionIndex, runStart, runCount, runFullyInside);
                runStart = range.firstVertex;
                runCount = range.vertexCount();
                runFullyInside = fullyInside;
            }
        }
        else if (runStart >= 0)
        {
            appendVu1Range(commands, sectionIndex, runStart, runCount, runFullyInside);
            runStart = -1;
            runCount = 0;
        }
    }
    if (runStart >= 0)
        appendVu1Range(commands, sectionIndex, runStart, runCount, runFullyInside);
#endif
}

void buildProbeCommands(Ps2TerrainCommandBuffer& commands,
                        const Ps2QueuedTerrainSection& queued,
                        int sectionIndex,
                        const Ps2TerrainClusterTarget* clusterTarget,
                        const bool* clusterClipSafe,
                        const bool* faceVisibility)
{
#if PS2_VU1_CLIPPED_PROBE_BATCHES_PER_FRAME > 0 && !PS2_VU1_CLIPPED_PARTIALS
    const Ps2TerrainSectionView& section = queued.section;
    const std::vector<Ps2MeshRange>& ranges = section.faceGroups->ranges;
    for (std::size_t i = 0; i < ranges.size(); ++i)
    {
        const Ps2MeshRange& range = ranges[i];
        const bool fullyInside = clusterClipSafe[range.cluster()];
        if (clusterTarget[range.cluster()] != PS2_TERRAIN_CLUSTER_VU0 || fullyInside ||
            !faceVisibility[range.faceGroup()])
        {
            continue;
        }
        Ps2TerrainProbeCommand command = {};
        command.sectionIndex = sectionIndex;
        command.firstVertex = range.firstVertex;
        command.vertexCount = range.vertexCount();
        commands.probeCommands.push_back(command);
    }
#else
    (void)commands;
    (void)queued;
    (void)sectionIndex;
    (void)clusterTarget;
    (void)clusterClipSafe;
    (void)faceVisibility;
#endif
}

void updateClassificationStats(Ps2TerrainClusterStats& stats,
                               const Ps2TerrainSectionView& section,
                               const int* clusterClass,
                               const int* clusterGuardRisk,
                               const Ps2TerrainClusterTarget* clusterTarget,
                               bool directVu1Usable)
{
    int clusterVertices[PS2_MESH_CLUSTER_COUNT] = {};
    const std::vector<Ps2MeshRange>& ranges = section.faceGroups->ranges;
    for (std::size_t i = 0; i < ranges.size(); ++i)
        clusterVertices[ranges[i].cluster()] += ranges[i].vertexCount();

    PS2_TERRAIN_COMMAND_STAT(++stats.sections);
    for (int cluster = 0; cluster < PS2_MESH_CLUSTER_COUNT; ++cluster)
    {
        const int vertices = clusterVertices[cluster];
        if (vertices <= 0)
            continue;
        if (clusterClass[cluster] == 2)
            PS2_TERRAIN_COMMAND_STAT(++stats.insideClusters);
        else if (clusterClass[cluster] == 1)
            PS2_TERRAIN_COMMAND_STAT(++stats.partialClusters);
        else
        {
            PS2_TERRAIN_COMMAND_STAT(++stats.outsideClusters);
            PS2_TERRAIN_COMMAND_STAT(stats.outsideVertices += vertices);
            continue;
        }

        const int risk = clusterGuardRisk[cluster];
        if (risk == PS2_CLUSTER_GUARD_SAFE && clusterClass[cluster] == 1)
        {
            PS2_TERRAIN_COMMAND_STAT(++stats.guardSafePartialClusters);
            PS2_TERRAIN_COMMAND_STAT(stats.guardSafePartialVertices += vertices);
        }
        if ((risk & PS2_CLUSTER_GUARD_NEAR) != 0)
            PS2_TERRAIN_COMMAND_STAT(++stats.guardNearRiskClusters);
        if ((risk & PS2_CLUSTER_GUARD_SIDE) != 0)
            PS2_TERRAIN_COMMAND_STAT(++stats.guardSideRiskClusters);

        if (clusterTarget[cluster] == PS2_TERRAIN_CLUSTER_VU1)
        {
            PS2_TERRAIN_COMMAND_STAT(stats.vu1EligibleVertices += vertices);
            if (ps2_terrain_cluster_uses_side_clip(clusterClass[cluster], risk,
                                                   directVu1Usable))
            {
                PS2_TERRAIN_COMMAND_STAT(stats.vu1SideClipVertices += vertices);
            }
            continue;
        }

        switch (ps2_terrain_vu0_reason(clusterClass[cluster], risk,
                                       directVu1Usable))
        {
            case PS2_TERRAIN_VU0_UNAVAILABLE:
                PS2_TERRAIN_COMMAND_STAT(stats.vu0UnavailableVertices += vertices);
                break;
            case PS2_TERRAIN_VU0_NEAR_RISK:
                PS2_TERRAIN_COMMAND_STAT(stats.vu0NearRiskVertices += vertices);
                break;
            case PS2_TERRAIN_VU0_SIDE_RISK:
                PS2_TERRAIN_COMMAND_STAT(stats.vu0SideRiskVertices += vertices);
                break;
            case PS2_TERRAIN_VU0_MIXED_RISK:
                PS2_TERRAIN_COMMAND_STAT(stats.vu0MixedRiskVertices += vertices);
                break;
            case PS2_TERRAIN_VU0_POLICY:
                PS2_TERRAIN_COMMAND_STAT(stats.vu0PolicyVertices += vertices);
                break;
            default:
                break;
        }
    }
}
}

bool ps2_terrain_build_section_commands(int sectionIndex)
{
    Ps2TerrainRuntimeState& runtime = ps2_terrain_runtime();
    if (sectionIndex < 0 || sectionIndex >= runtime.queuedCount)
        return false;

    Ps2QueuedTerrainSection& queued = runtime.queued[sectionIndex];
    queued.commandReady = false;
    queued.commandFailed = false;
    queued.forceVu0All = false;

    const Ps2TerrainSectionView& section = queued.section;
    if (runtime.currentPass != PS2_TERRAIN_PASS_OPAQUE ||
        section.vertexCount <= 0 || !section.nativeEnabled ||
        section.drawMode != PS2_NATIVE_PRIM_QUADS || !section.hasTexture ||
        section.hasNormals || section.opaqueMesh == nullptr ||
        !section.opaqueMesh->valid() ||
        section.opaqueMesh->vertexCount() < section.vertexCount ||
        section.faceGroups == nullptr || !section.faceGroups->valid ||
        section.faceGroups->ranges.empty())
    {
        return false;
    }

    const std::vector<Ps2MeshRange>& ranges = section.faceGroups->ranges;
    for (std::size_t i = 0; i < ranges.size(); ++i)
    {
        const Ps2MeshRange& range = ranges[i];
        if (range.firstVertex < 0 || range.vertexCount() <= 0 ||
            (range.firstVertex & 3) != 0 || (range.vertexCount() & 3) != 0 ||
            range.firstVertex + range.vertexCount() > section.vertexCount)
        {
            return false;
        }
    }

    if (!ps2_renderer_prepare_terrain_context(
            queued.commandContext, queued.frame,
            section.translateX, section.translateY, section.translateZ,
            section.fullyInside))
    {
        return false;
    }

    int clusterClass[PS2_MESH_CLUSTER_COUNT];
    int clusterGuardRisk[PS2_MESH_CLUSTER_COUNT];
    Ps2TerrainCullingContext cullingContext = {};
    ps2_terrain_build_culling_context(cullingContext, queued.commandContext.mvp,
                                      queued.frame.native.viewW,
                                      queued.frame.native.viewH);
    ps2_terrain_classify_clusters(section.faceGroups->clusters,
                                  section.fullyInside, true, &cullingContext,
                                  clusterClass, clusterGuardRisk);
    const bool directVu1Usable = PS2_DIRECT_VU1_TERRAIN &&
        ps2_vu1_terrain_pass_ready();
    bool clusterClipSafe[PS2_MESH_CLUSTER_COUNT];
    Ps2TerrainClusterTarget clusterTarget[PS2_MESH_CLUSTER_COUNT];
    for (int cluster = 0; cluster < PS2_MESH_CLUSTER_COUNT; ++cluster)
    {
        clusterClipSafe[cluster] = ps2_terrain_cluster_can_skip_clip(
            clusterClass[cluster], clusterGuardRisk[cluster]);
        clusterTarget[cluster] = ps2_terrain_cluster_target(
            clusterClass[cluster], clusterGuardRisk[cluster], directVu1Usable);
    }
    updateClassificationStats(runtime.clusterStats, section,
                              clusterClass, clusterGuardRisk, clusterTarget,
                              directVu1Usable);

    bool faceVisibility[PS2_FACE_GROUP_COUNT];
    for (int group = 0; group < PS2_FACE_GROUP_COUNT; ++group)
        faceVisibility[group] = faceVisible(section, group);

    if (directVu1Usable)
    {
        buildVu1Commands(runtime.commands, queued, sectionIndex,
                         clusterTarget, clusterClipSafe, faceVisibility);
        buildProbeCommands(runtime.commands, queued, sectionIndex,
                           clusterTarget, clusterClipSafe, faceVisibility);
    }
    buildVu0Commands(runtime.commands, queued, sectionIndex,
                     clusterTarget, clusterClipSafe, faceVisibility);

    queued.commandReady = true;
    return true;
}

#endif
