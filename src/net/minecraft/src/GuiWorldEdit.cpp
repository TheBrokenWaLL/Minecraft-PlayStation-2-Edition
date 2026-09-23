#include "GuiWorldEdit.h"

#include <algorithm>
#include <memory>

#include "EnumOptions.h"
#include "FontRenderer.h"
#include "GameSettings.h"
#include "GuiButton.h"
#include "GuiSelectWorld.h"
#include "GuiStorageMessage.h"
#include "GuiTextField.h"
#include "GuiTextFieldSelector.h"
#include "GuiYesNo.h"
#include "ISaveFormat.h"
#include "Minecraft.h"
#include "SoundManager.h"
#include "StringTranslate.h"
#include "UiStrings.h"
#include "WorldInfo.h"
#include "java/String.h"
#include "legacy/LegacyGuiButton.h"
#include "legacy/LegacyOptionLabel.h"
#include "legacy/LegacyOptionSlider.h"
#include "pc/lwjgl/Keyboard.h"
#include "platform/Input.h"
#include "platform/PlatformConfig.h"
#ifdef PS2_PLATFORM
#include "ps2/storage/save/Ps2SaveStorage.h"
#endif

namespace
{
class WorldDifficultySlider : public LegacyOptionSlider
{
public:
    WorldDifficultySlider(int_t x, int_t y, int_t w, int_t h, GameSettings *settings, int_t &difficultyValue)
        : LegacyOptionSlider(101, x, y, w, h, settings, EnumOptions::DIFFICULTY),
          difficulty(difficultyValue)
    {
        refreshFromSettings();
    }

protected:
    float_t readValue() const override
    {
        return difficulty / 3.0f;
    }

    void writeValue(float_t value) override
    {
        difficulty = std::max(0, std::min(3, static_cast<int_t>(value * 3.0f + 0.5f)));
    }

    float_t keyboardStep() const override
    {
        return 1.0f / 3.0f;
    }

    std::string buildLabel() const override
    {
        static const char *keys[] = {
            "options.difficulty.peaceful",
            "options.difficulty.easy",
            "options.difficulty.normal",
            "options.difficulty.hard"
        };

        StringTranslate *tr = StringTranslate::getInstance();
        return tr->translateKey("options.difficulty") + ": " + tr->translateKey(keys[difficulty]);
    }

private:
    int_t &difficulty;
};

constexpr int_t WORLD_EDIT_TITLE_OFFSET_Y = 8;

bool pointInside(int_t x, int_t y, int_t left, int_t top, int_t width, int_t height)
{
    return x >= left && y >= top && x < left + width && y < top + height;
}

void drawPanelTitle(FontRenderer *font, const std::string &text, int_t centerX, int_t y)
{
    if (font == nullptr)
        return;

    const int_t x = centerX - font->getStringWidth(text) / 2;
    font->drawString(text, x + 1, y + 1, 0xd0d0d0);
    font->drawString(text, x, y, 0x303030);
}
}

GuiWorldEdit::GuiWorldEdit(GuiSelectWorld *parent, GameSettings *settings, int_t index,
    const std::string &folderValue, const std::string &nameValue)
    : LegacyOptionsScreen(parent, settings), worldList(parent), worldIndex(index),
      folder(folderValue), name(nameValue)
{
}

GuiWorldEdit::~GuiWorldEdit()
{
    delete nameField;
}

