#include "platform/WorkProfiler.h"

#if PLATFORM_PS2 && MC_LOG_LEVEL >= 2

#include "net/minecraft/src/Entity.h"
#include "net/minecraft/src/EntityList.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <typeinfo>

namespace
{
struct Sample
{
    unsigned long long cycles = 0;
    std::uint32_t maxCycles = 0;
    unsigned int count = 0;
};

constexpr int kEntitySlots = 48;
constexpr int kReportedEntities = 8;
struct EntitySample
{
    const std::type_info *type = nullptr;
    char name[40] = {};
    Sample sample;
};

Sample s_load[static_cast<int>(PlatformLoadWork::Count)];
const char *const kLoadNames[] = {
    "regionOpen", "regionRead", "inflate", "nbt", "chunkDecode", "textureLoad", "palette"
};
static_assert(sizeof(kLoadNames) / sizeof(kLoadNames[0]) ==
              static_cast<int>(PlatformLoadWork::Count), "Load profile names must match phases");
Sample s_mesh[static_cast<int>(PlatformMeshWork::Count)];
const char *const kMeshNames[] = {
    "cacheSetup", "greedy", "scanSetup", "blockScan",
    "capture", "faceSort", "pack", "commit"
};
static_assert(sizeof(kMeshNames) / sizeof(kMeshNames[0]) ==
              static_cast<int>(PlatformMeshWork::Count), "Mesh profile names must match phases");
EntitySample s_entities[kEntitySlots];
int s_entityCount = 0;
Sample s_overflow;
EntitySample s_tickEntities[kEntitySlots];
int s_tickEntityCount = 0;
Sample s_tickOverflow;

// One slot per decoration stage. Slots are keyed by the name pointer the caller
// passed, so BiomeDecorator's stage list stays the only place the stages are
// enumerated. 40 covers the current thirty-odd stages with room to add; past
// that the cost folds into an overflow slot rather than being dropped.
constexpr int kDecorSlots = 40;
constexpr int kReportedDecorStages = 12;
struct DecorSample
{
    const char *stage = nullptr;
    Sample sample;
};
DecorSample s_decor[kDecorSlots];
int s_decorCount = 0;
Sample s_decorOverflow;

unsigned int s_populationBlockWrites = 0;

void add(Sample &sample, std::uint32_t elapsed)
{
    sample.cycles += elapsed;
    sample.maxCycles = std::max(sample.maxCycles, elapsed);
    ++sample.count;
}

void report(int frame, const char *group, const char *name, const Sample &sample)
{
    if (sample.count == 0)
        return;
    const double totalMs = static_cast<double>(sample.cycles) / 294000.0;
    MC_LOG_DEBUG("work", "frame=%d %s=%s n=%u total=%.2fms avgCall=%.3fms maxCall=%.3fms\n",
                 frame, group, name, sample.count, totalMs,
                 totalMs / sample.count, static_cast<double>(sample.maxCycles) / 294000.0);
}
}

void platformProfileLoadWork(std::uint32_t start, PlatformLoadWork work)
{
    const std::uint32_t elapsed = platformProfileRenderPhaseBegin() - start;
    const int index = static_cast<int>(work);
    if (index >= 0 && index < static_cast<int>(PlatformLoadWork::Count))
        add(s_load[index], elapsed);
}

void platformProfileMeshWork(std::uint32_t start, PlatformMeshWork work)
{
    const std::uint32_t elapsed = platformProfileRenderPhaseBegin() - start;
    const int index = static_cast<int>(work);
    if (index >= 0 && index < static_cast<int>(PlatformMeshWork::Count))
        add(s_mesh[index], elapsed);
}

void platformProfileDecorWork(std::uint32_t start, const char *stage)
{
    // Stop timing before the slot search so profiling is not charged to the stage.
    const std::uint32_t elapsed = platformProfileRenderPhaseBegin() - start;
    if (stage == nullptr)
        return;
    for (int i = 0; i < s_decorCount; ++i)
    {
        if (s_decor[i].stage == stage)
        {
            add(s_decor[i].sample, elapsed);
            return;
        }
    }
    if (s_decorCount == kDecorSlots)
    {
        add(s_decorOverflow, elapsed);
        return;
    }
    DecorSample &slot = s_decor[s_decorCount++];
    slot.stage = stage;
    add(slot.sample, elapsed);
}

void platformProfilePopulationBlockWrite()
{
    ++s_populationBlockWrites;
}

void platformProfileEntityDraw(std::uint32_t start, Entity *entity)
{
    // Stop timing before resolving a type/name so profiling is not charged to it.
    const std::uint32_t elapsed = platformProfileRenderPhaseBegin() - start;
    if (entity == nullptr)
        return;
    const std::type_info &type = typeid(*entity);
    for (int i = 0; i < s_entityCount; ++i)
    {
        if (*s_entities[i].type == type)
        {
            add(s_entities[i].sample, elapsed);
            return;
        }
    }
    if (s_entityCount == kEntitySlots)
    {
        add(s_overflow, elapsed);
        return;
    }
    EntitySample &slot = s_entities[s_entityCount++];
    slot.type = &type;
    const std::string name = EntityList::getEntityString(entity);
    std::snprintf(slot.name, sizeof(slot.name), "%s",
                  name.empty() ? (entity->isPlayer() ? "Player" : type.name()) : name.c_str());
    add(slot.sample, elapsed);
}

