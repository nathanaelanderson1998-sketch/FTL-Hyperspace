#include "Global.h"

#ifdef _WIN32

#include <windows.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <GL/gl.h>

// Fold layout: lets the in-game screen use a canvas bigger than 1280x720 (e.g. a near-square phone screen).
//
// With fullscreen=2 ("borders") FTL creates a desktop-sized window, skips its 1280x720 framebuffer, draws the
// game 1:1 centred on the canvas and clips it to 1280x720. When the canvas is bigger than that, this feature:
//   - removes the 1280x720 clip,
//   - pins the HUD to the canvas corners and enlarges it (hull/crew/FTL button top-left, systems/weapons
//     bottom-left, subsystems/More Info bottom-right),
//   - enlarges the star map and event boxes,
//   - scales the space background to cover the canvas,
//   - lets the ships and space in between zoom (mouse wheel) and pan (middle-button drag),
//   - remaps mouse input so clicks land on what is drawn under the pointer.
// Set FTL_FOLD_LAYOUT=0 to turn it off.

Button *GetMoreInfoButton(); // game/UserInterface/MoreInfoButton.cpp

namespace
{
    // HUD groups, each pinned to a canvas corner and scaled up around it; WORLD = ships and space (zoom/pan);
    // MODAL = the star map or an event box, scaled up around the canvas centre. On a tall canvas WEAPONS, DRONES
    // (the weapon and drone boxes) and CREW (crew boxes, stations buttons) get places and scales of their own;
    // otherwise they follow the bottom-left and top-left groups.
    enum class Region { NONE, TOP_LEFT, BOTTOM_LEFT, BOTTOM_RIGHT, WORLD, MODAL, WEAPONS, CREW, DRONES };

    // HUD bands in the game's own 1280x720 coordinates.
    const int TOP_BAND_BOTTOM = 145;    // hull, shields, evade/O2, resources, FTL/ship/store/options buttons
    const int TOP_BAND_RIGHT = 800;     // the enemy window starts to the right of this
    const int BOTTOM_BAND_TOP = 590;    // system power bars, weapons, drones, subsystems
    const int BOTTOM_RIGHT_LEFT = 1020; // subsystems and the More Info button sit right of this

    const float MIN_ZOOM = 1.f;
    const float MAX_ZOOM = 2.5f;

    bool inGuiRender = false;
    bool inCrewControlRender = false;
    float zoom = 1.f;
    float panX = 0.f;
    float panY = 0.f;
    Region lastRegion = Region::NONE;
    Region dragRegion = Region::NONE;
    bool dragging = false;
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

    // In a run (not the main menu or hangar).
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

    // The windows that get scaled up: the star map, event boxes, the store and ship screens (upgrades, crew,
    // cargo) and the pause menu. The options and controls screens already fill the game and stay 1:1.
    bool StarMapOpen(CommandGui *gui) { return gui->starMap != nullptr && gui->starMap->bOpen; }
    bool ChoiceOpen(CommandGui *gui) { return gui->choiceBox.bOpen; }

    bool ScaledWindow(CommandGui *gui, FocusWindow *window)
    {
        return window == (FocusWindow *)gui->starMap || window == (FocusWindow *)&gui->choiceBox || window == (FocusWindow *)&gui->shipScreens ||
               window == (FocusWindow *)&gui->storeScreens || window == (FocusWindow *)&gui->menuBox;
    }

    bool ScaledModalOpen(CommandGui *gui)
    {
        if (gui->gameover) return false;
        bool any = ChoiceOpen(gui);
        for (auto window : gui->focusWindows)
        {
            if (window == nullptr || !window->bOpen) continue;
            // Any other window open with them (options, controls...) keeps 1:1 input.
            if (!ScaledWindow(gui, window)) return false;
            any = true;
        }
        return any;
    }

    int ExtraX(CApp *app) { return app->modifier_x; }
    int ExtraY(CApp *app) { return app->modifier_y; }

    // How much the HUD is enlarged: up to 1.4x when the canvas has room (Fold inner screen), 1x on a 720-high canvas.
    float HudScale(CApp *app)
    {
        static const float forced = []
        {
            const char *value = std::getenv("FTL_FOLD_HUD_SCALE");
            return value != nullptr ? (float)std::atof(value) : 0.f;
        }();
        if (forced >= 1.f) return forced;
        float scale = (float)(app->screen_y - 480) / 240.f;
        return (std::max)(1.f, (std::min)(1.4f, scale));
    }

    // The star map window is about 750x580 in game coordinates: let it fill the canvas width. The store and ship
    // screens are about 590x560 and the pause menu smaller: up to twice their size.
    float ModalScale(CApp *app)
    {
        if (app->gui != nullptr && !StarMapOpen(app->gui) && !ChoiceOpen(app->gui))
        {
            float scale = (std::min)((float)(app->screen_x - 24) / 600.f, (float)(app->screen_y - 24) / 570.f);
            return (std::max)(1.f, (std::min)(2.f, scale));
        }
        float scale = (std::min)((float)app->screen_x / 760.f, (float)app->screen_y / 600.f);
        return (std::max)(1.f, (std::min)(1.68f, scale));
    }

