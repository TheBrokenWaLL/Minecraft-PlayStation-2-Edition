// Display_ps2.cpp — PS2 implementation of lwjgl::Display.
// No SDL. Buffer swaps go through gsKit; messages are polled from the pad.
#ifdef PS2_PLATFORM

#include "platform/Log.h"
#include "platform/ExtendedProfiler.h"
#include "platform/PlatformCompat.h"
#include "lwjgl/Display.h"

#include "client/Minecraft.h"
#include "net/minecraft/src/GuiScreen.h"
#include "ps2/render/Ps2Tuning.h"
#include "ps2/render/Ps2Draw2D.h"
#include "ps2/render/Ps2Graphics.h"
#include "ps2/render/Ps2GsQueue.h"
#include "ps2/render/Ps2RenderBackend.h"
#include "ps2/input/Ps2Input.h"
#include "ps2/render/Ps2RenderPhase.h"
#include "ps2/tuning/Ps2FramePacingPolicy.h"
#include "ps2/system/Ps2Perf.h"
#include "java/System.h"

#include <gsKit.h>
#include <algorithm>
#include <tamtypes.h>
#include <stdio.h>
#include <string.h>


namespace
{
    // WorldRenderer's greedy/step build alternates opaque (0) and translucent
    // (1) passes per section — see ps2BuildPass in WorldRenderer.cpp.
    static const int kMeshPassCount = 2;

    // Tick-phase attribution.
    //
    // Both tick paths already bracket every phase they run -- six client phases
    // in Minecraft::runTick (ClientProfiler::tickPhase), four world phases in
    // World::tick, four entity sub-phases in World::updateEntities and five
    // streaming spans in Profiler_PS2.cpp. This sink kept only the single
    // slowest sample of all of them, so the FRAME line could report
    // "slowTick=entities 27.8ms" without saying what the other eighteen cost,
    // which is not enough to decide where a 25ms tick actually goes.
    //
    // Accumulate per name instead. Slots are claimed on first use and the
    // slowest sample is still reported, from the same data.
    static const int kTickPhaseSlots = 24;
    static const int kTickPhaseNameChars = 16;

    struct Ps2TickPhase
    {
        char name[kTickPhaseNameChars];
        long long sumNs;
        long long maxNs;
        int count;
    };

    struct Ps2PerfCounters
    {
        long long frameStartNs = 0;
        long long frameSumNs = 0;
        long long frameMaxNs = 0;
        int frameCount = 0;
#if MC_LOG_LEVEL > 2
        static const int kFrameSampleCapacity = 128;
        long long frameSamples[kFrameSampleCapacity] = {};
        int frameSampleCount = 0;
#endif

        long long gsSwapSumNs = 0;
        long long gsSwapMaxNs = 0;
        int gsSwapCount = 0;

        // Presentation split. gsSwap above is the whole swapBuffers() call;
        // these isolate the expensive blocking points inside it so a 15 ms
        // displayUpdate can be attributed to GS submission, the mandatory
        // vblank, or the optional extra pacing field(s).
        long long queueExecSumNs = 0;
        long long queueExecMaxNs = 0;
        int queueExecCount = 0;
        long long mandatoryVsyncSumNs = 0;
        long long mandatoryVsyncMaxNs = 0;
        int mandatoryVsyncCount = 0;
        long long pacingVsyncSumNs = 0;
        long long pacingVsyncMaxNs = 0;
        int pacingVsyncCount = 0;
        long long pacingExtraFields = 0;
        int pacingMaxExtraFields = 0;

        long long chunkBuildSumNs = 0;
        long long chunkBuildMaxNs = 0;
        int chunkBuildCount = 0;
        int chunkBuildVerts = 0;

        long long tickSumNs = 0;
        long long tickMaxNs = 0;
        int tickFrameCount = 0;
        int tickTotalCount = 0;
        int tickMaxCount = 0;

        long long renderSumNs = 0;
        long long renderMaxNs = 0;
        int renderCount = 0;

        long long displayUpdateSumNs = 0;
        long long displayUpdateMaxNs = 0;
        int displayUpdateCount = 0;

        long long lightingSumNs = 0;
        long long lightingMaxNs = 0;
        int lightingCount = 0;

        long long slowTickPhaseNs = 0;
        char slowTickPhase[32] = {};

        Ps2TickPhase tickPhases[kTickPhaseSlots] = {};
        int tickPhaseCount = 0;

		long long populateSumNs[PS2_POP_PHASE_COUNT] = {};
		long long populateMaxNs[PS2_POP_PHASE_COUNT] = {};
		int populateCount = 0;

        long long meshPassSumNs[kMeshPassCount] = {};
        long long meshPassMaxNs[kMeshPassCount] = {};
        int meshPassCount[kMeshPassCount] = {};
        int meshPassVerts[kMeshPassCount] = {};

        int snowColumnCalls = 0;
        int snowIcePlaced = 0;
        int snowPlacedCount = 0;
        long long snowNotifySum = 0;
        int snowNotifyMax = 0;

        long long chunkLoadSumNs = 0;
        long long chunkLoadMaxNs = 0;
        int chunkLoadCount = 0;

