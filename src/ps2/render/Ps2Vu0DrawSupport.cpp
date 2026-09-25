#include "ps2/render/Ps2Vu0DrawSupport.h"

#ifdef PS2_PLATFORM

#include <cstring>

#include <gsInline.h>

#include "ps2/render/Ps2GsQueue.h"
#include "ps2/render/Ps2RenderStats.h"
#include "ps2/render/Ps2RenderBackend.h"
#include "ps2/system/Ps2Scratchpad.h"

namespace
{
int log2u(unsigned int value)
{
    int result = 0;
    while ((1u << result) < value)
        ++result;
    return result;
}
}

void ps2_vu0_queue_guard(GSGLOBAL* gs, int vertexCount)
{
    if (!gs || !gs->Os_Queue)
        return;

    GSQUEUE* queue = gs->Os_Queue;
    if (gs->CurQueue != queue || !queue->pool_cur || !queue->pool_max[queue->dbuf])
        return;

    const unsigned int needed = 8192u + (unsigned int)vertexCount * 128u;
    const unsigned char* cursor = (const unsigned char*)queue->pool_cur;
    const unsigned char* limit = (const unsigned char*)queue->pool_max[queue->dbuf];
    if (cursor > limit)
        ps2_gs_queue_note_overflow((long)(cursor - limit));
    if (cursor < limit && (unsigned int)(limit - cursor) > needed)
        return;
    PS2_VU0_CYC_BEGIN(queueFlushStart);
    ps2_gs_queue_flush_oneshot();
    PS2_VU0_CYC_END(queueFlushStart, ps2_render_stats().cycleVu0QueueFlush);
    PS2_RENDER_STAT(++ps2_render_stats().vu0QueueFlushes);
}

VU_MATRIX ps2_vu0_mvp_to_vu(const float* matrix)
{
    VU_MATRIX out;
    __builtin_memcpy(&out, matrix, 16 * sizeof(float));
    return out;
}

void ps2_vu0_project(const Ps2DepthMap& depth,
                     bool forceNearZ,
                     float cx,
                     float cy,
                     float cz,
                     float cw,
                     float halfWidth,
                     float halfHeight,
                     Ps2ProjVert& out)
{
    const float q = 1.0f / cw;
    out.q = q;
    out.w = cw;
    out.x = (cx * q + 1.0f) * halfWidth;
    out.y = (-cy * q + 1.0f) * halfHeight;
    if (forceNearZ)
    {
        out.z = PS2_GS_Z_MAX;
        return;
    }
    out.z = ps2_gs_depth(depth, cz, cw, q, (float)PS2_GS_Z_MAX);
}

bool ps2_vu0_cull(bool cullFace,
                  bool ccw,
                  bool back,
                  float x0,
                  float y0,
                  float x1,
                  float y1,
                  float x2,
                  float y2)
{
    if (!cullFace)
        return false;

    const float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (area > -0.0001f && area < 0.0001f)
        return false;
    const bool front = ccw ? (area < 0.0f) : (area > 0.0f);
    return back ? !front : front;
}

u64 ps2_vu0_clamp_reg(const Ps2ClampSel& selection)
{
    switch (selection.mode)
    {
    case PS2_CLAMPSEL_REPEAT:
        return GS_SETREG_CLAMP(GS_CMODE_REPEAT, GS_CMODE_REPEAT, 0, 0, 0, 0);
    case PS2_CLAMPSEL_REGION:
        return GS_SETREG_CLAMP(GS_CMODE_REGION_REPEAT, GS_CMODE_REGION_REPEAT,
                               15, selection.ufix, 15, selection.vfix);
    default:
        return GS_SETREG_CLAMP(GS_CMODE_CLAMP, GS_CMODE_CLAMP, 0, 0, 0, 0);
    }
}

u64 ps2_vu0_tex0(const GSGLOBAL* gs, const GSTEXTURE* texture)
{
    const int tw = log2u((unsigned int)texture->Width);
    const int th = log2u((unsigned int)texture->Height);
    if (texture->VramClut == 0)
    {
        return GS_SETREG_TEX0(texture->Vram / 256, texture->TBW, texture->PSM,
                              tw, th, gs->PrimAlphaEnable, 0,
                              0, 0, 0, 0, 0);
    }
    return GS_SETREG_TEX0(texture->Vram / 256, texture->TBW, texture->PSM,
                          tw, th, gs->PrimAlphaEnable, 0,
                          texture->VramClut / 256, texture->ClutPSM,
                          texture->ClutStorageMode, 0, 1);
}

namespace
{
#if PS2_ENABLE_SCRATCHPAD
static_assert(sizeof(GSPRIMSTQPOINT) * PS2_VU0_STRIP_MAX_VERTS <= PS2_SPR_STRIP_BATCH_BYTES,
              "PS2_VU0_STRIP_MAX_VERTS outgrew its scratchpad region");
#else
GSPRIMSTQPOINT s_triangleBatch[PS2_VU0_BATCH_SIZE * 3] __attribute__((aligned(16)));
GSPRIMSTQPOINT s_stripBatch[PS2_VU0_STRIP_MAX_VERTS] __attribute__((aligned(16)));
#endif
Ps2ClampSel s_stripClamp[PS2_VU0_STRIP_MAX_VERTS / 4];
}

GSPRIMSTQPOINT* ps2_vu0_triangle_batch()
{
#if PS2_ENABLE_SCRATCHPAD
    static_assert(sizeof(GSPRIMSTQPOINT) * PS2_VU0_BATCH_SIZE * 3 <= PS2_SPR_TRI_BATCH_BYTES,
                  "PS2_VU0_BATCH_SIZE outgrew its scratchpad region");
    return ps2_spr_region<GSPRIMSTQPOINT>(PS2_SPR_TRI_BATCH_OFFSET);
#else
    return s_triangleBatch;
#endif
}

GSPRIMSTQPOINT* ps2_vu0_strip_batch()
{
#if PS2_ENABLE_SCRATCHPAD
    return ps2_spr_region<GSPRIMSTQPOINT>(PS2_SPR_STRIP_BATCH_OFFSET);
#else
    return s_stripBatch;
#endif
}

Ps2ClampSel* ps2_vu0_strip_clamp()
{
    return s_stripClamp;
}

#endif