    // On a tall canvas (the Fold's inner screen) the bottom HUD is rearranged into a panel: systems stacked over
    // subsystems at the bottom left (under the player ship), weapons at the bottom right (under the enemy window)
    // with drones above them. That leaves room to enlarge it well beyond the top HUD.
    bool TwoRowBottom(CApp *app) { return ExtraY(app) >= 150; }

    const float SUBSYSTEM_ROW_TOP = 580.f; // the subsystems row, with the enlarged door buttons, in game y
    const float DOOR_BUTTON_SCALE = 1.3f;  // the open/close all doors buttons (and the doors box) on top of that

    // Where the weapon boxes start, in game x; the systems end there.
    float WeaponsLeft(CApp *app)
    {
        if (app->gui == nullptr) return 245.f;
        CombatControl &combat = app->gui->combatControl;
        int x = combat.weapControl.location.x;
        if (x < 100 || x > 800) x = combat.droneControl.location.x;
        return (x < 100 || x > 800) ? 245.f : (float)x;
    }

    bool HasDrones(CApp *app)
    {
        if (app->gui == nullptr) return false;
        ShipManager *ship = app->gui->shipComplete != nullptr ? app->gui->shipComplete->shipManager : nullptr;
        int x = app->gui->combatControl.droneControl.location.x;
        return ship != nullptr && ship->HasSystem(4) && x >= 100 && x <= 1100; // drone control
    }

    // The ships move up a little to make room for the bottom panel.
    float WorldShiftY(CApp *app)
    {
        if (!TwoRowBottom(app)) return 0.f;
        return -(std::max)(0.f, (std::min)(60.f, (float)ExtraY(app) - 157.f));
    }

    // Systems/subsystems panel scale.
    float BottomScale(CApp *app)
    {
        float s = HudScale(app);
        if (!TwoRowBottom(app)) return s;
        static const float forced = []
        {
            const char *value = std::getenv("FTL_FOLD_BOTTOM_SCALE");
            return value != nullptr ? (float)std::atof(value) : 0.f;
        }();
        float target = forced >= 1.f ? forced : 1.7f;
        // Both panel rows fit under the (shifted) player ship, whose lowest point is about game y 525.
        float room = (float)ExtraY(app) + 720.f - (525.f + WorldShiftY(app)) - 4.f;
        float fitY = room / ((720.f - SUBSYSTEM_ROW_TOP) + (720.f - BOTTOM_BAND_TOP));
        return (std::max)(s, (std::min)(target, fitY));
    }

    // Right edge of the panel, in canvas x.
    float PanelRight(CApp *app)
    {
        return -(float)ExtraX(app) + 4.f + BottomScale(app) * (std::max)(WeaponsLeft(app), 1280.f - BOTTOM_RIGHT_LEFT);
    }

    // Weapons (and drones) scale and where they start, in canvas x.
    float WeaponsX(CApp *app) { return PanelRight(app) + 12.f; }

    float WeaponsScale(CApp *app)
    {
        float b = BottomScale(app);
        if (!TwoRowBottom(app)) return b;
        float fitX = (1280.f + ExtraX(app) - 4.f - WeaponsX(app)) / 430.f;
        // With drones, two rows sit under the enemy window (its lowest point is about game y 580).
        float room = (float)ExtraY(app) + 720.f - (580.f + WorldShiftY(app)) - 4.f;
        float fitY = HasDrones(app) ? room / 270.f : 10.f;
        return (std::max)(1.f, (std::min)(1.7f, (std::min)(fitX, fitY)));
    }

    // Drones make the bottom-left group so wide that, enlarged, it would reach the subsystems: lift those a row.
    bool BottomRightLifted(CApp *app)
    {
        if (TwoRowBottom(app)) return false;
        float s = BottomScale(app);
        ShipManager *ship = app->gui->shipComplete != nullptr ? app->gui->shipComplete->shipManager : nullptr;
        bool drones = ship != nullptr && ship->HasSystem(4); // drone control
        float bottomLeftRight = -ExtraX(app) + s * (drones ? 905.f : 645.f);
        float bottomRightLeft = 1280.f + ExtraX(app) - s * (1280.f - BOTTOM_RIGHT_LEFT);
        return bottomLeftRight + 4.f > bottomRightLeft;
    }

    // The crew boxes and the save/return stations buttons under them.
    float CrewScale(CApp *app) { return TwoRowBottom(app) ? 1.7f : HudScale(app); }
    const float CREW_PANEL_TOP = 145.f;

    // A HUD group or window maps game point p to canvas point c + s * (p - a).
    struct Anchor
    {
        float ax, ay, cx, cy, s;
    };

