#include "Global.h"

#ifdef _WIN32

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// Draw checker for the fold layout test harness (FTL_FOLD_TEST set): follows FTL's matrix calls and the images,
// rectangles and primitives it draws, and reports
//   - OFFSCREEN: anything drawn by a HUD group, a window or a tooltip that is not wholly on the canvas,
//   - OVERLAP:   two HUD groups (or the touch buttons) drawn on top of each other, or the store/ship screens
//                covering the hull/scrap/fuel/missiles/drones rows.
// Ships and space (they zoom and pan) and the main menu (cropped on purpose) are not checked.

const char *FoldLayoutCheckRegion(bool &windowOverBalances); // FoldLayout.cpp: what is being drawn now
float FoldLayoutBalancesBottom();                             // FoldLayout.cpp: window y below the balances rows
bool FoldLayoutCheckClip(float &x1, float &y1, float &x2, float &y2); // FoldLayout.cpp: scissor in force

namespace
{
    struct Matrix
    {
        float sx = 1.f, sy = 1.f, tx = 0.f, ty = 0.f;
    };

    struct Box
    {
        float x1, y1, x2, y2;
    };

    struct Drawn
    {
        std::string region;
        Box box;
        bool windowOverBalances;
        std::string what; // what was drawn, for the report
    };

    // A remembered primitive: its local bounds and what made it.
    struct Prim
    {
        Box box;
        std::string what;
    };

    std::string Label(const char *kind, GL_Texture *tex, float x, float y, float w, float h)
    {
        char text[96];
        if (tex != nullptr) std::snprintf(text, sizeof(text), "%s tex %dx%d at %.0f,%.0f %.0fx%.0f", kind, tex->width_, tex->height_, x, y, w, h);
        else std::snprintf(text, sizeof(text), "%s at %.0f,%.0f %.0fx%.0f", kind, x, y, w, h);
        return text;
    }

    bool Enabled()
    {
        static const bool enabled = std::getenv("FTL_FOLD_TEST") != nullptr;
        return enabled;
    }

    std::vector<Matrix> stack(1);
    // The matrix at the start of the in-game render: whether FTL's own centring offset is in it or applied
    // elsewhere, window position = tracked position - base + modifier.
    float baseTx = 0.f, baseTy = 0.f;
    std::unordered_map<const void *, Prim> primitives;
    std::vector<Drawn> frame;
    std::set<std::string> reported;
    int offscreenCount = 0;
    int overlapCount = 0;

    Matrix &Top() { return stack.back(); }

    void Report(const char *kind, const std::string &what)
    {
        std::string key = std::string(kind) + " " + what;
        if (!reported.insert(key).second) return;
        (std::string(kind) == "OFFSCREEN" ? offscreenCount : overlapCount)++;
        hs_log_file("Fold check: %s %s\n", kind, what.c_str());
    }

    std::string Describe(const std::string &region, const Box &b)
    {
        char text[128];
        std::snprintf(text, sizeof(text), "%s [%d,%d - %d,%d]", region.c_str(), (int)b.x1, (int)b.y1, (int)b.x2, (int)b.y2);
        return text;
    }

