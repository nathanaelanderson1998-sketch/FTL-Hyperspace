#include "Global.h"

#ifdef _WIN32

#include <windows.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>

// Fold layout: lets the in-game screen use a canvas bigger than 1280x720 (e.g. a near-square phone screen).
//
// With fullscreen=2 ("borders") FTL creates a desktop-sized window, skips its 1280x720 framebuffer, draws the
// game 1:1 centred on the canvas and clips it to 1280x720. When the canvas is bigger than that, this feature:
//   - removes the 1280x720 clip,
//   - pins the HUD to the canvas edges (hull/crew/FTL button to the top-left, systems/weapons to the bottom-left,
//     the options button to the top-right),
//   - scales the space background to cover the canvas,
//   - lets the ships and space in between zoom (mouse wheel) and pan (middle-button drag),
//   - remaps mouse input so clicks land on what is drawn under the pointer.
// Set FTL_FOLD_LAYOUT=0 to turn it off.

namespace
{
    enum class Region { NONE, TOP_LEFT, TOP_RIGHT, BOTTOM_LEFT, WORLD };

    // HUD bands in the game's own 1280x720 coordinates.
    const int TOP_BAND_BOTTOM = 110;   // hull, shields, resources, FTL button, ship/store/options buttons
    const int BOTTOM_BAND_TOP = 590;   // system power bars, weapons, drones

    const float MIN_ZOOM = 1.f;
    const float MAX_ZOOM = 2.5f;

    bool inGuiRender = false;
    float zoom = 1.f;
    float panX = 0.f;
    float panY = 0.f;
    Region lastRegion = Region::NONE;
    int mmbLastX = 0;
    int mmbLastY = 0;
    bool mmbPanning = false;

    bool FeatureEnabled()
    {
        static const bool enabled = []
        {
            const char *value = std::getenv("FTL_FOLD_LAYOUT");
            return value == nullptr || value[0] != '0';
        }();
        return enabled;
    }

    // The canvas is bigger than the game and FTL draws it unscaled (no framebuffer).
    bool LayoutActive(CApp *app)
    {
        return FeatureEnabled() && app != nullptr && !app->useFrameBuffer &&
               (app->screen_x > 1280 || app->screen_y > 720);
    }

    // In a run, on the main ship view, with no window (map, store, event, menu...) on top.
    bool InGame(CApp *app)
    {
        return LayoutActive(app) && !app->menu.bOpen && !app->langChooser.bOpen && app->gui != nullptr;
    }

    bool ModalOpen(CommandGui *gui)
    {
        if (gui->gameover || gui->choiceBoxOpen) return true;
        for (auto window : gui->focusWindows)
        {
            if (window != nullptr && window->bOpen) return true;
        }
        return false;
    }

    int ExtraX(CApp *app) { return app->modifier_x; }
    int ExtraY(CApp *app) { return app->modifier_y; }

    void ClampView(CApp *app)
    {
        zoom = (std::max)(MIN_ZOOM, (std::min)(MAX_ZOOM, zoom));
        float maxX = 640.f * zoom;
        float maxY = 360.f * zoom + ExtraY(app);
        panX = (std::max)(-maxX, (std::min)(maxX, panX));
        panY = (std::max)(-maxY, (std::min)(maxY, panY));
        if (zoom == MIN_ZOOM)
        {
            panX = 0.f;
            panY = 0.f;
        }
    }

    // Game point -> where it is drawn, for each region.
    void RegionOffset(CApp *app, Region region, int &dx, int &dy)
    {
        dx = 0;
        dy = 0;
        switch (region)
        {
        case Region::TOP_LEFT: dx = -ExtraX(app); dy = -ExtraY(app); break;
        case Region::TOP_RIGHT: dx = ExtraX(app); dy = -ExtraY(app); break;
        case Region::BOTTOM_LEFT: dx = -ExtraX(app); dy = ExtraY(app); break;
        default: break;
        }
    }

    void PushRegion(CApp *app, Region region)
    {
        CSurface::GL_PushMatrix();
        if (region == Region::WORLD)
        {
            CSurface::GL_Translate(640.f + panX, 360.f + panY, 0.f);
            CSurface::GL_Scale(zoom, zoom, 1.f);
            CSurface::GL_Translate(-640.f, -360.f, 0.f);
        }
        else
        {
            int dx, dy;
            RegionOffset(app, region, dx, dy);
            CSurface::GL_Translate((float)dx, (float)dy, 0.f);
        }
    }

    bool InsideRect(const Globals::Rect &rect, int x, int y)
    {
        return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
    }

    bool OnCrewBox(CommandGui *gui, int x, int y)
    {
        for (auto box : gui->crewControl.crewBoxes)
        {
            if (box != nullptr && InsideRect(box->box, x, y)) return true;
        }
        return false;
    }

