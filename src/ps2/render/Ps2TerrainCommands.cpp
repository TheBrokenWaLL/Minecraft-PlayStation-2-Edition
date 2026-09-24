#include "ps2/render/Ps2TerrainCommands.h"

#ifdef PS2_PLATFORM

#include "ps2/render/Ps2TerrainRuntime.h"

std::size_t ps2_terrain_command_ram_bytes()
{
    const Ps2TerrainCommandBuffer& commands = ps2_terrain_runtime().commands;
    return commands.vu1Commands.capacity() * sizeof(Ps2TerrainVu1Command) +
        commands.vu1Slices.capacity() * sizeof(Ps2Vu1TerrainSlice) +
        commands.vu0Commands.capacity() * sizeof(Ps2TerrainVu0Command) +
        commands.vu0Slices.capacity() * sizeof(Ps2NativeSlice) +
        commands.probeCommands.capacity() * sizeof(Ps2TerrainProbeCommand);
}

void ps2_terrain_commands_reset()
{
    Ps2TerrainCommandBuffer& commands = ps2_terrain_runtime().commands;
    commands.vu1Commands.clear();
    commands.vu1Slices.clear();
    commands.vu0Commands.clear();
    commands.vu0Slices.clear();
    commands.probeCommands.clear();
}

#endif
