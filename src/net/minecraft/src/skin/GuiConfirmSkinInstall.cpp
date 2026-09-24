#include "GuiConfirmSkinInstall.h"
#include "GuiSkinSelector.h"
#include "SkinManager.h"
#include "GuiButton.h"
#include "Minecraft.h"
#include "FontRenderer.h"
#include "SoundManager.h"
#include "RenderEngine.h"
#include "platform/Storage.h"
#include "platform/RenderAPI.h"
#include "Tessellator.h"
#include "stb_image.h"

#if PLATFORM_PS2 || PLATFORM_WII
#include "platform/Input.h"
#endif

#include "util/Memory.h"
#include <cstring>

GuiConfirmSkinInstall::GuiConfirmSkinInstall(GuiScreen *parent, const std::string &source, const std::string &name)
    : parentScreen(parent)
    , sourcePath(source)
    , skinName(name)
    , isError(false)
    , previewTextureId(-1)
{
}

void GuiConfirmSkinInstall::initGui()
{
    controlList.clear();

    controlList.push_back(new GuiButton(1, width / 2 - 155, height / 4 + 115, 150, 20, "Install Skin"));
    controlList.push_back(new GuiButton(2, width / 2 + 5, height / 4 + 115, 150, 20, "Cancel"));

    // Pre-load texture for front preview if not already loaded
    if (previewTextureId < 0 && mc != nullptr && mc->renderEngine != nullptr)
    {
        std::vector<unsigned char> data;
        if (PlatformStorage::readFile(sourcePath, data) && !data.empty())
        {
            int w = 0, h = 0, comp = 0;
            unsigned char *rgba = stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &comp, 4);
            if (rgba != nullptr)
            {
                std::unique_ptr<unsigned char[]> pixels = Util::make_unique<unsigned char[]>(w * h * 4);
                std::memcpy(pixels.get(), rgba, w * h * 4);
                stbi_image_free(rgba);
                BufferedImage img(w, h, std::move(pixels));
                previewTextureId = mc->renderEngine->allocateAndSetupTexture(&img);
            }
        }
    }
}

void GuiConfirmSkinInstall::actionPerformed(GuiButton *button)
{
    if (!button->enabled)
        return;

    if (button->id == 1) // Install
    {
        std::string err;
        bool ok = SkinManager::installCustomSkin(sourcePath, skinName, err);
        if (ok)
        {
            if (mc != nullptr && mc->sndManager != nullptr)
                mc->sndManager->playSoundFX("random.click", 1.0f, 1.0f);

            if (previewTextureId >= 0 && mc != nullptr && mc->renderEngine != nullptr)
            {
                mc->renderEngine->deleteTexture(previewTextureId);
                previewTextureId = -1;
            }

            // Navigate directly to GuiSkinSelector so the new skin is ready in the carousel
            mc->displayGuiScreen(new GuiSkinSelector(nullptr));
        }
        else
        {
            statusMessage = "Error: " + err;
            isError = true;
        }
    }
    else if (button->id == 2) // Cancel
    {
        if (previewTextureId >= 0 && mc != nullptr && mc->renderEngine != nullptr)
        {
            mc->renderEngine->deleteTexture(previewTextureId);
            previewTextureId = -1;
        }
        mc->displayGuiScreen(parentScreen);
    }
}

void GuiConfirmSkinInstall::keyTyped(char_t c, int_t key)
{
    if (key == 1) // ESC
    {
        if (previewTextureId >= 0 && mc != nullptr && mc->renderEngine != nullptr)
        {
            mc->renderEngine->deleteTexture(previewTextureId);
            previewTextureId = -1;
        }
        mc->displayGuiScreen(parentScreen);
        return;
    }
    GuiScreen::keyTyped(c, key);
}

void GuiConfirmSkinInstall::handleSpecializedMenuInput()
{
#if PLATFORM_PS2 || PLATFORM_WII
    const PlatformTextInputSnapshot pad = platformTextInputSnapshot(platformMenuPad());
    if (!pad.connected)
        return;

    if ((pad.pressed & PLATFORM_TEXT_TYPE) != 0) // Cross / Install
    {
        if (!controlList.empty())
            actionPerformed(controlList[0]);
    }
    else if ((pad.pressed & (PLATFORM_TEXT_BACK | PLATFORM_TEXT_CLOSE)) != 0) // Circle / Cancel
    {
        if (previewTextureId >= 0 && mc != nullptr && mc->renderEngine != nullptr)
        {
            mc->renderEngine->deleteTexture(previewTextureId);
            previewTextureId = -1;
        }
        mc->displayGuiScreen(parentScreen);
    }
#endif
}