        // Distinct from populateSumNs[PS2_POP_TOTAL] above: that one times a
        // single PS2 populate task (ChunkProviderGeneratePopulateIncremental.cpp), this
        // times the per-tick drainPendingPopulate() call that can run several
        // of those tasks (ChunkProvider::unload100OldestChunks).
        long long populateDrainSumNs = 0;
        long long populateDrainMaxNs = 0;
        int populateDrainCount = 0;

        long long generateSumNs = 0;
        long long generateMaxNs = 0;
        int generateCount = 0;

        long long unloadSaveSumNs = 0;
        long long unloadSaveMaxNs = 0;
        int unloadSaveCount = 0;

        long long chunkEvictSumNs = 0;
        long long chunkEvictMaxNs = 0;
        int chunkEvictCount = 0;
    };

    static Ps2PerfCounters s_perf;

    // Render-phase attribution, in EE cycles. See Ps2RenderPhase.h.
    static unsigned long s_renderPhase[PS2_RPHASE_COUNT] = {};
    // Sum alone averages a single-frame spike away across the 120-frame
    // reporting window: an 8-way-split render can hide a multi-second outlier
    // inside one phase's per-frame average of a few tenths of a millisecond.
    // Tracked separately from the sum, per phase, so a spike like that is
    // attributable instead of merely visible in the coarser whole-render
    // avg/max pair.
    static unsigned long s_renderPhaseMax[PS2_RPHASE_COUNT] = {};
    static const char* const s_renderPhaseName[PS2_RPHASE_COUNT] =
        { "sky", "frustum", "build", "pass0", "ents", "pass1", "hand", "hud",
          "entDraw", "tileDraw", "hudItems", "hudText", "hudHints" };
}

extern "C" void ps2_perf_add_render_phase(int phase, unsigned int cycles)
{
    if (phase >= 0 && phase < PS2_RPHASE_COUNT)
    {
        s_renderPhase[phase] += cycles;
        if (cycles > s_renderPhaseMax[phase])
            s_renderPhaseMax[phase] = cycles;
    }
}

namespace
{

    static void ps2_perf_add_sample(long long ns, long long& sum, long long& max, int& count)
    {
        if (ns < 0)
            ns = 0;
        sum += ns;
        if (ns > max)
            max = ns;
        count++;
    }

    static float ps2_perf_ms(long long ns)
    {
        return (float)((double)ns / 1000000.0);
    }

    static float ps2_perf_avg_ms(long long sum, int count)
    {
        return count > 0 ? ps2_perf_ms(sum / count) : 0.0f;
    }

    static int ps2_perf_pct_of_60(float ms)
    {
        int pct = (int)((ms * 100.0f / 16.6667f) + 0.5f);
        if (pct < 0) pct = 0;
        return pct;
    }
}

extern "C" void ps2_perf_frame_begin()
{
    s_perf.frameStartNs = System::nanoTime();
}

extern "C" void ps2_perf_frame_end()
{
    if (s_perf.frameStartNs == 0)
        return;
    long long now = System::nanoTime();
    const long long frameNs = now - s_perf.frameStartNs;
    ps2_perf_add_sample(frameNs,
                         s_perf.frameSumNs, s_perf.frameMaxNs, s_perf.frameCount);
#if MC_LOG_LEVEL > 2
    if (s_perf.frameSampleCount < Ps2PerfCounters::kFrameSampleCapacity)
        s_perf.frameSamples[s_perf.frameSampleCount++] = frameNs < 0 ? 0 : frameNs;
#endif
    s_perf.frameStartNs = 0;
}

extern "C" void ps2_perf_add_chunk_build_ns(long long ns, int vertices)
{
    ps2_perf_add_sample(ns, s_perf.chunkBuildSumNs, s_perf.chunkBuildMaxNs, s_perf.chunkBuildCount);
    if (vertices > 0)
        s_perf.chunkBuildVerts += vertices;
}

extern "C" void ps2_perf_add_tick_ns(long long ns, int ticks)
{
    ps2_perf_add_sample(ns, s_perf.tickSumNs, s_perf.tickMaxNs, s_perf.tickFrameCount);
    if (ticks > 0)
        s_perf.tickTotalCount += ticks;
    if (ticks > s_perf.tickMaxCount)
        s_perf.tickMaxCount = ticks;
}

extern "C" void ps2_perf_add_render_ns(long long ns)
{
    ps2_perf_add_sample(ns, s_perf.renderSumNs, s_perf.renderMaxNs, s_perf.renderCount);
}

extern "C" void ps2_perf_add_display_update_ns(long long ns)
{
    ps2_perf_add_sample(ns, s_perf.displayUpdateSumNs, s_perf.displayUpdateMaxNs, s_perf.displayUpdateCount);
}

extern "C" void ps2_perf_add_lighting_ns(long long ns)
{
    ps2_perf_add_sample(ns, s_perf.lightingSumNs, s_perf.lightingMaxNs, s_perf.lightingCount);
}