    // A point drawn on the canvas (game coordinates, i.e. window minus the centring offset) -> the game
    // coordinates the game should see for it.
    Point MapDrawnPoint(CApp *app, int x, int y)
    {
        CommandGui *gui = app->gui;
        if (ModalOpen(gui))
        {
            lastRegion = Region::NONE;
            return Point(x, y);
        }

        int dx, dy;
        RegionOffset(app, Region::TOP_RIGHT, dx, dy);
        if (InsideRect(gui->optionsButton.hitbox, x - dx, y - dy))
        {
            lastRegion = Region::TOP_RIGHT;
            return Point(x - dx, y - dy);
        }

        RegionOffset(app, Region::TOP_LEFT, dx, dy);
        if (y - dy < TOP_BAND_BOTTOM || OnCrewBox(gui, x - dx, y - dy))
        {
            lastRegion = Region::TOP_LEFT;
            return Point(x - dx, y - dy);
        }

        RegionOffset(app, Region::BOTTOM_LEFT, dx, dy);
        if (y - dy >= BOTTOM_BAND_TOP)
        {
            lastRegion = Region::BOTTOM_LEFT;
            return Point(x - dx, y - dy);
        }

        lastRegion = Region::WORLD;
        int worldX = (int)std::floor(640.f + (x - 640.f - panX) / zoom);
        int worldY = (int)std::floor(360.f + (y - 360.f - panY) / zoom);

        // Parts of the world that sit under the HUD in the original layout must not hit the (moved) HUD.
        if (worldY < TOP_BAND_BOTTOM || worldY >= BOTTOM_BAND_TOP || OnCrewBox(gui, worldX, worldY))
        {
            return Point(-10000, -10000);
        }
        return Point(worldX, worldY);
    }

    // Window coordinates in -> window coordinates that FTL's own transform turns into the mapped game point.
    void MapWindowPoint(CApp *app, int &x, int &y)
    {
        if (!InGame(app)) return;
        Point game = MapDrawnPoint(app, x - app->modifier_x, y - app->modifier_y);
        x = game.x + app->modifier_x;
        y = game.y + app->modifier_y;
    }

    void ZoomAt(CApp *app, int windowX, int windowY, float factor)
    {
        float x = (float)(windowX - app->modifier_x);
        float y = (float)(windowY - app->modifier_y);
        // Keep the world point under the pointer where it is.
        float worldX = 640.f + (x - 640.f - panX) / zoom;
        float worldY = 360.f + (y - 360.f - panY) / zoom;
        zoom *= factor;
        zoom = (std::max)(MIN_ZOOM, (std::min)(MAX_ZOOM, zoom));
        panX = x - 640.f - (worldX - 640.f) * zoom;
        panY = y - 360.f - (worldY - 360.f) * zoom;
        ClampView(app);
    }

    // FTL ignores the mouse wheel, so it is caught at the window.
    HWND gameWindow = nullptr;
    WNDPROC originalWndProc = nullptr;

    LRESULT CALLBACK FoldWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_MOUSEWHEEL)
        {
            CApp *app = G_->GetCApp();
            if (InGame(app) && !ModalOpen(app->gui))
            {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ScreenToClient(hwnd, &pt);
                float notches = (float)GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
                ZoomAt(app, pt.x, pt.y, std::pow(1.1f, notches));
                return 0;
            }
        }
        return CallWindowProc(originalWndProc, hwnd, msg, wParam, lParam);
    }

    BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd))
        {
            gameWindow = hwnd;
            return FALSE;
        }
        return TRUE;
    }

    void HookWindowOnce()
    {
        static bool done = false;
        if (done) return;
        EnumWindows(FindGameWindow, 0);
        if (gameWindow == nullptr) return;
        done = true;
        originalWndProc = (WNDPROC)SetWindowLongPtrA(gameWindow, GWLP_WNDPROC, (LONG_PTR)FoldWndProc);
        CApp *app = G_->GetCApp();
        hs_log_file("Fold layout: canvas %dx%d, HUD offsets %d/%d, %s\n", app->screen_x, app->screen_y,
                    app->modifier_x, app->modifier_y, LayoutActive(app) ? "active" : "inactive (needs fullscreen=2 and a canvas bigger than 1280x720)");
    }

    // Wraps one render call in a region transform while the in-game screen is drawn.
    struct RegionScope
    {
        bool pushed = false;
        RegionScope(Region region)
        {
            CApp *app = G_->GetCApp();
            if (inGuiRender && region != Region::NONE && InGame(app))
            {
                PushRegion(app, region);
                pushed = true;
            }
        }
        ~RegionScope()
        {
            if (pushed) CSurface::GL_PopMatrix();
        }
    };
}

// ---- Window / clip ----

