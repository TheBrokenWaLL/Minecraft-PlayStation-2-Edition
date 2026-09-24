#pragma once

#ifdef PS2_PLATFORM

#include <cstddef>
#include <vector>

#include "ps2/render/Ps2NativeDraw.h"
#include "ps2/render/Ps2Vu1TerrainPackets.h"

struct Ps2TerrainVu1Command
{
    enum Kind
    {
        Slices,
        Range
    };

    int sectionIndex;
    int sliceOffset;
    int sliceCount;
    int totalVertices;
    int firstVertex;
    int vertexCount;
    unsigned char tileX;
    unsigned char tileY;
    bool fullyInside;
    Kind kind;
};

struct Ps2TerrainVu0Command
{
    int sectionIndex;
    int sliceOffset;
    int sliceCount;
    int totalVertices;
    bool fullyInside;
};

struct Ps2TerrainProbeCommand
{
    int sectionIndex;
    int firstVertex;
    int vertexCount;
};

struct Ps2TerrainCommandBuffer
{
    std::vector<Ps2TerrainVu1Command> vu1Commands;
    std::vector<Ps2Vu1TerrainSlice> vu1Slices;
    std::vector<Ps2TerrainVu0Command> vu0Commands;
    std::vector<Ps2NativeSlice> vu0Slices;
    std::vector<Ps2TerrainProbeCommand> probeCommands;
};

std::size_t ps2_terrain_command_ram_bytes();
void ps2_terrain_commands_reset();
bool ps2_terrain_build_section_commands(int sectionIndex);
void ps2_terrain_submit_vu1_commands();
void ps2_terrain_submit_vu0_commands();

#endif