extern "C" void ps2_perf_add_populate_phase(int phase, long long ns)
{
	if (phase < 0 || phase >= PS2_POP_PHASE_COUNT)
		return;
	if (ns < 0)
		ns = 0;
	s_perf.populateSumNs[phase] += ns;
	if (ns > s_perf.populateMaxNs[phase])
		s_perf.populateMaxNs[phase] = ns;
	if (phase == PS2_POP_TOTAL)
		s_perf.populateCount++;
}

extern "C" void ps2_perf_add_mesh_pass_ns(int pass, long long ns, int vertices)
{
    if (pass < 0 || pass >= kMeshPassCount)
        return;
    ps2_perf_add_sample(ns, s_perf.meshPassSumNs[pass], s_perf.meshPassMaxNs[pass],
                         s_perf.meshPassCount[pass]);
    if (vertices > 0)
        s_perf.meshPassVerts[pass] += vertices;
}

extern "C" void ps2_perf_add_snow_column(int icePlaced, int snowPlaced, int notifyCalls)
{
    s_perf.snowColumnCalls++;
    if (icePlaced)
        s_perf.snowIcePlaced++;
    if (snowPlaced)
        s_perf.snowPlacedCount++;
    if (notifyCalls > 0)
        s_perf.snowNotifySum += notifyCalls;
    if (notifyCalls > s_perf.snowNotifyMax)
        s_perf.snowNotifyMax = notifyCalls;
}

extern "C" void ps2_perf_add_chunk_load_ns(long long ns)
{
    ps2_perf_add_sample(ns, s_perf.chunkLoadSumNs, s_perf.chunkLoadMaxNs, s_perf.chunkLoadCount);
}

extern "C" void ps2_perf_add_populate_ns(long long ns)
{
    ps2_perf_add_sample(ns, s_perf.populateDrainSumNs, s_perf.populateDrainMaxNs,
                         s_perf.populateDrainCount);
}

extern "C" void ps2_perf_add_generate_ns(long long ns)
{
    ps2_perf_add_sample(ns, s_perf.generateSumNs, s_perf.generateMaxNs, s_perf.generateCount);
}

extern "C" void ps2_perf_add_unload_save_ns(long long ns)
{
    ps2_perf_add_sample(ns, s_perf.unloadSaveSumNs, s_perf.unloadSaveMaxNs, s_perf.unloadSaveCount);
}

extern "C" void ps2_perf_add_chunk_evict_ns(long long ns)
{
    ps2_perf_add_sample(ns, s_perf.chunkEvictSumNs, s_perf.chunkEvictMaxNs, s_perf.chunkEvictCount);
}

extern "C" void ps2_perf_note_tick_phase(const char* name, long long ns)
{
    if (name == nullptr)
        return;
    if (ns < 0)
        ns = 0;

    if (ns > s_perf.slowTickPhaseNs)
    {
        s_perf.slowTickPhaseNs = ns;
        strncpy(s_perf.slowTickPhase, name, sizeof(s_perf.slowTickPhase) - 1);
        s_perf.slowTickPhase[sizeof(s_perf.slowTickPhase) - 1] = '\0';
    }

    // Linear scan over at most kTickPhaseSlots entries, once per bracketed
    // phase per tick. Names are string literals, but compare the text rather
    // than the pointer: the same phase name is written at more than one call
    // site and nothing guarantees the linker pools those literals.
    for (int i = 0; i < s_perf.tickPhaseCount; i++)
    {
        Ps2TickPhase& slot = s_perf.tickPhases[i];
        if (strncmp(slot.name, name, kTickPhaseNameChars - 1) != 0)
            continue;
        slot.sumNs += ns;
        if (ns > slot.maxNs)
            slot.maxNs = ns;
        ++slot.count;
        return;
    }

    // Out of slots: the sample is dropped from the breakdown but still counts
    // toward the slowest-phase report above, so a new phase never goes
    // completely unreported.
    if (s_perf.tickPhaseCount >= kTickPhaseSlots)
        return;

    Ps2TickPhase& slot = s_perf.tickPhases[s_perf.tickPhaseCount++];
    strncpy(slot.name, name, kTickPhaseNameChars - 1);
    slot.name[kTickPhaseNameChars - 1] = '\0';
    slot.sumNs = ns;
    slot.maxNs = ns;
    slot.count = 1;
}

