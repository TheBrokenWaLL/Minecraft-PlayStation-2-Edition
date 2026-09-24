#include "ps2/render/Ps2TerrainRenderer.h"
#include "ps2/render/Ps2RenderBackend.h"
#include "ps2/render/Ps2TerrainRuntime.h"
#include "ps2/render/Ps2TerrainCommands.h"
#include "ps2/render/Ps2Vu1Terrain.h"

#ifdef PS2_PLATFORM

#include "platform/Log.h"
#include "platform/PlatformTuning.h"

namespace
{
Ps2NativeTerrainPass nativePass(Ps2TerrainPass pass)
{
    return pass == PS2_TERRAIN_PASS_TRANSLUCENT
        ? PS2_NATIVE_TERRAIN_TRANSLUCENT
        : PS2_NATIVE_TERRAIN_OPAQUE;
}
}

void ps2_terrain_begin(unsigned int textureId, Ps2TerrainPass pass)
{
    Ps2TerrainRuntimeState& runtime = ps2_terrain_runtime();
    Ps2TerrainPass& s_currentPass = runtime.currentPass;
    int& s_queuedTerrainCount = runtime.queuedCount;
    bool& s_forceVu0All = runtime.forceVu0All;
    bool& s_currentVu1Retry = runtime.currentVu1Retry;
    Ps2ClusterClassification*& s_classification = runtime.classification;
    Ps2OpaqueSubmitPhase& s_opaqueSubmitPhase = runtime.opaqueSubmitPhase;
    // A previous pass should already have released Path1.  Keeping this guard
    // makes terrain setup robust against an early-returning caller and ensures
    // the texture/state work below always starts on Path3.
    ps2_render_release_path1();
#ifdef PS2_RENDER_STATS
    if (pass == PS2_TERRAIN_PASS_OPAQUE)
        ++runtime.clusterStats.opaquePasses;
#endif
    s_currentPass = pass;
    ps2_native_begin_terrain_pass(textureId, nativePass(pass));
    ps2_vu1_terrain_begin_pass();
	s_queuedTerrainCount = 0;
	ps2_terrain_commands_reset();
	s_forceVu0All = false;
	s_currentVu1Retry = false;
	// Only ps2_terrain_end()'s opaque submit phase has a legacy classification
	// slot; immediate draws must not inherit a previous section slot.
	s_classification = nullptr;
	s_opaqueSubmitPhase = pass == PS2_TERRAIN_PASS_OPAQUE
		? PS2_OPAQUE_SUBMIT_QUEUE
		: PS2_OPAQUE_SUBMIT_IMMEDIATE;
}

void ps2_terrain_end(Ps2TerrainPass pass)
{
    Ps2TerrainRuntimeState& runtime = ps2_terrain_runtime();
    Ps2QueuedTerrainSection* const s_queuedTerrain = runtime.queued;
    int& s_queuedTerrainCount = runtime.queuedCount;
    Ps2OpaqueSubmitPhase& s_opaqueSubmitPhase = runtime.opaqueSubmitPhase;
    bool& s_currentVu1Retry = runtime.currentVu1Retry;
    bool& s_forceVu0All = runtime.forceVu0All;
    Ps2ClusterClassification*& s_classification = runtime.classification;
    int& s_traceSection = runtime.traceSection;
	if (pass == PS2_TERRAIN_PASS_OPAQUE && s_queuedTerrainCount > 0)
	{
		// Opaque depth-tested terrain has no ordering dependency. Submit every
		// direct VU1 range while Path1 is owned, release it once, then run all
		// clipping-capable VU0 work through Path3. This removes the per-section
		// Path1/Path3 ping-pong without deferring mutable build data past the pass.
		MC_LOG_TRACE("terrain", "[PS2] opaque submit: %d sections, VU1 phase\n",
			s_queuedTerrainCount);
		s_opaqueSubmitPhase = PS2_OPAQUE_SUBMIT_VU1;
		ps2_terrain_submit_vu1_commands();
		for (int i = 0; i < s_queuedTerrainCount; ++i)
		{
			if (!s_queuedTerrain[i].commandReady)
			{
				// Legacy opaque sections retain the old replay path. Packed
				// command-ready sections were classified once while queued and
				// never re-enter ps2_terrain_draw_section here.
				s_traceSection = i;
				s_currentVu1Retry = false;
				s_forceVu0All = false;
				s_queuedTerrain[i].classification.valid = false;
				s_classification = &s_queuedTerrain[i].classification;
				(void)ps2_terrain_draw_section(s_queuedTerrain[i].frame,
					s_queuedTerrain[i].section, s_queuedTerrain[i].fallback);
				s_queuedTerrain[i].forceVu0All = s_currentVu1Retry;
			}
		}

		ps2_render_release_path1();
		MC_LOG_TRACE("terrain", "[PS2] opaque submit: VU0 phase\n");
		s_opaqueSubmitPhase = PS2_OPAQUE_SUBMIT_VU0;
		ps2_terrain_submit_vu0_commands();
		for (int i = 0; i < s_queuedTerrainCount; ++i)
		{
			if (!s_queuedTerrain[i].commandReady)
			{
				s_traceSection = i;
				s_forceVu0All = s_queuedTerrain[i].forceVu0All;
				s_classification = &s_queuedTerrain[i].classification;
				(void)ps2_terrain_draw_section(s_queuedTerrain[i].frame,
					s_queuedTerrain[i].section, s_queuedTerrain[i].fallback);
			}
		}
		s_classification = nullptr;
		s_traceSection = -1;

		// The pass can report itself ready and still route everything to VU0 if
		// visible clusters are near/mixed-risk or the direct path rejects them.
		// Guard-safe and side-only clusters can use VU1; sections vs vertices still
		// separates "VU1 is off" from "VU1 is on and nothing qualified".
		static bool s_submitReported = false;
		if (!s_submitReported)
		{
			s_submitReported = true;
			MC_LOG_INFO("terrain",
				"[PS2] first opaque submit: %d sections, VU1 submitted %ld vertices\n",
				s_queuedTerrainCount, ps2_vu1_terrain_submitted_vertices());
		}
	}

	s_queuedTerrainCount = 0;
	s_opaqueSubmitPhase = PS2_OPAQUE_SUBMIT_IMMEDIATE;
	s_forceVu0All = false;
	s_currentVu1Retry = false;

#if PS2_VU1_TERRAIN_CANARY
    if (pass == PS2_TERRAIN_PASS_TRANSLUCENT)
        (void)ps2_vu1_terrain_draw_canary();
#endif

    // XGKICK (Path1) must be fully drained before entities/HUD resume gsKit
    // submissions on Path3.
    ps2_render_release_path1();
    ps2_native_end_terrain_pass(nativePass(pass));
}

#endif
