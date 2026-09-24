#include "ControlIcon.h"
#include "Minecraft.h"
#include "FontRenderer.h"
#include "RenderEngine.h"
#include "Tessellator.h"
#include "platform/PlatformConfig.h"
#include "platform/RenderAPI.h"
#include <algorithm>
#if !PLATFORM_PS2 && !PLATFORM_WII
#include <map>
#endif

ControlIcon controlIconTexture(Minecraft *mc, const std::string &label)
{
    if (!mc || !mc->renderEngine || !mc->fontRenderer || label.empty()) return {};
#if PLATFORM_PS2 || PLATFORM_WII
    struct Entry { const char *label; int_t cell; };
#if PLATFORM_PS2
    constexpr const char *path = "/gui/buttons_ps2.png";
    static constexpr Entry entries[] = {
        {"Cross", 0}, {"X", 0}, {"Circle", 1}, {"O", 1},
        {"Square", 2}, {"Triangle", 3}, {"D-Pad", 4},
        {"L1", 5}, {"R1", 6}, {"L2", 7}, {"R2", 8},
        {"L3", 9}, {"R3", 10}, {"Select", 11}, {"Start", 12}
    };
#else
    constexpr const char *path = "/gui/buttons_wii.png";
    static constexpr Entry entries[] = {
        {"1", 0}, {"2", 1}, {"A", 2}, {"B", 3},
        {"D-Pad", 4}, {"L", 5}, {"R", 6}, {"-", 7},
        {"+", 8}, {"Z", 9}, {"Nun-Z", 9}
    };
#endif
    int_t cell = -1;
    for (const Entry &entry : entries)
        if (label == entry.label) { cell = entry.cell; break; }
    if (cell < 0) return {};
    static RenderEngine *owner = nullptr;
    static unsigned revision = 0;
    static bool exists = false;
    const unsigned current = mc->fontRenderer->getTextCacheRevision();
    if (owner != mc->renderEngine || revision != current)
    {
        owner = mc->renderEngine; revision = current;
        exists = owner->hasResource(path);
    }
    if (!exists) return {};
    // RenderEngine owns and caches the texture, including pack reloads.
    const int_t texture = owner->getTexture(path);
    int_t w = 0, h = 0;
    if (texture < 0 || !owner->getTextureDimensions(texture, &w, &h) ||
        w <= 0 || w != h || w % 4 != 0) return {};
    return {texture, cell};
#else
    static RenderEngine *owner = nullptr;
    static unsigned revision = 0;
    struct Resource { std::string path; bool exists; };
    static std::map<std::string, Resource> available;
    const unsigned current = mc->fontRenderer->getTextCacheRevision();
    if (owner != mc->renderEngine || revision != current)
    {
        available.clear(); owner = mc->renderEngine; revision = current;
    }
#if PLATFORM_PS2
    const char *prefix = "/gui/controls/ps2/";
#elif PLATFORM_WII
    const char *prefix = "/gui/controls/wii/";
#else
    const char *prefix = "/gui/controls/keyboard/";
#endif
    auto it = available.find(label);
    if (it == available.end())
    {
        std::string path(prefix);
        // Punctuation-only button names otherwise collide at "_".
        const std::string iconLabel = label == "+" ? "plus" : label == "-" ? "minus" : label == "Enter" ? "return" : label;
        for (unsigned char c : iconLabel)
        {
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            path += (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? char(c) : '_';
        }
        path += ".png";
        if (available.size() >= 128) available.clear();
        const bool exists = owner->hasResource(path);
        it = available.emplace(label, Resource{path, exists}).first;
    }
    if (!it->second.exists) return {};
    const int_t id = owner->getTexture(it->second.path);
    int_t w = 0, h = 0;
    return {id >= 0 && owner->getTextureDimensions(id, &w, &h) && w > 0 && h > 0 ? id : -1, 0};
#endif
}

void drawControlIcon(Minecraft *mc, ControlIcon icon, int_t x, int_t y)
{
    if (!mc || !mc->renderEngine || icon.texture < 0) return;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
#if PLATFORM_PS2 || PLATFORM_WII
    int_t w = 0, h = 0;
    if (!mc->renderEngine->getTextureDimensions(icon.texture, &w, &h) || w <= 0 || h <= 0) return;
    // Sample inside the cell edges so filtering cannot bleed into its neighbors.
    u0 = (icon.cell % 4) * 0.25f + 0.5f / w;
    v0 = (icon.cell / 4) * 0.25f + 0.5f / h;
    u1 = (icon.cell % 4 + 1) * 0.25f - 0.5f / w;
    v1 = (icon.cell / 4 + 1) * 0.25f - 0.5f / h;
#endif
    mc->renderEngine->bindTexture(icon.texture);
    renderEnable(RenderCapability::Texture2D);
    renderEnable(RenderCapability::Blend);
    renderBlendFunc(RenderBlendFactor::SrcAlpha, RenderBlendFactor::OneMinusSrcAlpha);
    renderColor4f(1, 1, 1, 1);
    Tessellator &t = Tessellator::instance;
    t.startDrawingQuads();
    t.addVertexWithUV(x, y + 12, 0, u0, v1);
    t.addVertexWithUV(x + 12, y + 12, 0, u1, v1);
    t.addVertexWithUV(x + 12, y, 0, u1, v0);
    t.addVertexWithUV(x, y, 0, u0, v0);
    t.draw();
}

void drawControlHintRow(Minecraft *mc, int_t width, int_t y,
    const std::string *buttons, const std::string *actions, int_t count)
{
    if (!mc || !mc->fontRenderer || count < 1 || count > 4) return;
    FontRenderer *font = mc->fontRenderer;
    ControlIcon icons[4], secondIcons[4];
    int_t iconWidths[4] = {};
    int_t widths[4], total = 0;
    std::string texts[4];
    const int_t cellLimit = std::max<int_t>(1, (width - 16 - (count - 1) * 6) / count);
    for (int_t i = 0; i < count; ++i)
    {
        icons[i] = controlIconTexture(mc, buttons[i]);
        const auto separator = buttons[i].find('/');
        if (icons[i].texture < 0 && separator != std::string::npos)
        {
            const auto first = controlIconTexture(mc, buttons[i].substr(0, separator));
            const auto second = controlIconTexture(mc, buttons[i].substr(separator + 1));
            if (first.texture >= 0 && second.texture >= 0)
            {
                icons[i] = first;
                secondIcons[i] = second;
            }
        }
        iconWidths[i] = icons[i].texture >= 0 ? (secondIcons[i].texture >= 0 ? 30 : 15) : 0;
        texts[i] = font->trimStringToWidth(icons[i].texture >= 0 ? actions[i] :
            "[" + buttons[i] + "] " + actions[i], std::max<int_t>(1, cellLimit - iconWidths[i]));
        widths[i] = font->getStringWidth(texts[i]) + iconWidths[i];
        total += widths[i];
    }
    int_t x = std::max<int_t>(8, (width - total - (count - 1) * 6) / 2);
    for (int_t i = 0; i < count; ++i)
    {
        if (icons[i].texture >= 0) drawControlIcon(mc, icons[i], x, y - 2);
        if (secondIcons[i].texture >= 0) drawControlIcon(mc, secondIcons[i], x + 15, y - 2);
        font->drawStringWithShadow(texts[i], x + iconWidths[i], y, 0xf0f0f0);
        x += widths[i] + 6;
    }
}
