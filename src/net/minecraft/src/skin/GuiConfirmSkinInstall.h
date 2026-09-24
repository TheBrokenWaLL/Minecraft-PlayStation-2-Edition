#pragma once

#include "net/minecraft/src/GuiScreen.h"
#include <string>

class GuiButton;

class GuiConfirmSkinInstall : public GuiScreen
{
public:
    GuiConfirmSkinInstall(GuiScreen *parent, const std::string &sourcePath, const std::string &skinName);
    ~GuiConfirmSkinInstall() override = default;

    void initGui() override;
    void drawScreen(int_t mouseX, int_t mouseY, float_t partialTick) override;
    void actionPerformed(GuiButton *button) override;
    void keyTyped(char_t c, int_t key) override;

protected:
    bool allowsPlatformPointerInput() const override { return true; }
    void handleSpecializedMenuInput() override;

private:
    void drawFrontPreview(float x, float y, float w, float h);

    GuiScreen *parentScreen;
    std::string sourcePath;
    std::string skinName;
    std::string statusMessage;
    bool isError;
    int previewTextureId;
};
