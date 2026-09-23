#pragma once

#include "legacy/LegacyOptionsScreen.h"
#include <string>

class GuiButton;
class GuiSelectWorld;
class GuiTextField;

class GuiWorldEdit : public LegacyOptionsScreen
{
public:
    GuiWorldEdit(GuiSelectWorld *parent, GameSettings *settings, int_t index,
        const std::string &folder, const std::string &name);
    ~GuiWorldEdit() override;

    void initGui() override;
    void updateScreen() override;
    void drawScreen(int_t x, int_t y, float_t tick) override;
    void mouseClicked(int_t x, int_t y, int_t button) override;
    void keyTyped(char_t c, int_t key) override;
    void onGuiClosed() override;
    void confirmClicked(bool confirmed, int_t id) override;

protected:
    void actionPerformed(GuiButton *button) override;

private:
    enum SelectionIndex
    {
        SELECTION_NAME = 0,
        SELECTION_GAME_MODE = 1,
        SELECTION_DIFFICULTY = 2,
        SELECTION_LOAD = 3,
        SELECTION_DELETE = 4,
        SELECTION_BACK = 5,
        SELECTION_COUNT = 6
    };

    GuiButton *buttonForSelection(int_t index) const;
    int_t selectionForButton(const GuiButton *button) const;
    bool isSelectionAvailable(int_t index) const;
    void syncSelectedControl();
    void updatePointerHover(int_t mouseX, int_t mouseY);
    void selectControl(int_t index);
    void moveSelection(int_t direction);
    bool adjustSelection(int_t direction);
    void activateSelection();
    std::string editedWorldName() const;
    void updateActionButtons();
    bool applyPendingChanges();

    GuiSelectWorld *worldList;
    int_t worldIndex;
    std::string folder;
    std::string name;
    int_t gameType = 0;
    int_t difficulty = 1;
    bool hardcore = false;
    bool initialized = false;
    bool readable = false;

    GuiTextField *nameField = nullptr;
    GuiButton *nameSelector = nullptr;
    GuiButton *modeButton = nullptr;
    GuiButton *difficultySlider = nullptr;
    GuiButton *loadButton = nullptr;
    GuiButton *deleteButton = nullptr;
    GuiButton *backButton = nullptr;
};
