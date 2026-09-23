#include "ControlIcon.h"
#include "Minecraft.h"
#include "FontRenderer.h"
#include "RenderEngine.h"
#include "Tessellator.h"
#include "platform/PlatformConfig.h"
#include "platform/RenderAPI.h"
#include <algorithm>
#include <map>

int_t controlIconTexture(Minecraft *mc, const std::string &label)
{
    if (!mc || !mc->renderEngine || !mc->fontRenderer || label.empty()) return -1;
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
        const std::string iconLabel = label == "+" ? "plus" : label == "-" ? "minus" : label;
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
    if (!it->second.exists) return -1;
    const int_t id = owner->getTexture(it->second.path);
    int_t w = 0, h = 0;
    return id >= 0 && owner->getTextureDimensions(id, &w, &h) && w > 0 && h > 0 ? id : -1;
}

void drawControlIcon(Minecraft *mc, int_t texture, int_t x, int_t y)
{
    mc->renderEngine->bindTexture(texture);
    renderEnable(RenderCapability::Texture2D);
    renderEnable(RenderCapability::Blend);
    renderBlendFunc(RenderBlendFactor::SrcAlpha, RenderBlendFactor::OneMinusSrcAlpha);
    renderColor4f(1, 1, 1, 1);
    Tessellator &t = Tessellator::instance;
    t.startDrawingQuads();
    t.addVertexWithUV(x, y + 12, 0, 0, 1);
    t.addVertexWithUV(x + 12, y + 12, 0, 1, 1);
    t.addVertexWithUV(x + 12, y, 0, 1, 0);
    t.addVertexWithUV(x, y, 0, 0, 0);
    t.draw();
}

void drawControlHintRow(Minecraft *mc, int_t width, int_t y,
    const std::string *buttons, const std::string *actions, int_t count)
{
    if (!mc || !mc->fontRenderer || count < 1 || count > 4) return;
    FontRenderer *font = mc->fontRenderer;
    int_t icons[4], widths[4], total = 0;
    std::string texts[4];
    const int_t cellLimit = std::max<int_t>(1, (width - 16 - (count - 1) * 6) / count);
    for (int_t i = 0; i < count; ++i)
    {
        icons[i] = controlIconTexture(mc, buttons[i]);
        texts[i] = font->trimStringToWidth(icons[i] >= 0 ? actions[i] :
            "[" + buttons[i] + "] " + actions[i], std::max<int_t>(1, cellLimit - (icons[i] >= 0 ? 15 : 0)));
        widths[i] = font->getStringWidth(texts[i]) + (icons[i] >= 0 ? 15 : 0);
        total += widths[i];
    }
    int_t x = std::max<int_t>(8, (width - total - (count - 1) * 6) / 2);
    for (int_t i = 0; i < count; ++i)
    {
        if (icons[i] >= 0) drawControlIcon(mc, icons[i], x, y - 2);
        font->drawStringWithShadow(texts[i], x + (icons[i] >= 0 ? 15 : 0), y, 0xf0f0f0);
        x += widths[i] + 6;
    }
}
