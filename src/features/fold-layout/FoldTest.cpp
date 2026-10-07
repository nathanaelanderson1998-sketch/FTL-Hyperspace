#include "Global.h"

#include <algorithm>

#ifdef _WIN32

#include <windows.h>
#include <GL/gl.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// Test harness for the fold layout, so it can be played through on a PC without the phone.
//
// FTL_FOLD_TEST_CANVAS=WxH  opens a W x H window instead of using the desktop (with FTL_FOLD_LAYOUT=1 the
//                           layout then treats it like the phone's canvas).
// FTL_FOLD_TEST=<file>      runs a script of timed input. Each line: "<seconds> <command> [args]".
//   move P | click P | rclick P | down P | up P | drag P1 P2 [frames] |
//   mdown x y | mdrag x y | mup x y | wheel x y notches | key sdlkey | cmd <console command> |
//   shot name | state | expect key=value (or key!=value) | quit
//   A point P is window pixels "x y" or a layout region and a game point, e.g. "bl 300 638" (tl/bl/br/world/modal). "shot" writes <script dir>/<name>.bmp; "state" logs the game state that
//   "expect" checks; every expect logs "Fold test: PASS ..." or "Fold test: FAIL ...".

void FoldLayoutWheel(CApp *app, int windowX, int windowY, float notches); // FoldLayout.cpp
const char *FoldLayoutDescribe();                                       // FoldLayout.cpp
int FoldLayoutChoiceOnScreen(); // FoldLayout.cpp
int FoldLayoutTooltipOnScreen(); // FoldLayout.cpp
bool FoldLayoutTouchButton(int index, int &windowX, int &windowY); // FoldLayout.cpp
int FoldCheckOffscreenCount(); // FoldCheck.cpp
int FoldCheckOverlapCount();   // FoldCheck.cpp
bool FoldLayoutToWindow(const std::string &region, float x, float y, int &windowX, int &windowY); // FoldLayout.cpp

namespace
{
    struct Step
    {
        double time;
        std::string command;
        std::string rest;
    };

    std::vector<Step> steps;
    size_t nextStep = 0;
    bool loaded = false;
    std::string shotDir;
    std::string pendingShot;
    std::chrono::steady_clock::time_point startTime;
    std::deque<std::function<void(CApp *)>> frameQueue; // one entry runs per frame
    int passes = 0, failures = 0;

