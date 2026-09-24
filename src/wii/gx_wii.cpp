// gx_wii.cpp — Wii video and GX display integration.
//
// The renderer itself lives in src/wii/render. This file owns VIDEO/GX setup,
// the external framebuffers and the EFB-to-XFB presentation sequence.

#ifdef WII_PLATFORM

#include "platform/Log.h"
#include "wii/gx_wii.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <gccore.h>
#include <malloc.h>
#include <ogc/lwp_watchdog.h>


#include "wii/WiiEarlyInit.h"
#include "wii/render/WiiNativeStateSnapshot.h"
#include "wii/tuning/WiiFrameTuning.h"

// cmake/wii.cmake always defines this; the fallback exists so that compiling
// this file on its own does not silently pick the opposite of the documented
// default. Missing-and-therefore-zero would turn the deflicker filter OFF, which
// is a visible regression on an interlaced TV rather than a harmless one.
#ifndef WII_DEFLICKER
#define WII_DEFLICKER 1
#endif

// Per-frame raster bounding box plus XFB sampling: a hardware-side answer to
// "did any primitive actually reach the screen?", which is exactly the question
// a blank frame raises.
//
// Off by default because both halves of it lie under Dolphin, and lie in the
// most misleading direction: they report an empty frame while a perfectly good
// menu is on screen. Dolphin does not emulate the bounding box registers unless
// bounding box emulation is switched on, and its default "Store XFB Copies to
// Texture Only" means the XFB in main memory is never written at all. Enable it
// only when reading the log from real hardware.
#ifndef WII_FRAME_SAMPLING
#define WII_FRAME_SAMPLING 0
#endif

namespace
{

// The GX command FIFO. 256 KB is the usual homebrew size: big enough that the
// CPU rarely stalls waiting for the GP to drain it, small enough not to matter
// against MEM1. Must be 32-byte aligned and uncached.
constexpr unsigned int kFifoSize = 256 * 1024;

void       *g_fifo        = nullptr;
GXRModeObj *g_rmode       = nullptr;
void       *g_xfb[2]      = { nullptr, nullptr };
int         g_activeFb    = 0;
bool        g_widescreen  = false;
bool        g_initialised = false;
// A frame has been closed with GX_SetDrawDone() and not yet presented. Guards
// GX_WaitDrawDone(), which blocks forever if no token was ever queued, and makes
// wiigl_submit_frame() idempotent inside one frame.
bool        g_framePending = false;
bool        g_legacyGammaRequested = false;
// Current copy-filter state; the options screen flips it at runtime.
bool        g_deflickerEnabled = WII_DEFLICKER != 0;
// Whether that token has already been consumed by a GX_WaitDrawDone(). Tracked
// separately rather than assuming a second wait on the same token returns
// immediately, because a screenshot readback also has to wait for the copy and
// must not leave the present without its own wait.
bool        g_frameWaited  = false;
// How the last wiigl_end_frame() split its wait: GP drain versus retrace.
// Read back by the client profiler, which otherwise only sees their sum.
long long   g_lastGpWaitNs = 0;
long long   g_lastVsyncNs  = 0;
// Retraces seen since init, bumped from the VI post-retrace callback, and the
// value at the last present. The cap is paced against these rather than by
// waiting a fixed number of retraces after the frame is ready.
volatile u32 g_retraceCount = 0;
u32          g_lastPresentRetrace = 0;

void onPostRetrace(u32)
{
	++g_retraceCount;
}

// Block until the submitted frame's EFB->XFB copy has retired. No-op when
// nothing is pending or the wait already happened.
void waitPendingFrame()
{
	if (g_framePending && !g_frameWaited)
	{
		GX_WaitDrawDone();
		g_frameWaited = true;
	}
}

} // namespace

// [NOTA] wiigl_width() y wiigl_height() ahora están definidas como inline en wii/gx_wii.h
// para garantizar que cualquier unidad de traducción las resuelva directamente sin fallos de enlace.

