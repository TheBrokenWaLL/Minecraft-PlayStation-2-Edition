#include "LegacyMenuHints.h"
#include "net/minecraft/src/ControlIcon.h"
#include "net/minecraft/src/UiStrings.h"
#include "platform/PlatformConfig.h"

void drawLegacyMenuHints(Minecraft *mc, int_t screenWidth, int_t screenHeight, bool showBack)
{
#if PLATFORM_PS2
    const std::string buttons[] = {"D-Pad", "Cross", "Circle"};
#elif PLATFORM_WII
    const std::string buttons[] = {"D-Pad", "A", "B"};
#else
    const std::string buttons[] = {"Up/Down", "Enter", "Esc"};
#endif
    const std::string actions[] = {uiText("Navigate"), uiText("Select"), uiText("Back")};
    drawControlHintRow(mc, screenWidth, legacyHintRowY(screenHeight), buttons, actions, showBack ? 3 : 2);
}
