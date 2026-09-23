#include "net/minecraft/src/UiStrings.h"
#include "LegacyControlsScreen.h"

#include <algorithm>

#include "LegacyGuiButton.h"
#include "net/minecraft/src/RenderEngine.h"
#include "net/minecraft/src/Tessellator.h"
#include "platform/RenderAPI.h"
#include "LegacyOptionCheckbox.h"
#include "LegacyOptionSlider.h"
#include "net/minecraft/src/EnumOptions.h"
#include "net/minecraft/src/FontRenderer.h"
#include "net/minecraft/src/GameSettings.h"
#include "net/minecraft/src/GuiButton.h"
#include "net/minecraft/src/GuiDeadzoneSettings.h"
#include "net/minecraft/src/Minecraft.h"
#include "LegacyControllerLayoutScreen.h"
#include "pc/lwjgl/Keyboard.h"
#include "platform/Input.h"
#include "platform/PlatformConfig.h"

#if PLATFORM_PS2
#include "ps2/input/Ps2PadKeyCodes.h"
#endif

#if PLATFORM_WII
#include "wii/input/WiiPadKeyCodes.h"
#endif

namespace
{
#if PLATFORM_WII
constexpr const char *LAYOUT_ART = "/gui/controls/wii/controller.png";
#else
constexpr const char *LAYOUT_ART = "/gui/controls/keyboard/layout.png";
#endif

constexpr int_t BUTTON_SENSITIVITY = 6998;
constexpr int_t BUTTON_INVERT_CONTROLS = 6999;
constexpr int_t BUTTON_ROW_BASE = 7000;
constexpr int_t BUTTON_PREVIOUS = 7100;
constexpr int_t BUTTON_NEXT = 7101;
constexpr int_t BUTTON_RESET = 7102;
constexpr int_t BUTTON_BACK = 7103;
constexpr int_t BUTTON_DEADZONE = 7104;

bool reservedCaptureKey(int_t key)
{
#if PLATFORM_PS2
    return key == PS2_KEY_CIRCLE || key == lwjgl::Keyboard::KEY_ESCAPE;
#elif PLATFORM_WII
    return key == WII_KEY_GC_B || key == WII_KEY_WM_B || key == WII_KEY_CC_B ||
        key == lwjgl::Keyboard::KEY_ESCAPE;
#else
    return key == lwjgl::Keyboard::KEY_ESCAPE;
#endif
}

std::string capturePrompt()
{
#if PLATFORM_PS2 || PLATFORM_WII
    return uiText("Press a button...");
#else
    return uiText("Press a key...");
#endif
}
}

LegacyControlsScreen::LegacyControlsScreen(GuiScreen *parent, GameSettings *settingsValue,
    LegacyOptionsBackgroundMode backgroundModeValue, bool editLayout)
    : LegacyOptionsScreen(parent, settingsValue, backgroundModeValue),
      editingLayout(editLayout), invertControlsCheckbox(nullptr), captureRow(-1), page(0), rowsPerPage(8)
{
}

