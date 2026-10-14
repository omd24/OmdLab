#if defined(OMD_PROTOTYPE)

#include "Splash.h"

#include "Asset/Mesh.h"
#include "Foundation/Input.h"
#include "Renderer/Buffer.h"
#include "Renderer/Texture.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <imgui.h>

namespace Game
{
    namespace
    {
        constexpr float kQuadDistance = 1.2f;

        // Page timing, in seconds since the start: fade in, hold, fade out.
        constexpr float kPage1FadeIn = 0.8f;
        constexpr float kPage1HoldEnd = 4.8f;
        constexpr float kPage1FadeOut = 0.7f;
        constexpr float kPage2Start = kPage1HoldEnd + kPage1FadeOut;
        constexpr float kPage2FadeIn = 0.8f;
        constexpr float kPage2HoldEnd = kPage2Start + kPage2FadeIn + 9.0f;
        constexpr float kPage2FadeOut = 0.8f;
        constexpr float kPage2End = kPage2HoldEnd + kPage2FadeOut;

        float PageAlpha(float t, float start, float fadeIn, float holdEnd, float fadeOut)
        {
            const float in = std::clamp((t - start) / fadeIn, 0.0f, 1.0f);
            const float out = 1.0f - std::clamp((t - holdEnd) / fadeOut, 0.0f, 1.0f);
            return t < start ? 0.0f : in * out;
        }

        // The black cover stays opaque through both pages, then fades away over the rest.
        float CoverAlpha(float t)
        {
            return 1.0f - std::clamp((t - kPage2End) / (Splash::kDuration - kPage2End), 0.0f, 1.0f);
        }

        const char* const kRhyme[] = {
            "Roses are red,",
            "The deadline is near,",
            "\"Explain what this code does\"",
            "Is now my whole career.",
        };

        const char* const kDisclaimer =
            "If you have heart disease, dyslexia, diabetes type 1, 2, 3 and so on, you should be fine. "
            "But if you have an allergy to AI slop, viewer discretion is advised. "
            "Seriously, this project by many definitions counts as AI slop. "
            "The project was developed with an AI coding agent and I am not even sorry. "
            "No game engine was used or harmed in the process, though several deadlines were. "
            "Any resemblance to a finished game is purely coincidental. "
            "All code and resources are available for poking around at the corresponding GitHub repository: "
            "github.com/omd24/OmdLab. "
            "Please keep your hands and feet inside the build at all times.";
    }

