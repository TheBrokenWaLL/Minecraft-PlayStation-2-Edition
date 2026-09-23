#pragma once
#include "java/Type.h"
#include <string>
class Minecraft;
class FontRenderer;

// Optional pack resources: /gui/controls/{ps2,wii,keyboard}/<label>.png.
// Labels are lower-case ASCII; punctuation/spaces become underscores.
int_t controlIconTexture(Minecraft *mc, const std::string &label);
void drawControlIcon(Minecraft *mc, int_t texture, int_t x, int_t y);
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
