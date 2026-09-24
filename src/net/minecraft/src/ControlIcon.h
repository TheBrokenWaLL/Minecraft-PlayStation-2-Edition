#pragma once
#include "java/Type.h"
#include <string>
class Minecraft;
class FontRenderer;

// Console icons share a 4x4 atlas; desktop icons retain individual textures.
struct ControlIcon
{
    int_t texture = -1;
    int_t cell = 0;
    bool operator!=(const ControlIcon &other) const
    { return texture != other.texture || cell != other.cell; }
};
ControlIcon controlIconTexture(Minecraft *mc, const std::string &label);
void drawControlIcon(Minecraft *mc, ControlIcon icon, int_t x, int_t y);
void drawControlHintRow(Minecraft *mc, int_t width, int_t y,
    const std::string *buttons, const std::string *actions, int_t count);

inline std::string buttonLabelWithoutEllipsis(std::string label)
{
    for (const std::string suffix : {std::string(" (...)"), std::string("(...)"),
            std::string("..."), std::string("\xe2\x80\xa6")})
        if (label.size() >= suffix.size() &&
            label.compare(label.size() - suffix.size(), suffix.size(), suffix) == 0)
            label.erase(label.size() - suffix.size());
    while (!label.empty() && label.back() == ' ') label.pop_back();
    return label;
}