    Splash CreateSplash()
    {
        Splash splash;
        const unsigned char white[4] = { 255, 255, 255, 255 };
        const Asset::Vertex vertices[4] = {
            Asset::Vertex{ { -0.5f, 0.5f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },
            Asset::Vertex{ { 0.5f, 0.5f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },
            Asset::Vertex{ { 0.5f, -0.5f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f } },
            Asset::Vertex{ { -0.5f, -0.5f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f } },
        };
        const uint32_t indices[6] = { 0, 1, 2, 0, 2, 3 };
        Renderer::StaticMeshDrawItem item;
        item.vertexBuffer = Renderer::Buffer::Create(vertices, sizeof(vertices));
        item.vertexStride = sizeof(Asset::Vertex);
        item.indexBuffer = Renderer::Buffer::Create(indices, sizeof(indices));
        item.indexCount = 6;
        DirectX::XMFLOAT4X4 world;
        DirectX::XMStoreFloat4x4(&world, DirectX::XMMatrixTranspose(DirectX::XMMatrixScaling(0.0f, 0.0f, 0.0f)));
        item.worldBuffer = Renderer::Buffer::Create(&world, sizeof(world));
        item.baseColorTexture = Renderer::Texture::Create(white, 1, 1);
        const DirectX::XMFLOAT4 tint = { 0.0f, 0.0f, 0.0f, 0.0f };
        item.tintBuffer = Renderer::Buffer::Create(&tint, sizeof(tint));
        item.transparent = true;
        splash.items.push_back(item);
        return splash;
    }

    void UpdateSplash(Splash& splash, const Engine::Camera& camera, float aspectRatio, float t, bool active)
    {
        if (splash.items.empty())
        {
            return;
        }
        const float cover = active ? CoverAlpha(t) : 0.0f;
        // The camera looks down +Z, so "in front" is simply a larger Z.
        const float height = 2.0f * kQuadDistance * std::tan(camera.fovYRadians * 0.5f) * 1.15f;
        const float width = height * aspectRatio * 1.15f;
        DirectX::XMFLOAT4X4 world;
        DirectX::XMStoreFloat4x4(
            &world, DirectX::XMMatrixTranspose(DirectX::XMMatrixScaling(width, height, 1.0f) *
                                               DirectX::XMMatrixTranslation(camera.position.x, camera.position.y, camera.position.z + kQuadDistance)));
        Renderer::Buffer::Update(splash.items[0].worldBuffer, &world, sizeof(world));
        const DirectX::XMFLOAT4 tint = { 0.0f, 0.0f, 0.0f, cover };
        Renderer::Buffer::Update(splash.items[0].tintBuffer, &tint, sizeof(tint));
    }

    float DrawSplashText(Splash& splash, float t, bool focused)
    {
        // Skip: jump to the fade-out of the page being shown.
        const bool skip = focused && (Foundation::IsKeyDown(VK_RETURN) || Foundation::IsKeyDown(VK_SPACE) || ImGui::IsMouseDown(0));
        if (skip && !splash.skipWas)
        {
            if (t < kPage1HoldEnd)
            {
                t = kPage1HoldEnd;
            }
            else if (t > kPage2Start && t < kPage2HoldEnd)
            {
                t = kPage2HoldEnd;
            }
        }
        splash.skipWas = skip;

        ImDrawList* list = ImGui::GetBackgroundDrawList();
        const ImVec2 screen = ImGui::GetIO().DisplaySize;
        const float s = screen.y / 720.0f;
        ImFont* font = ImGui::GetFont();

        const auto shadowed = [&](ImVec2 pos, float size, ImU32 color, ImU32 shadow, const char* text, float wrap)
        {
            list->AddText(font, size, ImVec2(pos.x + 2.0f * s, pos.y + 2.0f * s), shadow, text, nullptr, wrap);
            list->AddText(font, size, pos, color, text, nullptr, wrap);
        };

        // Page 1: the rhyme.
        const float a1 = PageAlpha(t, 0.0f, kPage1FadeIn, kPage1HoldEnd, kPage1FadeOut);
        if (a1 > 0.0f)
        {
            const ImU32 a = static_cast<ImU32>(a1 * 255.0f);
            const float size = 46.0f * s;
            const float lineStep = 70.0f * s;
            const float top = screen.y * 0.5f - lineStep * 2.0f + 8.0f * s;
            for (int i = 0; i < 4; ++i)
            {
                const ImVec2 extent = font->CalcTextSizeA(size, 10000.0f, 0.0f, kRhyme[i]);
                shadowed(ImVec2((screen.x - extent.x) * 0.5f, top + lineStep * static_cast<float>(i)), size,
                         IM_COL32(235, 230, 220, a), IM_COL32(0, 0, 0, a), kRhyme[i], 0.0f);
            }
        }

        // Page 2: the disclaimer, as a block of text.
        const float a2 = PageAlpha(t, kPage2Start, kPage2FadeIn, kPage2HoldEnd, kPage2FadeOut);
        if (a2 > 0.0f)
        {
            const ImU32 a = static_cast<ImU32>(a2 * 255.0f);
            const float titleSize = 64.0f * s;
            const char* title = "DISCLAIMER";
            const ImVec2 titleExtent = font->CalcTextSizeA(titleSize, 10000.0f, 0.0f, title);
            shadowed(ImVec2((screen.x - titleExtent.x) * 0.5f, screen.y * 0.10f), titleSize, IM_COL32(200, 45, 35, a),
                     IM_COL32(0, 0, 0, a), title, 0.0f);

            const float size = 31.0f * s;
            const float wrap = screen.x * 0.74f;
            const ImVec2 extent = font->CalcTextSizeA(size, 10000.0f, wrap, kDisclaimer);
            shadowed(ImVec2((screen.x - wrap) * 0.5f, screen.y * 0.10f + titleExtent.y + 36.0f * s), size,
                     IM_COL32(225, 220, 210, a), IM_COL32(0, 0, 0, a), kDisclaimer, wrap);
            (void)extent;
        }
        return t;
    }
}

#endif // OMD_PROTOTYPE