void wiigl_flush_cache(const void *data, unsigned int bytes)
{
	if (data && bytes)
		DCFlushRange(const_cast<void *>(data), bytes);
}

void wiigl_init(bool widescreen)
{
	if (g_initialised) return;
	g_widescreen = widescreen;

	// Video is already up: WiiEarlyInit.cpp brings it and the text console
	// online before main() so early failures are readable. Adopt its render mode
	// and framebuffer as buffer 0 rather than starting over -- calling
	// VIDEO_Init twice and allocating a third framebuffer would waste ~600 KB of
	// MEM1 purely so the diagnostic console could exist.
	g_rmode  = wiiGetRenderMode();
	g_xfb[0] = wiiGetEarlyFramebuffer();

	// [FIX WII / ISSUE #9] Sincronizar el ancho de línea del Video Interface (VI)
	// según la bandera panorámica recibida, garantizando que llene la pantalla
	// 16:9 sin barras negras verticales (viWidth = 678).
	if (g_rmode != nullptr)
	{
		if (g_widescreen)
		{
			g_rmode->viWidth = 678;
			const u32 maxWidth = (g_rmode->viTVMode >> 2) == VI_PAL ? VI_MAX_WIDTH_PAL : VI_MAX_WIDTH_NTSC;
			g_rmode->viXOrigin = (maxWidth - 678) / 2;
		}
		else
		{
			g_rmode->viWidth = 640;
			const u32 maxWidth = (g_rmode->viTVMode >> 2) == VI_PAL ? VI_MAX_WIDTH_PAL : VI_MAX_WIDTH_NTSC;
			g_rmode->viXOrigin = (maxWidth - 640) / 2;
		}
		VIDEO_Configure(g_rmode);
		VIDEO_Flush();
	}

	// Two external framebuffers so the VI can scan one while GX copies into the
	// other; flipping between them is what makes the image tear-free.
	g_xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(g_rmode));

	// --- GX ---
	g_fifo = memalign(32, kFifoSize);
	std::memset(g_fifo, 0, kFifoSize);
	GX_Init(g_fifo, kFifoSize);

	GXColor background = { 0, 0, 0, 255 };
	GX_SetCopyClear(background, GX_MAX_Z24);

	GX_SetViewport(0.0f, 0.0f, g_rmode->fbWidth, g_rmode->efbHeight, 0.0f, 1.0f);
	GX_SetDispCopyYScale((f32)g_rmode->xfbHeight / (f32)g_rmode->efbHeight);
	GX_SetScissor(0, 0, g_rmode->fbWidth, g_rmode->efbHeight);
	GX_SetDispCopySrc(0, 0, g_rmode->fbWidth, g_rmode->efbHeight);
	GX_SetDispCopyDst(g_rmode->fbWidth, g_rmode->xfbHeight);
	// The vertical (deflicker) filter runs during every EFB->XFB copy, so it is
	// per-frame copy bandwidth, and it blurs vertically by design.
	//
	// It is ON by default because it earns its cost on the hardware this port
	// targets: a 480i TV flickers badly on high-contrast horizontal edges, and
	// Minecraft's terrain is nothing but high-contrast horizontal edges. Turning
	// it off is a real win in sharpness and a small one in copy bandwidth, but
	// only on a progressive display -- build with -DWII_DEFLICKER=OFF to try it,
	// and judge it on the TV it will actually run on, not in Dolphin. The
	// build default seeds g_deflickerEnabled; the video options screen can
	// flip it at runtime (wiigl_set_deflicker_enabled).
	GX_SetCopyFilter(GX_FALSE, g_rmode->sample_pattern,
	                 g_deflickerEnabled ? GX_TRUE : GX_FALSE, g_rmode->vfilter);
	GX_SetFieldMode(g_rmode->field_rendering,
	                ((g_rmode->viHeight == 2 * g_rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));

	// The EFB is intentionally non-AA. GX_PF_RGB8_Z24 is the depth precision
	// Minecraft needs; multisampled EFB modes use RGB565_Z16 instead. Keep the
	// copy filter's AA input disabled so the copy configuration matches Z24.
	// 24-bit Z, no alpha in the EFB. This one line is what makes every PS2 depth
	// workaround unnecessary: 24 bits against the GS's 15 signed bits is roughly
	// 500x the depth resolution.
	GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
	GX_SetCullMode(GX_CULL_NONE);
	GX_SetDispCopyGamma(GX_GM_1_0);

	// Copy into buffer 1, NOT buffer 0, and leave the display where it is.
	//
	// The EFB has never been written at this point, so this copy moves
	// uninitialised garbage -- aiming it at buffer 0 would splatter that over
	// the boot log the user is still reading (it showed up as a magenta screen
	// with the pre-GX lines missing). GX_TRUE means "clear after copying", so
	// the real purpose of the call is to leave the EFB clean for the first
	// genuine frame; where the garbage lands is irrelevant as long as it is not
	// on screen. The console stays visible until wiigl_end_frame flips to buffer
	// 1 with actual rendered content.
	GX_CopyDisp(g_xfb[1], GX_TRUE);

	// Retrace counter for the presentation cap (wiigl_end_frame). Runs inside
	// the VI interrupt, so it is always current by the time VIDEO_WaitVSync()
	// returns.
	VIDEO_SetPostRetraceCallback(onPostRetrace);

	// Initialize the OptiCraft fixed-function state after GX itself is ready.
	wii_native_state_initialize();

	// Establish a complete, known fixed-function baseline. OpenGX's first draw
	// reprogrammed all dirty GX state; the native backend has no global dirty
	// pass, so inheriting unspecified GX_Init state makes the first GUI frame
	// depend on whichever helper ran before it. Every native draw still programs
	// its own state, but this removes that dependency at the frame boundary.
	GX_SetNumChans(1);
	GX_SetChanMatColor(GX_COLOR0A0, GXColor{255, 255, 255, 255});
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG,
	               GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetNumIndStages(0);
	GX_SetNumTexGens(0);
	GX_SetNumTevStages(1);
	GX_SetTevDirect(GX_TEVSTAGE0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_DISABLE, GX_COLOR0A0);
	GX_SetZTexture(GX_ZT_DISABLE, GX_TF_Z24X8, 0);
	GX_SetZMode(GX_FALSE, GX_LESS, GX_FALSE);
	GX_SetZCompLoc(GX_ENABLE);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	GX_SetColorUpdate(GX_TRUE);
	GX_SetAlphaUpdate(GX_TRUE);
	GX_SetCullMode(GX_CULL_NONE);

	g_initialised = true;

	MC_LOG_INFO("wii", "[WII][CFG] log=%d renderer=GX-native deflicker=%d texture=%s\n",
	       MC_LOG_LEVEL, WII_DEFLICKER,
	       WII_TEXTURE_RGB5A3 ? "RGB5A3" : "RGBA8");

	// aa is logged because it constrains the pixel format: GX only supports
	// anti-aliasing with GX_PF_RGB565_Z16, and we ask for GX_PF_RGB8_Z24 above.
	// If a render mode ever comes back with aa=1 the two disagree and the copy
	// out of the EFB produces wrong colours -- worth ruling out before chasing a
	// tint as a texture-format problem.
	MC_LOG_INFO("wii", "GX native ready: %dx%d (efb %d), %s, aa=%d vf=%d\n",
	       g_rmode->fbWidth, g_rmode->xfbHeight, g_rmode->efbHeight,
	       g_widescreen ? "16:9" : "4:3",
	       (int)g_rmode->aa, (int)g_rmode->vfilter[2]);

	// Last thing in the function, and after that final line rather than before
	// it: the console is still the only thing on screen at this point -- the
	// first rendered frame does not reach the TV until wiigl_end_frame flips to
	// buffer 1 -- so the bring-up summary is still worth painting. From the next
	// caller onwards the console and the renderer share buffer 0, and the unified Wii log sink has
	// to stop drawing into it. Every other channel keeps working.
	wiiPlatformLogEndBootPhase();
}