void platformProfileEntityTickWork(std::uint32_t start, Entity *entity)
{
    // Same low-overhead type attribution as entity draw profiling, but for the
    // per-world-tick update. This stays in the level-2 profiler so ocean/frame
    // captures can identify the expensive entity class without enabling the
    // much heavier extended profiler.
    const std::uint32_t elapsed = platformProfileRenderPhaseBegin() - start;
    if (entity == nullptr)
        return;
    const std::type_info &type = typeid(*entity);
    for (int i = 0; i < s_tickEntityCount; ++i)
    {
        if (*s_tickEntities[i].type == type)
        {
            add(s_tickEntities[i].sample, elapsed);
            return;
        }
    }
    if (s_tickEntityCount == kEntitySlots)
    {
        add(s_tickOverflow, elapsed);
        return;
    }
    EntitySample &slot = s_tickEntities[s_tickEntityCount++];
    slot.type = &type;
    const std::string name = EntityList::getEntityString(entity);
    std::snprintf(slot.name, sizeof(slot.name), "%s",
                  name.empty() ? (entity->isPlayer() ? "Player" : type.name()) : name.c_str());
    add(slot.sample, elapsed);
}

void platformLogWorkProfileAndReset(int frame)
{
    for (int i = 0; i < static_cast<int>(PlatformLoadWork::Count); ++i)
    {
        report(frame, "load", kLoadNames[i], s_load[i]);
        s_load[i] = Sample();
    }

    for (int i = 0; i < static_cast<int>(PlatformMeshWork::Count); ++i)
    {
        report(frame, "meshWork", kMeshNames[i], s_mesh[i]);
        s_mesh[i] = Sample();
    }

    int order[kEntitySlots];
    for (int i = 0; i < s_entityCount; ++i)
        order[i] = i;
    std::sort(order, order + s_entityCount, [](int a, int b) {
        return s_entities[a].sample.cycles > s_entities[b].sample.cycles;
    });
    Sample other = s_overflow;
    for (int i = 0; i < s_entityCount; ++i)
    {
        EntitySample &slot = s_entities[order[i]];
        if (i < kReportedEntities)
            report(frame, "entity", slot.name, slot.sample);
        else
        {
            other.cycles += slot.sample.cycles;
            other.count += slot.sample.count;
            other.maxCycles = std::max(other.maxCycles, slot.sample.maxCycles);
        }
        slot.sample = Sample();
    }
    report(frame, "entity", "Other", other);
    s_overflow = Sample();

    int tickOrder[kEntitySlots];
    for (int i = 0; i < s_tickEntityCount; ++i)
        tickOrder[i] = i;
    std::sort(tickOrder, tickOrder + s_tickEntityCount, [](int a, int b) {
        return s_tickEntities[a].sample.cycles > s_tickEntities[b].sample.cycles;
    });
    Sample tickOther = s_tickOverflow;
    for (int i = 0; i < s_tickEntityCount; ++i)
    {
        EntitySample &slot = s_tickEntities[tickOrder[i]];
        if (i < kReportedEntities)
            report(frame, "entityTick", slot.name, slot.sample);
        else
        {
            tickOther.cycles += slot.sample.cycles;
            tickOther.count += slot.sample.count;
            tickOther.maxCycles = std::max(tickOther.maxCycles, slot.sample.maxCycles);
        }
        slot.sample = Sample();
    }
    report(frame, "entityTick", "Other", tickOther);
    s_tickOverflow = Sample();

    int decorOrder[kDecorSlots];
    for (int i = 0; i < s_decorCount; ++i)
        decorOrder[i] = i;
    std::sort(decorOrder, decorOrder + s_decorCount, [](int a, int b) {
        return s_decor[a].sample.cycles > s_decor[b].sample.cycles;
    });
    Sample decorOther = s_decorOverflow;
    for (int i = 0; i < s_decorCount; ++i)
    {
        DecorSample &slot = s_decor[decorOrder[i]];
        if (i < kReportedDecorStages)
            report(frame, "decor", slot.stage, slot.sample);
        else
        {
            decorOther.cycles += slot.sample.cycles;
            decorOther.count += slot.sample.count;
            decorOther.maxCycles = std::max(decorOther.maxCycles, slot.sample.maxCycles);
        }
        slot.sample = Sample();
    }
    report(frame, "decor", "Other", decorOther);
    s_decorOverflow = Sample();

    // Reported even at zero: a populate window that decorated nothing is worth
    // telling apart from one whose writes were not counted.
    MC_LOG_DEBUG("work", "frame=%d populationBlockWrites=%u\n", frame, s_populationBlockWrites);
    s_populationBlockWrites = 0;
}

#endif