extern "C" void ps2_perf_format_and_reset(char* out, int outSize)
{
    if (out == nullptr || outSize <= 0)
        return;

    const float frameAvg = ps2_perf_avg_ms(s_perf.frameSumNs, s_perf.frameCount);
    const float frameMax = ps2_perf_ms(s_perf.frameMaxNs);
    const float gsAvg = ps2_perf_avg_ms(s_perf.gsSwapSumNs, s_perf.gsSwapCount);
    const float gsMax = ps2_perf_ms(s_perf.gsSwapMaxNs);
    const float queueExecAvg = ps2_perf_avg_ms(s_perf.queueExecSumNs, s_perf.queueExecCount);
    const float queueExecMax = ps2_perf_ms(s_perf.queueExecMaxNs);
    const float mandatoryVsyncAvg = ps2_perf_avg_ms(s_perf.mandatoryVsyncSumNs, s_perf.mandatoryVsyncCount);
    const float mandatoryVsyncMax = ps2_perf_ms(s_perf.mandatoryVsyncMaxNs);
    const float pacingVsyncAvg = ps2_perf_avg_ms(s_perf.pacingVsyncSumNs, s_perf.pacingVsyncCount);
    const float pacingVsyncMax = ps2_perf_ms(s_perf.pacingVsyncMaxNs);
    const float chunkAvg = ps2_perf_avg_ms(s_perf.chunkBuildSumNs, s_perf.chunkBuildCount);
    const float chunkMax = ps2_perf_ms(s_perf.chunkBuildMaxNs);
    const float tickAvg = ps2_perf_avg_ms(s_perf.tickSumNs, s_perf.tickFrameCount);
    const float tickMax = ps2_perf_ms(s_perf.tickMaxNs);
    const float renderAvg = ps2_perf_avg_ms(s_perf.renderSumNs, s_perf.renderCount);
    const float renderMax = ps2_perf_ms(s_perf.renderMaxNs);
    const float displayAvg = ps2_perf_avg_ms(s_perf.displayUpdateSumNs, s_perf.displayUpdateCount);
    const float displayMax = ps2_perf_ms(s_perf.displayUpdateMaxNs);
    const float lightAvg = ps2_perf_avg_ms(s_perf.lightingSumNs, s_perf.lightingCount);
    const float lightMax = ps2_perf_ms(s_perf.lightingMaxNs);
    const float slowPhaseMs = ps2_perf_ms(s_perf.slowTickPhaseNs);
    const int framePct = ps2_perf_pct_of_60(frameAvg);
    const int gsPct = ps2_perf_pct_of_60(gsAvg);

    snprintf(out, outSize,
             "EE frame avg=%.1fms max=%.1fms ~%d%%60 | tick avg=%.1fms max=%.1fms ticks=%d maxTicks=%d slowTick=%s %.1fms | render avg=%.1fms max=%.1fms | display avg=%.1fms max=%.1fms | light avg=%.1fms max=%.1fms | GS wait avg=%.1fms max=%.1fms ~%d%%60 | chunk build n=%d avg=%.1fms max=%.1fms verts=%d",
             frameAvg, frameMax, framePct,
             tickAvg, tickMax, s_perf.tickTotalCount, s_perf.tickMaxCount,
             s_perf.slowTickPhase[0] ? s_perf.slowTickPhase : "none", slowPhaseMs,
             renderAvg, renderMax,
             displayAvg, displayMax,
             lightAvg, lightMax,
             gsAvg, gsMax, gsPct,
             s_perf.chunkBuildCount, chunkAvg, chunkMax, s_perf.chunkBuildVerts);

#if MC_LOG_LEVEL >= 2
    {
        const float avgExtraFields = s_perf.pacingVsyncCount > 0
            ? static_cast<float>(s_perf.pacingExtraFields) / static_cast<float>(s_perf.pacingVsyncCount)
            : 0.0f;
        size_t len = strlen(out);
        if (len < (size_t)outSize)
        {
            snprintf(out + len, (size_t)outSize - len,
                " | present exec=%.1f/max%.1f vblank=%.1f/max%.1f pace=%.1f/max%.1f fields=%.2f/max%d",
                queueExecAvg, queueExecMax, mandatoryVsyncAvg, mandatoryVsyncMax,
                pacingVsyncAvg, pacingVsyncMax, avgExtraFields, s_perf.pacingMaxExtraFields);
        }
    }
#endif

#if MC_LOG_LEVEL > 2
    if (s_perf.frameSampleCount > 0)
    {
        long long sorted[Ps2PerfCounters::kFrameSampleCapacity] = {};
        std::copy(s_perf.frameSamples, s_perf.frameSamples + s_perf.frameSampleCount, sorted);
        std::sort(sorted, sorted + s_perf.frameSampleCount);
        auto percentileNs = [&](int pct) -> long long
        {
            int index = (s_perf.frameSampleCount * pct + 99) / 100 - 1;
            if (index < 0) index = 0;
            if (index >= s_perf.frameSampleCount) index = s_perf.frameSampleCount - 1;
            return sorted[index];
        };
        size_t len = strlen(out);
        if (len < (size_t)outSize)
        {
            snprintf(out + len, (size_t)outSize - len,
                " | frame pct p50=%.1f p90=%.1f p95=%.1f p99=%.1f",
                ps2_perf_ms(percentileNs(50)), ps2_perf_ms(percentileNs(90)),
                ps2_perf_ms(percentileNs(95)), ps2_perf_ms(percentileNs(99)));
        }
    }

    if (frameAvg > 0.0f)
    {
        const float accounted = tickAvg + renderAvg + displayAvg + lightAvg;
        const float otherAvg = accounted < frameAvg ? frameAvg - accounted : 0.0f;
        const float invFrame = 100.0f / frameAvg;
        size_t len = strlen(out);
        if (len < (size_t)outSize)
        {
            snprintf(out + len, (size_t)outSize - len,
                " | budget tick=%.0f%% render=%.0f%% display=%.0f%% light=%.0f%% other=%.0f%%",
                tickAvg * invFrame, renderAvg * invFrame, displayAvg * invFrame,
                lightAvg * invFrame, otherAvg * invFrame);
        }
    }
#endif

    // Per-phase breakdown of that render figure, appended to the same line.
    // These are per-frame averages so they read directly against "render avg";
    // what they do not account for is the rest of updateCameraAndRender (camera
    // setup, fog, particles, the held item and the HUD).
    {
        const int frames = s_perf.frameCount > 0 ? s_perf.frameCount : 1;
        size_t len = strlen(out);
        if (len < (size_t)outSize)
        {
            int n = snprintf(out + len, (size_t)outSize - len, " | render phases");
            len += (n > 0) ? (size_t)n : 0;
        }
        for (int p = 0; p < PS2_RPHASE_COUNT && len < (size_t)outSize; p++)
        {
            const double ms = (double)s_renderPhase[p] / 294000.0 / (double)frames;
            const double maxMs = (double)s_renderPhaseMax[p] / 294000.0;
            int n = snprintf(out + len, (size_t)outSize - len, " %s=%.1f/max%.1f",
                              s_renderPhaseName[p], ms, maxMs);
            len += (n > 0) ? (size_t)n : 0;
        }
        for (int p = 0; p < PS2_RPHASE_COUNT; p++)
        {
            s_renderPhase[p] = 0;
            s_renderPhaseMax[p] = 0;
        }
    }

    // Per-phase breakdown of that tick figure. Averaged over frames rather than
    // over each phase's own invocation count, on purpose: the render phases
    // above use the same denominator, so both breakdowns read directly against
    // "EE frame avg" and against each other. A phase that runs once every few
    // seconds (chunkLoad) therefore shows a small average and a large max,
    // which is exactly the distinction that matters for a hitch.
    if (s_perf.tickPhaseCount > 0)
    {
        const int frames = s_perf.frameCount > 0 ? s_perf.frameCount : 1;
        size_t len = strlen(out);
        if (len < (size_t)outSize)
        {
            int n = snprintf(out + len, (size_t)outSize - len, " | tick phases");
            len += (n > 0) ? (size_t)n : 0;
        }
        for (int p = 0; p < s_perf.tickPhaseCount && len < (size_t)outSize; p++)
        {
            const Ps2TickPhase& slot = s_perf.tickPhases[p];
            // The invocation count is carried here and not in the render phase
            // list above because these rates differ: a render phase runs once
            // per frame, while a tick phase may run every tick, once every few
            // hundred, or not at all. Without it a one-shot 12ms chunkLoad and
            // a steady 0.1ms phase can print the same per-frame average.
            int n = snprintf(out + len, (size_t)outSize - len, " %s=%.1f/max%.1f/n%d",
                              slot.name,
                              ps2_perf_ms(slot.sumNs) / (float)frames,
                              ps2_perf_ms(slot.maxNs),
                              slot.count);
            len += (n > 0) ? (size_t)n : 0;
        }
    }

	// Population runs synchronously inside the phase historically named
	// "chunkUnload". Report total average/max plus each feature group's maximum;
	// maxima are what explain the visible one-frame hitch.
	if (s_perf.populateCount > 0)
	{
		const float totalAvg = ps2_perf_avg_ms(
			s_perf.populateSumNs[PS2_POP_TOTAL], s_perf.populateCount);
		const float totalMax = ps2_perf_ms(s_perf.populateMaxNs[PS2_POP_TOTAL]);
		size_t len = strlen(out);
		if (len < (size_t)outSize)
		{
			snprintf(out + len, (size_t)outSize - len,
				" | pop n=%d avg/max=%.1f/%.1f max struct=%.1f lake=%.1f dung=%.1f fill=%.1f ore=%.1f deco=%.1f spring=%.1f snow=%.1f",
				s_perf.populateCount, totalAvg, totalMax,
				ps2_perf_ms(s_perf.populateMaxNs[PS2_POP_STRUCTURES]),
				ps2_perf_ms(s_perf.populateMaxNs[PS2_POP_LAKES]),
				ps2_perf_ms(s_perf.populateMaxNs[PS2_POP_DUNGEONS]),
				ps2_perf_ms(s_perf.populateMaxNs[PS2_POP_FILLERS]),
				ps2_perf_ms(s_perf.populateMaxNs[PS2_POP_ORES]),
				ps2_perf_ms(s_perf.populateMaxNs[PS2_POP_DECORATION]),
				ps2_perf_ms(s_perf.populateMaxNs[PS2_POP_SPRINGS]),
				ps2_perf_ms(s_perf.populateMaxNs[PS2_POP_SNOW]));
		}
	}

    // Streaming chunk lifecycle: load/generate/populate-drain/unload-save/evict
    // are each a single named span (unlike populate's per-feature breakdown
    // above), so one compact "n/avg/max" triplet per metric is enough.
    {
        size_t len = strlen(out);
        if (s_perf.chunkLoadCount > 0 && len < (size_t)outSize)
        {
            int n = snprintf(out + len, (size_t)outSize - len,
                " | chunkLoad n=%d avg=%.1fms max=%.1fms",
                s_perf.chunkLoadCount,
                ps2_perf_avg_ms(s_perf.chunkLoadSumNs, s_perf.chunkLoadCount),
                ps2_perf_ms(s_perf.chunkLoadMaxNs));
            len += (n > 0) ? (size_t)n : 0;
        }
        if (s_perf.generateCount > 0 && len < (size_t)outSize)
        {
            int n = snprintf(out + len, (size_t)outSize - len,
                " | generate n=%d avg=%.1fms max=%.1fms",
                s_perf.generateCount,
                ps2_perf_avg_ms(s_perf.generateSumNs, s_perf.generateCount),
                ps2_perf_ms(s_perf.generateMaxNs));
            len += (n > 0) ? (size_t)n : 0;
        }
        if (s_perf.populateDrainCount > 0 && len < (size_t)outSize)
        {
            int n = snprintf(out + len, (size_t)outSize - len,
                " | populateDrain n=%d avg=%.1fms max=%.1fms",
                s_perf.populateDrainCount,
                ps2_perf_avg_ms(s_perf.populateDrainSumNs, s_perf.populateDrainCount),
                ps2_perf_ms(s_perf.populateDrainMaxNs));
            len += (n > 0) ? (size_t)n : 0;
        }
        if (s_perf.unloadSaveCount > 0 && len < (size_t)outSize)
        {
            int n = snprintf(out + len, (size_t)outSize - len,
                " | unloadSave n=%d avg=%.1fms max=%.1fms",
                s_perf.unloadSaveCount,
                ps2_perf_avg_ms(s_perf.unloadSaveSumNs, s_perf.unloadSaveCount),
                ps2_perf_ms(s_perf.unloadSaveMaxNs));
            len += (n > 0) ? (size_t)n : 0;
        }
        if (s_perf.chunkEvictCount > 0 && len < (size_t)outSize)
        {
            int n = snprintf(out + len, (size_t)outSize - len,
                " | chunkEvict n=%d avg=%.1fms max=%.1fms",
                s_perf.chunkEvictCount,
                ps2_perf_avg_ms(s_perf.chunkEvictSumNs, s_perf.chunkEvictCount),
                ps2_perf_ms(s_perf.chunkEvictMaxNs));
            len += (n > 0) ? (size_t)n : 0;
        }
    }

    // Per-pass world mesh build cost (PS2_RENDER_STATS builds only feed this;
    // see the platformProfileChunkMeshPass call sites in WorldRenderer.cpp).
    {
        bool any = false;
        for (int p = 0; p < kMeshPassCount; p++)
            any = any || s_perf.meshPassCount[p] > 0;
        if (any)
        {
            size_t len = strlen(out);
            if (len < (size_t)outSize)
            {
                int n = snprintf(out + len, (size_t)outSize - len, " | mesh pass");
                len += (n > 0) ? (size_t)n : 0;
            }
            for (int p = 0; p < kMeshPassCount && len < (size_t)outSize; p++)
            {
                if (s_perf.meshPassCount[p] <= 0)
                    continue;
                int n = snprintf(out + len, (size_t)outSize - len,
                    " %d=avg%.1f/max%.1f/v%d", p,
                    ps2_perf_avg_ms(s_perf.meshPassSumNs[p], s_perf.meshPassCount[p]),
                    ps2_perf_ms(s_perf.meshPassMaxNs[p]), s_perf.meshPassVerts[p]);
                len += (n > 0) ? (size_t)n : 0;
            }
        }
    }

    if (s_perf.snowColumnCalls > 0)
    {
        const float notifyAvg = (float)s_perf.snowNotifySum / (float)s_perf.snowColumnCalls;
        size_t len = strlen(out);
        if (len < (size_t)outSize)
        {
            snprintf(out + len, (size_t)outSize - len,
                " | snow n=%d ice=%d snow=%d notify avg=%.1f max=%d",
                s_perf.snowColumnCalls, s_perf.snowIcePlaced, s_perf.snowPlacedCount,
                notifyAvg, s_perf.snowNotifyMax);
        }
    }

    s_perf = Ps2PerfCounters();
}