    Anchor GetAnchor(CApp *app, Region region)
    {
        float ex = (float)ExtraX(app), ey = (float)ExtraY(app), s = HudScale(app), b = BottomScale(app);
        bool panel = TwoRowBottom(app);
        switch (region)
        {
        case Region::TOP_LEFT: return {0.f, 0.f, -ex, -ey, s};
        case Region::CREW:
            if (!panel) return {0.f, 0.f, -ex, -ey, s};
            return {0.f, CREW_PANEL_TOP, -ex, -ey + s * CREW_PANEL_TOP + 4.f, CrewScale(app)};
        case Region::BOTTOM_LEFT:
            if (!panel) return {0.f, 720.f, -ex, 720.f + ey, b};
            // Systems: the upper row of the panel.
            return {0.f, 720.f, -ex + 4.f, 720.f + ey - b * (720.f - SUBSYSTEM_ROW_TOP), b};
        case Region::BOTTOM_RIGHT:
        {
            // Subsystems and More Info: the lower row of the panel.
            if (panel) return {BOTTOM_RIGHT_LEFT, 720.f, -ex + 4.f, 720.f + ey, b};
            float lift = BottomRightLifted(app) ? b * (720.f - BOTTOM_BAND_TOP + 10.f) : 0.f;
            return {1280.f, 720.f, 1280.f + ex, 720.f + ey - lift, b};
        }
        case Region::WEAPONS:
            if (!panel) return {0.f, 720.f, -ex, 720.f + ey, b};
            return {WeaponsLeft(app) - 8.f, 720.f, WeaponsX(app), 720.f + ey, WeaponsScale(app)};
        case Region::DRONES:
        {
            if (!panel || app->gui == nullptr) return {0.f, 720.f, -ex, 720.f + ey, b};
            float w = WeaponsScale(app);
            float left = (float)app->gui->combatControl.droneControl.location.x - 8.f;
            return {left, 720.f, WeaponsX(app), 720.f + ey - w * (720.f - BOTTOM_BAND_TOP + 5.f), w};
        }
        case Region::MODAL:
            // The star map window sits right of centre in the 1280x720 layout; centre it on the canvas.
            if (StarMapOpen(app->gui)) return {715.f, 375.f, 640.f, 360.f, ModalScale(app)};
            if (ChoiceOpen(app->gui)) return {640.f, 360.f, 640.f, 360.f, ModalScale(app)};
            return {635.f, 338.f, 640.f, 360.f, ModalScale(app)}; // store, ship screens, pause menu
        default: return {0.f, 0.f, 0.f, 0.f, 1.f};
        }
    }

    // The doors box (the subsystem with the open/close all doors buttons) is drawn bigger again inside the
    // subsystems row, grown up and to the right from just left of its icon.
    DoorBox *lastDoorBox = nullptr;

    bool DoorAnchor(DoorBox *box, float &ax, float &ay)
    {
        if (box == nullptr) return false;
        // The buttons' hitboxes are relative to buttonOffset (DoorBox::MouseMove tests the mouse minus it).
        ax = (float)(box->buttonOffset.x + (std::min)(box->openDoors.hitbox.x, box->closeDoors.hitbox.x)) - 25.f;
        ay = 700.f;
        // Only where the subsystems are: anything else means the layout is not what this expects; leave it alone.
        return ax >= BOTTOM_RIGHT_LEFT && ax < 1280.f;
    }

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

    void ApplyTransform(CApp *app, Region region)
    {
        if (region == Region::WORLD)
        {
            CSurface::GL_Translate(640.f + panX, 360.f + panY + WorldShiftY(app), 0.f);
            CSurface::GL_Scale(zoom, zoom, 1.f);
            CSurface::GL_Translate(-640.f, -360.f, 0.f);
            return;
        }
        Anchor a = GetAnchor(app, region);
        CSurface::GL_Translate(a.cx, a.cy, 0.f);
        CSurface::GL_Scale(a.s, a.s, 1.f);
        CSurface::GL_Translate(-a.ax, -a.ay, 0.f);
    }

    // Undoes ApplyTransform(region), for drawing in another space from inside a region.
    void ApplyInverse(CApp *app, Region region)
    {
        if (region == Region::WORLD)
        {
            CSurface::GL_Translate(640.f, 360.f, 0.f);
            CSurface::GL_Scale(1.f / zoom, 1.f / zoom, 1.f);
            CSurface::GL_Translate(-640.f - panX, -360.f - panY - WorldShiftY(app), 0.f);
            return;
        }
        Anchor a = GetAnchor(app, region);
        CSurface::GL_Translate(a.ax, a.ay, 0.f);
        CSurface::GL_Scale(1.f / a.s, 1.f / a.s, 1.f);
        CSurface::GL_Translate(-a.cx, -a.cy, 0.f);
    }

    void PushRegion(CApp *app, Region region)
    {
        CSurface::GL_PushMatrix();
        ApplyTransform(app, region);
    }