    void LoadScript()
    {
        loaded = true;
        const char *path = std::getenv("FTL_FOLD_TEST");
        if (path == nullptr || path[0] == '\0') return;
        std::ifstream file(path);
        if (!file)
        {
            hs_log_file("Fold test: can't open script %s\n", path);
            return;
        }
        std::string dir(path);
        size_t slash = dir.find_last_of("/\\");
        shotDir = slash == std::string::npos ? "." : dir.substr(0, slash);

        std::string line;
        while (std::getline(file, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            std::istringstream in(line);
            Step step;
            if (!(in >> step.time >> step.command)) continue;
            std::getline(in, step.rest);
            size_t first = step.rest.find_first_not_of(' ');
            step.rest = first == std::string::npos ? "" : step.rest.substr(first);
            steps.push_back(step);
        }
        startTime = std::chrono::steady_clock::now();
        hs_log_file("Fold test: %d steps from %s\n", (int)steps.size(), path);
    }

    void WriteShot(const std::string &name, int width, int height)
    {
        std::vector<unsigned char> pixels((size_t)width * height * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        for (size_t i = 0; i < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]); // RGBA -> BGRA

        // Bottom-up 32-bit BMP, which is exactly glReadPixels' row order.
        BITMAPFILEHEADER fileHeader = {};
        BITMAPINFOHEADER infoHeader = {};
        infoHeader.biSize = sizeof(infoHeader);
        infoHeader.biWidth = width;
        infoHeader.biHeight = height;
        infoHeader.biPlanes = 1;
        infoHeader.biBitCount = 32;
        infoHeader.biCompression = BI_RGB;
        fileHeader.bfType = 0x4D42;
        fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
        fileHeader.bfSize = fileHeader.bfOffBits + (DWORD)pixels.size();

        std::string path = shotDir + "\\" + name + ".bmp";
        FILE *out = std::fopen(path.c_str(), "wb");
        if (out == nullptr) return;
        std::fwrite(&fileHeader, sizeof(fileHeader), 1, out);
        std::fwrite(&infoHeader, sizeof(infoHeader), 1, out);
        std::fwrite(pixels.data(), 1, pixels.size(), out);
        std::fclose(out);
        hs_log_file("Fold test: wrote %s (%dx%d)\n", path.c_str(), width, height);
    }

    // The game state the expectations check, as key=value pairs.
    std::map<std::string, std::string> GameState(CApp *app)
    {
        std::map<std::string, std::string> state;
        std::istringstream layout(FoldLayoutDescribe());
        std::string token;
        while (layout >> token)
        {
            size_t eq = token.find('=');
            if (eq != std::string::npos) state[token.substr(0, eq)] = token.substr(eq + 1);
        }

        CommandGui *gui = app->gui;
        state["offscreen"] = std::to_string(FoldCheckOffscreenCount());
        state["overlap"] = std::to_string(FoldCheckOverlapCount());
        state["menu"] = std::to_string((int)app->menu.bOpen);
        if (gui == nullptr || app->menu.bOpen) return state;

        state["paused"] = std::to_string((int)gui->bPaused);
        state["map"] = std::to_string((int)(gui->starMap != nullptr && gui->starMap->bOpen));
        state["choice"] = std::to_string((int)gui->choiceBox.bOpen);
        state["choice_onscreen"] = std::to_string(FoldLayoutChoiceOnScreen());
        state["tooltip_onscreen"] = std::to_string(FoldLayoutTooltipOnScreen());
        state["store"] = std::to_string((int)gui->storeScreens.bOpen);
        state["pausemenu"] = std::to_string((int)gui->menuBox.bOpen);
        state["ship"] = std::to_string((int)gui->shipScreens.bOpen);
        state["enemy"] = std::to_string((int)(gui->combatControl.currentTarget != nullptr));
        auto pt = [](const Point &p) { return std::to_string(p.x) + "," + std::to_string(p.y); };
        state["cc_pos"] = pt(gui->combatControl.position);
        state["cc_target"] = pt(gui->combatControl.targetPosition);
        state["cc_box"] = pt(gui->combatControl.boxPosition);
        if (gui->combatControl.currentTarget != nullptr)
        {
            ShipGraph *graph = ShipGraph::GetShipInfo(1);
            if (graph != nullptr)
            {
                state["e_rooms"] = std::to_string((int)graph->rooms.size());
                for (size_t i = 0; i < graph->rooms.size() && i < 4; i++)
                {
                    const Globals::Rect &r = graph->rooms[i]->rect;
                    state["e_room" + std::to_string(i)] = std::to_string(r.x) + "," + std::to_string(r.y) + "," + std::to_string(r.w) + "," + std::to_string(r.h);
                }
            }
        }
        state["autofire"] = std::to_string((int)gui->combatControl.weapControl.autoFiring);
        state["weap_x"] = pt(gui->combatControl.weapControl.location);
        state["drone_x"] = pt(gui->combatControl.droneControl.location);
        if (gui->starMap != nullptr && gui->starMap->currentLoc != nullptr)
        {
            auto &locs = gui->starMap->locations;
            state["loc"] = std::to_string((int)(std::find(locs.begin(), locs.end(), gui->starMap->currentLoc) - locs.begin()));
        }
        state["sel"] = std::to_string((int)gui->crewControl.selectedCrew.size());
        state["armed"] = std::to_string(gui->combatControl.weapControl.armedWeapon != nullptr ? gui->combatControl.weapControl.armedSlot : -1);

        ShipManager *ship = gui->shipComplete != nullptr ? gui->shipComplete->shipManager : nullptr;
        if (ship == nullptr) return state;

        int index = 0;
        for (CrewMember *crew : ship->vCrewList)
        {
            if (crew == nullptr || crew->iShipId != 0) continue;
            state["crew" + std::to_string(index) + "_room"] = std::to_string(crew->iRoomId);
            state["crew" + std::to_string(index) + "_dest"] = std::to_string(crew->currentSlot.roomId);
            index++;
        }
        for (ShipSystem *system : ship->vSystemList)
        {
            if (system != nullptr) state["pow_" + std::to_string(system->iSystemType)] = std::to_string(system->powerState.first);
        }
        int doorsOpen = 0;
        for (Door *door : ship->ship.vDoorList)
        {
            if (door != nullptr && door->bOpen) doorsOpen++;
        }
        state["doors_open"] = std::to_string(doorsOpen);
        if (ship->weaponSystem != nullptr)
        {
            for (size_t i = 0; i < ship->weaponSystem->weapons.size(); i++)
            {
                ProjectileFactory *weapon = ship->weaponSystem->weapons[i];
                if (weapon == nullptr) continue;
                state["w" + std::to_string(i) + "_targets"] = std::to_string((int)weapon->targets.size());
                state["w" + std::to_string(i) + "_auto"] = std::to_string((int)weapon->autoFiring);
                state["w" + std::to_string(i) + "_powered"] = std::to_string((int)weapon->powered);
            }
        }
        return state;
    }

    std::string StateText(const std::map<std::string, std::string> &state)
    {
        std::string text;
        for (auto &entry : state) text += entry.first + "=" + entry.second + " ";
        return text;
    }

    void Expect(CApp *app, const Step &step)
    {
        std::string rule = step.rest;
        bool negate = false;
        size_t op = rule.find("!=");
        if (op != std::string::npos) negate = true;
        else op = rule.find('=');
        if (op == std::string::npos) return;
        std::string key = rule.substr(0, op);
        std::string want = rule.substr(op + (negate ? 2 : 1));

        auto state = GameState(app);
        auto found = state.find(key);
        std::string have = found == state.end() ? "<missing>" : found->second;
        bool ok = negate ? have != want : have == want;
        (ok ? passes : failures)++;
        hs_log_file("Fold test: %s %.1f expect %s (have %s)\n", ok ? "PASS" : "FAIL", step.time, rule.c_str(), have.c_str());
        if (!ok) hs_log_file("Fold test:   state: %s\n", StateText(state).c_str());
    }

    // A point is either window pixels ("x y") or a game point in a layout region ("bl 300 638"), placed where
    // that region draws it right now.
    bool ReadPoint(std::istringstream &in, int &x, int &y)
    {
        std::string first;
        if (!(in >> first)) return false;
        if (first == "proom" || first == "eroom")
        {
            // Centre of a room of the player ship or the enemy ship, wherever the game currently draws it.
            int index = 0;
            in >> index;
            CApp *app = G_->GetCApp();
            CommandGui *gui = app != nullptr ? app->gui : nullptr;
            if (gui == nullptr) return false;
            bool enemy = first == "eroom";
            CompleteShip *ship = enemy ? gui->combatControl.currentTarget : gui->shipComplete;
            ShipGraph *graph = ShipGraph::GetShipInfo(enemy ? 1 : 0);
            if (ship == nullptr || graph == nullptr || index < 0 || index >= (int)graph->rooms.size()) return false;
            // GetWorldCoordinates maps a game-screen point to ship coordinates (a pure offset), the same way the game
            // maps real clicks, so the screen point of a ship point is that point minus the offset.
            Point world = gui->GetWorldCoordinates(Point(0, 0), enemy);
            Pointf center = graph->GetRoomCenter(index);
            return FoldLayoutToWindow(enemy ? "target" : "world", center.x - world.x, center.y - world.y, x, y);
        }
        if (first == "crew")
        {
            // A player crew member on the player ship (same numbering as the crewN_ state keys).
            int index = 0, n = 0;
            in >> index;
            CApp *app = G_->GetCApp();
            CommandGui *gui = app != nullptr ? app->gui : nullptr;
            if (gui == nullptr || gui->shipComplete == nullptr) return false;
            ShipManager *ship = gui->shipComplete->shipManager;
            Point world = gui->GetWorldCoordinates(Point(0, 0), false);
            for (CrewMember *crew : ship->vCrewList)
            {
                if (crew == nullptr || crew->iShipId != 0) continue;
                if (n++ == index) return FoldLayoutToWindow("world", crew->x - world.x, crew->y - world.y, x, y);
            }
            return false;
        }
        if (first == "button")
        {
            // A touch button: "button menu" or "button pause".
            std::string which;
            in >> which;
            return FoldLayoutTouchButton(which == "pause" ? 1 : 0, x, y);
        }
        if (first == "beacon")
        {
            // Near (dx, dy away from) the Nth beacon connected to the current one, on the open star map.
            int index = 0, n = 0;
            float dx = 0.f, dy = 0.f;
            in >> index >> dx >> dy;
            CApp *app = G_->GetCApp();
            StarMap *map = app != nullptr && app->gui != nullptr ? app->gui->starMap : nullptr;
            if (map == nullptr || map->currentLoc == nullptr) return false;
            for (Location *loc : map->currentLoc->connectedLocations)
            {
                if (loc == nullptr || n++ != index) continue;
                return FoldLayoutToWindow("modal", map->position.x + map->translation.x + loc->loc.x + dx,
                                          map->position.y + map->translation.y + loc->loc.y + dy, x, y);
            }
            return false;
        }
        if (first == "tl" || first == "bl" || first == "br" || first == "wp" || first == "cr" || first == "dr" || first == "door" || first == "st" || first == "target" || first == "menu" ||
            first == "world" || first == "modal" || first == "none")
        {
            float gx = 0.f, gy = 0.f;
            in >> gx >> gy;
            return FoldLayoutToWindow(first, gx, gy, x, y);
        }
        x = std::atoi(first.c_str());
        in >> y;
        return true;
    }

    void Move(CApp *app, int x, int y, bool left) { app->OnMouseMove(x, y, 0, 0, left, false, false); }

    void RunStep(CApp *app, const Step &step)
    {
        const std::string &cmd = step.command;
        std::istringstream in(step.rest);
        int x = 0, y = 0, a = 0, b = 0, c = 0;
        hs_log_file("Fold test: %.1f %s %s\n", step.time, cmd.c_str(), step.rest.c_str());
        if (cmd == "move")
        {
            ReadPoint(in, x, y);
            Move(app, x, y, false);
        }
        else if (cmd == "click" || cmd == "rclick")
        {
            ReadPoint(in, x, y);
            bool right = cmd == "rclick";
            // Same as a tap on the phone: pointer moves there, one frame later press, next frame release.
            Move(app, x, y, false);
            for (int i = 0; i < 6; i++) frameQueue.push_back([](CApp *) {});
            frameQueue.push_back([=](CApp *g) { right ? g->OnRButtonDown(x, y) : g->OnLButtonDown(x, y); });
            frameQueue.push_back([=](CApp *g) { right ? g->OnRButtonUp(x, y) : g->OnLButtonUp(x, y); });
        }
        else if (cmd == "down")
        {
            ReadPoint(in, x, y);
            Move(app, x, y, false);
            frameQueue.push_back([=](CApp *g) { g->OnLButtonDown(x, y); });
        }
        else if (cmd == "up")
        {
            ReadPoint(in, x, y);
            app->OnLButtonUp(x, y);
        }
        else if (cmd == "drag")
        {
            ReadPoint(in, x, y);
            ReadPoint(in, a, b);
            int frames = 12;
            in >> frames;
            Move(app, x, y, false);
            for (int i = 0; i < 6; i++) frameQueue.push_back([](CApp *) {});
            frameQueue.push_back([=](CApp *g) { g->OnLButtonDown(x, y); });
            for (int i = 1; i <= frames; i++)
            {
                int px = x + (a - x) * i / frames, py = y + (b - y) * i / frames;
                frameQueue.push_back([=](CApp *g) { Move(g, px, py, true); });
            }
            frameQueue.push_back([=](CApp *g) { g->OnLButtonUp(a, b); });
        }
        else if (cmd == "mdown")
        {
            ReadPoint(in, x, y);
            app->OnMButtonDown(x, y);
        }
        else if (cmd == "mdrag")
        {
            ReadPoint(in, x, y);
            app->OnMouseMove(x, y, 0, 0, false, false, true);
        }
        else if (cmd == "mup")
        {
            ReadPoint(in, x, y);
            app->OnMouseMove(x, y, 0, 0, false, false, false);
        }
        else if (cmd == "wheel")
        {
            ReadPoint(in, x, y);
            in >> c;
            FoldLayoutWheel(app, x, y, (float)c);
        }
        else if (cmd == "key")
        {
            in >> a;
            app->OnKeyDown((SDLKey)a);
            frameQueue.push_back([=](CApp *g) { g->OnKeyUp((SDLKey)a); });
        }
        else if (cmd == "cmd")
        {
            if (app->gui != nullptr)
            {
                std::string command = step.rest;
                app->gui->RunCommand(command);
            }
        }
        else if (cmd == "shot") pendingShot = step.rest;
        else if (cmd == "state") hs_log_file("Fold test: state %s\n", StateText(GameState(app)).c_str());
        else if (cmd == "expect") Expect(app, step);
        else if (cmd == "quit")
        {
            hs_log_file("Fold test: DONE %d passed, %d failed\n", passes, failures);
            std::exit(0);
        }
    }
}

bool FoldTestCanvas(int &width, int &height)
{
    const char *value = std::getenv("FTL_FOLD_TEST_CANVAS");
    return value != nullptr && std::sscanf(value, "%dx%d", &width, &height) == 2 && width >= 1280 && height >= 720;
}

HOOK_METHOD_PRIORITY(CApp, OnLoop, -9000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnLoop -> Begin (FoldTest.cpp)\n")
    if (!loaded) LoadScript();

    if (!frameQueue.empty())
    {
        auto action = frameQueue.front();
        frameQueue.pop_front();
        action(this);
    }
    else
    {
        double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
        while (nextStep < steps.size() && steps[nextStep].time <= elapsed)
        {
            RunStep(this, steps[nextStep++]);
            if (!pendingShot.empty() || !frameQueue.empty()) break;
        }
    }
    super();
}

// Read the frame just before it is presented.
HOOK_STATIC_PRIORITY(CSurface, FinishFrame, -9000, () -> void)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::FinishFrame -> Begin (FoldTest.cpp)\n")
    CApp *app = G_->GetCApp();
    if (!pendingShot.empty() && app != nullptr)
    {
        WriteShot(pendingShot, app->screen_x, app->screen_y);
        pendingShot.clear();
    }
    super();
}

#endif // _WIN32
