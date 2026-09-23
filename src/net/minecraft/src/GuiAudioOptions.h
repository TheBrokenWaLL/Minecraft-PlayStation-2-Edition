#pragma once
#include "legacy/LegacyOptionsScreen.h"
class GuiAudioOptions : public LegacyOptionsScreen
{
public:
    GuiAudioOptions(GuiScreen *parent, GameSettings *settings, LegacyOptionsBackgroundMode mode = LegacyOptionsBackgroundMode::Panorama);
    void initGui() override;
    void drawScreen(int_t x, int_t y, float_t tick) override;
protected:
    void actionPerformed(GuiButton *button) override;
};