    // Canvas point (game coordinates) -> the game point drawn there in a region.
    void InverseMap(CApp *app, Region region, float x, float y, float &outX, float &outY)
    {
        if (region == Region::WORLD)
        {
            outX = 640.f + (x - 640.f - panX) / zoom;
            outY = 360.f + (y - 360.f - panY - WorldShiftY(app)) / zoom;
            return;
        }
        if (region == Region::NONE)
        {
            outX = x;
            outY = y;
            return;
        }
        Anchor a = GetAnchor(app, region);
        outX = a.ax + (x - a.cx) / a.s;
        outY = a.ay + (y - a.cy) / a.s;
    }

    bool InsideRect(const Globals::Rect &rect, int x, int y)
    {
        return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
    }

    // Crew boxes and the save/return-stations buttons under them (top-left group).
    bool OnCrewPanel(CommandGui *gui, int x, int y)
    {
        for (auto box : gui->crewControl.crewBoxes)
        {
            if (box != nullptr && InsideRect(box->box, x, y)) return true;
        }
        return InsideRect(gui->crewControl.saveStations.hitbox, x, y) || InsideRect(gui->crewControl.returnStations.hitbox, x, y);
    }

    bool InTopBand(int x, int y) { return y < TOP_BAND_BOTTOM && x < TOP_BAND_RIGHT; }

    // Which region a canvas point belongs to, and the game point under it.
    Region Classify(CApp *app, float x, float y, float &gx, float &gy)
    {
        CommandGui *gui = app->gui;
        if (ModalOpen(gui))
        {
            Region region = ScaledModalOpen(gui) ? Region::MODAL : Region::NONE;
            InverseMap(app, region, x, y, gx, gy);
            return region;
        }

        if (TwoRowBottom(app))
        {
            // Panel layout: subsystems (bottom-left, lower row), systems (above them), weapons and drones (bottom right).
            float wl = WeaponsLeft(app), weaponsEnd = wl + 430.f;
            if (HasDrones(app))
            {
                InverseMap(app, Region::DRONES, x, y, gx, gy);
                float left = (float)gui->combatControl.droneControl.location.x - 8.f;
                weaponsEnd = (std::min)(weaponsEnd, left);
                if (gy >= BOTTOM_BAND_TOP && gy < 720.f && gx >= left && gx < left + 280.f) return Region::DRONES;
            }
            InverseMap(app, Region::WEAPONS, x, y, gx, gy);
            if (gy >= BOTTOM_BAND_TOP && gy < 720.f && gx >= wl - 8.f && gx < weaponsEnd) return Region::WEAPONS;

            bool inPanel = x < PanelRight(app);
            InverseMap(app, Region::BOTTOM_RIGHT, x, y, gx, gy);
            if (inPanel && gy >= SUBSYSTEM_ROW_TOP && gy < 720.f && gx >= BOTTOM_RIGHT_LEFT && gx < 1280.f) return Region::BOTTOM_RIGHT;

            InverseMap(app, Region::BOTTOM_LEFT, x, y, gx, gy);
            if (inPanel && gy >= BOTTOM_BAND_TOP && gy < 720.f)
            {
                // The weapons are no longer drawn right of the systems: nothing to hit there.
                if (gx >= wl)
                {
                    gx = -10000.f;
                    gy = -10000.f;
                }
                return Region::BOTTOM_LEFT;
            }
        }
        else
        {
            InverseMap(app, Region::BOTTOM_RIGHT, x, y, gx, gy);
            if (gy >= BOTTOM_BAND_TOP && gy < 720.f && gx >= BOTTOM_RIGHT_LEFT && gx < 1280.f) return Region::BOTTOM_RIGHT;

            InverseMap(app, Region::BOTTOM_LEFT, x, y, gx, gy);
            if (gy >= BOTTOM_BAND_TOP && gx < BOTTOM_RIGHT_LEFT) return Region::BOTTOM_LEFT;
        }

        InverseMap(app, Region::CREW, x, y, gx, gy);
        if (OnCrewPanel(gui, (int)gx, (int)gy)) return Region::CREW;

        InverseMap(app, Region::TOP_LEFT, x, y, gx, gy);
        if (InTopBand((int)gx, (int)gy)) return Region::TOP_LEFT;

        InverseMap(app, Region::WORLD, x, y, gx, gy);
        return Region::WORLD;
    }

    // A canvas point -> the game point FTL should see. Taps on parts of the world that sit under the HUD in the
    // original layout are dropped (they would hit the moved HUD); a drag keeps the region it started in.
    Point MapDrawnPoint(CApp *app, int x, int y, bool startDrag)
    {
        float gx, gy;
        if (dragging)
        {
            InverseMap(app, dragRegion, (float)x, (float)y, gx, gy);
            lastRegion = dragRegion;
            return Point((int)std::floor(gx), (int)std::floor(gy));
        }

        Region region = Classify(app, (float)x, (float)y, gx, gy);
        lastRegion = region;
        if (startDrag)
        {
            dragging = true;
            dragRegion = region;
        }
        int ix = (int)std::floor(gx), iy = (int)std::floor(gy);
        if (region == Region::WORLD && (InTopBand(ix, iy) || iy >= BOTTOM_BAND_TOP || OnCrewPanel(app->gui, ix, iy)))
        {
            return Point(-10000, -10000);
        }
        return Point(ix, iy);
    }