void GuiWorldEdit::initGui()
{
    lwjgl::Keyboard::enableRepeatEvents(true);

    if (!initialized)
    {
        std::unique_ptr<WorldInfo> info(mc->getSaveLoader()->getWorldInfo(folder));
        readable = info != nullptr;
        difficulty = settings != nullptr ? settings->difficulty : 1;
        if (info)
        {
            gameType = info->getGameType();
            hardcore = info->isHardcoreModeEnabled();
            if (info->getDifficulty() >= 0)
                difficulty = info->getDifficulty();
            const std::string worldName = info->getWorldName();
            if (!worldName.empty())
                name = worldName;
        }
        difficulty = hardcore ? 3 : std::max(0, std::min(3, difficulty));
        initialized = true;
    }

    const std::string preservedName = nameField != nullptr ? std::string(nameField->getText()) : name;
    delete nameField;
    nameField = nullptr;

    controlList.clear();
    configureLegacyLayout(5, true, LegacyOptionsLayoutPreset::Form);

    const int_t x = legacyLayout.contentX;
    const int_t w = legacyLayout.contentWidth;
    const int_t h = legacyLayout.rowHeight;
    const int_t gap = 4;
    const int_t half = (w - gap) / 2;

    nameField = new GuiTextField(this, fontRenderer, x, legacyLayout.rowY(0), w, h, preservedName);
    nameField->setMaxStringLength(32);
    nameField->setEnabled(readable);
    nameField->setFocused(false);

    nameSelector = new GuiTextFieldSelector(100, x, legacyLayout.rowY(0), w, h);
    nameSelector->enabled = readable;
    controlList.push_back(nameSelector);

    StringTranslate *tr = StringTranslate::getInstance();
    const std::string modeName = hardcore
        ? tr->translateKey("selectWorld.gameMode.hardcore")
        : tr->translateKey(gameType == 0 ? "selectWorld.gameMode.survival" : "selectWorld.gameMode.creative");
    modeButton = new LegacyGuiButton(110, x, legacyLayout.rowY(1), w, h,
        tr->translateKey("selectWorld.gameMode") + " " + modeName);
    modeButton->enabled = false;
    controlList.push_back(modeButton);

    difficultySlider = new WorldDifficultySlider(x, legacyLayout.rowY(2), w, h, settings, difficulty);
    difficultySlider->enabled = readable && !hardcore;
    controlList.push_back(difficultySlider);

    loadButton = new LegacyGuiButton(120, x, legacyLayout.rowY(3), half, h, uiText("Load World"));
    deleteButton = new LegacyGuiButton(121, x + half + gap, legacyLayout.rowY(3), w - half - gap, h,
        uiText("Delete World"));
    controlList.push_back(loadButton);
    controlList.push_back(deleteButton);

    backButton = new LegacyGuiButton(200, x, legacyLayout.rowY(4), w, h, uiText("Back"));
    controlList.push_back(backButton);

    selectedControlIndex = readable ? SELECTION_LOAD : SELECTION_BACK;
    hoveredControlIndex = -1;
    updateActionButtons();
    syncSelectedControl();
}

void GuiWorldEdit::onGuiClosed()
{
    lwjgl::Keyboard::enableRepeatEvents(false);
}

GuiButton *GuiWorldEdit::buttonForSelection(int_t index) const
{
    switch (index)
    {
    case SELECTION_NAME:
        return nameSelector;
    case SELECTION_GAME_MODE:
        return modeButton;
    case SELECTION_DIFFICULTY:
        return difficultySlider;
    case SELECTION_LOAD:
        return loadButton;
    case SELECTION_DELETE:
        return deleteButton;
    case SELECTION_BACK:
        return backButton;
    default:
        return nullptr;
    }
}

int_t GuiWorldEdit::selectionForButton(const GuiButton *button) const
{
    if (button == nameSelector)
        return SELECTION_NAME;
    if (button == modeButton)
        return SELECTION_GAME_MODE;
    if (button == difficultySlider)
        return SELECTION_DIFFICULTY;
    if (button == loadButton)
        return SELECTION_LOAD;
    if (button == deleteButton)
        return SELECTION_DELETE;
    if (button == backButton)
        return SELECTION_BACK;
    return -1;
}

bool GuiWorldEdit::isSelectionAvailable(int_t index) const
{
    switch (index)
    {
    case SELECTION_NAME:
        return nameSelector != nullptr && nameSelector->enabled && nameSelector->enabled2;
    case SELECTION_GAME_MODE:
        return modeButton != nullptr && modeButton->enabled && modeButton->enabled2;
    case SELECTION_DIFFICULTY:
        return difficultySlider != nullptr && difficultySlider->enabled && difficultySlider->enabled2;
    case SELECTION_LOAD:
        return loadButton != nullptr && loadButton->enabled && loadButton->enabled2;
    case SELECTION_DELETE:
        return deleteButton != nullptr && deleteButton->enabled && deleteButton->enabled2;
    case SELECTION_BACK:
        return backButton != nullptr && backButton->enabled && backButton->enabled2;
    default:
        return false;
    }
}