void LegacyControlsScreen::initGui()
{
    clearControlList();
    if (!editingLayout)
    {
    // Platform remapping lives in Edit Layout. Keep the parent menu focused on
    // general input settings, with no duplicate binding list or pagination.
    captureRow = -1;
    platformSetPadRebindExclusive(false);
    rows.clear();
    configureLegacyLayout(PLATFORM_PS2 ? 5 : 4, true, LegacyOptionsLayoutPreset::Wide);
    const int_t x = legacyLayout.contentX;
    const int_t w = legacyLayout.contentWidth;
    const int_t h = legacyLayout.rowHeight;
    controlList.push_back(new LegacyOptionSlider(BUTTON_SENSITIVITY, x, legacyLayout.rowY(0), w, h,
        settings, EnumOptions::SENSITIVITY));
    invertControlsCheckbox = new LegacyOptionCheckbox(BUTTON_INVERT_CONTROLS, x, legacyLayout.rowY(1), w, h,
        uiText("Invert Controls"), settings->invertMouse);
    controlList.push_back(invertControlsCheckbox);
    controlList.push_back(new LegacyGuiButton(7105, x, legacyLayout.rowY(2), w, h, uiText("Edit Layout")));
#if PLATFORM_PS2
    controlList.push_back(new LegacyGuiButton(BUTTON_DEADZONE, x, legacyLayout.rowY(3), w, h,
        uiText("Deadzone Settings")));
#endif
    controlList.push_back(new LegacyGuiButton(BUTTON_BACK, x, legacyLayout.rowY(PLATFORM_PS2 ? 4 : 3), w, h, uiText("Back")));
    syncLegacySelection();
    return;
    }
    captureRow = -1;
    platformSetPadRebindExclusive(false);
    rows = legacyControlsRows(settings);
#if PLATFORM_WII
    // Whole family pages keep the remote illustration honest and retain all
    // Classic/GameCube actions. Put the supplied remote layout first.
    std::stable_sort(rows.begin(), rows.end(), [](const LegacyControlsBindingRow &a,
        const LegacyControlsBindingRow &b) {
        auto rank = [](LegacyControlsWiiFamily f) {
            return f == LegacyControlsWiiFamily::Wiimote ? 0 :
                f == LegacyControlsWiiFamily::GameCube ? 1 : 2;
        };
        return rank(a.wiiFamily) < rank(b.wiiFamily);
    });
    rowsPerPage = 7;
#else
    rowsPerPage = 8;
#endif
    const int_t optionRows = 0;
    const int_t columnRows = (rowsPerPage + 1) / 2;
    configureLegacyLayout(columnRows + 4, true, LegacyOptionsLayoutPreset::Wide);
    legacyLayout.panelWidth = std::min<int_t>(width - 12, legacyLayout.panelWidth + 48);
    legacyLayout.panelX = (width - legacyLayout.panelWidth) / 2;
    legacyLayout.contentX = legacyLayout.panelX + 8;
    legacyLayout.contentWidth = legacyLayout.panelWidth - 16;
    // Fit the title, binding columns and footer within small Wii screens.
    const int_t availableRows = legacyOptionsMaxRows(width, height, LegacyOptionsLayoutPreset::Wide);
    if (availableRows < columnRows + 4)
    {
        const int_t savedWidth = legacyLayout.panelWidth;
        configureLegacyLayout(std::max<int_t>(1, availableRows), true, LegacyOptionsLayoutPreset::Wide);
        legacyLayout.panelWidth = savedWidth;
        legacyLayout.panelX = (width - savedWidth) / 2;
        legacyLayout.contentX = legacyLayout.panelX + 8;
        legacyLayout.contentWidth = savedWidth - 16;
        legacyLayout.rowHeight = std::max<int_t>(10, (legacyLayout.panelHeight - 16) / (columnRows + 4) - 1);
        legacyLayout.rowSpacing = 1;
    }
    artworkAvailable = mc && mc->renderEngine && mc->renderEngine->hasResource(LAYOUT_ART);

    const int_t x = legacyLayout.contentX;
    const int_t w = legacyLayout.contentWidth;
    const int_t h = legacyLayout.rowHeight;

    const int_t buttonWidth = std::max<int_t>(24, (w - 100) / 2);
    for (int_t i = 0; i < rowsPerPage; ++i)
        controlList.push_back(new LegacyGuiButton(BUTTON_ROW_BASE + i,
            i < columnRows ? x : x + w - buttonWidth,
            legacyLayout.rowY(1 + i % columnRows), buttonWidth, h, ""));

    const int_t navY = legacyLayout.rowY(columnRows + 1);
    const int_t gap = 2;
    const int_t halfWidth = (w - gap) / 2;
    controlList.push_back(new LegacyGuiButton(BUTTON_PREVIOUS, x, navY, halfWidth, h, uiText("Previous")));
    controlList.push_back(new LegacyGuiButton(BUTTON_NEXT, x + halfWidth + gap, navY, w - halfWidth - gap, h, uiText("Next")));
    controlList.push_back(new LegacyGuiButton(BUTTON_RESET, x, legacyLayout.rowY(columnRows + 2), w, h,
        uiText("Reset to Defaults")));
    controlList.push_back(new LegacyGuiButton(BUTTON_BACK, x, legacyLayout.rowY(columnRows + 3), w, h, uiText("Back")));

    rebuildPage();
}

int_t LegacyControlsScreen::pageCount() const
{
    if (rowsPerPage <= 0 || rows.empty())
        return 1;
    return (static_cast<int_t>(rows.size()) + rowsPerPage - 1) / rowsPerPage;
}

