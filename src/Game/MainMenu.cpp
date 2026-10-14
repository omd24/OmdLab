#if defined(OMD_PROTOTYPE)

#include "MainMenu.h"

#include "Foundation/Input.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <imgui.h>

namespace Game
{
    namespace
    {
        constexpr int kItemCount = 3;
        const char* const kItemLabels[kItemCount] = { "PLAY GAME", "OPTIONS", "QUIT" };

        void TextWithShadow(ImDrawList* list, float size, ImVec2 pos, ImU32 color, const char* text, float offset)
        {
            ImFont* font = ImGui::GetFont();
            const ImU32 alpha = (color >> IM_COL32_A_SHIFT) & 0xFF;
            list->AddText(font, size, ImVec2(pos.x + offset, pos.y + offset), IM_COL32(0, 0, 0, alpha), text);
            list->AddText(font, size, pos, color, text);
        }

        ImU32 WithAlpha(int r, int g, int b, float alpha)
        {
            return IM_COL32(r, g, b, static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
        }

        // Darkens the screen edges so the eye stays on the lit part of the picture.
        void DrawVignette(ImDrawList* list, ImVec2 screen, float alpha)
        {
            const ImU32 dark = WithAlpha(0, 0, 0, 0.78f * alpha);
            const ImU32 clear = WithAlpha(0, 0, 0, 0.0f);
            const float band = screen.x * 0.22f;
            const float bandY = screen.y * 0.28f;
            list->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(band, screen.y), dark, clear, clear, dark);
            list->AddRectFilledMultiColor(ImVec2(screen.x - band, 0), ImVec2(screen.x, screen.y), clear, dark, dark, clear);
            list->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(screen.x, bandY), dark, dark, clear, clear);
            list->AddRectFilledMultiColor(ImVec2(0, screen.y - bandY), ImVec2(screen.x, screen.y), clear, clear, dark, dark);
        }
    }

    ContinueResult DrawContinueScreen(ContinueState& state, float deltaSeconds, bool focused)
    {
        ImDrawList* list = ImGui::GetBackgroundDrawList();
        const ImVec2 screen = ImGui::GetIO().DisplaySize;
        if (screen.x <= 0.0f || screen.y <= 0.0f)
        {
            return ContinueResult::None;
        }
        state.time += deltaSeconds;
        state.secondsLeft -= deltaSeconds;
        const float s = screen.y / 720.0f;
        const float fade = std::clamp(state.time / 0.4f, 0.0f, 1.0f);

        bool up = false;
        bool down = false;
        bool confirm = false;
        if (focused)
        {
            const Foundation::GamepadState pad = Foundation::GetGamepadState(0);
            up = Foundation::IsKeyDown(VK_UP) || Foundation::IsKeyDown('W') || Foundation::IsKeyDown(VK_LEFT) || pad.dpadUp || pad.dpadLeft ||
                 pad.leftStickY > 0.6f;
            down = Foundation::IsKeyDown(VK_DOWN) || Foundation::IsKeyDown('S') || Foundation::IsKeyDown(VK_RIGHT) || pad.dpadDown ||
                   pad.dpadRight || pad.leftStickY < -0.6f;
            confirm = Foundation::IsKeyDown(VK_RETURN) || Foundation::IsKeyDown(VK_SPACE) || pad.buttonA;
        }
        if ((up && !state.upWas) || (down && !state.downWas))
        {
            state.selected = 1 - state.selected;
        }
        bool activated = confirm && !state.confirmWas;
        state.upWas = up;
        state.downWas = down;
        state.confirmWas = confirm;

        // Layout: a dimmed backdrop, the title, the counter, the two choices.
        list->AddRectFilled(ImVec2(0, 0), screen, WithAlpha(0, 0, 0, 0.45f * fade));
        ImFont* font = ImGui::GetFont();
        const auto centered = [&](float y, float size, ImU32 color, const char* text, float shadow)
        {
            const ImVec2 extent = font->CalcTextSizeA(size, 10000.0f, 0.0f, text);
            TextWithShadow(list, size, ImVec2((screen.x - extent.x) * 0.5f, y), color, text, shadow);
        };
        centered(screen.y * 0.07f, 78.0f * s, WithAlpha(190, 45, 35, fade), "CONTINUE?", 5.0f * s);

        const int seconds = (std::max)(0, static_cast<int>(std::ceil(state.secondsLeft)));
        char digit[8];
        snprintf(digit, sizeof(digit), "%d", seconds);
        const bool urgent = seconds <= 3;
        centered(screen.y * 0.17f, 150.0f * s, urgent ? WithAlpha(235, 70, 50, fade) : WithAlpha(240, 225, 200, fade), digit, 6.0f * s);

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const float itemSize = 46.0f * s;
        const float itemStep = 66.0f * s;
        const float top = screen.y * 0.44f;
        const char* const labels[2] = { "REMATCH", "CANCEL" };
        int hovered = -1;
        for (int i = 0; i < 2; ++i)
        {
            const ImVec2 extent = font->CalcTextSizeA(itemSize, 10000.0f, 0.0f, labels[i]);
            const float x = (screen.x - extent.x) * 0.5f;
            const float y = top + itemStep * static_cast<float>(i);
            if (focused && mouse.x >= x - 30.0f * s && mouse.x <= x + extent.x + 30.0f * s && mouse.y >= y && mouse.y <= y + extent.y)
            {
                hovered = i;
            }
        }
        if (hovered >= 0 && hovered != state.selected && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f))
        {
            state.selected = hovered;
        }
        const bool clickNow = ImGui::IsMouseDown(0);
        if (clickNow && !state.clickWas && hovered >= 0)
        {
            state.selected = hovered;
            activated = true;
        }
        state.clickWas = clickNow;

        const float flicker = 0.88f + 0.12f * (std::sin(state.time * 11.0f) * 0.5f + std::sin(state.time * 27.0f) * 0.3f + 0.2f);
        for (int i = 0; i < 2; ++i)
        {
            const ImVec2 extent = font->CalcTextSizeA(itemSize, 10000.0f, 0.0f, labels[i]);
            const float x = (screen.x - extent.x) * 0.5f;
            const float y = top + itemStep * static_cast<float>(i);
            if (i == state.selected)
            {
                list->AddRectFilled(ImVec2(x - 34.0f * s, y + 6.0f * s), ImVec2(x - 26.0f * s, y + itemSize - 4.0f * s),
                                    WithAlpha(255, 140, 50, fade * flicker));
                TextWithShadow(list, itemSize, ImVec2(x, y), WithAlpha(255, 175, 85, fade * flicker), labels[i], 3.0f * s);
            }
            else
            {
                TextWithShadow(list, itemSize, ImVec2(x, y), WithAlpha(150, 130, 120, fade * 0.85f), labels[i], 3.0f * s);
            }
        }

        if (state.secondsLeft <= 0.0f)
        {
            return ContinueResult::Cancel;
        }
        if (activated)
        {
            return state.selected == 0 ? ContinueResult::Rematch : ContinueResult::Cancel;
        }
        return ContinueResult::None;
    }

    void ResetMainMenu(MainMenuState& state)
    {
        state.selected = 0;
        state.optionsOpen = false;
        state.time = 0.0f;
    }

    MenuResult DrawMainMenu(MainMenuState& state, float deltaSeconds, bool focused)
    {
        ImDrawList* list = ImGui::GetBackgroundDrawList();
        const ImVec2 screen = ImGui::GetIO().DisplaySize;
        if (screen.x <= 0.0f || screen.y <= 0.0f)
        {
            return MenuResult::None;
        }
        state.time += deltaSeconds;
        const float s = screen.y / 720.0f;
        const float fade = std::clamp(state.time / 1.2f, 0.0f, 1.0f);

        // ---- Input -------------------------------------------------------------------------
        bool up = false;
        bool down = false;
        bool confirm = false;
        bool back = false;
        if (focused)
        {
            const Foundation::GamepadState pad = Foundation::GetGamepadState(0);
            up = Foundation::IsKeyDown(VK_UP) || Foundation::IsKeyDown('W') || pad.dpadUp || pad.leftStickY > 0.6f;
            down = Foundation::IsKeyDown(VK_DOWN) || Foundation::IsKeyDown('S') || pad.dpadDown || pad.leftStickY < -0.6f;
            confirm = Foundation::IsKeyDown(VK_RETURN) || Foundation::IsKeyDown(VK_SPACE) || pad.buttonA;
            back = Foundation::IsKeyDown(VK_ESCAPE);
        }
        const bool upEdge = up && !state.upWas;
        const bool downEdge = down && !state.downWas;
        const bool confirmEdge = confirm && !state.confirmWas;
        const bool backEdge = back && !state.backWas;
        state.upWas = up;
        state.downWas = down;
        state.confirmWas = confirm;
        state.backWas = back;

        const int itemCount = state.optionsOpen ? 1 : kItemCount;
        if (upEdge)
        {
            state.selected = (state.selected + itemCount - 1) % itemCount;
        }
        if (downEdge)
        {
            state.selected = (state.selected + 1) % itemCount;
        }

        // ---- Layout ------------------------------------------------------------------------
        const float left = screen.x * 0.10f;
        const float titleSize = 88.0f * s;
        const float itemSize = 46.0f * s;
        const float itemStep = 66.0f * s;
        const float itemsTop = screen.y * 0.46f;
        const float flicker = 0.88f + 0.12f * (std::sin(state.time * 11.0f) * 0.5f + std::sin(state.time * 27.0f) * 0.3f + 0.2f);

        DrawVignette(list, screen, fade);
        TextWithShadow(list, titleSize, ImVec2(left, screen.y * 0.13f), WithAlpha(190, 45, 35, fade * flicker),
                       state.optionsOpen ? "OPTIONS" : "FIGHTER LAB", 5.0f * s);

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool clickNow = ImGui::IsMouseDown(0);
        const bool clickEdge = clickNow && !state.clickWas;
        state.clickWas = clickNow;
        bool activated = confirmEdge;
        int hovered = -1;

        if (state.optionsOpen)
        {
            TextWithShadow(list, 30.0f * s, ImVec2(left, itemsTop - 70.0f * s), WithAlpha(190, 175, 160, fade * 0.8f),
                           "Nothing to set up here yet.", 2.0f * s);
        }

        const int shown = state.optionsOpen ? 1 : kItemCount;
        for (int i = 0; i < shown; ++i)
        {
            const char* label = state.optionsOpen ? "BACK" : kItemLabels[i];
            const ImVec2 extent = ImGui::GetFont()->CalcTextSizeA(itemSize, 10000.0f, 0.0f, label);
            const float y = itemsTop + itemStep * static_cast<float>(i);
            const bool over = mouse.x >= left - 20.0f * s && mouse.x <= left + extent.x + 60.0f * s && mouse.y >= y && mouse.y <= y + extent.y;
            if (over && focused)
            {
                hovered = i;
            }
        }
        if (hovered >= 0 && hovered != state.selected && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f))
        {
            state.selected = hovered;
        }
        if (clickEdge && hovered >= 0)
        {
            state.selected = hovered;
            activated = true;
        }

        for (int i = 0; i < shown; ++i)
        {
            const char* label = state.optionsOpen ? "BACK" : kItemLabels[i];
            const float y = itemsTop + itemStep * static_cast<float>(i);
            const bool selected = i == state.selected;
            if (selected)
            {
                // An ember-coloured bar and a nudge to the right mark the current choice.
                list->AddRectFilled(ImVec2(left - 22.0f * s, y + 6.0f * s), ImVec2(left - 14.0f * s, y + itemSize - 4.0f * s),
                                    WithAlpha(255, 140, 50, fade * flicker));
                TextWithShadow(list, itemSize, ImVec2(left + 8.0f * s, y), WithAlpha(255, 175, 85, fade * flicker), label, 3.0f * s);
            }
            else
            {
                TextWithShadow(list, itemSize, ImVec2(left, y), WithAlpha(150, 130, 120, fade * 0.8f), label, 3.0f * s);
            }
        }

        // ---- Result ------------------------------------------------------------------------
        if (state.optionsOpen)
        {
            if (backEdge || activated)
            {
                state.optionsOpen = false;
                state.selected = 1; // Back on "OPTIONS".
            }
            return MenuResult::None;
        }
        if (activated)
        {
            switch (state.selected)
            {
            case 0:
                return MenuResult::Play;
            case 1:
                state.optionsOpen = true;
                state.selected = 0;
                return MenuResult::None;
            default:
                return MenuResult::Quit;
            }
        }
        return MenuResult::None;
    }
}

#endif // OMD_PROTOTYPE