void GuiWorldEdit::syncSelectedControl()
{
    for (GuiButton *button : controlList)
    {
        if (button != nullptr)
            button->setKeyboardSelected(false);
    }

    if (selectedControlIndex != SELECTION_NAME || hoveredControlIndex >= 0)
        nameField->setFocused(false);

    if (hoveredControlIndex >= 0)
        return;

    GuiButton *selected = buttonForSelection(selectedControlIndex);
    if (selected != nullptr)
        selected->setKeyboardSelected(true);
}

void GuiWorldEdit::updatePointerHover(int_t mouseX, int_t mouseY)
{
    int_t hover = -1;
#if PLATFORM_PS2
    (void)mouseX;
    (void)mouseY;
#elif PLATFORM_WII
    if (platformMenuPointerActive())
#endif
    {
#if !PLATFORM_PS2
        if (pointInside(mouseX, mouseY, legacyLayout.contentX, legacyLayout.rowY(0),
            legacyLayout.contentWidth, legacyLayout.rowHeight))
        {
            hover = SELECTION_NAME;
        }
        else
        {
            for (GuiButton *button : controlList)
            {
                if (button == nullptr || !button->enabled2)
                    continue;
                if (pointInside(mouseX, mouseY, button->xPosition, button->yPosition,
                    button->getButtonWidth(), button->getButtonHeight()))
                {
                    hover = selectionForButton(button);
                    break;
                }
            }
        }
#endif
    }

    if (hoveredControlIndex != hover)
    {
        hoveredControlIndex = hover;
        if (hoveredControlIndex >= 0)
            selectedControlIndex = hoveredControlIndex;
        syncSelectedControl();
    }
}

void GuiWorldEdit::selectControl(int_t index)
{
    const int_t clamped = std::max<int_t>(0, std::min<int_t>(index, SELECTION_COUNT - 1));
    if (!isSelectionAvailable(clamped))
        return;

    selectedControlIndex = clamped;
    syncSelectedControl();
}

void GuiWorldEdit::moveSelection(int_t direction)
{
    if (hoveredControlIndex >= 0)
        return;

    const int_t previous = selectedControlIndex;
    int_t candidate = selectedControlIndex;

    if (selectedControlIndex == SELECTION_LOAD || selectedControlIndex == SELECTION_DELETE)
    {
        candidate = direction < 0 ? SELECTION_DIFFICULTY : SELECTION_BACK;
    }
    else
    {
        candidate += direction;
    }

    while (candidate >= 0 && candidate < SELECTION_COUNT)
    {
        if (isSelectionAvailable(candidate))
        {
            selectControl(candidate);
            if (selectedControlIndex != previous && mc != nullptr && mc->sndManager != nullptr)
                mc->sndManager->playSoundFX("random.focus", 1.0f, 1.0f);
            return;
        }
        candidate += direction;
    }
}

bool GuiWorldEdit::adjustSelection(int_t direction)
{
    if (hoveredControlIndex >= 0)
        return false;

    if (selectedControlIndex == SELECTION_LOAD && direction > 0 && isSelectionAvailable(SELECTION_DELETE))
    {
        selectControl(SELECTION_DELETE);
        if (mc != nullptr && mc->sndManager != nullptr)
            mc->sndManager->playSoundFX("random.focus", 1.0f, 1.0f);
        return true;
    }

    if (selectedControlIndex == SELECTION_DELETE && direction < 0 && isSelectionAvailable(SELECTION_LOAD))
    {
        selectControl(SELECTION_LOAD);
        if (mc != nullptr && mc->sndManager != nullptr)
            mc->sndManager->playSoundFX("random.focus", 1.0f, 1.0f);
        return true;
    }

    if (selectedControlIndex != SELECTION_DIFFICULTY)
        return false;

    if (difficultySlider != nullptr && difficultySlider->enabled && difficultySlider->enabled2 &&
        difficultySlider->adjustKeyboard(mc, direction))
    {
        if (mc != nullptr && mc->sndManager != nullptr)
            mc->sndManager->playSoundFX("random.focus", 1.0f, 1.0f);
        updateActionButtons();
        return true;
    }

    return false;
}

