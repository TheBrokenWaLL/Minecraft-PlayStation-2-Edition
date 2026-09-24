#ifdef WII_PLATFORM

#include "platform/Log.h"
#include "wii/WiiEarlyInit.h"

#include <gccore.h>
#include <ogc/conf.h>

namespace
{
bool g_videoReady = false;
GXRModeObj* g_renderMode = nullptr;
void* g_framebuffer = nullptr;
}

void wiiEnsureEarlyVideo()
{
	if (g_videoReady)
		return;
	g_videoReady = true;

	VIDEO_Init();
	CONF_Init();
	g_renderMode = VIDEO_GetPreferredMode(nullptr);

	// [FIX WII / ISSUE #9] Detección de relación de aspecto 16:9 desde la configuración de la consola Wii.
	// Si la consola está configurada en 16:9 (CONF_ASPECT_16_9), expandimos el ancho activo de la línea de video
	// (viWidth) a 678 píxeles (estándar libogc para llenar la pantalla panorámica) y recalculamos viXOrigin.
	// Esto elimina por completo las barras negras verticales laterales (pillarboxing) en televisores 16:9.
	if (CONF_GetAspectRatio() == CONF_ASPECT_16_9)
	{
		g_renderMode->viWidth = 678;
		const u32 maxWidth = (g_renderMode->viTVMode >> 2) == VI_PAL ? VI_MAX_WIDTH_PAL : VI_MAX_WIDTH_NTSC;
		g_renderMode->viXOrigin = (maxWidth - 678) / 2;
	}

	g_framebuffer = MEM_K0_TO_K1(SYS_AllocateFramebuffer(g_renderMode));
	console_init(g_framebuffer, 20, 20, g_renderMode->fbWidth, g_renderMode->xfbHeight,
	             g_renderMode->fbWidth * VI_DISPLAY_PIX_SZ);

	VIDEO_Configure(g_renderMode);
	VIDEO_SetNextFramebuffer(g_framebuffer);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (g_renderMode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();

	MC_LOG_INFO("wii", "OptiCraft - Wii\n");
	MC_LOG_INFO("wii", "video %dx%d ok (aspect: %s, viWidth: %d)\n",
		g_renderMode->fbWidth, g_renderMode->xfbHeight,
		CONF_GetAspectRatio() == CONF_ASPECT_16_9 ? "16:9" : "4:3",
		g_renderMode->viWidth);
}

GXRModeObj* wiiGetRenderMode()
{
	wiiEnsureEarlyVideo();
	return g_renderMode;
}

void* wiiGetEarlyFramebuffer()
{
	wiiEnsureEarlyVideo();
	return g_framebuffer;
}

#endif // WII_PLATFORM