// FTL_FOLD_LAYOUT=1 (set by the Android launcher) also takes over the display mode: borders mode on the whole
// desktop. Without this FTL can fall back to a 1280x720 window, e.g. when Wine reports the borderless window as
// not fullscreen (CApp::UpdateFullScreen then switches the setting back to windowed).
static bool ForceBordersMode()
{
    static const bool forced = []
    {
        const char *value = std::getenv("FTL_FOLD_LAYOUT");
        return value != nullptr && value[0] == '1';
    }();
    return forced;
}

HOOK_METHOD_PRIORITY(CApp, SetupWindow, -10000, () -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::SetupWindow -> Begin (FoldLayout.cpp)\n")
    if (ForceBordersMode())
    {
        auto settings = Global_Settings_Settings;
        settings->fullscreen = 2;
        settings->currentFullscreen = 2;
        settings->manualResolution = false;
        settings->manualWindowed = false;
        settings->manualStretched = false;
        hs_log_file("Fold layout: forcing fullscreen=2 (borders) on the desktop\n");
    }
    return super();
}

HOOK_METHOD_PRIORITY(CApp, UpdateFullScreen, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::UpdateFullScreen -> Begin (FoldLayout.cpp)\n")
    if (ForceBordersMode()) return;
    super();
}

HOOK_METHOD_PRIORITY(CApp, OnLoop, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnLoop -> Begin (FoldLayout.cpp)\n")
    if (FeatureEnabled()) HookWindowOnce();
    super();
}

HOOK_STATIC_PRIORITY(CSurface, GL_SetScissor, -10000, (int x, int y, int w, int h) -> void)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_SetScissor -> Begin (FoldLayout.cpp)\n")
    // CApp::OnRender clips the frame to the centred 1280x720 game area; let the HUD reach the edges instead.
    if (w == 1280 && h == 720 && LayoutActive(G_->GetCApp())) return super(0, 0, 0, 0);
    super(x, y, w, h);
}

// ---- Rendering ----

HOOK_METHOD_PRIORITY(CommandGui, RenderStatic, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::RenderStatic -> Begin (FoldLayout.cpp)\n")
    inGuiRender = true;
    super();
    inGuiRender = false;
}

HOOK_METHOD_PRIORITY(SpaceManager, OnRenderBackground, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> SpaceManager::OnRenderBackground -> Begin (FoldLayout.cpp)\n")
    CApp *app = G_->GetCApp();
    if (!inGuiRender || !InGame(app)) return super();

    // Cover the whole canvas with the 1280x720 backdrop.
    float scale = (std::max)((float)app->screen_x / 1280.f, (float)app->screen_y / 720.f);
    CSurface::GL_PushMatrix();
    CSurface::GL_Translate(640.f, 360.f, 0.f);
    CSurface::GL_Scale(scale, scale, 1.f);
    CSurface::GL_Translate(-640.f, -360.f, 0.f);
    super();
    CSurface::GL_PopMatrix();
}

HOOK_METHOD_PRIORITY(SpaceManager, OnRenderForeground, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> SpaceManager::OnRenderForeground -> Begin (FoldLayout.cpp)\n")
    RegionScope scope(Region::WORLD);
    super();
}

HOOK_METHOD_PRIORITY(SpaceManager, OnRenderAsteroids, -10000, (int fieldLayers, float alpha) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> SpaceManager::OnRenderAsteroids -> Begin (FoldLayout.cpp)\n")
    RegionScope scope(Region::WORLD);
    super(fieldLayers, alpha);
}

HOOK_METHOD_PRIORITY(CommandGui, RenderPlayerShip, -10000, (Point &shipCenter, float jumpScale) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::RenderPlayerShip -> Begin (FoldLayout.cpp)\n")
    RegionScope scope(Region::WORLD);
    super(shipCenter, jumpScale);
}

HOOK_METHOD_PRIORITY(CombatControl, OnRenderCombat, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CombatControl::OnRenderCombat -> Begin (FoldLayout.cpp)\n")
    RegionScope scope(Region::WORLD);
    super();
}

HOOK_METHOD_PRIORITY(CombatControl, OnRenderInterface, -10000, (bool front) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CombatControl::OnRenderInterface -> Begin (FoldLayout.cpp)\n")
    RegionScope scope(Region::BOTTOM_LEFT);
    super(front);
}

HOOK_METHOD_PRIORITY(SystemControl, OnRender, -10000, (bool front) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> SystemControl::OnRender -> Begin (FoldLayout.cpp)\n")
    RegionScope scope(Region::BOTTOM_LEFT);
    super(front);
}

HOOK_METHOD_PRIORITY(ShipStatus, OnRender, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipStatus::OnRender -> Begin (FoldLayout.cpp)\n")
    RegionScope scope(Region::TOP_LEFT);
    super();
}