// [FIX WII / ISSUE #9] Permite alternar la relación de aspecto panorámica en caliente
// reconfigurando dinámicamente los registros del Video Interface (VI) de la Wii sin reiniciar la consola.
void wiigl_set_widescreen(bool widescreen)
{
	g_widescreen = widescreen;
	if (g_initialised && g_rmode != nullptr)
	{
		if (g_widescreen)
		{
			g_rmode->viWidth = 678;
			const u32 maxWidth = (g_rmode->viTVMode >> 2) == VI_PAL ? VI_MAX_WIDTH_PAL : VI_MAX_WIDTH_NTSC;
			g_rmode->viXOrigin = (maxWidth - 678) / 2;
		}
		else
		{
			g_rmode->viWidth = 640;
			const u32 maxWidth = (g_rmode->viTVMode >> 2) == VI_PAL ? VI_MAX_WIDTH_PAL : VI_MAX_WIDTH_NTSC;
			g_rmode->viXOrigin = (maxWidth - 640) / 2;
		}
		VIDEO_Configure(g_rmode);
		VIDEO_Flush();
	}
}

bool wiigl_is_widescreen()
{
	return g_widescreen;
}

void wiigl_begin_frame()
{
	g_legacyGammaRequested = false;
	// The EFB was cleared by the copy that ended the previous frame. Reset the
	// raster bounding box too: this gives us a cheap, hardware-side answer to
	// "did any primitive actually reach rasterization?" in verbose builds.
#if MC_LOG_LEVEL >= 2 && WII_FRAME_SAMPLING
	GX_ClearBoundingBox();
#endif
}