    // A local rectangle drawn now: to window coordinates, then checked.
    void Drew(float x, float y, float w, float h, const std::string &what)
    {
        if (!Enabled() || w <= 0.f || h <= 0.f) return;
        bool windowOverBalances = false;
        const char *region = FoldLayoutCheckRegion(windowOverBalances);
        if (region == nullptr) return;
        CApp *app = G_->GetCApp();
        const Matrix &m = Top();
        float ox = app->modifier_x - baseTx, oy = app->modifier_y - baseTy;
        Box b{ox + m.tx + m.sx * x, oy + m.ty + m.sy * y, ox + m.tx + m.sx * (x + w), oy + m.ty + m.sy * (y + h)};
        if (b.x1 > b.x2) std::swap(b.x1, b.x2);
        if (b.y1 > b.y2) std::swap(b.y1, b.y2);
        float cx1, cy1, cx2, cy2;
        if (FoldLayoutCheckClip(cx1, cy1, cx2, cy2))
        {
            // Only what the scissor lets through is seen.
            b = Box{(std::max)(b.x1, cx1), (std::max)(b.y1, cy1), (std::min)(b.x2, cx2), (std::min)(b.y2, cy2)};
            if (b.x2 <= b.x1 || b.y2 <= b.y1) return;
        }
        // The star map clips its own contents (nebulae, the map image) to its window.
        bool clipsItself = std::string(region) == "star-map";
        // Rounded down to 8 px so one element sliding off a little is one report, not one per frame.
        auto snap = [](float v) { return (float)((int)v / 8 * 8); };
        if (!clipsItself && (b.x1 < -4.f || b.y1 < -4.f || b.x2 > app->screen_x + 4.f || b.y2 > app->screen_y + 4.f))
        {
            Report("OFFSCREEN", Describe(region, Box{snap(b.x1), snap(b.y1), snap(b.x2), snap(b.y2)}) + " (" + what + ")");
        }
        if (frame.size() < 4000) frame.push_back(Drawn{region, b, windowOverBalances, what});
    }

    bool Hud(const std::string &region)
    {
        return region == "top-left" || region == "crew" || region == "bottom-left" || region == "bottom-right" || region == "weapons" ||
               region == "drones" || region == "touch-buttons" || region == "target";
    }

    float Overlap(const Box &a, const Box &b)
    {
        float w = (std::min)(a.x2, b.x2) - (std::max)(a.x1, b.x1);
        float h = (std::min)(a.y2, b.y2) - (std::max)(a.y1, b.y1);
        return w > 4.f && h > 4.f ? w * h : 0.f;
    }

    // Once per frame: HUD groups must not cover each other; the store/ship screens must not cover the balances.
    void CheckFrame()
    {
        float balances = FoldLayoutBalancesBottom();
        for (size_t i = 0; i < frame.size(); i++)
        {
            const Drawn &a = frame[i];
            for (size_t j = i + 1; j < frame.size(); j++)
            {
                const Drawn &b = frame[j];
                if (a.region == b.region) continue;
                bool hudPair = Hud(a.region) && Hud(b.region);
                bool balancePair = (a.windowOverBalances && b.region == "top-left" && b.box.y2 <= balances) ||
                                   (b.windowOverBalances && a.region == "top-left" && a.box.y2 <= balances);
                if (!hudPair && !balancePair) continue;
                if (Overlap(a.box, b.box) <= 0.f) continue;
                auto snap = [](const Box &x) { return Box{(float)((int)x.x1 / 16 * 16), (float)((int)x.y1 / 16 * 16), (float)((int)x.x2 / 16 * 16), (float)((int)x.y2 / 16 * 16)}; };
                Report("OVERLAP", Describe(a.region, snap(a.box)) + " (" + a.what + ") with " + Describe(b.region, snap(b.box)) + " (" + b.what + ")");
            }
        }
        frame.clear();
    }

    void Primitive(const void *primitive)
    {
        auto found = primitives.find(primitive);
        if (found == primitives.end()) return;
        const Box &b = found->second.box;
        Drew(b.x1, b.y1, b.x2 - b.x1, b.y2 - b.y1, found->second.what);
    }

    void Remember(const void *primitive, float x, float y, float w, float h, const std::string &what)
    {
        if (Enabled() && primitive != nullptr) primitives[primitive] = Prim{Box{x, y, x + w, y + h}, what};
    }

    // A primitive this does not know the bounds of: forget whatever an earlier one at the same address was.
    void Forget(const void *primitive)
    {
        if (Enabled()) primitives.erase(primitive);
    }
}

