#pragma once

#include "net/minecraft/src/GuiScreen.h"
#include <string>
#include <vector>

class GuiButton;
class GuiSlotLoadSkins;

struct DiscoveredSkin
{
    std::string filePath;
    std::string fileName;
    std::string displayName;
    std::string fileSizeStr;
    int width = 0;
    int height = 0;
};

class GuiLoadSkinsList : public GuiScreen
{
public:
    enum class Source
    {
        USB,
        Device
    };

    GuiLoadSkinsList(GuiScreen *parent, Source source);
    ~GuiLoadSkinsList() override;

    void initGui() override;
    void drawScreen(int_t mouseX, int_t mouseY, float_t partialTick) override;
    void actionPerformed(GuiButton *button) override;
    void keyTyped(char_t c, int_t key) override;

    const std::vector<DiscoveredSkin>& getSkins() const { return availableSkins; }
    int getSelectedIndex() const { return selectedIndex; }
    void setSelectedIndex(int idx);

protected:
    bool allowsPlatformPointerInput() const override { return true; }
    void handleSpecializedMenuInput() override;

private:
    void scanSkins();
    void drawFrontPreview(float x, float y, float w, float h);

    GuiScreen *parentScreen;
    Source loadSource;
    GuiSlotLoadSkins *slotList;
    std::vector<DiscoveredSkin> availableSkins;
    int selectedIndex;

    int previewTextureId;
    std::string previewLoadedPath;

    std::string screenTitle;
    std::string emptyMessage1;
    std::string emptyMessage2;

    friend class GuiSlotLoadSkins;
};
