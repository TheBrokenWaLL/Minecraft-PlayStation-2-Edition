#include "GuiLoadSkinsList.h"
#include "GuiConfirmSkinInstall.h"
#include "GuiSlot.h"
#include "GuiButton.h"
#include "Minecraft.h"
#include "FontRenderer.h"
#include "SoundManager.h"
#include "RenderEngine.h"
#include "platform/Storage.h"
#include "platform/RenderAPI.h"
#include "Tessellator.h"
#include "stb_image.h"

#ifdef PS2_PLATFORM
#include "ps2/storage/assets/Ps2Assets.h"
#endif

#if PLATFORM_PS2 || PLATFORM_WII
#include "platform/Input.h"
#endif

#include <algorithm>
#include <cstring>
#include "util/Memory.h"

class GuiSlotLoadSkins : public GuiSlot
{
public:
    GuiSlotLoadSkins(GuiLoadSkinsList *parentScreen)
        : GuiSlot(parentScreen->mc, parentScreen->width / 2 + 10, parentScreen->height, 36, parentScreen->height - 38, 30)
        , parent(parentScreen)
    {
        setShowSelectionBox(true);
    }

    int_t getSize() override
    {
        return static_cast<int_t>(parent->getSkins().size());
    }

    void elementClicked(int_t index, bool doubleClicked) override
    {
        const auto &skins = parent->getSkins();
        if (index >= 0 && index < static_cast<int_t>(skins.size()))
        {
            parent->setSelectedIndex(index);

            if (parent->mc != nullptr && parent->mc->sndManager != nullptr)
                parent->mc->sndManager->playSoundFX("random.click", 1.0f, 1.0f);

            if (doubleClicked)
            {
                parent->actionPerformed(new GuiButton(1, 0, 0, 0, 0, ""));
            }
        }
    }

    bool isSelected(int_t index) override
    {
        return index == parent->getSelectedIndex();
    }

    int_t getContentHeight() override
    {
        return getSize() * 30;
    }

    void drawBackground() override
    {
    }

    void drawSlot(int_t slotIndex, int_t x, int_t y, int_t height, Tessellator *tess) override
    {
        (void)height;
        (void)tess;

        const auto &skins = parent->getSkins();
        if (slotIndex < 0 || slotIndex >= static_cast<int_t>(skins.size()))
            return;

        const DiscoveredSkin &skin = skins[slotIndex];
        FontRenderer *font = parent->mc->fontRenderer;
        if (font == nullptr)
            return;

        font->drawStringWithShadow(skin.displayName, x + 6, y + 3, 0xFFFFFF);

        std::string dimStr = std::to_string(skin.width) + "x" + std::to_string(skin.height);
        std::string sub = dimStr + " (" + skin.fileSizeStr + ")";
        font->drawStringWithShadow(sub, x + 6, y + 15, 0x888888);
    }

private:
    GuiLoadSkinsList *parent;
};

GuiLoadSkinsList::GuiLoadSkinsList(GuiScreen *parent, Source source)
    : parentScreen(parent)
    , loadSource(source)
    , slotList(nullptr)
    , selectedIndex(-1)
    , previewTextureId(-1)
{
    if (loadSource == Source::Device)
    {
        screenTitle = "Available Skins (Device)";
        emptyMessage1 = "No .png skins found on device.";
        emptyMessage2 = "Place .png files in the 'skins' folder next to OptiCraft.elf.";
    }
    else
    {
        screenTitle = "Available Skins (USB Storage)";
        emptyMessage1 = "No .png skins found on USB storage.";
        emptyMessage2 = "Place skins in mass:/skins/ or mass:/OptiCraftHeritage/skins/.";
    }
}

GuiLoadSkinsList::~GuiLoadSkinsList()
{
    if (previewTextureId >= 0 && mc != nullptr && mc->renderEngine != nullptr)
    {
        mc->renderEngine->deleteTexture(previewTextureId);
        previewTextureId = -1;
    }

    delete slotList;
    slotList = nullptr;
}

