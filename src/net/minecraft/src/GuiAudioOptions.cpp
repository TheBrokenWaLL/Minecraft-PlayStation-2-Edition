#include "GuiAudioOptions.h"
#include "legacy/LegacyOptionSlider.h"
#include "legacy/LegacyGuiButton.h"
#include "EnumOptions.h"
#include "UiStrings.h"
GuiAudioOptions::GuiAudioOptions(GuiScreen *parent, GameSettings *settings, LegacyOptionsBackgroundMode mode)
    : LegacyOptionsScreen(parent, settings, mode) {}
void GuiAudioOptions::initGui()
{
    configureLegacyLayout(3, true, LegacyOptionsLayoutPreset::Compact);
    controlList.push_back(new LegacyOptionSlider(0, legacyLayout.contentX, legacyLayout.rowY(0), legacyLayout.contentWidth, legacyLayout.rowHeight, settings, EnumOptions::MUSIC));
    controlList.push_back(new LegacyOptionSlider(1, legacyLayout.contentX, legacyLayout.rowY(1), legacyLayout.contentWidth, legacyLayout.rowHeight, settings, EnumOptions::SOUND));
    controlList.push_back(new LegacyGuiButton(200, legacyLayout.contentX, legacyLayout.rowY(2), legacyLayout.contentWidth, legacyLayout.rowHeight, uiText("Done")));
}
void GuiAudioOptions::actionPerformed(GuiButton *button)
{
    if (button && button->enabled && button->id == 200) returnToParent();
}
void GuiAudioOptions::drawScreen(int_t x, int_t y, float_t tick)
{
    drawLegacyBackground(tick);
    updateLegacyPointerHover(x, y);
    GuiScreen::drawScreen(x, y, tick);
}