HOOK_METHOD_PRIORITY(CrewControl, OnRender, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CrewControl::OnRender -> Begin (FoldLayout.cpp)\n")
    RegionScope scope(Region::TOP_LEFT);
    super();
}

HOOK_METHOD_PRIORITY(SpaceStatus, OnRender, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> SpaceStatus::OnRender -> Begin (FoldLayout.cpp)\n")
    RegionScope scope(Region::TOP_LEFT);
    super();
}

HOOK_METHOD_PRIORITY(FTLButton, OnRender, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> FTLButton::OnRender -> Begin (FoldLayout.cpp)\n")
    RegionScope scope(Region::TOP_LEFT);
    super();
}

HOOK_METHOD_PRIORITY(Button, OnRender, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> Button::OnRender -> Begin (FoldLayout.cpp)\n")
    CommandGui *gui = G_->GetCApp() != nullptr ? G_->GetCApp()->gui : nullptr;
    Region region = Region::NONE;
    if (gui != nullptr && this == &gui->upgradeButton) region = Region::TOP_LEFT;
    else if (gui != nullptr && this == &gui->optionsButton) region = Region::TOP_RIGHT;
    RegionScope scope(region);
    super();
}

HOOK_METHOD_PRIORITY(TextButton, OnRender, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> TextButton::OnRender -> Begin (FoldLayout.cpp)\n")
    CommandGui *gui = G_->GetCApp() != nullptr ? G_->GetCApp()->gui : nullptr;
    RegionScope scope(gui != nullptr && this == &gui->storeButton ? Region::TOP_LEFT : Region::NONE);
    super();
}

HOOK_METHOD_PRIORITY(WarningMessage, OnRender, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> WarningMessage::OnRender -> Begin (FoldLayout.cpp)\n")
    CommandGui *gui = G_->GetCApp() != nullptr ? G_->GetCApp()->gui : nullptr;
    RegionScope scope(gui != nullptr && this == &gui->upgradeWarning ? Region::TOP_LEFT : Region::NONE);
    super();
}

// Tooltips are placed at the (remapped) mouse position, so draw them where the pointer actually is.
HOOK_METHOD_PRIORITY(CombatControl, OnRenderTooltips, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CombatControl::OnRenderTooltips -> Begin (FoldLayout.cpp)\n")
    RegionScope scope(lastRegion);
    super();
}

HOOK_METHOD_PRIORITY(MouseControl, OnRender, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> MouseControl::OnRender -> Begin (FoldLayout.cpp)\n")
    CApp *app = G_->GetCApp();
    bool pushed = false;
    if (lastRegion != Region::NONE && InGame(app) && !ModalOpen(app->gui))
    {
        PushRegion(app, lastRegion);
        pushed = true;
    }
    super();
    if (pushed) CSurface::GL_PopMatrix();
}

// ---- Input ----

HOOK_METHOD_PRIORITY(CApp, OnMouseMove, -10000, (int x, int y, int xdiff, int ydiff, bool holdingLMB, bool holdingRMB, bool holdingMMB) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnMouseMove -> Begin (FoldLayout.cpp)\n")
    if (InGame(this) && holdingMMB && mmbPanning)
    {
        // Middle-button drag (two-finger drag on the phone) pans the world.
        panX += (float)(x - mmbLastX);
        panY += (float)(y - mmbLastY);
        mmbLastX = x;
        mmbLastY = y;
        ClampView(this);
        return;
    }
    if (!holdingMMB) mmbPanning = false;
    MapWindowPoint(this, x, y);
    super(x, y, xdiff, ydiff, holdingLMB, holdingRMB, holdingMMB);
}

HOOK_METHOD_PRIORITY(CApp, OnMButtonDown, -10000, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnMButtonDown -> Begin (FoldLayout.cpp)\n")
    if (InGame(this) && !ModalOpen(gui))
    {
        mmbPanning = true;
        mmbLastX = x;
        mmbLastY = y;
        return;
    }
    super(x, y);
}

HOOK_METHOD_PRIORITY(CApp, OnLButtonDown, -10000, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnLButtonDown -> Begin (FoldLayout.cpp)\n")
    MapWindowPoint(this, x, y);
    super(x, y);
}

HOOK_METHOD_PRIORITY(CApp, OnLButtonUp, -10000, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnLButtonUp -> Begin (FoldLayout.cpp)\n")
    MapWindowPoint(this, x, y);
    super(x, y);
}

HOOK_METHOD_PRIORITY(CApp, OnRButtonDown, -10000, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnRButtonDown -> Begin (FoldLayout.cpp)\n")
    MapWindowPoint(this, x, y);
    super(x, y);
}

HOOK_METHOD_PRIORITY(CApp, OnRButtonUp, -10000, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnRButtonUp -> Begin (FoldLayout.cpp)\n")
    MapWindowPoint(this, x, y);
    super(x, y);
}

#endif // _WIN32