    // Window coordinates in -> window coordinates that FTL's own transform turns into the mapped game point.
    void MapWindowPoint(CApp *app, int &x, int &y, bool startDrag = false)
    {
        if (!InGame(app))
        {
            dragging = false;
            return;
        }
        Point game = MapDrawnPoint(app, x - app->modifier_x, y - app->modifier_y, startDrag);
        x = game.x + app->modifier_x;
        y = game.y + app->modifier_y;
    }

    void ZoomAt(CApp *app, int windowX, int windowY, float factor)
    {
        float x = (float)(windowX - app->modifier_x);
        float y = (float)(windowY - app->modifier_y);
        // Keep the world point under the pointer where it is.
        float worldX = 640.f + (x - 640.f - panX) / zoom;
        float worldY = 360.f + (y - 360.f - panY - WorldShiftY(app)) / zoom;
        zoom *= factor;
        zoom = (std::max)(MIN_ZOOM, (std::min)(MAX_ZOOM, zoom));
        panX = x - 640.f - (worldX - 640.f) * zoom;
        panY = y - 360.f - WorldShiftY(app) - (worldY - 360.f) * zoom;
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
        hs_log_file("Fold layout: canvas %dx%d, HUD offsets %d/%d, HUD scale %.2f, %s\n", app->screen_x, app->screen_y,
                    app->modifier_x, app->modifier_y, HudScale(app), LayoutActive(app) ? "active" : "inactive (needs fullscreen=2 and a canvas bigger than 1280x720)");
    }

    // Wraps one render call in a region transform while the in-game screen is drawn.
    // Windows (star map, event box, store...) pass absolute = true: they can be drawn from inside another region's
    // render (an event box opened by a ship is drawn with the ships), and must not inherit its zoom.
    Region activeRegion = Region::NONE;

    struct RegionScope
    {
        bool pushed = false;
        Region saved;
        RegionScope(Region region, bool absolute = false)
        {
            saved = activeRegion;
            CApp *app = G_->GetCApp();
            if (!inGuiRender || !InGame(app)) return;
            if (absolute && activeRegion != Region::NONE && activeRegion != region)
            {
                CSurface::GL_PushMatrix();
                ApplyInverse(app, activeRegion);
                if (region != Region::NONE) ApplyTransform(app, region);
                pushed = true;
                activeRegion = region;
            }
            else if (region != Region::NONE)
            {
                PushRegion(app, region);
                pushed = true;
                activeRegion = region;
            }
        }
        ~RegionScope()
        {
            if (pushed) CSurface::GL_PopMatrix();
            activeRegion = saved;
        }
    };
}

// ---- Window / clip ----

// Hyperspace's More Info button is drawn from the top-left group but belongs in the bottom-right one: switches
// from the top-left transform to the bottom-right one until FoldLayoutPopMatrix().
bool FoldLayoutPushTopLeftToBottomRight()
{
    CApp *app = G_->GetCApp();
    if (!inGuiRender || !InGame(app)) return false;
    CSurface::GL_PushMatrix();
    ApplyInverse(app, Region::TOP_LEFT);
    ApplyTransform(app, Region::BOTTOM_RIGHT);
    return true;
}

void FoldLayoutPopMatrix() { CSurface::GL_PopMatrix(); }

// For the test harness: where a game point in a region ("tl", "bl", "br", "world", "modal", "none") is on the
// window right now, so test taps land on controls whatever the canvas size and zoom.
bool FoldLayoutToWindow(const std::string &region, float x, float y, int &windowX, int &windowY)
{
    CApp *app = G_->GetCApp();
    if (app == nullptr) return false;
    float cx = x, cy = y;
    if (InGame(app) && region != "none")
    {
        if (region == "world")
        {
            cx = 640.f + panX + zoom * (x - 640.f);
            cy = 360.f + panY + WorldShiftY(app) + zoom * (y - 360.f);
        }
        else
        {
            float dx, dy;
            if (region == "door" && TwoRowBottom(app) && DoorAnchor(lastDoorBox, dx, dy))
            {
                // A point of the doors box: enlarged inside the subsystems group.
                x = dx + DOOR_BUTTON_SCALE * (x - dx);
                y = dy + DOOR_BUTTON_SCALE * (y - dy);
            }
            Region r = region == "tl" ? Region::TOP_LEFT : region == "bl" ? Region::BOTTOM_LEFT :
                       region == "br" || region == "door" ? Region::BOTTOM_RIGHT : region == "wp" ? Region::WEAPONS :
                       region == "cr" ? Region::CREW : region == "dr" ? Region::DRONES : Region::MODAL;
            Anchor a = GetAnchor(app, r);
            cx = a.cx + a.s * (x - a.ax);
            cy = a.cy + a.s * (y - a.ay);
        }
    }
    windowX = (int)std::lround(cx) + app->modifier_x;
    windowY = (int)std::lround(cy) + app->modifier_y;
    return true;
}