void LegacyControlsScreen::refreshRowLabels()
{
    for (int_t visibleRow = 0; visibleRow < rowsPerPage; ++visibleRow)
    {
        GuiButton *button = controlList[fixedOptionRows() + visibleRow];
        const int_t rowIndex = page * rowsPerPage + visibleRow;
        const bool available = rowIndex >= 0 && rowIndex < static_cast<int_t>(rows.size());
        button->enabled = available;
        button->enabled2 = available;
        if (!available)
        {
            button->displayString.clear();
            continue;
        }

        const LegacyControlsBindingRow &row = rows[rowIndex];
        std::string value;
        if (captureRow == rowIndex)
            value = capturePrompt();
        else
        {
            value = legacyControlsBindingLabel(settings, row);
            if (legacyControlsBindingConflicts(settings, row))
                value = std::string("\xc2\xa7" "c") + value;
        }

        std::string label = row.label + "    " + value;
        if (fontRenderer != nullptr)
            label = fontRenderer->trimStringToWidth(label, button->getButtonWidth() - 6);
        button->displayString = label;
    }
}

void LegacyControlsScreen::rebuildPage()
{
    const int_t count = pageCount();
    if (page < 0)
        page = 0;
    if (page >= count)
        page = count - 1;

    refreshRowLabels();
    controlList[fixedOptionRows() + rowsPerPage]->enabled = page > 0;
    controlList[fixedOptionRows() + rowsPerPage + 1]->enabled = page + 1 < count;
    syncLegacySelection();
}

void LegacyControlsScreen::beginCapture(int_t visibleRow)
{
    const int_t rowIndex = page * rowsPerPage + visibleRow;
    if (rowIndex < 0 || rowIndex >= static_cast<int_t>(rows.size()))
        return;

    captureRow = rowIndex;
    platformSetPadRebindExclusive(true);
    refreshRowLabels();
}

void LegacyControlsScreen::cancelCapture()
{
    if (captureRow < 0)
        return;
    captureRow = -1;
    platformSetPadRebindExclusive(false);
    refreshRowLabels();
}

void LegacyControlsScreen::applyCapturedKey(int_t key)
{
    if (captureRow < 0 || captureRow >= static_cast<int_t>(rows.size()))
        return;
    if (!legacyControlsApplyCapturedKey(settings, rows[captureRow], key))
        return;

    captureRow = -1;
    platformSetPadRebindExclusive(false);
    refreshRowLabels();
}

void LegacyControlsScreen::resetDefaults()
{
    cancelCapture();
    if (settings == nullptr)
        return;
    settings->resetControlBindingsToDefaults();
    initGui();
}

void LegacyControlsScreen::actionPerformed(GuiButton *button)
{
    if (button == nullptr || !button->enabled)
        return;

    if (button->id == BUTTON_INVERT_CONTROLS)
    {
        settings->setOptionValue(EnumOptions::INVERT_MOUSE, 1);
        invertControlsCheckbox->setChecked(settings->invertMouse);
        return;
    }
#if !PLATFORM_PS2
    if (button->id >= BUTTON_ROW_BASE && button->id < BUTTON_ROW_BASE + rowsPerPage)
    {
        beginCapture(button->id - BUTTON_ROW_BASE);
        return;
    }
#endif
    if (button->id == 7105)
    {
#if PLATFORM_PS2
        cancelCapture();
        settings->saveOptions();
        mc->displayGuiScreen(new LegacyControllerLayoutScreen(this, settings, backgroundMode));
#else
        settings->saveOptions();
        mc->displayGuiScreen(new LegacyControlsScreen(this, settings, backgroundMode, true));
#endif
        return;
    }
#if !PLATFORM_PS2
    if (button->id == BUTTON_PREVIOUS)
    {
        --page;
        rebuildPage();
        return;
    }
    if (button->id == BUTTON_NEXT)
    {
        ++page;
        rebuildPage();
        return;
    }
    if (button->id == BUTTON_RESET)
    {
        resetDefaults();
        return;
    }
#endif
#if PLATFORM_PS2
    if (button->id == BUTTON_DEADZONE)
    {
        cancelCapture();
        settings->saveOptions();
        mc->displayGuiScreen(new GuiDeadzoneSettings(this, settings));
        return;
    }
#endif
    if (button->id == BUTTON_BACK)
    {
        cancelCapture();
        returnToParent();
    }
}