// For the test harness.
int FoldCheckOffscreenCount() { return offscreenCount; }
int FoldCheckOverlapCount() { return overlapCount; }

HOOK_METHOD_PRIORITY(CApp, OnRender, 100, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnRender -> Begin (FoldCheck.cpp)\n")
    if (Enabled())
    {
        stack.assign(1, Matrix());
        frame.clear();
    }
    super();
    if (Enabled()) CheckFrame();
}

HOOK_METHOD_PRIORITY(CommandGui, RenderStatic, 100, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::RenderStatic -> Begin (FoldCheck.cpp)\n")
    if (Enabled())
    {
        baseTx = Top().tx;
        baseTy = Top().ty;
    }
    super();
}

HOOK_STATIC_PRIORITY(CSurface, GL_LoadIdentity, 100, () -> int)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_LoadIdentity -> Begin (FoldCheck.cpp)\n")
    if (Enabled()) Top() = Matrix();
    return super();
}

HOOK_STATIC_PRIORITY(CSurface, GL_PushMatrix, 100, () -> int)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_PushMatrix -> Begin (FoldCheck.cpp)\n")
    if (Enabled()) stack.push_back(Top());
    return super();
}

HOOK_STATIC_PRIORITY(CSurface, GL_PopMatrix, 100, () -> int)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_PopMatrix -> Begin (FoldCheck.cpp)\n")
    if (Enabled() && stack.size() > 1) stack.pop_back();
    return super();
}

HOOK_STATIC_PRIORITY(CSurface, GL_Translate, 100, (float x, float y, float z) -> bool)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_Translate -> Begin (FoldCheck.cpp)\n")
    if (Enabled())
    {
        Matrix &m = Top();
        m.tx += m.sx * x;
        m.ty += m.sy * y;
    }
    return super(x, y, z);
}

HOOK_STATIC_PRIORITY(CSurface, GL_Scale, 100, (float x, float y, float z) -> void)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_Scale -> Begin (FoldCheck.cpp)\n")
    if (Enabled())
    {
        Matrix &m = Top();
        m.sx *= x;
        m.sy *= y;
    }
    super(x, y, z);
}

HOOK_STATIC_PRIORITY(CSurface, GL_DrawRect, 100, (float x1, float y1, float x2, float y2, GL_Color color) -> bool)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_DrawRect -> Begin (FoldCheck.cpp)\n")
    if (Enabled() && color.a > 0.05f) Drew(x1, y1, x2, y2, Label("rect", nullptr, x1, y1, x2, y2));
    return super(x1, y1, x2, y2, color);
}

HOOK_STATIC_PRIORITY(CSurface, GL_BlitImage, 100, (GL_Texture *tex, float x, float y, float x2, float y2, float rotation, GL_Color color, bool mirror) -> bool)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_BlitImage -> Begin (FoldCheck.cpp)\n")
    if (Enabled() && color.a > 0.05f) Drew(x, y, x2, y2, Label("blit", tex, x, y, x2, y2));
    return super(tex, x, y, x2, y2, rotation, color, mirror);
}

HOOK_STATIC_PRIORITY(CSurface, GL_BlitPixelImage, 100, (GL_Texture *tex, float x, float y, float x2, float y2, float rotation, GL_Color color, bool mirror) -> bool)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_BlitPixelImage -> Begin (FoldCheck.cpp)\n")
    if (Enabled() && color.a > 0.05f) Drew(x, y, x2, y2, Label("pixblit", tex, x, y, x2, y2));
    return super(tex, x, y, x2, y2, rotation, color, mirror);
}

HOOK_STATIC_PRIORITY(CSurface, GL_RenderPrimitive, 100, (GL_Primitive *primitive) -> void)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_RenderPrimitive -> Begin (FoldCheck.cpp)\n")
    if (Enabled()) Primitive(primitive);
    super(primitive);
}