// For the test harness: where the last input landed, and the view.
const char *FoldLayoutDescribe()
{
    static char text[220];
    static const char *names[] = {"none", "top-left", "bottom-left", "bottom-right", "world", "modal", "weapons", "crew", "drones"};
    CApp *app = G_->GetCApp();
    bool game = app != nullptr && InGame(app);
    std::snprintf(text, sizeof(text), "region=%s zoom=%.2f pan=%.0f,%.0f hudScale=%.2f bottomScale=%.2f modalScale=%.2f twoRow=%d lifted=%d",
                  names[(int)lastRegion], zoom, panX, panY, game ? HudScale(app) : 0.f, game ? BottomScale(app) : 0.f,
                  game ? ModalScale(app) : 0.f, game ? (int)TwoRowBottom(app) : 0, game ? (int)BottomRightLifted(app) : 0);
    return text;
}

bool FoldTestCanvas(int &width, int &height); // FoldTest.cpp

void FoldLayoutWheel(CApp *app, int windowX, int windowY, float notches)
{
    if (InGame(app) && !ModalOpen(app->gui)) ZoomAt(app, windowX, windowY, std::pow(1.1f, notches));
}

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
        int testWidth, testHeight;
        if (FoldTestCanvas(testWidth, testHeight))
        {
            // PC testing: a window the size of the phone's canvas (UpdateWindowSettings below draws it unscaled).
            settings->fullscreen = 0;
            settings->currentFullscreen = 0;
            settings->manualResolution = true;
            settings->manualWindowed = true;
            settings->manualStretched = false;
            settings->screenResolution = Point(testWidth, testHeight);
            hs_log_file("Fold layout: test canvas %dx%d\n", testWidth, testHeight);
        }
        else
        {
            settings->fullscreen = 2;
            settings->currentFullscreen = 2;
            settings->manualResolution = false;
            settings->manualWindowed = false;
            settings->manualStretched = false;
            hs_log_file("Fold layout: forcing fullscreen=2 (borders) on the desktop\n");
        }
    }
    return super();
}

HOOK_METHOD_PRIORITY(CApp, UpdateFullScreen, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::UpdateFullScreen -> Begin (FoldLayout.cpp)\n")
    if (ForceBordersMode()) return;
    super();
}

// Whatever window FTL ended up with, draw the game unscaled and centred on it (borders mode's behaviour), so a
// canvas bigger than 1280x720 is never letterboxed through the framebuffer.
HOOK_METHOD_PRIORITY(CApp, UpdateWindowSettings, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::UpdateWindowSettings -> Begin (FoldLayout.cpp)\n")
    super();
    if (!ForceBordersMode() || screen_x < 1280 || screen_y < 720) return;
    useFrameBuffer = false;
    x_bar = 0;
    y_bar = 0;
    mouseModifier_x = 1.f;
    mouseModifier_y = 1.f;
    modifier_x = (screen_x - 1280) / 2;
    modifier_y = (screen_y - 720) / 2;
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
    RegionScope scope(Region::WEAPONS);
    super(front);
}

HOOK_METHOD_PRIORITY(DroneControl, OnRender, -10000, (bool front) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> DroneControl::OnRender -> Begin (FoldLayout.cpp)\n")
    RegionScope scope(Region::DRONES, true);
    super(front);
}

HOOK_METHOD_PRIORITY(DoorBox, OnRender, -10000, (bool ignoreStatus) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> DoorBox::OnRender -> Begin (FoldLayout.cpp)\n")
    CApp *app = G_->GetCApp();
    float ax, ay;
    if (!inGuiRender || !InGame(app) || !TwoRowBottom(app) || !DoorAnchor(this, ax, ay)) return super(ignoreStatus);
    lastDoorBox = this;
    CSurface::GL_PushMatrix();
    CSurface::GL_Translate(ax, ay, 0.f);
    CSurface::GL_Scale(DOOR_BUTTON_SCALE, DOOR_BUTTON_SCALE, 1.f);
    CSurface::GL_Translate(-ax, -ay, 0.f);
    super(ignoreStatus);
    CSurface::GL_PopMatrix();
}

HOOK_METHOD_PRIORITY(DoorBox, MouseMove, -10000, (int mX, int mY) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> DoorBox::MouseMove -> Begin (FoldLayout.cpp)\n")
    CApp *app = G_->GetCApp();
    float ax, ay;
    if (InGame(app) && TwoRowBottom(app) && DoorAnchor(this, ax, ay) && mX > -9000)
    {
        mX = (int)std::floor(ax + (mX - ax) / DOOR_BUTTON_SCALE);
        mY = (int)std::floor(ay + (mY - ay) / DOOR_BUTTON_SCALE);
    }
    super(mX, mY);
}

