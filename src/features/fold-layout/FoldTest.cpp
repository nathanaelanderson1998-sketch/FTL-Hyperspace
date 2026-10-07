#include "Global.h"

#ifdef _WIN32

#include <windows.h>
#include <GL/gl.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// Test harness for the fold layout, so it can be checked on a PC without the phone.
//
// FTL_FOLD_TEST_CANVAS=WxH  opens a W x H window instead of using the desktop (with FTL_FOLD_LAYOUT=1 the
//                           layout then treats it like the phone's canvas).
// FTL_FOLD_TEST=<file>      runs a script of timed input. Each line: "<seconds> <command> [args]".
//   move x y | click x y | rclick x y | down x y | up x y | mdown x y | mdrag x y | mup x y |
//   wheel x y notches | key sdlkey | shot name | quit
//   Coordinates are window pixels. "shot" writes <script dir>/<name>.bmp from the frame being drawn.

void FoldLayoutWheel(CApp *app, int windowX, int windowY, float notches); // FoldLayout.cpp

namespace
{
    struct Step
    {
        double time;
        std::string command;
        int a = 0, b = 0, c = 0;
        std::string name;
    };

    std::vector<Step> steps;
    size_t nextStep = 0;
    bool loaded = false;
    std::string shotDir;
    std::string pendingShot;
    std::chrono::steady_clock::time_point startTime;
    bool releaseLeftNextFrame = false;
    bool releaseRightNextFrame = false;
    int releaseX = 0, releaseY = 0;

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
            if (line.empty() || line[0] == '#') continue;
            std::istringstream in(line);
            Step step;
            if (!(in >> step.time >> step.command)) continue;
            if (step.command == "shot") in >> step.name;
            else in >> step.a >> step.b >> step.c;
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

    void RunStep(CApp *app, const Step &step)
    {
        const std::string &cmd = step.command;
        int x = step.a, y = step.b;
        hs_log_file("Fold test: %.1f %s %d %d %d %s\n", step.time, cmd.c_str(), step.a, step.b, step.c, step.name.c_str());
        if (cmd == "move") app->OnMouseMove(x, y, 0, 0, false, false, false);
        else if (cmd == "click" || cmd == "down")
        {
            app->OnMouseMove(x, y, 0, 0, false, false, false);
            app->OnLButtonDown(x, y);
            if (cmd == "click")
            {
                releaseLeftNextFrame = true;
                releaseX = x;
                releaseY = y;
            }
        }
        else if (cmd == "up") app->OnLButtonUp(x, y);
        else if (cmd == "rclick")
        {
            app->OnMouseMove(x, y, 0, 0, false, false, false);
            app->OnRButtonDown(x, y);
            releaseRightNextFrame = true;
            releaseX = x;
            releaseY = y;
        }
        else if (cmd == "mdown") app->OnMButtonDown(x, y);
        else if (cmd == "mdrag") app->OnMouseMove(x, y, 0, 0, false, false, true);
        else if (cmd == "mup") app->OnMouseMove(x, y, 0, 0, false, false, false);
        else if (cmd == "wheel") FoldLayoutWheel(app, x, y, (float)step.c);
        else if (cmd == "key")
        {
            app->OnKeyDown((SDLKey)step.a);
            app->OnKeyUp((SDLKey)step.a);
        }
        else if (cmd == "shot") pendingShot = step.name;
        else if (cmd == "quit") std::exit(0);
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

    if (releaseLeftNextFrame)
    {
        releaseLeftNextFrame = false;
        OnLButtonUp(releaseX, releaseY);
    }
    if (releaseRightNextFrame)
    {
        releaseRightNextFrame = false;
        OnRButtonUp(releaseX, releaseY);
    }

    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
    while (nextStep < steps.size() && steps[nextStep].time <= elapsed)
    {
        RunStep(this, steps[nextStep++]);
        if (!pendingShot.empty() || releaseLeftNextFrame || releaseRightNextFrame) break;
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