void GuiConfirmSkinInstall::drawFrontPreview(float x, float y, float w, float h)
{
    if (previewTextureId < 0 || mc == nullptr || mc->renderEngine == nullptr)
        return;

    mc->renderEngine->bindTexture(previewTextureId);
    renderEnable(RenderCapability::Texture2D);
    renderEnable(RenderCapability::Blend);
    renderBlendFunc(RenderBlendFactor::SrcAlpha, RenderBlendFactor::OneMinusSrcAlpha);
    renderColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    int_t tw = 64, th = 32;
    mc->renderEngine->getTextureDimensions(previewTextureId, &tw, &th);
    if (th <= 0) th = 32;

    const float uScale = 1.0f / 64.0f;
    const float vScale = 1.0f / static_cast<float>(th);
    const float unitW = w / 16.0f;
    const float unitH = h / 32.0f;

    Tessellator &tess = Tessellator::instance;
    tess.startDrawingQuads();

    auto drawQuad = [&](float qx, float qy, float qw, float qh, float su0, float sv0, float su1, float sv1) {
        tess.addVertexWithUV(qx,      qy + qh, zLevel, su0, sv1);
        tess.addVertexWithUV(qx + qw, qy + qh, zLevel, su1, sv1);
        tess.addVertexWithUV(qx + qw, qy,      zLevel, su1, sv0);
        tess.addVertexWithUV(qx,      qy,      zLevel, su0, sv0);
    };

    // 1. Head front (8,8 to 16,16)
    drawQuad(x + 4.0f * unitW, y, 8.0f * unitW, 8.0f * unitH, 8.0f * uScale, 8.0f * vScale, 16.0f * uScale, 16.0f * vScale);

    // 2. Hat overlay (40,8 to 48,16)
    drawQuad(x + 3.5f * unitW, y - 0.5f * unitH, 9.0f * unitW, 9.0f * unitH, 40.0f * uScale, 8.0f * vScale, 48.0f * uScale, 16.0f * vScale);

    // 3. Torso front (20,20 to 28,32)
    drawQuad(x + 4.0f * unitW, y + 8.0f * unitH, 8.0f * unitW, 12.0f * unitH, 20.0f * uScale, 20.0f * vScale, 28.0f * uScale, 32.0f * vScale);

    // 4. Right Arm front (44,20 to 48,32)
    drawQuad(x, y + 8.0f * unitH, 4.0f * unitW, 12.0f * unitH, 44.0f * uScale, 20.0f * vScale, 48.0f * uScale, 32.0f * vScale);

    // 5. Left Arm front
    if (th == 64)
        drawQuad(x + 12.0f * unitW, y + 8.0f * unitH, 4.0f * unitW, 12.0f * unitH, 36.0f * uScale, 52.0f * vScale, 40.0f * uScale, 64.0f * vScale);
    else
        drawQuad(x + 12.0f * unitW, y + 8.0f * unitH, 4.0f * unitW, 12.0f * unitH, 48.0f * uScale, 20.0f * vScale, 44.0f * uScale, 32.0f * vScale);

    // 6. Right Leg front (4,20 to 8,32)
    drawQuad(x + 4.0f * unitW, y + 20.0f * unitH, 4.0f * unitW, 12.0f * unitH, 4.0f * uScale, 20.0f * vScale, 8.0f * uScale, 32.0f * vScale);

    // 7. Left Leg front
    if (th == 64)
        drawQuad(x + 8.0f * unitW, y + 20.0f * unitH, 4.0f * unitW, 12.0f * unitH, 20.0f * uScale, 52.0f * vScale, 24.0f * uScale, 64.0f * vScale);
    else
        drawQuad(x + 8.0f * unitW, y + 20.0f * unitH, 4.0f * unitW, 12.0f * unitH, 8.0f * uScale, 20.0f * vScale, 4.0f * uScale, 32.0f * vScale);

    tess.draw();
}

void GuiConfirmSkinInstall::drawScreen(int_t mouseX, int_t mouseY, float_t partialTick)
{
    drawDefaultBackground();

    drawCenteredString(fontRenderer, "Install Custom Skin", width / 2, height / 4 - 20, 0xFFFFFF);
    drawCenteredString(fontRenderer, skinName, width / 2, height / 4 - 4, 0xFFFF55);

    // Draw front preview of the skin
    float pw = 32.0f;
    float ph = 64.0f;
    float px = static_cast<float>(width / 2) - pw * 0.5f;
    float py = static_cast<float>(height / 4 + 20);

    drawRect(static_cast<int_t>(px - 6), static_cast<int_t>(py - 4),
             static_cast<int_t>(px + pw + 6), static_cast<int_t>(py + ph + 4), 0xFF282828);
    drawFrontPreview(px, py, pw, ph);

    drawCenteredString(fontRenderer, "Will be saved to: " + SkinManager::getSkinsDir(), width / 2, height / 4 + 92, 0x888888);

    if (!statusMessage.empty())
    {
        int color = isError ? 0xFF5555 : 0x55FF55;
        drawCenteredString(fontRenderer, statusMessage, width / 2, height / 4 + 145, color);
    }

    GuiScreen::drawScreen(mouseX, mouseY, partialTick);
}