void GuiWorldEdit::activateSelection()
{
    const int_t targetIndex = hoveredControlIndex >= 0 ? hoveredControlIndex : selectedControlIndex;
    if (targetIndex < 0)
        return;

    if (targetIndex == SELECTION_NAME)
    {
        if (nameField != nullptr && readable)
            nameField->setFocused(true);
        return;
    }

    GuiButton *button = buttonForSelection(targetIndex);
    if (button == nullptr || !button->enabled || !button->enabled2)
        return;

    if (mc != nullptr && mc->sndManager != nullptr)
        mc->sndManager->playSoundFX("random.action", 1.0f, 1.0f);
    actionPerformed(button);
}

std::string GuiWorldEdit::editedWorldName() const
{
    return String::trimJava(nameField != nullptr ? nameField->getText() : name);
}

void GuiWorldEdit::updateActionButtons()
{
    const bool hasName = !editedWorldName().empty();

    if (nameSelector != nullptr)
        nameSelector->enabled = readable;
    if (loadButton != nullptr)
        loadButton->enabled = readable && hasName;
    if (deleteButton != nullptr)
        deleteButton->enabled = readable;
    if (difficultySlider != nullptr)
        difficultySlider->enabled = readable && !hardcore;
}

bool GuiWorldEdit::applyPendingChanges()
{
    const std::string newName = editedWorldName();
    if (newName.empty())
        return false;

    if (newName == name)
        return true;

#ifdef PS2_PLATFORM
    if (!Ps2SaveStorage::available(Ps2SaveStorage::target()))
    {
        mc->displayGuiScreen(new GuiStorageMessage(this, mc->gameSettings,
            "World storage unavailable. Check the selected device in Game Options."));
        return false;
    }
#endif

    ISaveFormat *fmt = mc != nullptr ? mc->getSaveLoader() : nullptr;
    if (fmt == nullptr)
        return false;

    fmt->renameWorld(folder, newName);
    name = newName;
    return true;
}

void GuiWorldEdit::updateScreen()
{
    GuiScreen::updateScreen();
    if (nameField != nullptr)
        nameField->updateCursorCounter();

    syncSelectedControl();

#if PLATFORM_PS2 || PLATFORM_WII
    if (platformTextInputExclusive())
        return;

    const PlatformTextInputSnapshot pad = platformTextInputSnapshot(platformMenuPad());
#if PLATFORM_PS2
    if ((pad.pressed & (PLATFORM_TEXT_CLOSE | PLATFORM_TEXT_SHIFT)) != 0)
    {
        if (mc != nullptr && mc->sndManager != nullptr)
            mc->sndManager->playSoundFX("random.back", 1.0f, 1.0f);
        returnToParent();
        return;
    }
#endif

    if ((pad.pressed & PLATFORM_TEXT_UP) != 0)
        moveSelection(-1);
    else if ((pad.pressed & PLATFORM_TEXT_DOWN) != 0)
        moveSelection(1);
    else if ((pad.pressed & PLATFORM_TEXT_LEFT) != 0)
        adjustSelection(-1);
    else if ((pad.pressed & PLATFORM_TEXT_RIGHT) != 0)
        adjustSelection(1);

#if PLATFORM_PS2
    if ((pad.pressed & PLATFORM_TEXT_TYPE) != 0)
        activateSelection();
#elif PLATFORM_WII
    if (!platformMenuPointerActive() && (pad.pressed & PLATFORM_TEXT_TYPE) != 0)
        activateSelection();
#endif
#if PLATFORM_WII
    if ((pad.pressed & PLATFORM_TEXT_BACK) != 0)
    {
        if (mc != nullptr && mc->sndManager != nullptr)
            mc->sndManager->playSoundFX("random.back", 1.0f, 1.0f);
        returnToParent();
    }
#endif
#endif
}