void wiigl_set_legacy_gamma_enabled(bool enabled)
{
	g_legacyGammaRequested = enabled;
}

void wiigl_set_deflicker_enabled(bool enabled)
{
	g_deflickerEnabled = enabled;
	// Only a copy-filter register write; the next GX_CopyDisp picks it up.
	// Before init the flag alone is enough: wiigl_init() reads it.
	if (g_initialised)
		GX_SetCopyFilter(GX_FALSE, g_rmode->sample_pattern,
		                 g_deflickerEnabled ? GX_TRUE : GX_FALSE, g_rmode->vfilter);
}

bool wiigl_deflicker_enabled()
{
	return g_deflickerEnabled;
}

void wiigl_submit_frame()
{
	if (!g_initialised || g_framePending) return;

	g_activeFb ^= 1;


	// Order matters, and it used to be the other way round: GX_DrawDone() then
	// GX_CopyDisp(). That is wrong in both directions.
	//
	// Waiting BEFORE the copy buys nothing. GX_CopyDisp is itself a GP command
	// and executes after the drawing commands already in the FIFO, so the
	// ordering it was protecting is free -- all the wait did was park the CPU
	// until the GP had drained, every frame, with no work overlapping it.
	//
	// Not waiting AFTER the copy is the actual bug. VIDEO_SetNextFramebuffer
	// hands the XFB to the video interface while the GP may still be copying
	// into it. The copy is fast enough that it almost always wins the race
	// before the next retrace, which is exactly what makes this the kind of
	// defect that never shows up in Dolphin and shows up on a TV as an
	// intermittent torn or garbled band across the top of the frame.
	//
	// Copy, then wait for the copy, then present -- the sequence in every libogc
	// example. What changed is only WHERE the wait happens: GX_DrawDone() is
	// GX_SetDrawDone() followed immediately by GX_WaitDrawDone(), and splitting
	// the two lets the caller put CPU work between them. The wait is still
	// before the present, so the race above stays closed.
	// Native zero-cost approximation of the Legacy4J gamma pass. GX only exposes
	// 1.0/1.7/2.2 display-copy curves, so 1.7 is the nearest visible option.
	GX_SetDispCopyGamma(g_legacyGammaRequested ? GX_GM_1_7 : GX_GM_1_0);
	GX_CopyDisp(g_xfb[g_activeFb], GX_TRUE);
	GX_SetDrawDone();
	g_framePending = true;
	g_frameWaited = false;
}

