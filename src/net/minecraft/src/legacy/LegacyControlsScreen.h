#pragma once

#include <vector>

#include "LegacyControlsBinding.h"
#include "LegacyOptionsScreen.h"

class GuiButton;
class LegacyOptionCheckbox;

class LegacyControlsScreen : public LegacyOptionsScreen
{
public:
    LegacyControlsScreen(GuiScreen *parent, GameSettings *settings,
        LegacyOptionsBackgroundMode backgroundMode = LegacyOptionsBackgroundMode::Panorama, bool editLayout = false);

    void initGui() override;
    void updateScreen() override;
    void drawScreen(int_t mouseX, int_t mouseY, float_t partialTick) override;
    void onGuiClosed() override;

protected:
    void actionPerformed(GuiButton *button) override;
    void keyTyped(char_t c, int_t key) override;
    void mouseClicked(int_t x, int_t y, int_t button) override;

private:
    void drawLayoutArtwork();
    bool editingLayout;
    bool artworkAvailable = false;
    void rebuildPage();
    void beginCapture(int_t visibleRow);
    void cancelCapture();
    void applyCapturedKey(int_t key);
    void resetDefaults();
    void refreshRowLabels();
    int_t pageCount() const;
    int_t fixedOptionRows() const;

    LegacyOptionCheckbox *invertControlsCheckbox;
    int_t captureRow;
    int_t page;
    int_t rowsPerPage;
    std::vector<LegacyControlsBindingRow> rows;
};