void LegacyControlsScreen::keyTyped(char_t c, int_t key)
{
    if (captureRow >= 0)
    {
        if (reservedCaptureKey(key))
        {
            cancelCapture();
#if PLATFORM_PS2 || PLATFORM_WII
            // The reserved Back button is also a menu-navigation edge. Consume
            // the same latched press so it cannot immediately close Controls.
            (void)platformTextInputSnapshot(platformMenuPad());
#endif
            return;
        }
        applyCapturedKey(key);
        return;
    }
    LegacyOptionsScreen::keyTyped(c, key);
}

void LegacyControlsScreen::mouseClicked(int_t x, int_t y, int_t button)
{
#if !PLATFORM_PS2 && !PLATFORM_WII
    if (captureRow >= 0)
    {
        applyCapturedKey(-100 + button);
        return;
    }
#else
    if (captureRow >= 0)
        return;
#endif
    LegacyOptionsScreen::mouseClicked(x, y, button);
}

void LegacyControlsScreen::updateScreen()
{
    if (captureRow >= 0)
    {
        GuiScreen::updateScreen();
        syncLegacySelection();
        return;
    }
    LegacyOptionsScreen::updateScreen();
}

void LegacyControlsScreen::drawScreen(int_t mouseX, int_t mouseY, float_t partialTick)
{
    drawLegacyBackground(partialTick);
    if (editingLayout)
        drawLayoutArtwork();
    updateLegacyPointerHover(mouseX, mouseY);
    GuiScreen::drawScreen(mouseX, mouseY, partialTick);
}

void LegacyControlsScreen::onGuiClosed()
{
    platformSetPadRebindExclusive(false);
    captureRow = -1;
}

int_t LegacyControlsScreen::fixedOptionRows() const
{
    if (editingLayout) return 0;
#if PLATFORM_PS2
    return 3;
#else
    return 2;
#endif
}

void LegacyControlsScreen::drawLayoutArtwork()
{
    const int_t active = captureRow >= 0 ? captureRow : page * rowsPerPage + selectedControlIndex;
    std::string title = uiText("Edit Layout");
    if (active >= page * rowsPerPage && active < std::min<int_t>(rows.size(), (page + 1) * rowsPerPage))
        title = rows[active].label + ": " + (captureRow >= 0 ? capturePrompt() : legacyControlsBindingLabel(settings, rows[active]));
    title = fontRenderer->trimStringToWidth(title, legacyLayout.contentWidth);
    drawCenteredString(fontRenderer, title, width / 2, legacyLayout.rowY(0), 0x404040);
    if (!artworkAvailable || !mc || !mc->renderEngine) return;
#if PLATFORM_WII
    if (rows.empty() || rows[page * rowsPerPage].wiiFamily != LegacyControlsWiiFamily::Wiimote) return;
#endif
    const int_t texture = mc->renderEngine->getTexture(LAYOUT_ART);
    int_t tw = 0, th = 0;
    if (texture < 0 || !mc->renderEngine->getTextureDimensions(texture, &tw, &th) || tw <= 0 || th <= 0) return;
    const int_t columnRows = (rowsPerPage + 1) / 2;
    const int_t areaTop = legacyLayout.rowY(1);
    const int_t areaHeight = legacyLayout.rowY(columnRows + 1) - areaTop - 2;
    const float scale = std::min(96.0f / tw, static_cast<float>(areaHeight) / th);
    const int_t w = std::max<int_t>(1, tw * scale), h = std::max<int_t>(1, th * scale);
    const int_t x = width / 2 - w / 2, y = areaTop + (areaHeight - h) / 2;
    mc->renderEngine->bindTexture(texture);
    renderEnable(RenderCapability::Texture2D);
    renderEnable(RenderCapability::Blend);
    renderBlendFunc(RenderBlendFactor::SrcAlpha, RenderBlendFactor::OneMinusSrcAlpha);
    renderColor4f(1, 1, 1, 1);
    Tessellator &t = Tessellator::instance;
    t.startDrawingQuads();
    t.addVertexWithUV(x, y + h, 0, 0, 1);
    t.addVertexWithUV(x + w, y + h, 0, 1, 1);
    t.addVertexWithUV(x + w, y, 0, 1, 0);
    t.addVertexWithUV(x, y, 0, 0, 0);
    t.draw();
    renderDisable(RenderCapability::Blend);
}