HOOK_METHOD_PRIORITY(SystemControl, OnRender, -10000, (bool front) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> SystemControl::OnRender -> Begin (FoldLayout.cpp)\n")
    CApp *app = G_->GetCApp();
    if (!inGuiRender || !InGame(app)) return super(front);
    if (app->useDirect3D)
    {
        RegionScope scope(Region::BOTTOM_LEFT);
        return super(front);
    }

    // It draws the systems and the subsystems in one go: draw it once per group, each pass clipped to the part
    // of the canvas that group owns.
    glEnable(GL_SCISSOR_TEST);
    if (TwoRowBottom(app))
    {
        // Panel: systems on top of subsystems. GL scissor y counts from the bottom of the window.
        GLsizei panelRight = (GLsizei)(app->modifier_x + PanelRight(app)) + 2;
        Anchor br = GetAnchor(app, Region::BOTTOM_RIGHT);
        GLint split = app->screen_y - (GLint)(app->modifier_y + br.cy - br.s * (720.f - SUBSYSTEM_ROW_TOP));
        glScissor(0, split, panelRight, app->screen_y - split);
        PushRegion(app, Region::BOTTOM_LEFT);
        super(front);
        CSurface::GL_PopMatrix();
        glScissor(0, 0, panelRight, split);
        PushRegion(app, Region::BOTTOM_RIGHT);
        super(front);
        CSurface::GL_PopMatrix();
        glDisable(GL_SCISSOR_TEST);
        return;
    }
    float split = app->modifier_x - ExtraX(app) + BottomScale(app) * BOTTOM_RIGHT_LEFT; // window x of the split, left pass
    Anchor br = GetAnchor(app, Region::BOTTOM_RIGHT);
    float rightStart = app->modifier_x + br.cx - br.s * (br.ax - BOTTOM_RIGHT_LEFT);
    glScissor(0, 0, (GLsizei)split, app->screen_y);
    PushRegion(app, Region::BOTTOM_LEFT);
    super(front);
    CSurface::GL_PopMatrix();
    glScissor((GLint)rightStart, 0, app->screen_x - (GLint)rightStart, app->screen_y);
    PushRegion(app, Region::BOTTOM_RIGHT);
    super(front);
    CSurface::GL_PopMatrix();
    glDisable(GL_SCISSOR_TEST);
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
    RegionScope scope(Region::CREW);
    inCrewControlRender = scope.pushed;
    super();
    inCrewControlRender = false;
}

// CrewControl::OnRender ends with the drag-selection rectangle, in the coordinates of the drag (the ships).
HOOK_STATIC_PRIORITY(CSurface, GL_DrawRectOutline, -10000, (int x1, int y1, int x2, int y2, GL_Color color, float lineWidth) -> bool)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_DrawRectOutline -> Begin (FoldLayout.cpp)\n")
    CApp *app = G_->GetCApp();
    if (!inCrewControlRender || app->gui == nullptr) return super(x1, y1, x2, y2, color, lineWidth);
    const CrewControl &crew = app->gui->crewControl;
    if (x1 != crew.firstMouse.x || y1 != crew.firstMouse.y) return super(x1, y1, x2, y2, color, lineWidth);

    CSurface::GL_PushMatrix();
    ApplyInverse(app, Region::CREW);
    ApplyTransform(app, dragging ? dragRegion : Region::WORLD);
    bool result = super(x1, y1, x2, y2, color, lineWidth);
    CSurface::GL_PopMatrix();
    return result;
}

HOOK_METHOD_PRIORITY(StarMap, OnRender, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> StarMap::OnRender -> Begin (FoldLayout.cpp)\n")
    CApp *app = G_->GetCApp();
    RegionScope scope(app != nullptr && app->gui != nullptr && ScaledModalOpen(app->gui) ? Region::MODAL : Region::NONE, true);
    super();
}

// The store and ship screens and the pause menu. A window drawn from inside another (a confirm box inside the
// store) is already in the scaled space.
static int modalWindowDepth = 0;

static Region ModalWindowRegion()
{
    CApp *app = G_->GetCApp();
    return modalWindowDepth == 0 && app != nullptr && app->gui != nullptr && ScaledModalOpen(app->gui) ? Region::MODAL : Region::NONE;
}

HOOK_METHOD_PRIORITY(TabbedWindow, OnRender, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> TabbedWindow::OnRender -> Begin (FoldLayout.cpp)\n")
    RegionScope scope(ModalWindowRegion(), modalWindowDepth == 0);
    modalWindowDepth++;
    super();
    modalWindowDepth--;
}

HOOK_METHOD_PRIORITY(MenuScreen, OnRender, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> MenuScreen::OnRender -> Begin (FoldLayout.cpp)\n")
    RegionScope scope(ModalWindowRegion(), modalWindowDepth == 0);
    modalWindowDepth++;
    super();
    modalWindowDepth--;
}

