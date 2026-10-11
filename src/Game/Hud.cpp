#if defined(OMD_PROTOTYPE)

#include "Hud.h"

#include <algorithm>
#include <imgui.h>

namespace Game
{
    namespace
    {
        constexpr float kDamageHoldSeconds = 0.45f;
        constexpr float kDamageDrainPerSecond = 0.7f;

        ImU32 FillColor(float fraction)
        {
            if (fraction > 0.5f) return IM_COL32(90, 205, 90, 255);
            if (fraction > 0.25f) return IM_COL32(235, 200, 60, 255);
            return IM_COL32(225, 70, 55, 255);
        }

        void TextWithShadow(ImDrawList* list, float size, ImVec2 pos, ImU32 color, const char* text, float offset)
        {
            ImFont* font = ImGui::GetFont();
            const ImU32 alpha = (color >> IM_COL32_A_SHIFT) & 0xFF;
            list->AddText(font, size, ImVec2(pos.x + offset, pos.y + offset), IM_COL32(0, 0, 0, alpha), text);
            list->AddText(font, size, pos, color, text);
        }
    }

    void DrawHud(HudState& state, const HudInput& input, float deltaSeconds)
    {
        ImDrawList* list = ImGui::GetBackgroundDrawList();
        const ImVec2 screen = ImGui::GetIO().DisplaySize;
        if (screen.x <= 0.0f || screen.y <= 0.0f)
        {
            return;
        }
        const float s = screen.y / 720.0f;
        const float margin = 40.0f * s;
        const float centerGap = 90.0f * s;
        const float top = 30.0f * s;
        const float height = 26.0f * s;
        const float barWidth = screen.x * 0.5f - margin - centerGap * 0.5f;

        for (int side = 0; side < 2; ++side)
        {
            const float fraction =
                std::clamp(static_cast<float>(input.health[side]) / static_cast<float>(std::max(1, input.maxHealth[side])), 0.0f, 1.0f);

            // The pale damage segment holds briefly where health was, then drains to the real value.
            if (fraction < state.lastFraction[side])
            {
                state.holdSeconds[side] = kDamageHoldSeconds;
            }
            state.lastFraction[side] = fraction;
            if (fraction >= state.trail[side])
            {
                state.trail[side] = fraction;
            }
            else if (state.holdSeconds[side] > 0.0f)
            {
                state.holdSeconds[side] -= deltaSeconds;
            }
            else
            {
                state.trail[side] = std::max(fraction, state.trail[side] - kDamageDrainPerSecond * deltaSeconds);
            }

            // The left bar is anchored at its inner (right) end and drains toward the outer
            // edge; the right bar mirrors it.
            const bool left = side == 0;
            const float x0 = left ? margin : screen.x - margin - barWidth;
            const float x1 = x0 + barWidth;
            const auto segment = [&](float f, ImU32 color)
            {
                const float w = barWidth * f;
                if (w <= 0.0f)
                {
                    return;
                }
                if (left)
                {
                    list->AddRectFilled(ImVec2(x1 - w, top), ImVec2(x1, top + height), color);
                }
                else
                {
                    list->AddRectFilled(ImVec2(x0, top), ImVec2(x0 + w, top + height), color);
                }
            };
            list->AddRectFilled(ImVec2(x0, top), ImVec2(x1, top + height), IM_COL32(20, 20, 24, 210));
            segment(state.trail[side], IM_COL32(245, 235, 200, 255));
            segment(fraction, FillColor(fraction));
            list->AddRect(ImVec2(x0, top), ImVec2(x1, top + height), IM_COL32(255, 255, 255, 230), 0.0f, 0, 2.0f * s);

            const float nameSize = 22.0f * s;
            const ImVec2 nameExtent = ImGui::GetFont()->CalcTextSizeA(nameSize, 10000.0f, 0.0f, input.name[side]);
            const ImVec2 namePos(left ? x0 : x1 - nameExtent.x, top + height + 6.0f * s);
            TextWithShadow(list, nameSize, namePos, IM_COL32(255, 255, 255, 255), input.name[side], 2.0f * s);
        }

        if (input.bannerVisible && input.bannerAlpha > 0.0f)
        {
            const ImU32 alpha = static_cast<ImU32>(std::clamp(input.bannerAlpha, 0.0f, 1.0f) * 255.0f);
            const float titleSize = 170.0f * s;
            const ImVec2 titleExtent = ImGui::GetFont()->CalcTextSizeA(titleSize, 10000.0f, 0.0f, input.bannerTitle);
            const ImVec2 titlePos((screen.x - titleExtent.x) * 0.5f, screen.y * 0.28f);
            TextWithShadow(list, titleSize, titlePos, IM_COL32(255, 70, 50, alpha), input.bannerTitle, 5.0f * s);

            if (input.bannerSubtitle != nullptr && input.bannerSubtitle[0] != '\0')
            {
                const float subSize = 44.0f * s;
                const ImVec2 subExtent = ImGui::GetFont()->CalcTextSizeA(subSize, 10000.0f, 0.0f, input.bannerSubtitle);
                const ImVec2 subPos((screen.x - subExtent.x) * 0.5f, titlePos.y + titleExtent.y + 8.0f * s);
                TextWithShadow(list, subSize, subPos, IM_COL32(255, 255, 255, alpha), input.bannerSubtitle, 3.0f * s);
            }
        }
    }
}

#endif // OMD_PROTOTYPE