void GuiLoadSkinsList::setSelectedIndex(int idx)
{
    selectedIndex = idx;

    if (!controlList.empty() && controlList[0] != nullptr)
        controlList[0]->enabled = (selectedIndex >= 0 && selectedIndex < static_cast<int>(availableSkins.size()));

    if (selectedIndex < 0 || selectedIndex >= static_cast<int>(availableSkins.size()))
        return;

    const std::string &path = availableSkins[selectedIndex].filePath;
    if (path == previewLoadedPath && previewTextureId >= 0)
        return;

    if (previewTextureId >= 0 && mc != nullptr && mc->renderEngine != nullptr)
    {
        mc->renderEngine->deleteTexture(previewTextureId);
        previewTextureId = -1;
    }

    std::vector<unsigned char> data;
    if (PlatformStorage::readFile(path, data) && !data.empty() && mc != nullptr && mc->renderEngine != nullptr)
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
            previewLoadedPath = path;
        }
    }
}

void GuiLoadSkinsList::scanSkins()
{
    availableSkins.clear();
    selectedIndex = -1;

    std::vector<std::string> scanDirs;
    if (loadSource == Source::Device)
    {
#ifdef PS2_PLATFORM
        std::string inst = Ps2Assets::installDir();
        if (!inst.empty())
        {
            scanDirs.push_back(PlatformStorage::join(inst, "skins"));
            scanDirs.push_back(PlatformStorage::join(inst, "SKINS"));
        }
        scanDirs.push_back("mc0:/OPTCRAFT/skins");
        scanDirs.push_back("cdrom0:/skins");
        scanDirs.push_back("host:skins");
#endif
        scanDirs.push_back("./skins");
        scanDirs.push_back("skins");
    }
    else
    {
        scanDirs.push_back("mass:/OptiCraftHeritage/skins");
        scanDirs.push_back("mass:/skins");
        scanDirs.push_back("mass:/SKINS");
        scanDirs.push_back("mass0:/skins");
        scanDirs.push_back("usb/skins");
    }

    std::vector<std::string> seenNames;

    for (const auto &dir : scanDirs)
    {
        std::vector<std::string> entries;
        if (!PlatformStorage::listPathEntries(dir, entries))
            continue;

        for (const auto &entry : entries)
        {
            if (entry.size() < 5)
                continue;

            std::string lower = entry;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower.compare(lower.size() - 4, 4, ".png") != 0)
                continue;

            if (lower.find("_32.png") != std::string::npos || lower.find("_front.png") != std::string::npos)
                continue;

            std::string baseName = entry.substr(0, entry.size() - 4);
            std::string lowerBase = baseName;
            std::transform(lowerBase.begin(), lowerBase.end(), lowerBase.begin(), ::tolower);
            if (std::find(seenNames.begin(), seenNames.end(), lowerBase) != seenNames.end())
                continue;
            seenNames.push_back(lowerBase);

            std::string fullPath = PlatformStorage::join(dir, entry);

            std::vector<unsigned char> data;
            if (!PlatformStorage::readFile(fullPath, data) || data.empty())
                continue;

            int w = 0, h = 0, comp = 0;
            if (!stbi_info_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &comp))
                continue;

            if (w != 64 || (h != 32 && h != 64))
                continue; // Only valid Minecraft skin dimensions

            DiscoveredSkin skin;
            skin.filePath = fullPath;
            skin.fileName = entry;
            skin.displayName = entry.substr(0, entry.size() - 4);
            skin.width = w;
            skin.height = h;

            size_t kb = (data.size() + 1023) / 1024;
            skin.fileSizeStr = std::to_string(kb) + " KB";

            availableSkins.push_back(skin);
        }
    }

    if (!availableSkins.empty())
        setSelectedIndex(0);
}

void GuiLoadSkinsList::initGui()
{
    controlList.clear();

    scanSkins();

    delete slotList;
    slotList = new GuiSlotLoadSkins(this);

    int_t btnW = 120;
    int_t btnH = 20;
    int_t btnY = height - 28;

    GuiButton *btnInstall = new GuiButton(1, width / 2 - btnW - 10, btnY, btnW, btnH, "Install Skin");
    btnInstall->enabled = (selectedIndex >= 0 && selectedIndex < static_cast<int>(availableSkins.size()));
    controlList.push_back(btnInstall);

    controlList.push_back(new GuiButton(2, width / 2 + 10, btnY, btnW, btnH, "Back"));
}