HOOK_METHOD_PRIORITY(ChoiceBox, OnRender, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ChoiceBox::OnRender -> Begin (FoldLayout.cpp)\n")
    CApp *app = G_->GetCApp();
    RegionScope scope(app != nullptr && app->gui != nullptr && this == &app->gui->choiceBox && ScaledModalOpen(app->gui) ? Region::MODAL : Region::NONE, true);
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
    if (gui != nullptr && (this == &gui->upgradeButton || this == &gui->optionsButton)) region = Region::TOP_LEFT;
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

// FTL_HIDE_CURSOR=1 (set by the Android launcher): a touch screen has no pointer, so the cursor and the icons that
// follow it are not drawn; tooltips still are. MouseControl::OnRender draws both, so colour writes are switched off
// for it and back on for the tooltip.
static bool HideCursor()
{
    static const bool hide = []
    {
        const char *value = std::getenv("FTL_HIDE_CURSOR");
        return value != nullptr && value[0] == '1';
    }();
    return hide;
}

static bool cursorMasked = false;

HOOK_METHOD_PRIORITY(MouseControl, OnRender, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> MouseControl::OnRender -> Begin (FoldLayout.cpp)\n")
    CApp *app = G_->GetCApp();
    bool pushed = false;
    if (lastRegion != Region::NONE && InGame(app))
    {
        PushRegion(app, lastRegion);
        pushed = true;
    }
    cursorMasked = HideCursor() && app != nullptr && !app->useDirect3D;
    if (cursorMasked) glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    super();
    if (cursorMasked) glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    cursorMasked = false;
    if (pushed) CSurface::GL_PopMatrix();
}

HOOK_METHOD_PRIORITY(MouseControl, RenderTooltip, -10000, (Point tooltipPoint, bool staticPos) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> MouseControl::RenderTooltip -> Begin (FoldLayout.cpp)\n")
    if (cursorMasked) glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    super(tooltipPoint, staticPos);
    if (cursorMasked) glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
}

// ---- Star map: beacons and sectors are small for a finger, so a touch near one counts as touching it ----

static bool OnButton(const GenericButton &button, int x, int y)
{
    return button.bActive && x >= button.hitbox.x && x < button.hitbox.x + button.hitbox.w && y >= button.hitbox.y && y < button.hitbox.y + button.hitbox.h;
}

static void SnapToStarMapTarget(StarMap *map, int &x, int &y)
{
    CApp *app = G_->GetCApp();
    if (!InGame(app) || !map->bOpen) return;
    if (OnButton(map->endButton, x, y) || OnButton(map->waitButton, x, y) || OnButton(map->distressButton, x, y) ||
        OnButton(map->jumpButton, x, y) || OnButton(map->closeButton, x, y) || OnButton(map->closeSectorButton, x, y)) return;

    const float radius = 34.f;
    float best = radius * radius;
    float bestX = 0.f, bestY = 0.f;
    bool found = false;
    auto consider = [&](float cx, float cy)
    {
        float d = (cx - x) * (cx - x) + (cy - y) * (cy - y);
        if (d < best)
        {
            best = d;
            bestX = cx;
            bestY = cy;
            found = true;
        }
    };
    if (map->bChoosingNewSector)
    {
        for (const Globals::Rect &r : map->sectorHitBoxes)
        {
            int rx = map->sectorMapOffset.x + r.x, ry = map->sectorMapOffset.y + r.y;
            if (x > rx && x < rx + r.w && y > ry && y < ry + r.h) return; // already on one
            consider(rx + r.w / 2.f, ry + r.h / 2.f);
        }
    }
    else
    {
        for (Location *loc : map->locations)
        {
            if (loc != nullptr) consider(map->position.x + map->translation.x + loc->loc.x, map->position.y + map->translation.y + loc->loc.y);
        }
    }
    if (found)
    {
        x = (int)std::lround(bestX);
        y = (int)std::lround(bestY);
    }
}

HOOK_METHOD_PRIORITY(StarMap, MouseMove, -10000, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> StarMap::MouseMove -> Begin (FoldLayout.cpp)\n")
    SnapToStarMapTarget(this, x, y);
    super(x, y);
}

HOOK_METHOD_PRIORITY(StarMap, MouseClick, -10000, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> StarMap::MouseClick -> Begin (FoldLayout.cpp)\n")
    SnapToStarMapTarget(this, x, y);
    super(x, y);
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
    if (!holdingLMB) dragging = false;
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
    dragging = false;
    MapWindowPoint(this, x, y, true);
    super(x, y);
}

HOOK_METHOD_PRIORITY(CApp, OnLButtonUp, -10000, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnLButtonUp -> Begin (FoldLayout.cpp)\n")
    MapWindowPoint(this, x, y);
    dragging = false;
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

#else

bool FoldLayoutPushTopLeftToBottomRight() { return false; }
void FoldLayoutPopMatrix() {}
const char *FoldLayoutDescribe() { return ""; }

#endif // _WIN32