namespace lwjgl
{
namespace Display
{

static DisplayMode s_mode(640, 448);
static bool s_closeRequested = false;

void clearStartupFramebuffers()
{
    GSGLOBAL* graphics = Ps2Graphics::context();
    if (!graphics)
        return;

    for (int buffer = 0; buffer < 2; ++buffer)
    {
        gsKit_clear(graphics, GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0));
        gsKit_queue_exec(graphics);
        gsKit_sync_flip(graphics);
        gsKit_queue_reset(graphics->Os_Queue);
    }

    ps2_render_invalidate_framebuffer_state();
}

void setDisplayMode(const DisplayMode& dm)
{
    // On PS2 the GS resolution is fixed at init; ignore what Minecraft requests
    // and report back the actual screen size so resize() converges immediately.
    (void)dm;
}

DisplayMode getDisplayMode() { return s_mode; }

void setTitle(const jstring&) {}
void setFullscreen(bool) {}
bool isCloseRequested() { return s_closeRequested; }
bool isVisible()        { return true; }
bool isActive()         { return true; }
void create()
{
    // GS is already up from main_ps2.cpp. Just sync the display mode.
    if (Ps2Graphics::context())
    {
        s_mode = DisplayMode(Ps2Graphics::width(), Ps2Graphics::height());
        clearStartupFramebuffers();
    }
}

int_t getX()      { return 0; }
int_t getY()      { return 0; }
int_t getWidth()  { return s_mode.getWidth(); }
int_t getHeight() { return s_mode.getHeight(); }

void processMessages()
{
    Minecraft* mc = Minecraft::getMinecraft();
    bool inMenu = (mc && mc->currentScreen != nullptr);
    const bool specializedMenuNavigation = inMenu &&
        mc->currentScreen->usesSpecializedMenuNavigationForPlatform();

    // Ensure the game captures mouse (camera) whenever no menu is open.
    if (!inMenu && mc && !mc->inGameHasFocus)
        mc->setIngameFocus();

    Ps2Input::poll(inMenu, specializedMenuNavigation);
}

void swapBuffers()
{
    if (!gsGlobal) return;
    long long startNs = System::nanoTime();

    // Breadcrumbs, not statistics. Both calls below can block forever on a GIF
    // that another engine still owns -- gsKit_queue_exec drives Path3 and
    // gsKit_vsync_wait/sync_flip waits on the video field -- and neither prints
    // anything of its own. With sync writes on (McLog::setSyncWrites) the last
    // line in debug.log then names which one the console stopped inside, which
    // is not otherwise recoverable from a frozen picture.
    static long s_frame = 0;
    ++s_frame;

    // Before the exec, while pool_cur still holds what this frame wrote.
    //
    // gsKit does not bounds-check the queue (see ps2_gs_queue_guard in
    // Ps2GsQueue.cpp), and the per-draw guards only cover the sites that
    // call them. An overrun corrupts the C++ heap behind the pool and is felt
    // much later, as an allocation that never returns -- so it has to be caught
    // here, once per frame, whatever draw produced it. This runs in the menu
    // too, which the periodic FRAME line does not: that one is gated on a World
    // existing, and the frames before a world is created are exactly the ones
    // nothing was watching.
    // Last barrier of the frame. The HUD is the final thing drawn, so
    // without this whatever the 2D batch still holds would never reach the
    // GS at all.
    ps2_draw_2d_flush_pending();

    ps2_gs_queue_check_overflow();
#if MC_LOG_LEVEL > 2
    platformProfileGsQueue(ps2_gs_queue_used_bytes(), ps2_gs_queue_capacity_bytes());
#endif
    ps2_gs_queue_report(s_frame);

    MC_LOG_TRACE("frame", "[PS2] frame %ld: gs queue exec\n", s_frame);
#if MC_LOG_LEVEL >= 2
    const std::uint64_t queueExecStartUs = PlatformCompat::getMonotonicMicros();
#endif
    gsKit_queue_exec(gsGlobal);
#if MC_LOG_LEVEL >= 2
    const std::uint64_t queueExecEndUs = PlatformCompat::getMonotonicMicros();
    ps2_perf_add_sample(static_cast<long long>(queueExecEndUs - queueExecStartUs) * 1000LL,
                        s_perf.queueExecSumNs, s_perf.queueExecMaxNs, s_perf.queueExecCount);
#endif
    const int activeBeforeFlip = (int)gsGlobal->ActiveBuffer;
    const int contextBeforeFlip = (int)gsGlobal->PrimContext;
    const int firstBeforeFlip = (int)gsGlobal->FirstFrame;
    MC_LOG_TRACE("frame", "[PS2] frame %ld: present active=%d ctx=%d first=%d\n",
                 s_frame, activeBeforeFlip, contextBeforeFlip, firstBeforeFlip);

#if PS2_TARGET_FPS > 0
    // Frame pacing uses the EE hardware clock rather than System::nanoTime().
    // The latter can resolve to a dead newlib timer on PS2; when that happened
    // the old fallback paid every configured field even if the frame had already
    // overrun its budget. The hardware clock is monotonic and already backs the
    // renderer's per-frame budgets.
    static int s_fieldsPerFrame = 0;
    if (s_fieldsPerFrame == 0)
    {
        const int fieldHz = (gsGlobal->Mode == GS_MODE_PAL) ? 50 : 60;
        s_fieldsPerFrame = Ps2FramePacingPolicy::fieldsPerFrame(fieldHz, PS2_TARGET_FPS);
    }

    static std::uint64_t s_lastPresentUs = 0;
    const std::uint64_t periodUs = Ps2FramePacingPolicy::targetPeriodUs(PS2_TARGET_FPS);

    // The first field is not optional. DISPFB2 is not latched at vblank, so the
    // flip below lands on whatever raster line the CRTC is scanning: presenting
    // without a preceding vsync tears the picture across that line, and leaves
    // the GS still rasterizing the buffer that just became visible. Only the
    // extra fields -- the ones that pace the field rate down to the target --
    // are dropped when the frame overran its budget, so a heavy frame presents
    // at the next vblank instead of halving the rate.
#if MC_LOG_LEVEL >= 2
    const std::uint64_t mandatoryVsyncStartUs = PlatformCompat::getMonotonicMicros();
#endif
    gsKit_vsync_wait();
#if MC_LOG_LEVEL >= 2
    const std::uint64_t mandatoryVsyncEndUs = PlatformCompat::getMonotonicMicros();
    ps2_perf_add_sample(static_cast<long long>(mandatoryVsyncEndUs - mandatoryVsyncStartUs) * 1000LL,
                        s_perf.mandatoryVsyncSumNs, s_perf.mandatoryVsyncMaxNs, s_perf.mandatoryVsyncCount);
    long long pacingWaitNs = 0;
    int extraFieldsWaited = 0;
#endif
    for (int field = 1; field < s_fieldsPerFrame; ++field)
    {
        const std::uint64_t nowUs = PlatformCompat::getMonotonicMicros();
        if (!Ps2FramePacingPolicy::shouldWaitForTarget(s_lastPresentUs, nowUs, periodUs))
            break;
#if MC_LOG_LEVEL >= 2
        const std::uint64_t pacingVsyncStartUs = PlatformCompat::getMonotonicMicros();
#endif
        gsKit_vsync_wait();
#if MC_LOG_LEVEL >= 2
        const std::uint64_t pacingVsyncEndUs = PlatformCompat::getMonotonicMicros();
        pacingWaitNs += static_cast<long long>(pacingVsyncEndUs - pacingVsyncStartUs) * 1000LL;
        ++extraFieldsWaited;
#endif
    }
#if MC_LOG_LEVEL >= 2
    ps2_perf_add_sample(pacingWaitNs,
                        s_perf.pacingVsyncSumNs, s_perf.pacingVsyncMaxNs, s_perf.pacingVsyncCount);
    s_perf.pacingExtraFields += extraFieldsWaited;
    if (extraFieldsWaited > s_perf.pacingMaxExtraFields)
        s_perf.pacingMaxExtraFields = extraFieldsWaited;
#endif

    // gsKit_sync_flip is not "wait then setactive": its own source (ee/gs/src/
    // gsCore.c) does the actual display flip itself, inline, before delegating
    // to setactive:
    //     GS_SET_DISPFB2(ScreenBuffer[ActiveBuffer & 1], ...);
    //     ActiveBuffer ^= 1;
    //     gsKit_setactive(gsGlobal);
    // setactive() alone only reprograms the DRAW-target registers (FRAME_1/2)
    // to whatever ActiveBuffer already is -- it never touches DISPFB2 (which
    // buffer the CRTC scans out) and never toggles ActiveBuffer. Calling only
    // setactive() after the pacing waits, as this branch used to, left the
    // display showing the same buffer forever while the game kept drawing into
    // it: confirmed on PCSX2 as a frame frozen right after the boot screen,
    // with the "gsKit flip invariant failed: active 0->0" warning below firing
    // on the very first check. Vsync/host-refresh-rate settings do not affect
    // this -- it is a missing register write, not a timing issue. Replicate
    // sync_flip's own flip step here, guarded the same way it guards it.
    if (!firstBeforeFlip && gsGlobal->DoubleBuffering == GS_SETTING_ON)
    {
        GS_SET_DISPFB2(gsGlobal->ScreenBuffer[gsGlobal->ActiveBuffer & 1] / 8192,
                       gsGlobal->Width / 64, gsGlobal->PSM, 0, 0);
        gsGlobal->ActiveBuffer ^= 1;
    }
    gsKit_setactive(gsGlobal);
    s_lastPresentUs = PlatformCompat::getMonotonicMicros();
#else
    gsKit_sync_flip(gsGlobal);
#endif

    if (!firstBeforeFlip && gsGlobal->DoubleBuffering == GS_SETTING_ON)
    {
        static bool warnedBadFlip = false;
        const bool activeDidNotFlip = (int)gsGlobal->ActiveBuffer == activeBeforeFlip;
        if (!warnedBadFlip && activeDidNotFlip)
        {
            warnedBadFlip = true;
            MC_LOG_WARN("render",
                "[PS2] gsKit flip invariant failed: active %d->%d\n",
                activeBeforeFlip, (int)gsGlobal->ActiveBuffer);
        }
    }

    ps2_render_invalidate_framebuffer_state();
    gsKit_queue_reset(gsGlobal->Os_Queue);
    MC_LOG_TRACE("frame", "[PS2] frame %ld: presented active=%d ctx=%d first=%d\n",
                 s_frame, (int)gsGlobal->ActiveBuffer, (int)gsGlobal->PrimContext,
                 (int)gsGlobal->FirstFrame);
    long long endNs = System::nanoTime();
    ps2_perf_add_sample(endNs - startNs,
                         s_perf.gsSwapSumNs, s_perf.gsSwapMaxNs, s_perf.gsSwapCount);
}

void update(bool doProcessMessages)
{
    swapBuffers();
    if (doProcessMessages)
        processMessages();
}

} // namespace Display
} // namespace lwjgl

#endif // PS2_PLATFORM