void wiigl_discard_pending_frame()
{
	if (!g_initialised || !g_framePending) return;

	// The copy is already queued, so it has to be waited for rather than dropped:
	// the buffer it targets is about to be reused by the next submission, and the
	// draw-done token has to be consumed before another one is issued.
	waitPendingFrame();
	g_framePending = false;
	g_activeFb ^= 1;
}

void wiigl_end_frame()
{
	if (!g_initialised) return;

	// Covers callers that present without having submitted first -- the loading
	// screen and the menu paths both call Display::update() on their own.
	wiigl_submit_frame();
	const u64 gpWaitStart = gettime();
	waitPendingFrame();
	g_lastGpWaitNs = (long long)ticks_to_nanosecs(gettime() - gpWaitStart);
	g_framePending = false;

#if MC_LOG_LEVEL >= 2 && WII_FRAME_SAMPLING
	{
		static unsigned int s_frame = 0;
		++s_frame;
		if (s_frame <= 8 || (s_frame % 120u) == 0u)
		{
			u16 top = 0, bottom = 0, left = 0, right = 0;
			GX_ReadBoundingBox(&top, &bottom, &left, &right);

			// XFB is Y1 Cb Y2 Cr. Sample a grid after GX_DrawDone(), so these
			// bytes are the actual frame about to be handed to VI rather than a
			// prediction based on CPU-side renderer state.
			const unsigned char *xfb = static_cast<const unsigned char *>(g_xfb[g_activeFb]);
			int minY = 255, maxY = 0, nonBlack = 0, samples = 0;
			if (xfb != nullptr && g_rmode != nullptr)
			{
				const int width = g_rmode->fbWidth;
				const int height = g_rmode->xfbHeight;
				for (int gy = 0; gy < 12; ++gy)
				{
					const int y = ((gy * 2 + 1) * height) / 24;
					for (int gx = 0; gx < 16; ++gx)
					{
						const int x = ((gx * 2 + 1) * width) / 32;
						const int pairX = x & ~1;
						const unsigned char *pair = xfb + (y * width + pairX) * 2;
						const int yy = pair[(x & 1) ? 2 : 0];
						if (yy < minY) minY = yy;
						if (yy > maxY) maxY = yy;
						// Studio-range black is Y=16. Leave margin for copy filtering.
						if (yy > 24) ++nonBlack;
						++samples;
					}
				}
			}
			MC_LOG_INFO("wii", "[WII][GX][FRAME] n=%u bbox=%u,%u,%u,%u xfbY=%d..%d nonblack=%d/%d fb=%d\n",
			       s_frame, (unsigned int)left, (unsigned int)top,
			       (unsigned int)right, (unsigned int)bottom,
			       minY, maxY, nonBlack, samples, g_activeFb);
		}
	}
#endif

	VIDEO_SetNextFramebuffer(g_xfb[g_activeFb]);
	VIDEO_Flush();
	const u64 vsyncStart = gettime();
	VIDEO_WaitVSync();
#if WII_TARGET_FPS > 0
	// Hold the frame until the cap's period has elapsed since the LAST present,
	// counted in retraces. This used to wait a fixed retraces_per_frame - 1
	// extra retraces after the frame was ready, which made the real budget one
	// retrace rather than the whole period: a 20 ms frame at the 30 fps cap
	// missed retrace 1, presented at retrace 2, and then still slept through
	// retrace 3 -- 50 ms for work that fit in 33. Counting from the previous
	// present, that same frame presents at retrace 2 and leaves immediately.
	// Frames under one retrace still sleep the full period, which is the gap
	// the async generation worker runs in.
	//
	// Unsigned difference so the counter wrapping is harmless.
	const u32 period = (u32)wiigl_retraces_per_frame();
	while (g_retraceCount - g_lastPresentRetrace < period)
		VIDEO_WaitVSync();
#endif
	g_lastPresentRetrace = g_retraceCount;
	g_lastVsyncNs = (long long)ticks_to_nanosecs(gettime() - vsyncStart);
}

