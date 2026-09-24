#pragma once

#include "net/minecraft/src/GuiScreen.h"

class GuiButton;

class GuiLoadSkinsMenu : public GuiScreen
{
public:
    explicit GuiLoadSkinsMenu(GuiScreen *parent);
    ~GuiLoadSkinsMenu() override = default;

    void initGui() override;
    void drawScreen(int_t mouseX, int_t mouseY, float_t partialTick) override;
    void actionPerformed(GuiButton *button) override;
    void keyTyped(char_t c, int_t key) override;

protected:
    bool allowsPlatformPointerInput() const override { return true; }
    void handleSpecializedMenuInput() override;

private:
    GuiScreen *parentScreen;
    int selectedButtonIndex;
};
