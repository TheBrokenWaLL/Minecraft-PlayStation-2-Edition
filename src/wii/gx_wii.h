// gx_wii.h — Wii VIDEO/GX display integration.
//
// Rendering state, textures and geometry live in src/wii/render and are
// submitted directly to libogc GX. This file owns only VIDEO/GX startup, the
// external framebuffers and the EFB->XFB flip.

#pragma once
#ifdef WII_PLATFORM

#include "wii/WiiEarlyInit.h"

void wiigl_init(bool widescreen);

// [FIX LINKER / ISSUE #9 & CI] Definiciones inline para wiigl_width() y wiigl_height().
// Al definirse inline en el encabezado consultando wiiGetRenderMode(), cualquier unidad de
// traducción (WiiNativeState.cpp, ClientPlatformPolicy_WII.cpp, Display_wii.cpp) genera
// el código directamente sin depender de la resolución de símbolos externos del enlazador.
inline int wiigl_width()
{
	GXRModeObj *rmode = wiiGetRenderMode();
	return rmode ? rmode->fbWidth : 640;
}

inline int wiigl_height()
{
	GXRModeObj *rmode = wiiGetRenderMode();
	return rmode ? rmode->efbHeight : 480;
}

void wiigl_begin_frame();
// Mark whether the current frame should use the native GX display-copy gamma approximation.
void wiigl_set_legacy_gamma_enabled(bool enabled);
// Vertical deflicker filter on the EFB->XFB copy (the "anti-aliasing" blur).
// Starts at the WII_DEFLICKER build default; takes effect from the next copy
// and is safe to call before wiigl_init().
void wiigl_set_deflicker_enabled(bool enabled);
bool wiigl_deflicker_enabled();

// [FIX WII / ISSUE #9] Soporte para alternar y consultar la relación de aspecto 16:9 / 4:3 en tiempo de ejecución.
void wiigl_set_widescreen(bool widescreen);
bool wiigl_is_widescreen();

// Close the frame on the GP side without waiting for it: queue the EFB->XFB
// copy and a draw-done token, then return. Whatever the caller does next runs
// while the GP is still draining the FIFO and copying.
//
// Idempotent within a frame -- a second call before wiigl_end_frame() does
// nothing -- so a caller that does not know whether the frame was already
// submitted can call it unconditionally.
void wiigl_submit_frame();

// Cancel a submitted-but-unpresented frame because a new one has started being
// drawn. Its queued copy holds the previous image, so presenting it would put
// the caller's frame one swap late; this retires the copy and hands its buffer
// back so the next wiigl_end_frame() submits what is in the EFB by then.
void wiigl_discard_pending_frame();

// Wait for the submitted frame, hand its XFB to the video interface and block
// until the retrace. Submits first if wiigl_submit_frame() was not called, so
// this remains a complete frame boundary on its own.
void wiigl_end_frame();
// Retraces wiigl_end_frame() waits per presented frame (WII_TARGET_FPS).
int wiigl_retraces_per_frame();
// Split of the last wiigl_end_frame() wait: time parked on GX_WaitDrawDone()
// (GP still drawing or copying) versus time in VIDEO_WaitVSync(). The first is
// the GP-bound signal, the second is the cap doing its job.
void wiigl_last_present_ns(long long *gpWaitNs, long long *vsyncNs);
void wiigl_shutdown();

// Read the currently displayed XFB into packed RGB8. This is a native VIDEO/XFB
// readback, not an OpenGL-style framebuffer emulation.
bool wiigl_read_display_rgb(unsigned char *rgb, int width, int height);

// GX does not snoop the Broadway data cache. Flush CPU-written buffers before
// GX reads them as vertices, display lists or texture data.
void wiigl_flush_cache(const void* data, unsigned int bytes);

#endif // WII_PLATFORM
