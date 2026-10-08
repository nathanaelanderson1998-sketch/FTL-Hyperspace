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
    // otherwise they follow the bottom-left and top-left groups. TARGET = the enemy window: part of the world (it
    // zooms with it), enlarged on top of that.
    enum class Region { NONE, TOP_LEFT, BOTTOM_LEFT, BOTTOM_RIGHT, WORLD, MODAL, WEAPONS, CREW, DRONES, TARGET };

    // HUD bands in the game's own 1280x720 coordinates.
    const int TOP_BAND_BOTTOM = 145;    // hull, shields, evade/O2, resources, FTL/ship/store/options buttons
    const int TOP_BAND_RIGHT = 800;     // the enemy window starts to the right of this
    const int BOTTOM_BAND_TOP = 590;    // system power bars, weapons, drones, subsystems
    const int BOTTOM_RIGHT_LEFT = 1020; // subsystems and the More Info button sit right of this

    const float MIN_ZOOM = 1.f;
    const float MAX_ZOOM = 2.5f;

    bool inGuiRender = false;
    bool inCrewControlRender = false;
    bool drawingStationPlate = false; // our enlarged stations plate, not FTL's own
    float zoom = 1.f;
    float panX = 0.f;
    float panY = 0.f;
    Region lastRegion = Region::NONE;
    Region pressRegion = Region::NONE; // where the last button press landed (for the test harness)
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
    // The store, ship screens and pause menu sit below the hull/scrap/fuel/missiles/drones rows (game y 0..82 of the
    // top-left group), so the player's balances stay in view while shopping. Canvas y range they may use:
    void WindowArea(CApp *app, float &top, float &bottom)
    {
        float ey = (float)ExtraY(app);
        top = -ey + HudScale(app) * 82.f + 6.f;
        bottom = 720.f + ey - 6.f;
    }

    float ModalScale(CApp *app)
    {
        if (app->gui != nullptr && !StarMapOpen(app->gui) && !ChoiceOpen(app->gui))
        {
            float top, bottom;
            WindowArea(app, top, bottom);
            float scale = (std::min)((float)(app->screen_x - 24) / 600.f, (bottom - top) / 565.f);
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
    const float DOOR_BUTTON_SCALE = 1.f;   // the doors box on top of that (the whole panel is door-sized now)

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

    // On the tall canvas the ships start at 0.9x (pinch zooms from there) to leave room for the big bottom panel.
    float BaseZoom(CApp *app) { return TwoRowBottom(app) ? 0.9f : 1.f; }

    // The highest the world may move: the enemy window's top (game y ~40 with its frame) stays below the top bar's buttons (which
    // end about game y 75 of the top-left group).
    float MaxWorldLift(CApp *app)
    {
        float buttonsBottom = -(float)ExtraY(app) + HudScale(app) * 75.f + 6.f;
        return (std::min)(0.f, buttonsBottom - 360.f + BaseZoom(app) * (360.f - 40.f));
    }

    float PanelHeight(CApp *app);

    // The ships move up (as far as the enemy window allows) to sit above the bottom panel, and a little right, away
    // from the crew list.
    float WorldShiftY(CApp *app)
    {
        if (!TwoRowBottom(app)) return 0.f;
        float panelTop = 720.f + (float)ExtraY(app) - PanelHeight(app);
        float shift = panelTop - 8.f - 360.f - BaseZoom(app) * (525.f - 360.f); // the player ship ends about game y 525
        return (std::max)(MaxWorldLift(app), (std::min)(0.f, shift));
    }

    float WorldShiftX(CApp *app) { return TwoRowBottom(app) ? 50.f : 0.f; }

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
        float target = forced >= 1.f ? forced : 2.2f;
        // The panel row fits under the player ship, with the world lifted as far as it may go.
        float shipBottom = 360.f + MaxWorldLift(app) + BaseZoom(app) * (525.f - 360.f);
        float room = (float)ExtraY(app) + 720.f - shipBottom - 8.f;
        float fitY = room / (720.f - SUBSYSTEM_ROW_TOP);
        // No fit to the width: a ship with many systems scrolls its systems row instead of shrinking it.
        return (std::max)(1.f, (std::min)(target, fitY));
    }

    // One row along the bottom: the systems, then the subsystems (with the doors box and More Info), scrolled
    // sideways when wider than the panel.
    float PanelHeight(CApp *app) { return BottomScale(app) * (720.f - SUBSYSTEM_ROW_TOP); }

    // Where the subsystems start in the row, in canvas x, before scrolling.
    // The systems end a little right of where FTL starts the weapon boxes: the last system box (and the end of its
    // frame) reaches about 20 game px past that.
    float SystemsEnd(CApp *app) { return WeaponsLeft(app) + 26.f; }

    // The doors box's open/close-all buttons sit in a slot this wide (game px) at the left end of the row, so they
    // show without scrolling; the systems start after it.
    const float DOOR_SLOT = 36.f;
    float DoorSlot(CApp *app) { return TwoRowBottom(app) ? DOOR_SLOT : 0.f; }

    // Canvas px from the end of the systems to the subsystems; the cut between them is SEAM_CUT px left of the
    // subsystems.
    const float SEAM_GAP = 50.f, SEAM_CUT = 30.f;
    float SubsystemsLeft(CApp *app) { return -(float)ExtraX(app) + 4.f + BottomScale(app) * (DoorSlot(app) + SystemsEnd(app)) + SEAM_GAP; }

    // Right edge of the whole row if nothing were cut, in canvas x.
    float PanelNaturalRight(CApp *app)
    {
        if (!TwoRowBottom(app)) return -(float)ExtraX(app) + 4.f + BottomScale(app) * (std::max)(WeaponsLeft(app), 1280.f - BOTTOM_RIGHT_LEFT);
        return SubsystemsLeft(app) + BottomScale(app) * (1280.f - BOTTOM_RIGHT_LEFT);
    }

    // Right edge of the panel as shown, in canvas x: the weapons keep at least WEAPONS_MIN_SCALE to the right of it.
    const float WEAPONS_MIN_SCALE = 1.5f;
    float PanelRight(CApp *app)
    {
        float natural = PanelNaturalRight(app);
        if (!TwoRowBottom(app)) return natural;
        return (std::min)(natural, 1280.f + (float)ExtraX(app) - 4.f - 12.f - 430.f * WEAPONS_MIN_SCALE);
    }

    // The row scrolls sideways (a swipe across it) when it is wider than the panel. Canvas px, >= 0.
    float systemsScroll = 0.f;

    float MaxSystemsScroll(CApp *app)
    {
        if (!TwoRowBottom(app)) return 0.f;
        return (std::max)(0.f, PanelNaturalRight(app) + 6.f - PanelRight(app));
    }

    float SystemsScroll(CApp *app)
    {
        systemsScroll = (std::max)(0.f, (std::min)(systemsScroll, MaxSystemsScroll(app)));
        return systemsScroll;
    }

    // Weapons (and drones) scale and where they start, in canvas x.
    float WeaponsX(CApp *app) { return PanelRight(app) + 12.f; }

    float WeaponsScale(CApp *app)
    {
        float b = BottomScale(app);
        if (!TwoRowBottom(app)) return b;
        float fitX = (1280.f + ExtraX(app) - 4.f - WeaponsX(app)) / 430.f;
        // With drones, two rows sit under the enemy window (enlarged, its lowest point is about game y 685).
        float enemyBottom = 360.f + WorldShiftY(app) + BaseZoom(app) * (685.f - 360.f);
        float room = (float)ExtraY(app) + 720.f - enemyBottom - 4.f;
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
    const float CREW_PANEL_TOP = 145.f;
    const float STATION_BUTTON_SCALE = 1.25f;

    // The stations buttons (and their plates) grow down and right from their top-left corner.
    bool StationsAnchor(CApp *app, float &ax, float &ay)
    {
        if (app->gui == nullptr || !TwoRowBottom(app)) return false;
        CrewControl &crew = app->gui->crewControl;
        ax = (float)(std::min)(crew.saveStations.hitbox.x, crew.returnStations.hitbox.x) - 4.f;
        ay = (float)(std::min)(crew.saveStations.hitbox.y, crew.returnStations.hitbox.y) - 4.f;
        return ax > 0.f && ax < 200.f && ay > 150.f && ay < 600.f;
    }

    // Bottom of the crew list with its enlarged stations buttons, in game y (the crew frame, before scaling).
    float CrewListBottom(CApp *app)
    {
        float sx, sy;
        if (!StationsAnchor(app, sx, sy)) return CREW_PANEL_TOP + 30.f * 3.f + 50.f;
        CrewControl &crew = app->gui->crewControl;
        float bottom = (float)(std::max)(crew.saveStations.hitbox.y + crew.saveStations.hitbox.h, crew.returnStations.hitbox.y + crew.returnStations.hitbox.h) + 12.f;
        return sy + STATION_BUTTON_SCALE * (bottom - sy);
    }

    // The reactor column grows up from the left end of the systems row, about REACTOR_BAR game px per bar. The crew
    // list always leaves room for REACTOR_ROOM bars; a bigger reactor is cut at the crew list (its top bars are the
    // allocated ones; the free power shows at the bottom).
    const float REACTOR_BOTTOM = 694.f, REACTOR_BAR = 9.f;
    const int REACTOR_ROOM = 10;

    int ReactorBars()
    {
        PowerManager *power = PowerManager::GetPowerManager(0);
        return power != nullptr ? power->currentPower.second : 8;
    }

    // Canvas y of the crew list's top, and the lowest its bottom may reach.
    float CrewTop(CApp *app) { return -(float)ExtraY(app) + HudScale(app) * CREW_PANEL_TOP + 4.f; }

    float CrewLimit(CApp *app)
    {
        float b = BottomScale(app);
        float rowBottom = 720.f + (float)ExtraY(app); // game y 720 of the row: the canvas bottom
        int bars = (std::min)(ReactorBars(), REACTOR_ROOM);
        return rowBottom + b * (REACTOR_BOTTOM - REACTOR_BAR * (float)bars - 720.f) - 8.f;
    }

    // 2x, smaller for a big crew so the list ends above the reactor.
    float CrewScale(CApp *app)
    {
        if (!TwoRowBottom(app)) return HudScale(app);
        float fit = (CrewLimit(app) - CrewTop(app)) / (std::max)(40.f, CrewListBottom(app) - CREW_PANEL_TOP);
        return (std::max)(1.f, (std::min)(2.f, fit));
    }

    // Canvas y the bottom-left (systems) pass is cut at: just under the crew list.
    float CrewBottom(CApp *app) { return CrewTop(app) + CrewScale(app) * (CrewListBottom(app) - CREW_PANEL_TOP); }

    // A HUD group or window maps game point p to canvas point c + s * (p - a).
    struct Anchor
    {
        float ax, ay, cx, cy, s;
    };

    bool TargetAnchor(CApp *app, float &ax, float &ay, float &scale);
    bool TouchButtonsShown(CApp *app);
    Globals::Rect TouchButtonRect(CApp *app, int index);
    Anchor ChoiceAnchor(CApp *app);

    Anchor GetAnchor(CApp *app, Region region)
    {
        float ex = (float)ExtraX(app), ey = (float)ExtraY(app), s = HudScale(app), b = BottomScale(app);
        bool panel = TwoRowBottom(app);
        switch (region)
        {
        case Region::TOP_LEFT: return {0.f, 0.f, -ex, -ey, s};
        case Region::CREW:
            if (!panel) return {0.f, 0.f, -ex, -ey, s};
            return {0.f, CREW_PANEL_TOP, -ex, CrewTop(app), CrewScale(app)};
        case Region::BOTTOM_LEFT:
            if (!panel) return {0.f, 720.f, -ex, 720.f + ey, b};
            // Systems: the start of the bottom row.
            return {0.f, 720.f, -ex + 4.f + b * DoorSlot(app) - SystemsScroll(app), 720.f + ey, b};
        case Region::BOTTOM_RIGHT:
        {
            // Subsystems and More Info: after the systems in the bottom row.
            if (panel) return {BOTTOM_RIGHT_LEFT, 720.f, SubsystemsLeft(app) - SystemsScroll(app), 720.f + ey, b};
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
            if (ChoiceOpen(app->gui)) return ChoiceAnchor(app);
        {
            // Store, ship screens, pause menu: centred in the area below the balances.
            float top, bottom;
            WindowArea(app, top, bottom);
            return {635.f, 338.f, 640.f, (top + bottom) / 2.f, ModalScale(app)};
        }
        default: return {0.f, 0.f, 0.f, 0.f, 1.f};
        }
    }

    // The doors box (the subsystem with the open/close all doors buttons) is drawn bigger again inside the
    // subsystems row, grown up and to the right from just left of its icon.
    DoorBox *lastDoorBox = nullptr;
    const float DOOR_FRAME_SHIFT_Y = 382.f; // measured: box frame y + this = game y (for the test harness)

    // The open/close-all buttons of the doors box, in its own frame (x = game x; y + DOOR_FRAME_SHIFT_Y = game y).
    bool DoorButtons(DoorBox *box, float &x1, float &y1, float &x2, float &y2)
    {
        if (box == nullptr) return false;
        const Globals::Rect &o = box->openDoors.hitbox, &c = box->closeDoors.hitbox;
        x1 = (float)(box->buttonOffset.x + (std::min)(o.x, c.x)) - 5.f;
        x2 = (float)(box->buttonOffset.x + (std::max)(o.x + o.w, c.x + c.w)) + 5.f;
        y1 = (float)(box->buttonOffset.y + (std::min)(o.y, c.y)) - 5.f;
        y2 = (float)(box->buttonOffset.y + (std::max)(o.y + o.h, c.y + c.h)) + 5.f;
        return x2 > x1 && y2 > y1 && x1 >= BOTTOM_RIGHT_LEFT && x1 < 1280.f;
    }

    // How far (game x) the buttons move: from among the subsystems to the slot at the left of the systems
    // (systems-group x, left of 0).
    float DoorButtonsShift(DoorBox *box)
    {
        float x1, y1, x2, y2;
        if (!DoorButtons(box, x1, y1, x2, y2)) return 0.f;
        return -DOOR_SLOT + (DOOR_SLOT - (x2 - x1)) / 2.f - x1;
    }

    bool DoorAnchor(DoorBox *box, float &ax, float &ay)
    {
        if (box == nullptr) return false;
        // The buttons' hitboxes are relative to buttonOffset (DoorBox::MouseMove tests the mouse minus it).
        ax = (float)(box->buttonOffset.x + (std::min)(box->openDoors.hitbox.x, box->closeDoors.hitbox.x)) - 25.f;
        // SystemControl draws (and feeds the mouse to) the subsystem boxes in a frame offset by about
        // DOOR_FRAME_SHIFT_Y; the box ends about 75 below its buttons' top there.
        ay = (float)(box->buttonOffset.y + (std::min)(box->openDoors.hitbox.y, box->closeDoors.hitbox.y)) + 75.f;
        // Only where the subsystems are: anything else means the layout is not what this expects; leave it alone.
        return ax >= BOTTOM_RIGHT_LEFT && ax < 1280.f;
    }

    float WZ(CApp *app) { return zoom * BaseZoom(app); }
    float WX(CApp *app) { return panX + WorldShiftX(app); }
    float WY(CApp *app) { return panY + WorldShiftY(app); }

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

    // The enemy window grows left and down from its top-right corner (the right side is the canvas edge).
    bool TargetAnchor(CApp *app, float &ax, float &ay, float &scale)
    {
        if (app->gui == nullptr || app->gui->combatControl.currentTarget == nullptr || !TwoRowBottom(app)) return false;
        CombatControl &combat = app->gui->combatControl;
        Point size = combat.GetHostileBoxSize();
        if (size.x <= 0 || size.y <= 0) return false;
        ax = (float)(combat.position.x + combat.boxPosition.x + size.x);
        ay = (float)(combat.position.y + combat.boxPosition.y);
        scale = 1.2f;
        if (TouchButtonsShown(app))
        {
            // World y just above the buttons (the window is drawn in the world, then scaled about its top right).
            float buttonsTop = (float)TouchButtonRect(app, 1).y - 28.f; // its frame image reaches ~20 px below the box
            float worldY = 360.f + (buttonsTop - 360.f - WY(app)) / WZ(app);
            scale = (std::max)(0.8f, (std::min)(scale, (worldY - ay) / (float)size.y));
        }
        return true;
    }

    // Event boxes: a centred one is scaled around the canvas centre. One FTL puts at the side (a ship hailing you,
    // with its window on the right) stays at the left edge, scaled to fit beside the enemy window.
    Anchor ChoiceAnchor(CApp *app)
    {
        float s = ModalScale(app);
        ChoiceBox &choice = app->gui->choiceBox;
        if (choice.box == nullptr || choice.box->rect.w <= 0 || choice.box->rect.h <= 0) return {640.f, 360.f, 640.f, 360.f, s};
        float w = (float)choice.box->rect.w, h = (float)choice.box->rect.h;
        bool beside = app->gui->combatControl.currentTarget != nullptr;
        float left = (beside ? 167.f : 327.f) + choice.box->rect.x, top = 140.f + choice.box->rect.y;
        static int logged = 0;
        if (logged < 4)
        {
            logged++;
            hs_log_file("Fold layout: event box frame %d,%d %dx%d beside enemy %d\n", choice.box->rect.x, choice.box->rect.y,
                        choice.box->rect.w, choice.box->rect.h, (int)beside);
        }
        float ex = (float)ExtraX(app), ey = (float)ExtraY(app);
        s = (std::min)(s, (720.f + 2.f * ey - 16.f) / h);
        if (!beside)
        {
            s = (std::min)(s, (1280.f + 2.f * ex - 16.f) / w);
            return {left + w / 2.f, top + h / 2.f, 640.f, 360.f, s};
        }
        // Room left of the enemy window, wherever it is drawn now.
        float room = 1280.f + 2.f * ex - 16.f;
        float ax, ay, t;
        if (TargetAnchor(app, ax, ay, t))
        {
            Point size = app->gui->combatControl.GetHostileBoxSize();
            float boxLeft = ax - t * size.x;
            float canvasLeft = 640.f + WX(app) + WZ(app) * (boxLeft - 640.f);
            room = canvasLeft + ex - 16.f;
        }
        s = (std::min)(s, (std::max)(1.3f, room / w));
        return {left, top + h / 2.f, -ex + 8.f, 360.f, s};
    }

    bool InsideTargetBox(CApp *app, float x, float y)
    {
        CombatControl &combat = app->gui->combatControl;
        Point size = combat.GetHostileBoxSize();
        float left = (float)(combat.position.x + combat.boxPosition.x), top = (float)(combat.position.y + combat.boxPosition.y);
        return x >= left && x < left + size.x && y >= top && y < top + size.y;
    }

    void ApplyTransform(CApp *app, Region region)
    {
        if (region == Region::TARGET)
        {
            ApplyTransform(app, Region::WORLD);
            float ax, ay, t;
            if (!TargetAnchor(app, ax, ay, t)) return;
            CSurface::GL_Translate(ax, ay, 0.f);
            CSurface::GL_Scale(t, t, 1.f);
            CSurface::GL_Translate(-ax, -ay, 0.f);
            return;
        }
        if (region == Region::WORLD)
        {
            CSurface::GL_Translate(640.f + WX(app), 360.f + WY(app), 0.f);
            CSurface::GL_Scale(WZ(app), WZ(app), 1.f);
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
        if (region == Region::TARGET)
        {
            float ax, ay, t;
            if (TargetAnchor(app, ax, ay, t))
            {
                CSurface::GL_Translate(ax, ay, 0.f);
                CSurface::GL_Scale(1.f / t, 1.f / t, 1.f);
                CSurface::GL_Translate(-ax, -ay, 0.f);
            }
            ApplyInverse(app, Region::WORLD);
            return;
        }
        if (region == Region::WORLD)
        {
            CSurface::GL_Translate(640.f, 360.f, 0.f);
            CSurface::GL_Scale(1.f / WZ(app), 1.f / WZ(app), 1.f);
            CSurface::GL_Translate(-640.f - WX(app), -360.f - WY(app), 0.f);
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
        if (region == Region::TARGET)
        {
            InverseMap(app, Region::WORLD, x, y, outX, outY);
            float ax, ay, t;
            if (TargetAnchor(app, ax, ay, t))
            {
                outX = ax + (outX - ax) / t;
                outY = ay + (outY - ay) / t;
            }
            return;
        }
        if (region == Region::WORLD)
        {
            outX = 640.f + (x - 640.f - WX(app)) / WZ(app);
            outY = 360.f + (y - 360.f - WY(app)) / WZ(app);
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

    // Game point in a region -> the canvas point where it is drawn now.
    void ToCanvas(CApp *app, Region region, float x, float y, float &cx, float &cy)
    {
        if (region == Region::TARGET)
        {
            float ax, ay, t;
            if (TargetAnchor(app, ax, ay, t))
            {
                x = ax + t * (x - ax);
                y = ay + t * (y - ay);
            }
            region = Region::WORLD;
        }
        if (region == Region::WORLD)
        {
            cx = 640.f + WX(app) + WZ(app) * (x - 640.f);
            cy = 360.f + WY(app) + WZ(app) * (y - 360.f);
            return;
        }
        if (region == Region::NONE)
        {
            cx = x;
            cy = y;
            return;
        }
        Anchor a = GetAnchor(app, region);
        cx = a.cx + a.s * (x - a.ax);
        cy = a.cy + a.s * (y - a.ay);
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

    // The plain main menu (no hangar, options, stats, credits or dialog over it) on a tall canvas.
    bool MenuScaled(CApp *app)
    {
        if (!LayoutActive(app) || !app->menu.bOpen || app->langChooser.bOpen || app->screen_y <= 760) return false;
        MainMenu &menu = app->menu;
        return !menu.shipBuilder.bOpen && !menu.bScoreScreen && !menu.optionScreen.bOpen && !menu.bCreditScreen &&
               !menu.changelog.bOpen && !menu.confirmNewGame.bOpen && !menu.bSelectSave;
    }

    struct MenuAnchor
    {
        float ax, ay, cx, cy, s;
    };

    MenuAnchor GetMenuAnchor(CApp *app)
    {
        float s = (std::min)(2.f, (float)app->screen_y / 720.f);
        return {1280.f, 360.f, 1280.f + ExtraX(app), 360.f, s};
    }

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

            bool inPanel = x < PanelRight(app) && x >= -(float)ExtraX(app);
            bool subsystems = x >= SubsystemsLeft(app) - SystemsScroll(app) - SEAM_CUT;
            InverseMap(app, Region::BOTTOM_RIGHT, x, y, gx, gy);
            if (inPanel && subsystems && gy >= SUBSYSTEM_ROW_TOP && gy < 720.f && gx >= BOTTOM_RIGHT_LEFT && gx < 1280.f) return Region::BOTTOM_RIGHT;

            InverseMap(app, Region::BOTTOM_LEFT, x, y, gx, gy);
            if (inPanel && !subsystems && gy >= BOTTOM_BAND_TOP && gy < 720.f)
            {
                // The weapons are no longer drawn right of the systems: nothing to hit there.
                if (gx >= SystemsEnd(app))
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
        float sx, sy;
        if (StationsAnchor(app, sx, sy) && gx >= sx && gy >= sy)
        {
            // Inside the enlarged stations buttons: the point on the buttons as FTL places them.
            float ux = sx + (gx - sx) / STATION_BUTTON_SCALE, uy = sy + (gy - sy) / STATION_BUTTON_SCALE;
            CrewControl &crew = gui->crewControl;
            if (InsideRect(crew.saveStations.hitbox, (int)ux, (int)uy) || InsideRect(crew.returnStations.hitbox, (int)ux, (int)uy))
            {
                gx = ux;
                gy = uy;
                return Region::CREW;
            }
        }
        if (OnCrewPanel(gui, (int)gx, (int)gy)) return Region::CREW;

        InverseMap(app, Region::TOP_LEFT, x, y, gx, gy);
        if (InTopBand((int)gx, (int)gy)) return Region::TOP_LEFT;

        float ax, ay, t;
        if (TargetAnchor(app, ax, ay, t))
        {
            InverseMap(app, Region::TARGET, x, y, gx, gy);
            if (InsideTargetBox(app, gx, gy)) return Region::TARGET;
        }

        InverseMap(app, Region::WORLD, x, y, gx, gy);
        return Region::WORLD;
    }

    // A canvas point -> the game point FTL should see. Taps on parts of the world that sit under the HUD in the
    // original layout are dropped (they would hit the moved HUD); a drag keeps the region it started in.
    float pointerX = 0.f, pointerY = 0.f; // where the pointer really is, in canvas coordinates

    Point MapDrawnPoint(CApp *app, int x, int y, bool startDrag)
    {
        pointerX = (float)x;
        pointerY = (float)y;
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
        if (MenuScaled(app))
        {
            dragging = false;
            MenuAnchor a = GetMenuAnchor(app);
            float cx = (float)(x - app->modifier_x), cy = (float)(y - app->modifier_y);
            x = (int)std::floor(a.ax + (cx - a.cx) / a.s) + app->modifier_x;
            y = (int)std::floor(a.ay + (cy - a.cy) / a.s) + app->modifier_y;
            return;
        }
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
        float worldX = 640.f + (x - 640.f - WX(app)) / WZ(app);
        float worldY = 360.f + (y - 360.f - WY(app)) / WZ(app);
        zoom *= factor;
        zoom = (std::max)(MIN_ZOOM, (std::min)(MAX_ZOOM, zoom));
        panX = x - 640.f - WorldShiftX(app) - (worldX - 640.f) * WZ(app);
        panY = y - 360.f - WorldShiftY(app) - (worldY - 360.f) * WZ(app);
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
    bool drawingTooltip = false;
    bool drawingCrewPopup = false; // a held crew box, with its skills box hanging below it
    bool drawingTouchButtons = false;
    bool drawingCursor = false;
    bool clipActive = false;          // the panel passes' scissor, in window coordinates (for the draw checker)
    float clipX1 = 0.f, clipY1 = 0.f, clipX2 = 0.f, clipY2 = 0.f;
    float clipSeam = -1.f; // window x where the systems meet the subsystems in the bottom row (-1: none drawn now)

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
// Whether the fold layout is on now (other features hide desktop-only bits).
bool FoldLayoutActive() { return LayoutActive(G_->GetCApp()); }

bool FoldLayoutPushTopLeftToBottomRight()
{
    CApp *app = G_->GetCApp();
    if (!inGuiRender || !InGame(app)) return false;
    CSurface::GL_PushMatrix();
    ApplyInverse(app, Region::TOP_LEFT);
    ApplyTransform(app, Region::BOTTOM_RIGHT);
    activeRegion = Region::BOTTOM_RIGHT;
    if (TwoRowBottom(app) && !app->useDirect3D)
    {
        // It scrolls with the subsystems: only inside the panel.
        GLint left = (std::max)(0, (GLint)(app->modifier_x + SubsystemsLeft(app) - SystemsScroll(app) - SEAM_CUT));
        GLint right = (GLint)(app->modifier_x + PanelRight(app)) + 2;
        glEnable(GL_SCISSOR_TEST);
        glScissor(left, 0, (std::max)(0, right - left), app->screen_y);
        clipActive = true;
        clipX1 = (float)left;
        clipX2 = (float)right;
        clipY1 = 0.f;
        clipY2 = (float)app->screen_y;
    }
    return true;
}

void FoldLayoutPopMatrix()
{
    CSurface::GL_PopMatrix();
    if (clipActive)
    {
        glDisable(GL_SCISSOR_TEST);
        clipActive = false;
    }
    activeRegion = Region::TOP_LEFT;
}

// For the test harness: where a game point in a region ("tl", "bl", "br", "world", "modal", "none") is on the
// window right now, so test taps land on controls whatever the canvas size and zoom.
bool FoldLayoutToWindow(const std::string &region, float x, float y, int &windowX, int &windowY)
{
    CApp *app = G_->GetCApp();
    if (app == nullptr) return false;
    float cx = x, cy = y;
    if (region == "menu" && MenuScaled(app))
    {
        MenuAnchor a = GetMenuAnchor(app);
        cx = a.cx + a.s * (x - a.ax);
        cy = a.cy + a.s * (y - a.ay);
    }
    else if (InGame(app) && region == "target")
    {
        float ax, ay, t;
        if (TargetAnchor(app, ax, ay, t))
        {
            x = ax + t * (x - ax);
            y = ay + t * (y - ay);
        }
        cx = 640.f + WX(app) + WZ(app) * (x - 640.f);
        cy = 360.f + WY(app) + WZ(app) * (y - 360.f);
    }
    else if (InGame(app) && region != "none" && region != "menu")
    {
        if (region == "world")
        {
            cx = 640.f + WX(app) + WZ(app) * (x - 640.f);
            cy = 360.f + WY(app) + WZ(app) * (y - 360.f);
        }
        else
        {
            float dx, dy;
            if (region == "st" && StationsAnchor(app, dx, dy))
            {
                x = dx + STATION_BUTTON_SCALE * (x - dx);
                y = dy + STATION_BUTTON_SCALE * (y - dy);
            }
            if (region == "door" && TwoRowBottom(app) && DoorAnchor(lastDoorBox, dx, dy))
            {
                // A point of the doors box's buttons: moved to the slot at the left of the systems.
                x += DoorButtonsShift(lastDoorBox);
            }
            if (region == "row")
            {
                // A fraction of the way across the visible row (for swipes that must start on screen).
                Anchor a = GetAnchor(app, Region::BOTTOM_LEFT);
                float left = -(float)ExtraX(app) + 4.f;
                windowX = (int)std::lround(left + x * (PanelRight(app) - left)) + app->modifier_x;
                windowY = (int)std::lround(a.cy + a.s * (y - a.ay)) + app->modifier_y;
                return true;
            }
            Region r = region == "tl" ? Region::TOP_LEFT : region == "bl" ? Region::BOTTOM_LEFT :
                       region == "door" && TwoRowBottom(app) ? Region::BOTTOM_LEFT : region == "br" || region == "door" ? Region::BOTTOM_RIGHT : region == "wp" ? Region::WEAPONS :
                       region == "cr" || region == "st" ? Region::CREW : region == "dr" ? Region::DRONES : Region::MODAL;
            Anchor a = GetAnchor(app, r);
            cx = a.cx + a.s * (x - a.ax);
            cy = a.cy + a.s * (y - a.ay);
        }
    }
    windowX = (int)std::lround(cx) + app->modifier_x;
    windowY = (int)std::lround(cy) + app->modifier_y;
    return true;
}

// For the draw checker (FoldCheck.cpp): what is being drawn now, if it must stay on the canvas (nullptr: the
// ships, space, menus...). windowOverBalances: a store/ship screen, which must leave the balances rows visible.
const char *FoldLayoutCheckRegion(bool &windowOverBalances)
{
    windowOverBalances = false;
    CApp *app = G_->GetCApp();
    if (app == nullptr || !InGame(app)) return nullptr;
    if (drawingTooltip) return "tooltip";
    if (drawingCrewPopup) return "popup";
    if (drawingCursor) return nullptr; // the pointer itself (hidden on the phone)
    if (drawingTouchButtons) return "touch-buttons";
    switch (activeRegion)
    {
    case Region::TOP_LEFT: return "top-left";
    case Region::CREW: return "crew";
    case Region::BOTTOM_LEFT: return "bottom-left";
    case Region::BOTTOM_RIGHT: return "bottom-right";
    case Region::WEAPONS: return "weapons";
    case Region::DRONES: return "drones";
    case Region::TARGET: return zoom == 1.f ? "target" : nullptr;
    case Region::MODAL:
        windowOverBalances = !StarMapOpen(app->gui) && !ChoiceOpen(app->gui) && !app->gui->menuBox.bOpen;
        return StarMapOpen(app->gui) ? "star-map" : ChoiceOpen(app->gui) ? "event" : "window";
    default: return nullptr;
    }
}

// For the draw checker: the scissor rectangle in force (window coordinates), if any.
// For the draw checker: window x of the seam between the systems and the subsystems while the row is drawn
// (-1: none). Nothing may be cut in two there.
float FoldLayoutCheckSeam() { return clipActive ? clipSeam : -1.f; }

bool FoldLayoutCheckClip(float &x1, float &y1, float &x2, float &y2)
{
    if (!clipActive) return false;
    x1 = clipX1;
    y1 = clipY1;
    x2 = clipX2;
    y2 = clipY2;
    return true;
}

// For the draw checker: window y of the bottom of the hull/scrap/fuel/missiles/drones rows.
float FoldLayoutBalancesBottom()
{
    CApp *app = G_->GetCApp();
    if (app == nullptr) return 0.f;
    return (float)app->modifier_y - (float)ExtraY(app) + HudScale(app) * 82.f;
}

// For the test harness: whether the open event box is drawn wholly on the canvas (-1 when none is open).
int FoldLayoutChoiceOnScreen()
{
    CApp *app = G_->GetCApp();
    if (app == nullptr || !InGame(app) || !ChoiceOpen(app->gui) || app->gui->choiceBox.box == nullptr) return -1;
    if (!ScaledModalOpen(app->gui)) return 1;
    Anchor a = GetAnchor(app, Region::MODAL);
    ChoiceBox &choice = app->gui->choiceBox;
    float boxLeft = (app->gui->combatControl.currentTarget != nullptr ? 167.f : 327.f) + choice.box->rect.x, boxTop = 140.f + choice.box->rect.y;
    float left = a.cx + a.s * (boxLeft - a.ax), right = left + a.s * choice.box->rect.w;
    float top = a.cy + a.s * (boxTop - a.ay), bottom = top + a.s * choice.box->rect.h;
    float ex = (float)ExtraX(app), ey = (float)ExtraY(app);
    return left >= -ex - 1.f && right <= 1280.f + ex + 1.f && top >= -ey - 1.f && bottom <= 720.f + ey + 1.f;
}

// For the test harness: where the last input landed, and the view.
const char *FoldLayoutDescribe()
{
    static char text[400];
    static const char *names[] = {"none", "top-left", "bottom-left", "bottom-right", "world", "modal", "weapons", "crew", "drones", "target"};
    CApp *app = G_->GetCApp();
    bool game = app != nullptr && InGame(app);
    std::snprintf(text, sizeof(text), "region=%s pressRegion=%s zoom=%.2f pan=%.0f,%.0f hudScale=%.2f bottomScale=%.2f modalScale=%.2f twoRow=%d lifted=%d sysScrollable=%d sysScrolled=%d crewScale=%.2f",
                  names[(int)lastRegion], names[(int)pressRegion], zoom, panX, panY, game ? HudScale(app) : 0.f, game ? BottomScale(app) : 0.f,
                  game ? ModalScale(app) : 0.f, game ? (int)TwoRowBottom(app) : 0, game ? (int)BottomRightLifted(app) : 0,
                  game ? (int)(MaxSystemsScroll(app) > 0.f) : 0, game ? (int)(SystemsScroll(app) > 1.f) : 0, game ? CrewScale(app) : 0.f);
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

// ---- Touch buttons: Esc (game menu) and Pause, above the weapons at the bottom right ----

namespace
{
    const float TOUCH_BUTTON_W = 112.f, TOUCH_BUTTON_H = 76.f, TOUCH_BUTTON_GAP = 10.f;
    bool swallowLeftUp = false;

    bool TouchButtonsShown(CApp *app)
    {
        return InGame(app) && TwoRowBottom(app) && !ModalOpen(app->gui);
    }

    // Button 0 = Esc, 1 = Pause, in canvas coordinates: above the weapons at the right, or with drones (a row
    // above the weapons), at the right end of that row.
    Globals::Rect TouchButtonRect(CApp *app, int index)
    {
        Anchor w = GetAnchor(app, Region::WEAPONS);
        float bottom = w.cy - w.s * (720.f - BOTTOM_BAND_TOP) - 10.f;
        float width = TOUCH_BUTTON_W, height = TOUCH_BUTTON_H;
        float rightEdge = 1280.f + ExtraX(app) - 8.f;
        if (HasDrones(app))
        {
            Anchor d = GetAnchor(app, Region::DRONES);
            float dronesRight = d.cx + d.s * 296.f;
            width = (std::min)(TOUCH_BUTTON_W, (rightEdge - dronesRight - 8.f - TOUCH_BUTTON_GAP) / 2.f);
            bottom = d.cy - d.s * 20.f;
            if (width < 70.f)
            {
                width = TOUCH_BUTTON_W;
                bottom = d.cy - d.s * (720.f - BOTTOM_BAND_TOP + 5.f) - 10.f;
            }
        }
        float right = rightEdge - (1 - index) * (width + TOUCH_BUTTON_GAP);
        return Globals::Rect({(int)(right - width), (int)(bottom - height), (int)width, (int)height});
    }

    int TouchButtonAt(CApp *app, int x, int y)
    {
        if (!TouchButtonsShown(app)) return -1;
        for (int i = 1; i < 2; i++) // only Pause: the wrench at the top opens the game menu
        {
            Globals::Rect r = TouchButtonRect(app, i);
            if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return i;
        }
        return -1;
    }

    void PressKey(CApp *app, SDLKey key)
    {
        app->OnKeyDown(key);
        app->OnKeyUp(key);
    }

    void RenderTouchButtons(CApp *app)
    {
        if (!TouchButtonsShown(app)) return;
        drawingTouchButtons = true;
        static const char *labels[] = {"MENU", "PAUSE"};
        for (int i = 1; i < 2; i++) // only Pause, where it was
        {
            Globals::Rect r = TouchButtonRect(app, i);
            bool active = i == 1 && app->gui->bPaused;
            GL_Color fill = active ? GL_Color(0.85f, 0.55f, 0.1f, 0.9f) : GL_Color(0.06f, 0.1f, 0.12f, 0.85f);
            CSurface::GL_DrawRect((float)r.x, (float)r.y, (float)r.w, (float)r.h, fill);
            CSurface::GL_DrawRectOutline(r.x, r.y, r.w, r.h, GL_Color(0.78f, 0.92f, 0.86f, 1.f), 3.f);
            CSurface::GL_SetColor(GL_Color(0.92f, 1.f, 0.96f, 1.f));
            freetype::easy_printCenter(24, r.x + r.w / 2.f, r.y + r.h / 2.f - 12.f, labels[i]);
            CSurface::GL_SetColor(GL_Color(1.f, 1.f, 1.f, 1.f));
        }
        drawingTouchButtons = false;
    }
}

// For the test harness: the window point at the centre of touch button 0 (menu) or 1 (pause).
bool FoldLayoutTouchButton(int index, int &windowX, int &windowY)
{
    CApp *app = G_->GetCApp();
    if (app == nullptr || !TouchButtonsShown(app) || index != 1) return false; // only Pause is drawn
    Globals::Rect r = TouchButtonRect(app, index);
    windowX = r.x + r.w / 2 + app->modifier_x;
    windowY = r.y + r.h / 2 + app->modifier_y;
    return true;
}

HOOK_METHOD_PRIORITY(CommandGui, RenderStatic, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::RenderStatic -> Begin (FoldLayout.cpp)\n")
    inGuiRender = true;
    super();
    inGuiRender = false;
    RenderTouchButtons(G_->GetCApp());
}

HOOK_METHOD_PRIORITY(MainMenu, OnRender, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> MainMenu::OnRender -> Begin (FoldLayout.cpp)\n")
    CApp *app = G_->GetCApp();
    if (app == nullptr || this != &app->menu || !MenuScaled(app)) return super();
    MenuAnchor a = GetMenuAnchor(app);
    CSurface::GL_PushMatrix();
    CSurface::GL_Translate(a.cx, a.cy, 0.f);
    CSurface::GL_Scale(a.s, a.s, 1.f);
    CSurface::GL_Translate(-a.ax, -a.ay, 0.f);
    super();
    CSurface::GL_PopMatrix();
}

// The hangar uses the whole 1280 width, so it stays 1:1; its dock backdrop, scaled to cover the canvas, fills the
// bands above and below it.
HOOK_METHOD_PRIORITY(ShipBuilder, OnRender, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipBuilder::OnRender -> Begin (FoldLayout.cpp)\n")
    CApp *app = G_->GetCApp();
    if (LayoutActive(app) && app->screen_y > 760 && baseImage != nullptr)
    {
        float scale = (std::max)((float)app->screen_x / 1280.f, (float)app->screen_y / 720.f);
        CSurface::GL_PushMatrix();
        CSurface::GL_Translate(640.f, 360.f, 0.f);
        CSurface::GL_Scale(scale, scale, 1.f);
        CSurface::GL_Translate(-640.f, -360.f, 0.f);
        CSurface::GL_RenderPrimitiveWithAlpha(baseImage, 0.55f);
        CSurface::GL_PopMatrix();
    }
    super();
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
    RegionScope scope(Region::TARGET);
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
    float ax = 0.f, ay = 0.f;
    lastDoorBox = this;
    static int logged = 0;
    if (logged < 3)
    {
        logged++;
        bool anchor = DoorAnchor(this, ax, ay);
        hs_log_file("Fold layout: doors box location %d,%d buttonOffset %d,%d open %d,%d close %d,%d anchor %d %.0f,%.0f inGui %d\n",
                    location.x, location.y, buttonOffset.x, buttonOffset.y, openDoors.hitbox.x, openDoors.hitbox.y,
                    closeDoors.hitbox.x, closeDoors.hitbox.y, (int)anchor, ax, ay, (int)inGuiRender);
    }
    float bx1, by1, bx2, by2;
    if (!inGuiRender || !InGame(app) || !TwoRowBottom(app) || !clipActive || app->useDirect3D || !DoorButtons(this, bx1, by1, bx2, by2))
        return super(ignoreStatus);
    // Window rectangle of the buttons, drawn shifted (systems pass) or where FTL puts them (subsystems pass).
    float shift = activeRegion == Region::BOTTOM_LEFT ? DoorButtonsShift(this) : 0.f;
    float cx1, cy1, cx2, cy2;
    ToCanvas(app, activeRegion, bx1 + shift, by1 + DOOR_FRAME_SHIFT_Y, cx1, cy1);
    ToCanvas(app, activeRegion, bx2 + shift, by2 + DOOR_FRAME_SHIFT_Y, cx2, cy2);
    float wx1 = cx1 + app->modifier_x, wx2 = cx2 + app->modifier_x, wy1 = cy1 + app->modifier_y, wy2 = cy2 + app->modifier_y;
    GLint saved[4];
    glGetIntegerv(GL_SCISSOR_BOX, saved);
    float sx1 = (float)saved[0], sx2 = (float)(saved[0] + saved[2]);
    float sTop = (float)(app->screen_y - saved[1] - saved[3]), sBottom = (float)(app->screen_y - saved[1]);
    float savedClip[4] = {clipX1, clipY1, clipX2, clipY2};
    if (activeRegion == Region::BOTTOM_LEFT)
    {
        // Only the buttons, in the slot.
        float x1 = (std::max)(sx1, wx1), x2 = (std::min)(sx2, wx2), y1 = (std::max)(sTop, wy1), y2 = (std::min)(sBottom, wy2);
        if (x2 <= x1 || y2 <= y1) return;
        glScissor((GLint)x1, app->screen_y - (GLint)y2, (GLsizei)(x2 - x1), (GLsizei)(y2 - y1));
        clipX1 = x1; clipX2 = x2; clipY1 = y1; clipY2 = y2;
        CSurface::GL_PushMatrix();
        CSurface::GL_Translate(shift, 0.f, 0.f);
        super(ignoreStatus);
        CSurface::GL_PopMatrix();
    }
    else
    {
        // Everything but the buttons (the doors icon and its power stay with the subsystems).
        float x2 = (std::min)(sx2, wx1);
        if (x2 > sx1)
        {
            glScissor(saved[0], saved[1], (GLsizei)(x2 - sx1), saved[3]);
            clipX2 = x2;
            super(ignoreStatus);
        }
    }
    glScissor(saved[0], saved[1], saved[2], saved[3]);
    clipX1 = savedClip[0]; clipY1 = savedClip[1]; clipX2 = savedClip[2]; clipY2 = savedClip[3];
}

HOOK_METHOD_PRIORITY(DoorBox, MouseMove, -10000, (int mX, int mY) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> DoorBox::MouseMove -> Begin (FoldLayout.cpp)\n")
    CApp *app = G_->GetCApp();
    float bx1, by1, bx2, by2;
    if (InGame(app) && TwoRowBottom(app) && DoorButtons(this, bx1, by1, bx2, by2) && mX > -9000)
    {
        if (lastRegion == Region::BOTTOM_LEFT) mX -= (int)std::lround(DoorButtonsShift(this));
        else if (mX >= bx1 && mX < bx2) mX = -10000;
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
        // Panel: one row, systems then subsystems. GL scissor y counts from the bottom of the window.
        GLint panelRight = (GLint)(app->modifier_x + PanelRight(app)) + 2;
        GLint panelLeft = (std::max)(0, (GLint)(app->modifier_x - ExtraX(app)));
        GLint split = (std::max)(panelLeft, (std::min)(panelRight, (GLint)(app->modifier_x + SubsystemsLeft(app) - SystemsScroll(app) - SEAM_CUT)));
        // Nothing of the row above the crew list (a reactor taller than the room left for it).
        GLint panelTop = (std::max)(0, (GLint)(app->modifier_y + CrewBottom(app)) + 4);
        GLsizei height = (std::max)(0, app->screen_y - panelTop);
        Region saved = activeRegion;
        clipActive = true;
        clipY1 = (float)panelTop;
        clipY2 = (float)app->screen_y;
        glScissor(panelLeft, 0, split - panelLeft, height);
        clipX1 = (float)panelLeft;
        clipX2 = (float)split;
        clipSeam = split > panelLeft && split < panelRight ? (float)split : -1.f;
        PushRegion(app, Region::BOTTOM_LEFT);
        activeRegion = Region::BOTTOM_LEFT;
        super(front);
        CSurface::GL_PopMatrix();
        glScissor(split, 0, panelRight - split, height);
        clipX1 = (float)split;
        clipX2 = (float)panelRight;
        PushRegion(app, Region::BOTTOM_RIGHT);
        activeRegion = Region::BOTTOM_RIGHT;
        super(front);
        CSurface::GL_PopMatrix();
        clipActive = false;
        clipSeam = -1.f;
        glDisable(GL_SCISSOR_TEST);

        // A row wider than the panel: a scroll bar along the bottom edge (canvas coordinates).
        float maxScroll = MaxSystemsScroll(app);
        if (maxScroll > 0.f)
        {
            activeRegion = Region::BOTTOM_LEFT;
            float left = -(float)ExtraX(app) + 4.f, right = PanelRight(app);
            float y = 720.f + (float)ExtraY(app) - 6.f;
            float track = right - left, visible = track * track / (track + maxScroll);
            float thumb = left + (track - visible) * (SystemsScroll(app) / maxScroll);
            CSurface::GL_DrawRect(left, y, track, 4.f, GL_Color(0.35f, 0.4f, 0.45f, 0.6f));
            CSurface::GL_DrawRect(thumb, y - 1.f, visible, 5.f, GL_Color(0.85f, 0.9f, 0.95f, 0.95f));
        }
        activeRegion = saved;
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
    Region saved = activeRegion;
    activeRegion = dragging ? dragRegion : Region::WORLD;
    bool result = super(x1, y1, x2, y2, color, lineWidth);
    activeRegion = saved;
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

// The description box shown while an item is held in the store (and the ship screens) sits left of the window in
// FTL's layout; enlarged with the window it would leave the canvas. It is drawn at the left edge instead, over the
// window (it only shows while an item is held, like a tooltip).
HOOK_METHOD_PRIORITY(InfoBox, OnRender, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> InfoBox::OnRender -> Begin (FoldLayout.cpp)\n")
    CApp *app = G_->GetCApp();
    if (!InGame(app) || activeRegion != Region::MODAL) return super();
    Anchor a = GetAnchor(app, Region::MODAL);
    float ex = (float)ExtraX(app);
    float left = a.cx + a.s * (location.x - a.ax);
    float width = a.s * (float)(std::max)(descBoxSize.x, 260);
    if (left >= -ex + 4.f && left + width <= 1280.f + ex - 4.f) return super();
    float top, bottom;
    WindowArea(app, top, bottom);
    float ib = (std::min)(a.s, 1.5f);
    // At the top of the window area: a tall box (a weapon with its stats) placed at the item ran off the bottom.
    float y = top;
    CSurface::GL_PushMatrix();
    ApplyInverse(app, Region::MODAL);
    CSurface::GL_Translate(-ex + 8.f, y, 0.f);
    CSurface::GL_Scale(ib, ib, 1.f);
    CSurface::GL_Translate((float)-location.x, (float)-location.y, 0.f);
    super();
    CSurface::GL_PopMatrix();
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
    CApp *app = G_->GetCApp();
    float sx, sy;
    if (gui != nullptr && inGuiRender && InGame(app) && (this == &gui->crewControl.saveStations || this == &gui->crewControl.returnStations) &&
        StationsAnchor(app, sx, sy))
    {
        // Drawn bigger, with its plate (FTL's own smaller plate is covered).
        CSurface::GL_PushMatrix();
        CSurface::GL_Translate(sx, sy, 0.f);
        CSurface::GL_Scale(STATION_BUTTON_SCALE, STATION_BUTTON_SCALE, 1.f);
        CSurface::GL_Translate(-sx, -sy, 0.f);
        GL_Primitive *plate = this == &gui->crewControl.saveStations ? gui->crewControl.saveStationsBase : gui->crewControl.returnStationsBase;
        drawingStationPlate = true;
        if (plate != nullptr) CSurface::GL_RenderPrimitive(plate);
        drawingStationPlate = false;
        super();
        CSurface::GL_PopMatrix();
        return;
    }
    super();
}

// FTL draws the stations plates itself, at their original size, before the buttons. The buttons draw their own
// enlarged plates (above), so the originals would show behind them: skip those.
static bool SkipStationPlate(GL_Primitive *primitive)
{
    if (!inCrewControlRender || drawingStationPlate || primitive == nullptr) return false;
    CApp *app = G_->GetCApp();
    float sx, sy;
    if (app == nullptr || app->gui == nullptr || !StationsAnchor(app, sx, sy)) return false;
    const CrewControl &crew = app->gui->crewControl;
    return primitive == crew.saveStationsBase || primitive == crew.returnStationsBase;
}

HOOK_STATIC_PRIORITY(CSurface, GL_RenderPrimitive, -10000, (GL_Primitive *primitive) -> void)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_RenderPrimitive -> Begin (FoldLayout.cpp)\n")
    if (SkipStationPlate(primitive)) return;
    super(primitive);
}

HOOK_STATIC_PRIORITY(CSurface, GL_RenderPrimitiveWithAlpha, -10000, (GL_Primitive *primitive, float alpha) -> void)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_RenderPrimitiveWithAlpha -> Begin (FoldLayout.cpp)\n")
    if (SkipStationPlate(primitive)) return;
    super(primitive, alpha);
}

HOOK_STATIC_PRIORITY(CSurface, GL_RenderPrimitiveWithColor, -10000, (GL_Primitive *primitive, GL_Color color) -> void)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_RenderPrimitiveWithColor -> Begin (FoldLayout.cpp)\n")
    if (SkipStationPlate(primitive)) return;
    super(primitive, color);
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

// A held crew box shows its skills in a box hanging below it, over whatever is there (only while held, like a tooltip).
HOOK_METHOD_PRIORITY(CrewBox, OnRender, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CrewBox::OnRender -> Begin (FoldLayout.cpp)\n")
    bool popup = mouseHover && activeRegion == Region::CREW;
    drawingCrewPopup = popup;
    super();
    if (popup) drawingCrewPopup = false;
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
static int tooltipFrame = -100, renderFrame = 0;
static float tooltipLeft = 0.f, tooltipTop = 0.f, tooltipRight = 0.f, tooltipBottom = 0.f;

HOOK_METHOD_PRIORITY(MouseControl, OnRender, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> MouseControl::OnRender -> Begin (FoldLayout.cpp)\n")
    CApp *app = G_->GetCApp();
    renderFrame++;
    bool pushed = false;
    Region saved = activeRegion;
    if (lastRegion != Region::NONE && InGame(app))
    {
        PushRegion(app, lastRegion);
        pushed = true;
        activeRegion = lastRegion;
    }
    cursorMasked = HideCursor() && app != nullptr && !app->useDirect3D;
    if (cursorMasked) glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    drawingCursor = true;
    super();
    drawingCursor = false;
    if (cursorMasked) glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    cursorMasked = false;
    activeRegion = saved;
    if (pushed) CSurface::GL_PopMatrix();
}

// FTL keeps tooltips inside its own 1280x720 area, which no longer matches where the HUD is drawn, so every tooltip
// drawn over a moved or scaled part of the screen is placed here, on the canvas: one that follows the pointer goes
// centred above the finger (below it if there is no room); one FTL pins to a spot (e.g. above a weapon) goes where
// that spot is drawn now. Either way it is kept wholly on the canvas.
HOOK_METHOD_PRIORITY(MouseControl, RenderTooltip, -10000, (Point tooltipPoint, bool staticPos) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> MouseControl::RenderTooltip -> Begin (FoldLayout.cpp)\n")
    if (cursorMasked) glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    CApp *app = G_->GetCApp();
    Region region = activeRegion != Region::NONE ? activeRegion : lastRegion;
    if (region == Region::NONE || !InGame(app))
    {
        super(tooltipPoint, staticPos);
    }
    else
    {
        float ts = region == Region::WORLD || region == Region::TARGET ? HudScale(app) : (std::min)(1.7f, GetAnchor(app, region).s);
        int width = overrideTooltipWidth > 0 ? overrideTooltipWidth : 350;
        Point size = MeasureTooltip(width);
        float w = ts * (size.x + 36.f), h = ts * (size.y + 36.f);
        float ex = (float)ExtraX(app), ey = (float)ExtraY(app);
        float cx, cy;
        if (staticPos)
        {
            ToCanvas(app, region, (float)tooltipPoint.x, (float)tooltipPoint.y, cx, cy);
        }
        else
        {
            cx = pointerX - w / 2.f;
            cy = pointerY - h - 24.f;
            if (cy < -ey + 4.f) cy = pointerY + 36.f;
        }
        cx = (std::max)(-ex + 4.f, (std::min)(1280.f + ex - 4.f - w, cx));
        cy = (std::max)(-ey + 4.f, (std::min)(720.f + ey - 4.f - h, cy));
        tooltipFrame = renderFrame;
        tooltipLeft = cx;
        tooltipTop = cy;
        tooltipRight = cx + w;
        tooltipBottom = cy + h;
        CSurface::GL_PushMatrix();
        if (activeRegion != Region::NONE) ApplyInverse(app, activeRegion);
        CSurface::GL_Scale(ts, ts, 1.f);
        drawingTooltip = true;
        super(Point((int)(cx / ts), (int)(cy / ts)), true);
        drawingTooltip = false;
        CSurface::GL_PopMatrix();
    }
    if (cursorMasked) glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
}

// For the test harness: whether the tooltip drawn in the last frames is wholly on the canvas (-1: none drawn).
int FoldLayoutTooltipOnScreen()
{
    CApp *app = G_->GetCApp();
    if (app == nullptr || renderFrame - tooltipFrame > 3) return -1;
    float ex = (float)ExtraX(app), ey = (float)ExtraY(app);
    return tooltipLeft >= -ex - 1.f && tooltipRight <= 1280.f + ex + 1.f && tooltipTop >= -ey - 1.f && tooltipBottom <= 720.f + ey + 1.f;
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

// A press on a scrollable systems row waits for the release: a sideways swipe scrolls the row, anything else is
// handed to FTL as the tap it was (down and up together), so a swipe never changes a system's power.
static bool systemsPress = false, systemsSwiping = false, replayingPress = false;
static int systemsPressX = 0, systemsPressY = 0;
static float systemsPressScroll = 0.f;

HOOK_METHOD_PRIORITY(CApp, OnMouseMove, -10000, (int x, int y, int xdiff, int ydiff, bool holdingLMB, bool holdingRMB, bool holdingMMB) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnMouseMove -> Begin (FoldLayout.cpp)\n")
    if (systemsPress)
    {
        if (!holdingLMB)
        {
            systemsPress = systemsSwiping = false; // the release never came
        }
        else
        {
            if (!systemsSwiping && std::abs(x - systemsPressX) > 12) systemsSwiping = true;
            if (systemsSwiping)
            {
                systemsScroll = systemsPressScroll - (float)(x - systemsPressX);
                SystemsScroll(this);
            }
            return;
        }
    }
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
    int button = TouchButtonAt(this, x - modifier_x, y - modifier_y);
    if (button != -1)
    {
        swallowLeftUp = true;
        PressKey(this, button == 0 ? SDLK_ESCAPE : SDLK_SPACE);
        return;
    }
    dragging = false;
    int rawX = x, rawY = y;
    MapWindowPoint(this, x, y, true);
    pressRegion = lastRegion;
    if (!replayingPress && (lastRegion == Region::BOTTOM_LEFT || lastRegion == Region::BOTTOM_RIGHT) && InGame(this) && !ModalOpen(gui) &&
        MaxSystemsScroll(this) > 0.f)
    {
        systemsPress = true;
        systemsSwiping = false;
        systemsPressX = rawX;
        systemsPressY = rawY;
        systemsPressScroll = SystemsScroll(this);
        return;
    }
    if (InGame(this) && !ModalOpen(gui))
    {
        // Touch: with a weapon (or teleporter, hacking, mind control) armed, FTL only lets go on a right click. A
        // tap anywhere but the enemy window and the weapon boxes cancels the aim, and still reaches what was
        // tapped (e.g. the jump button). Weapons that can target the player ship can still be aimed at it.
        CombatControl &combat = gui->combatControl;
        bool armed = combat.WeaponsArmed() || combat.TeleporterArmed() != 0 || combat.MindControlArmed() != 0 || combat.HackingArmed();
        bool onEnemy = combat.currentTarget != nullptr && InsideTargetBox(this, (float)(x - modifier_x), (float)(y - modifier_y));
        bool aiming = lastRegion == Region::TARGET || lastRegion == Region::WEAPONS || lastRegion == Region::DRONES ||
                      (lastRegion == Region::WORLD && (onEnemy || combat.CanTargetSelf()));
        if (armed && !aiming)
        {
            combat.DisarmAll();
            // FTL keeps buttons from hovering while something is armed; hover again so this tap reaches them.
            gui->MouseMove(x - modifier_x, y - modifier_y);
        }
    }
    super(x, y);
}

HOOK_METHOD_PRIORITY(CApp, OnLButtonUp, -10000, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnLButtonUp -> Begin (FoldLayout.cpp)\n")
    if (swallowLeftUp)
    {
        swallowLeftUp = false;
        return;
    }
    if (systemsPress)
    {
        bool swiped = systemsSwiping;
        systemsPress = systemsSwiping = false;
        if (swiped) return; // FTL never saw the press

        // A tap: FTL gets the press where it started, then this release.
        replayingPress = true;
        OnLButtonDown(systemsPressX, systemsPressY);
        replayingPress = false;
    }
    MapWindowPoint(this, x, y);
    dragging = false;
    super(x, y);
}

HOOK_METHOD_PRIORITY(CApp, OnRButtonDown, -10000, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnRButtonDown -> Begin (FoldLayout.cpp)\n")
    MapWindowPoint(this, x, y);
    pressRegion = lastRegion;
    super(x, y);
}

HOOK_METHOD_PRIORITY(CApp, OnRButtonUp, -10000, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnRButtonUp -> Begin (FoldLayout.cpp)\n")
    MapWindowPoint(this, x, y);
    super(x, y);
}

#else

bool FoldLayoutActive() { return false; }
bool FoldLayoutPushTopLeftToBottomRight() { return false; }
int FoldLayoutChoiceOnScreen() { return -1; }
int FoldLayoutTooltipOnScreen() { return -1; }
const char *FoldLayoutCheckRegion(bool &windowOverBalances) { windowOverBalances = false; return nullptr; }
float FoldLayoutBalancesBottom() { return 0.f; }
bool FoldLayoutCheckClip(float &x1, float &y1, float &x2, float &y2) { return false; }
float FoldLayoutCheckSeam() { return -1.f; }
void FoldLayoutPopMatrix() {}
const char *FoldLayoutDescribe() { return ""; }

#endif // _WIN32