void GuiWorldEdit::actionPerformed(GuiButton *button)
{
    if (button == nullptr || !button->enabled)
        return;

    const int_t selection = selectionForButton(button);
    if (selection >= 0)
        selectControl(selection);

    if (button == nameSelector)
    {
        if (nameField != nullptr && readable)
            nameField->setFocused(true);
        return;
    }

    if (button == difficultySlider)
    {
        adjustSelection(1);
        return;
    }

    if (button == loadButton)
    {
        if (applyPendingChanges())
            worldList->loadWorld(worldIndex, difficulty);
        return;
    }

    if (button == deleteButton)
    {
        StringTranslate *tr = StringTranslate::getInstance();
        mc->displayGuiScreen(new GuiYesNo(this,
            tr->translateKey("selectWorld.deleteQuestion"),
            "\"" + editedWorldName() + "\" " + tr->translateKey("selectWorld.deleteWarning"),
            tr->translateKey("selectWorld.deleteButton"),
            tr->translateKey("gui.cancel"), 1));
        return;
    }

    if (button == backButton)
        returnToParent();
}

void GuiWorldEdit::confirmClicked(bool confirmed, int_t id)
{
    if (confirmed && id == 1)
        worldList->deleteWorldFromEditor(worldIndex);
    else
        mc->displayGuiScreen(this);
}

void GuiWorldEdit::keyTyped(char_t c, int_t key)
{
    if (nameField != nullptr && nameField->getFocused())
    {
        if (key == lwjgl::Keyboard::KEY_ESCAPE)
        {
            nameField->setFocused(false);
            hoveredControlIndex = -1;
            syncSelectedControl();
            return;
        }
        if (key == lwjgl::Keyboard::KEY_RETURN || c == '\r')
        {
            nameField->setFocused(false);
            hoveredControlIndex = -1;
            syncSelectedControl();
            moveSelection(1);
            return;
        }

        nameField->textboxKeyTyped(c, key);
        updateActionButtons();
        return;
    }

    if (key == lwjgl::Keyboard::KEY_ESCAPE)
    {
        if (mc != nullptr && mc->sndManager != nullptr)
            mc->sndManager->playSoundFX("random.back", 1.0f, 1.0f);
        returnToParent();
        return;
    }

#if !PLATFORM_PS2 && !PLATFORM_WII
    if (key == lwjgl::Keyboard::KEY_UP)
    {
        moveSelection(-1);
        return;
    }
    if (key == lwjgl::Keyboard::KEY_DOWN || key == lwjgl::Keyboard::KEY_TAB)
    {
        moveSelection(1);
        return;
    }
    if (key == lwjgl::Keyboard::KEY_LEFT)
    {
        adjustSelection(-1);
        return;
    }
    if (key == lwjgl::Keyboard::KEY_RIGHT)
    {
        adjustSelection(1);
        return;
    }
    if (key == lwjgl::Keyboard::KEY_RETURN || c == '\r')
    {
        activateSelection();
        return;
    }
#endif
}

void GuiWorldEdit::mouseClicked(int_t x, int_t y, int_t button)
{
    GuiScreen::mouseClicked(x, y, button);
    if (button != 0)
        return;

    if (nameField != nullptr)
    {
        nameField->mouseClicked(x, y, button);
        if (pointInside(x, y, legacyLayout.contentX, legacyLayout.rowY(0),
            legacyLayout.contentWidth, legacyLayout.rowHeight))
        {
            selectControl(SELECTION_NAME);
            if (readable)
                nameField->setFocused(true);
        }
        else if (nameField->getFocused())
        {
            nameField->setFocused(false);
        }
    }
}

void GuiWorldEdit::drawScreen(int_t mouseX, int_t mouseY, float_t tick)
{
    drawLegacyBackground(tick);

    drawPanelTitle(fontRenderer, uiText("World Edit"), width / 2, legacyLayout.panelY + WORLD_EDIT_TITLE_OFFSET_Y);
    legacyDrawOptionLabel(fontRenderer, StringTranslate::getInstance()->translateKey("selectWorld.enterName"),
        legacyLayout.contentX, legacyLayout.rowY(0) - std::max<int_t>(1, legacyLayout.rowHeight / 2));

    if (nameField != nullptr)
        nameField->drawTextBox();

    updatePointerHover(mouseX, mouseY);
    GuiScreen::drawScreen(mouseX, mouseY, tick);
}
