#pragma once

#include <vector>

#include "LegacyOptionsScreen.h"

class GuiButton;

class LegacyControllerLayoutScreen : public LegacyOptionsScreen
{
public:
    LegacyControllerLayoutScreen(GuiScreen *parent, GameSettings *settings,
        LegacyOptionsBackgroundMode backgroundMode = LegacyOptionsBackgroundMode::Panorama);

    void initGui() override;
    void updateScreen() override;
    void drawScreen(int_t mouseX, int_t mouseY, float_t partialTick) override;
    void onGuiClosed() override;

protected:
    void actionPerformed(GuiButton *button) override;
    void keyTyped(char_t c, int_t key) override;
    void mouseClicked(int_t x, int_t y, int_t button) override;

private:
    struct ActionSlot
    {
        int_t bindingIndex = -1;
        bool leftSide = true;
        int_t row = 0;
        GuiButton *button = nullptr;
    };

    void refreshLabels();
    void syncSelectedControl();
    void updatePointerHover(int_t mouseX, int_t mouseY);
    void moveSelection(int_t dirX, int_t dirY);
    void activateSelection();
    void beginCapture(int_t bindingIndex);
    void cancelCapture();
    void applyCapturedKey(int_t keyCode);
    void resetDefaults();
    void assignActionToCode(int_t keyCode, int_t bindingIndex);

    void drawControllerImage(int_t centerX, int_t topY);
    void drawControllerConnectors(int_t centerX, int_t topY);
    void drawConnectorForSlot(const ActionSlot &slot, int_t centerX, int_t topY, bool highlighted);

    int_t selectionForButton(const GuiButton *button) const;
    GuiButton *buttonForSelection(int_t index) const;
    bool isSelectionAvailable(int_t index) const;
    int_t currentKeyCodeForSlot(const ActionSlot &slot) const;
    int_t visualPadKeyForSlot(const ActionSlot &slot) const;
    bool actionHasConflict(int_t bindingIndex) const;

    std::vector<ActionSlot> actionSlots;
    GuiButton *resetButton;
    GuiButton *backButton;
    int_t captureBindingIndex;
    bool controllerImageAvailable = false;
};