void GuiLoadSkinsList::actionPerformed(GuiButton *button)
{
    if (!button->enabled)
        return;

    if (button->id == 1) // Install
    {
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(availableSkins.size()))
        {
            const auto &skin = availableSkins[selectedIndex];
            mc->displayGuiScreen(new GuiConfirmSkinInstall(this, skin.filePath, skin.displayName));
        }
    }
    else if (button->id == 2) // Back
    {
        mc->displayGuiScreen(parentScreen);
    }
}

void GuiLoadSkinsList::keyTyped(char_t c, int_t key)
{
    if (key == 1) // ESC
    {
        mc->displayGuiScreen(parentScreen);
        return;
    }
    GuiScreen::keyTyped(c, key);
}

void GuiLoadSkinsList::handleSpecializedMenuInput()
{
#if PLATFORM_PS2 || PLATFORM_WII
    const PlatformTextInputSnapshot pad = platformTextInputSnapshot(platformMenuPad());
    if (!pad.connected)
        return;

    if ((pad.pressed & PLATFORM_TEXT_UP) != 0)
    {
        if (!availableSkins.empty() && selectedIndex > 0)
            setSelectedIndex(selectedIndex - 1);
    }
    else if ((pad.pressed & PLATFORM_TEXT_DOWN) != 0)
    {
        if (!availableSkins.empty() && selectedIndex < static_cast<int>(availableSkins.size()) - 1)
            setSelectedIndex(selectedIndex + 1);
    }

    if ((pad.pressed & PLATFORM_TEXT_TYPE) != 0) // Cross / Confirm
    {
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(availableSkins.size()))
        {
            actionPerformed(controlList[0]);
        }
    }
    else if ((pad.pressed & (PLATFORM_TEXT_BACK | PLATFORM_TEXT_CLOSE)) != 0) // Circle / Cancel
    {
        mc->displayGuiScreen(parentScreen);
    }
#endif
}

void GuiLoadSkinsList::drawFrontPreview(float x, float y, float w, float h)
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

void GuiLoadSkinsList::drawScreen(int_t mouseX, int_t mouseY, float_t partialTick)
{
    drawDefaultBackground();

    if (slotList != nullptr)
        slotList->drawScreen(mouseX, mouseY, partialTick);

    drawCenteredString(fontRenderer, screenTitle, width / 4, 16, 0xFFFFFF);

    if (availableSkins.empty())
    {
        drawCenteredString(fontRenderer, emptyMessage1, width / 4, height / 2 - 10, 0xAAAAAA);
        drawCenteredString(fontRenderer, emptyMessage2, width / 4, height / 2 + 6, 0x777777);
    }

    // Right Side Preview Box
    int_t previewLeft = width / 2 + 20;
    int_t previewRight = width - 20;
    int_t previewTop = 40;
    int_t previewBottom = height - 42;

    drawRect(previewLeft, previewTop, previewRight, previewBottom, 0xFF202020);

    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(availableSkins.size()))
    {
        const auto &skin = availableSkins[selectedIndex];
        int_t cx = (previewLeft + previewRight) / 2;

        drawCenteredString(fontRenderer, skin.displayName, cx, previewTop + 10, 0xFFFF55);

        std::string dimDesc = (skin.height == 64) ? "64x64 (Modern)" : "64x32 (Classic)";
        drawCenteredString(fontRenderer, dimDesc, cx, previewTop + 24, 0xAAAAAA);
        drawCenteredString(fontRenderer, "Size: " + skin.fileSizeStr, cx, previewTop + 36, 0x888888);

        // Draw character standing front
        float pw = 36.0f;
        float ph = 72.0f;
        float px = static_cast<float>(cx) - pw * 0.5f;
        float py = static_cast<float>(previewTop + 54);

        drawRect(static_cast<int_t>(px - 6), static_cast<int_t>(py - 4),
                 static_cast<int_t>(px + pw + 6), static_cast<int_t>(py + ph + 4), 0xFF141414);
        drawFrontPreview(px, py, pw, ph);
    }
    else
    {
        int_t cx = (previewLeft + previewRight) / 2;
        drawCenteredString(fontRenderer, "Select a skin to preview", cx, (previewTop + previewBottom) / 2, 0x777777);
    }

    if (!controlList.empty())
        controlList[0]->enabled = (selectedIndex >= 0 && selectedIndex < static_cast<int>(availableSkins.size()));

    GuiScreen::drawScreen(mouseX, mouseY, partialTick);
}