HOOK_STATIC_PRIORITY(CSurface, GL_RenderPrimitiveWithAlpha, 100, (GL_Primitive *primitive, float alpha) -> void)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_RenderPrimitiveWithAlpha -> Begin (FoldCheck.cpp)\n")
    if (Enabled() && alpha > 0.05f) Primitive(primitive);
    super(primitive, alpha);
}

HOOK_STATIC_PRIORITY(CSurface, GL_RenderPrimitiveWithColor, 100, (GL_Primitive *primitive, GL_Color color) -> void)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_RenderPrimitiveWithColor -> Begin (FoldCheck.cpp)\n")
    if (Enabled() && color.a > 0.05f) Primitive(primitive);
    super(primitive, color);
}

HOOK_STATIC_PRIORITY(CSurface, GL_DestroyPrimitive, 100, (GL_Primitive *primitive) -> void)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_DestroyPrimitive -> Begin (FoldCheck.cpp)\n")
    if (Enabled()) primitives.erase(primitive);
    super(primitive);
}

HOOK_STATIC_PRIORITY(CSurface, GL_CreateImagePrimitive, 100, (GL_Texture *tex, float x, float y, float size_x, float size_y, float rotate, GL_Color color) -> GL_Primitive*)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_CreateImagePrimitive -> Begin (FoldCheck.cpp)\n")
    GL_Primitive *primitive = super(tex, x, y, size_x, size_y, rotate, color);
    if (Enabled()) Remember(primitive, x, y, size_x, size_y, Label("image prim", tex, x, y, size_x, size_y));
    return primitive;
}

HOOK_STATIC_PRIORITY(CSurface, GL_CreatePixelImagePrimitive, 100, (GL_Texture *tex, float x, float y, float size_x, float size_y, float rotate, GL_Color color, bool unk) -> GL_Primitive*)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_CreatePixelImagePrimitive -> Begin (FoldCheck.cpp)\n")
    GL_Primitive *primitive = super(tex, x, y, size_x, size_y, rotate, color, unk);
    if (Enabled()) Remember(primitive, x, y, size_x, size_y, Label("pixel prim", tex, x, y, size_x, size_y));
    return primitive;
}

HOOK_STATIC_PRIORITY(CSurface, GL_CreateRectPrimitive, 100, (float x, float y, float w, float h, GL_Color color) -> GL_Primitive*)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_CreateRectPrimitive -> Begin (FoldCheck.cpp)\n")
    GL_Primitive *primitive = super(x, y, w, h, color);
    if (Enabled()) Remember(primitive, x, y, w, h, Label("rect prim", nullptr, x, y, w, h));
    return primitive;
}

HOOK_STATIC_PRIORITY(CSurface, GL_CreateMultiImagePrimitive, 100, (GL_Texture *tex, std::vector<GL_TexVertex> *vec, GL_Color color) -> GL_Primitive*)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_CreateMultiImagePrimitive -> Begin (FoldCheck.cpp)\n")
    GL_Primitive *primitive = super(tex, vec, color);
    if (Enabled() && primitive != nullptr && vec != nullptr && !vec->empty())
    {
        float x1 = 1e9f, y1 = 1e9f, x2 = -1e9f, y2 = -1e9f;
        for (const GL_TexVertex &v : *vec)
        {
            x1 = (std::min)(x1, v.x);
            y1 = (std::min)(y1, v.y);
            x2 = (std::max)(x2, v.x);
            y2 = (std::max)(y2, v.y);
        }
        primitives[primitive] = Prim{Box{x1, y1, x2, y2}, Label("multi prim", tex, x1, y1, x2 - x1, y2 - y1)};
    }
    return primitive;
}

