// Display_wii.cpp — Wii implementation of lwjgl::Display.
//
// Thin wrapper: gx_wii.cpp owns VI, the framebuffers and the flip (see the
// comment on wiigl_init in gx_wii.h for why that sequence is not split up).
// What lives here is the lwjgl-shaped surface plus the two things that are
// genuinely display-level on this console:
//
//   * "Close requested" has no window to close. It maps to the console's own
//     exit affordances -- the RESET and POWER buttons on the front panel, and
//     HOME on a Wiimote -- which arrive as libogc callbacks rather than events.
//     Handling them matters: homebrew that ignores RESET/POWER feels broken,
//     and an unsaved world dies with it.
//
//   * The video mode is not ours to choose. VIDEO_GetPreferredMode reports what
//     the console and the user's cable are set to (NTSC 640x480, PAL 640x528,
//     progressive if a component cable is attached), so setDisplayMode is
//     advisory only.
#ifdef WII_PLATFORM

#include "lwjgl/Display.h"

#include <gccore.h>
#include <ogc/conf.h>

#include "wii/gx_wii.h"
#include "wii/input/WiiInput.h"
#include "wii/system/WiiSystemEvents.h"

#include "client/Minecraft.h"
#include "net/minecraft/src/GuiScreen.h"

namespace
{
bool g_created = false;
}

namespace lwjgl
{
namespace Display
{

void create()
{
	if (g_created) return;

	// [FIX WII / ISSUE #9] Consultar la configuración de aspecto de la consola en lugar de
	// depender únicamente de una macro estática #ifdef WII_WIDESCREEN. Si la consola está en 16:9,
	// se pasa widescreen = true a wiigl_init().
	CONF_Init();
#ifdef WII_WIDESCREEN
	const bool widescreen = true;
#else
	const bool widescreen = (CONF_GetAspectRatio() == CONF_ASPECT_16_9);
#endif
	wiigl_init(widescreen);

	WiiInput::initialize(wiigl_width(), wiigl_height());
	WiiSystemEvents::install();

	g_created = true;
}

void setDisplayMode(const DisplayMode &)
{
	// The console decides. See the header comment.
}

DisplayMode getDisplayMode()
{
	return DisplayMode(wiigl_width(), wiigl_height());
}

void setTitle(const jstring &) {}
void setFullscreen(bool)       {}

bool isCloseRequested() { return WiiSystemEvents::exitRequested(); }
bool isVisible()        { return true; }
bool isActive()         { return true; }

void processMessages()
{
	// The input layer cannot infer this for itself, and used to try: wiiPadPoll()
	// selected between its gameplay and menu branches on lwjgl::Mouse::isGrabbed(),
	// and WiiRemote decided whether the IR pointer was a cursor or a turn rate from
	// the alternativeControls user setting. Neither actually tracks whether a screen
	// is open, which is why the legacy menu got gameplay input: no cursor drawn, and
	// A/B still mapped to their in-world actions. Ask the game directly, the way the
	// PS2 front end already does (Display_ps2.cpp).
	Minecraft* mc = Minecraft::getMinecraft();
	const bool inMenu = (mc != nullptr && mc->currentScreen != nullptr);
	const bool specializedMenuNavigation = inMenu &&
		mc->currentScreen->usesSpecializedMenuNavigationForPlatform();

	// Recapture the camera whenever no menu is open AND there is a world to
	// recapture it into. Nothing Wii-side did this, so the grab state was left to
	// whatever the shared GUI code last set.
	//
	// The world check is not optional. setIngameFocus() ends in
	// displayGuiScreen(nullptr), and that call reads "no screen and no world" as
	// "show the title" (Minecraft.cpp: guiscreen == nullptr && theWorld ==
	// nullptr). During the frames between closing the menu and theWorld being
	// assigned, calling it here bounces straight back to the main menu -- with the
	// first tutorial toast already on screen, because the world did start loading.
	if (!inMenu && mc != nullptr && mc->theWorld != nullptr && !mc->inGameHasFocus)
		mc->setIngameFocus();

	WiiInput::poll(inMenu, specializedMenuNavigation);
}

void swapBuffers()
{
	wiigl_end_frame();
	wiigl_begin_frame();
}

void update(bool doProcessMessages)
{
	swapBuffers();
	if (doProcessMessages)
		processMessages();
}

int_t getX() { return 0; }
int_t getY() { return 0; }
int_t getWidth()  { return wiigl_width(); }
int_t getHeight() { return wiigl_height(); }

} // namespace Display
} // namespace lwjgl

#endif // WII_PLATFORM
