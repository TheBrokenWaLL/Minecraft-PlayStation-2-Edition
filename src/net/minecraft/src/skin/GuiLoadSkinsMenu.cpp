#include "GuiLoadSkinsMenu.h"
#include "GuiLoadSkinsList.h"
#include "GuiButton.h"
#include "Minecraft.h"
#include "FontRenderer.h"
#include "SoundManager.h"

#if PLATFORM_PS2 || PLATFORM_WII
#include "platform/Input.h"
#endif

GuiLoadSkinsMenu::GuiLoadSkinsMenu(GuiScreen *parent)
    : parentScreen(parent)
    , selectedButtonIndex(0)
{
}

void GuiLoadSkinsMenu::initGui()
{
    controlList.clear();

    const int_t btnW = 200;
    const int_t btnH = 20;
    const int_t centerX = width / 2 - btnW / 2;
    const int_t startY = height / 4 + 30;

    controlList.push_back(new GuiButton(2, centerX, startY, btnW, btnH, "Load from Device (Recommended)"));
    controlList.push_back(new GuiButton(1, centerX, startY + 28, btnW, btnH, "Load from USB Storage"));
    controlList.push_back(new GuiButton(3, centerX, startY + 70, btnW, btnH, "Back"));
}

void GuiLoadSkinsMenu::actionPerformed(GuiButton *button)
{
    if (!button->enabled)
        return;

    if (mc != nullptr && mc->sndManager != nullptr)
        mc->sndManager->playSoundFX("random.click", 1.0f, 1.0f);

    if (button->id == 1) // USB
    {
        mc->displayGuiScreen(new GuiLoadSkinsList(this, GuiLoadSkinsList::Source::USB));
    }
    else if (button->id == 2) // Device
    {
        mc->displayGuiScreen(new GuiLoadSkinsList(this, GuiLoadSkinsList::Source::Device));
    }
    else if (button->id == 3) // Back
    {
        mc->displayGuiScreen(parentScreen);
    }
}

void GuiLoadSkinsMenu::keyTyped(char_t c, int_t key)
{
    if (key == 1) // ESC
    {
        mc->displayGuiScreen(parentScreen);
        return;
    }
    GuiScreen::keyTyped(c, key);
}

void GuiLoadSkinsMenu::handleSpecializedMenuInput()
{
#if PLATFORM_PS2 || PLATFORM_WII
    const PlatformTextInputSnapshot pad = platformTextInputSnapshot(platformMenuPad());
    if (!pad.connected)
        return;

    if ((pad.pressed & PLATFORM_TEXT_UP) != 0)
    {
        if (selectedButtonIndex > 0)
            selectedButtonIndex--;
        else
            selectedButtonIndex = static_cast<int>(controlList.size()) - 1;

        if (mc != nullptr && mc->sndManager != nullptr)
            mc->sndManager->playSoundFX("random.click", 1.0f, 1.2f);
    }
    else if ((pad.pressed & PLATFORM_TEXT_DOWN) != 0)
    {
        if (selectedButtonIndex < static_cast<int>(controlList.size()) - 1)
            selectedButtonIndex++;
        else
            selectedButtonIndex = 0;

        if (mc != nullptr && mc->sndManager != nullptr)
            mc->sndManager->playSoundFX("random.click", 1.0f, 1.2f);
    }

    if ((pad.pressed & PLATFORM_TEXT_TYPE) != 0) // Cross / Confirm
    {
        if (selectedButtonIndex >= 0 && selectedButtonIndex < static_cast<int>(controlList.size()))
            actionPerformed(controlList[selectedButtonIndex]);
    }
    else if ((pad.pressed & (PLATFORM_TEXT_BACK | PLATFORM_TEXT_CLOSE)) != 0) // Circle / Cancel
    {
        if (mc != nullptr && mc->sndManager != nullptr)
            mc->sndManager->playSoundFX("random.back", 1.0f, 1.0f);
        mc->displayGuiScreen(parentScreen);
    }
#endif
}

void GuiLoadSkinsMenu::drawScreen(int_t mouseX, int_t mouseY, float_t partialTick)
{
    drawDefaultBackground();

    drawCenteredString(fontRenderer, "Load Skins from Storage", width / 2, height / 4, 0xFFFFFF);
    drawCenteredString(fontRenderer, "Place standard .png skins in a 'skins' folder", width / 2, height / 4 + 14, 0x808080);

    GuiScreen::drawScreen(mouseX, mouseY, partialTick);
}
