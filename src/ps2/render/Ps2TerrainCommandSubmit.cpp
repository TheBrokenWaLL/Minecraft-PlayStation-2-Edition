#include "ps2/render/Ps2TerrainCommands.h"

#ifdef PS2_PLATFORM

#include <cstddef>

#include "ps2/render/Ps2RenderBackend.h"
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
void submitVu0List(const std::vector<Ps2TerrainVu0Command>& commandList,
                   const std::vector<Ps2NativeSlice>& sliceStorage)
{
    Ps2TerrainRuntimeState& runtime = ps2_terrain_runtime();
    for (std::size_t i = 0; i < commandList.size(); ++i)
    {
        const Ps2TerrainVu0Command& command = commandList[i];
        if (command.sectionIndex < 0 || command.sectionIndex >= runtime.queuedCount)
            continue;
        Ps2QueuedTerrainSection& queued = runtime.queued[command.sectionIndex];
        if (!queued.commandReady || queued.commandFailed ||
            queued.forceVu0All)
        {
            continue;
        }
        if (command.sliceOffset < 0 || command.sliceCount <= 0 ||
            command.sliceOffset + command.sliceCount > (int)sliceStorage.size())
        {
            queued.commandFailed = true;
            continue;
        }

        const Ps2TerrainMesh* meshSource = queued.section.opaqueMesh;
        if (meshSource == nullptr || !meshSource->valid())
        {
            queued.commandFailed = true;
            continue;
        }
        const std::vector<Ps2TerrainTileRun>& clampRuns = meshSource->runs();
        const Ps2NativeMeshView mesh = ps2_terrain_make_packed_mesh(
            meshSource->positions(), meshSource->texCoords(), meshSource->colors(),
            0, command.totalVertices,
            clampRuns.empty() ? nullptr : &clampRuns[0], (int)clampRuns.size(),
            &sliceStorage[command.sliceOffset], command.sliceCount);
        Ps2NativeDrawContext context = queued.commandContext;
        context.fullyInside = command.fullyInside;
        runtime.traceSection = command.sectionIndex;
        if (!ps2_renderer_draw_prepared(mesh, context))
        {
            queued.commandFailed = true;
            continue;
        }
        PS2_TERRAIN_COMMAND_STAT(++runtime.clusterStats.vu0GatherBatches);
        PS2_TERRAIN_COMMAND_STAT(
            runtime.clusterStats.vu0GatherVertices += command.totalVertices);
    }
}
}

void ps2_terrain_submit_vu1_commands()
{
    Ps2TerrainRuntimeState& runtime = ps2_terrain_runtime();
    Ps2TerrainCommandBuffer& commands = runtime.commands;

#if defined(PS2_ENABLE_VU1_TERRAIN)
    for (std::size_t i = 0; i < commands.vu1Commands.size(); ++i)
    {
        const Ps2TerrainVu1Command& command = commands.vu1Commands[i];
        if (command.sectionIndex < 0 || command.sectionIndex >= runtime.queuedCount)
            continue;
        Ps2QueuedTerrainSection& queued = runtime.queued[command.sectionIndex];
        if (!queued.commandReady || queued.commandFailed || queued.forceVu0All)
            continue;

        runtime.traceSection = command.sectionIndex;
        Ps2Vu1TerrainDrawResult result = { PS2_VU1_TERRAIN_RETRY_NATIVE, 0 };
        if (command.kind == Ps2TerrainVu1Command::Slices)
        {
            if (command.sliceOffset < 0 || command.sliceCount <= 0 ||
                command.sliceOffset + command.sliceCount > (int)commands.vu1Slices.size())
            {
                queued.forceVu0All = true;
                continue;
            }
            result = ps2_vu1_terrain_draw_slices(
                *queued.section.opaqueMesh,
                &commands.vu1Slices[command.sliceOffset],
                command.sliceCount, command.totalVertices,
                command.tileX, command.tileY, queued.frame.native,
                queued.section.translateX, queued.section.translateY,
                queued.section.translateZ);
        }
        else
        {
            result = ps2_vu1_terrain_draw_range(
                *queued.section.opaqueMesh,
                command.firstVertex, command.vertexCount,
                queued.frame.native,
                queued.section.translateX, queued.section.translateY,
                queued.section.translateZ, command.fullyInside);
        }

        if (result.status == PS2_VU1_TERRAIN_SUBMITTED)
        {
            PS2_TERRAIN_COMMAND_STAT(++runtime.clusterStats.vu1Ranges);
            PS2_TERRAIN_COMMAND_STAT(runtime.clusterStats.vu1Vertices += result.vertices);
            continue;
        }

        // RETRY and FATAL both request whole-section VU0 replay.
        // A fatal status can mean VU1 accepted a prefix before failing; opaque
        // depth testing makes replaying the complete section safe and matches
        // the legacy two-phase behavior.
        queued.forceVu0All = true;
    }

#if PS2_VU1_CLIPPED_PROBE_BATCHES_PER_FRAME > 0 && !PS2_VU1_CLIPPED_PARTIALS
    for (std::size_t i = 0; i < commands.probeCommands.size() &&
         ps2_vu1_terrain_clipped_probe_available(); ++i)
    {
        const Ps2TerrainProbeCommand& command = commands.probeCommands[i];
        if (command.sectionIndex < 0 || command.sectionIndex >= runtime.queuedCount)
            continue;
        Ps2QueuedTerrainSection& queued = runtime.queued[command.sectionIndex];
        if (!queued.commandReady || queued.commandFailed || queued.forceVu0All)
            continue;
        runtime.traceSection = command.sectionIndex;
        (void)ps2_vu1_terrain_probe_clipped_range(
            *queued.section.opaqueMesh, command.firstVertex, command.vertexCount,
            queued.frame.native,
            queued.section.translateX, queued.section.translateY,
            queued.section.translateZ);
    }

#endif
#else
    (void)commands;
#endif
}

void ps2_terrain_submit_vu0_commands()
{
    Ps2TerrainRuntimeState& runtime = ps2_terrain_runtime();
    submitVu0List(runtime.commands.vu0Commands,
                  runtime.commands.vu0Slices);

    // Path1 has been released by terrain_end. Failed VU1 sections skipped
    // normal VU0 commands above; hand them to its existing whole-section
    // VU0 replay loop instead of prebuilding an unused fallback every frame.
    // Invalidate classification because command-ready sections do not fill
    // the legacy cache, which may still describe an earlier queued section.
    for (int i = 0; i < runtime.queuedCount; ++i)
    {
        Ps2QueuedTerrainSection& queued = runtime.queued[i];
        if (queued.commandReady && queued.forceVu0All)
        {
            queued.classification.valid = false;
            queued.commandReady = false;
        }
    }
    runtime.traceSection = -1;
}

#endif