// Primitives made by functions this does not measure: drop any stale entry at the address they reuse.
HOOK_STATIC_PRIORITY(CSurface, GL_CreateImagePartialPrimitive, 100, (GL_Texture *tex, float x, float y, float size_x, float size_y, float start_x, float end_x, float start_y, float end_y, float alpha, GL_Color color, bool mirror) -> GL_Primitive*)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_CreateImagePartialPrimitive -> Begin (FoldCheck.cpp)\n")
    GL_Primitive *primitive = super(tex, x, y, size_x, size_y, start_x, end_x, start_y, end_y, alpha, color, mirror);
    if (Enabled()) Remember(primitive, x, y, size_x, size_y, Label("partial prim", tex, x, y, size_x, size_y));
    return primitive;
}

HOOK_STATIC_PRIORITY(CSurface, GL_CreateMultiRectPrimitive, 100, (std::vector<Globals::Rect> &vec, GL_Color color) -> GL_Primitive*)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_CreateMultiRectPrimitive -> Begin (FoldCheck.cpp)\n")
    GL_Primitive *primitive = super(vec, color);
    if (Enabled() && !vec.empty())
    {
        float x1 = 1e9f, y1 = 1e9f, x2 = -1e9f, y2 = -1e9f;
        for (const Globals::Rect &r : vec)
        {
            x1 = (std::min)(x1, (float)r.x);
            y1 = (std::min)(y1, (float)r.y);
            x2 = (std::max)(x2, (float)(r.x + r.w));
            y2 = (std::max)(y2, (float)(r.y + r.h));
        }
        Remember(primitive, x1, y1, x2 - x1, y2 - y1, Label("multirect prim", nullptr, x1, y1, x2 - x1, y2 - y1));
    }
    else Forget(primitive);
    return primitive;
}

HOOK_STATIC_PRIORITY(CSurface, GL_CreateRectOutlinePrimitive, 100, (int x, int y, int w, int h, GL_Color color, float lineWidth) -> GL_Primitive*)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_CreateRectOutlinePrimitive -> Begin (FoldCheck.cpp)\n")
    GL_Primitive *primitive = super(x, y, w, h, color, lineWidth);
    if (Enabled()) Remember(primitive, (float)x, (float)y, (float)w, (float)h, Label("outline prim", nullptr, (float)x, (float)y, (float)w, (float)h));
    return primitive;
}

HOOK_STATIC_PRIORITY(CSurface, GL_CreateCirclePrimitive, 100, (int x, int y, float radius, GL_Color color) -> GL_Primitive*)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_CreateCirclePrimitive -> Begin (FoldCheck.cpp)\n")
    GL_Primitive *primitive = super(x, y, radius, color);
    Forget(primitive);
    return primitive;
}

HOOK_STATIC_PRIORITY(CSurface, GL_CreatePiePartialPrimitive, 100, (int x, int y, float radius, float deg1, float deg2, float thickness, GL_Color color) -> GL_Primitive*)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_CreatePiePartialPrimitive -> Begin (FoldCheck.cpp)\n")
    GL_Primitive *primitive = super(x, y, radius, deg1, deg2, thickness, color);
    Forget(primitive);
    return primitive;
}

HOOK_STATIC_PRIORITY(CSurface, GL_CreateMultiLinePrimitive, 100, (std::vector<GL_Line> &vec, GL_Color color, float thickness) -> GL_Primitive*)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_CreateMultiLinePrimitive -> Begin (FoldCheck.cpp)\n")
    GL_Primitive *primitive = super(vec, color, thickness);
    Forget(primitive);
    return primitive;
}

HOOK_STATIC_PRIORITY(CSurface, GL_CreateMultiImageColorPrimitive, 100, (GL_Texture *tex, std::vector<GL_ColorTexVertex> *vertices) -> GL_Primitive*)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_CreateMultiImageColorPrimitive -> Begin (FoldCheck.cpp)\n")
    GL_Primitive *primitive = super(tex, vertices);
    Forget(primitive);
    return primitive;
}

#else

int FoldCheckOffscreenCount() { return 0; }
int FoldCheckOverlapCount() { return 0; }

#endif // _WIN32