void wiigl_last_present_ns(long long *gpWaitNs, long long *vsyncNs)
{
	if (gpWaitNs != nullptr) *gpWaitNs = g_lastGpWaitNs;
	if (vsyncNs != nullptr) *vsyncNs = g_lastVsyncNs;
}

int wiigl_retraces_per_frame()
{
#if WII_TARGET_FPS > 0
	// VIDEO_WaitVSync returns once per retrace: 60/s on NTSC, PAL60 and
	// progressive modes, 50/s on PAL50.
	const int refreshHz = (g_rmode != nullptr && (g_rmode->viTVMode >> 2) == VI_PAL) ? 50 : 60;
	const int retraces = (refreshHz + WII_TARGET_FPS / 2) / WII_TARGET_FPS;
	return retraces < 1 ? 1 : retraces;
#else
	return 1;
#endif
}

static inline unsigned char clampRgb(int value)
{
	return static_cast<unsigned char>(value < 0 ? 0 : value > 255 ? 255 : value);
}

bool wiigl_read_display_rgb(unsigned char *rgb, int width, int height)
{
	if (!g_initialised || g_rmode == nullptr || g_xfb[g_activeFb] == nullptr || rgb == nullptr || width <= 0 || height <= 0)
		return false;

	// The XFB is only stable once the copy that filled it has retired. A caller
	// between wiigl_submit_frame() and wiigl_end_frame() would otherwise read a
	// buffer the GP is still writing, and get a torn screenshot out of it.
	//
	// The pending flag is deliberately left set, so wiigl_end_frame() still
	// presents the buffer that was read rather than starting a new copy.
	waitPendingFrame();

	const int srcWidth = g_rmode->fbWidth;
	const int srcHeight = g_rmode->xfbHeight;
	const unsigned char *xfb = static_cast<const unsigned char *>(g_xfb[g_activeFb]);

	for (int y = 0; y < height; ++y)
	{
		const int sy = (y * srcHeight) / height;
		for (int x = 0; x < width; ++x)
		{
			const int sx = (x * srcWidth) / width;
			const int pairX = sx & ~1;
			const unsigned char *pair = xfb + (sy * srcWidth + pairX) * 2;
			const int yy = pair[(sx & 1) ? 2 : 0];
			const int cb = pair[1] - 128;
			const int cr = pair[3] - 128;
			const int c = yy - 16;
			const int r = (298 * c + 409 * cr + 128) >> 8;
			const int g = (298 * c - 100 * cb - 208 * cr + 128) >> 8;
			const int b = (298 * c + 516 * cb + 128) >> 8;
			unsigned char *dst = rgb + (y * width + x) * 3;
			dst[0] = clampRgb(r); dst[1] = clampRgb(g); dst[2] = clampRgb(b);
		}
	}
	return true;
}

void wiigl_shutdown()
{
	if (!g_initialised) return;
	// Retire an unpresented frame before adding another draw-done token; the
	// GP is otherwise aborted with a copy still in flight.
	waitPendingFrame();
	g_framePending = false;
	GX_DrawDone();
	GX_AbortFrame();
	g_initialised = false;
}

#endif // WII_PLATFORM
