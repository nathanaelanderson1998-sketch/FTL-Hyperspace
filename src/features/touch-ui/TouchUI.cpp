#include "Global.h"

#ifdef _WIN32

#include <cstdlib>

// FTL's iPad touch controls (CommandGui::OnTouch, MainMenu::OnTouch, ...) are compiled into the Windows build,
// but the platform layer that fed them is not: CApp::OnTouchDown and friends are empty stubs here.
// With FTL_TOUCH_UI=1 in the environment, the left mouse button is turned into a single touch and routed
// the same way CApp::OnLButtonDown routes clicks (language chooser -> main menu -> in-game GUI).
static bool TouchUIEnabled()
{
    static const bool enabled = []
    {
        const char *value = std::getenv("FTL_TOUCH_UI");
        bool on = value != nullptr && value[0] == '1';
        if (on) hs_log_file("Touch UI: enabled (FTL_TOUCH_UI=1)\n");
        return on;
    }();
    return enabled;
}

static const int TOUCH_ID = 1;
static bool touchActive = false;
static Point touchStart;

// Same window -> game transform CApp::OnLButtonDown applies to mouse clicks.
static Point ToGamePos(CApp *app, int x, int y)
{
    return Point((int)((x - app->x_bar) * app->mouseModifier_x) - app->modifier_x,
                 (int)((y - app->y_bar) * app->mouseModifier_y) - app->modifier_y);
}

static void RouteTouch(CApp *app, TouchAction action, Point pos)
{
    if (app->menu.bOpen)
    {
        app->menu.OnTouch(action, TOUCH_ID, pos.x, pos.y, touchStart.x, touchStart.y);
    }
    else if (app->gui != nullptr)
    {
        app->gui->OnTouch(action, TOUCH_ID, pos.x, pos.y, touchStart.x, touchStart.y);
    }
}

HOOK_METHOD(CApp, OnLButtonDown, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD -> CApp::OnLButtonDown -> Begin (TouchUI.cpp)\n")
    if (!TouchUIEnabled() || langChooser.bOpen) return super(x, y);

    touchStart = ToGamePos(this, x, y);
    touchActive = true;
    RouteTouch(this, TouchAction::TOUCH_DOWN, touchStart);
}

HOOK_METHOD(CApp, OnMouseMove, (int x, int y, int xdiff, int ydiff, bool holdingLMB, bool holdingRMB, bool holdingMMB) -> void)
{
    LOG_HOOK("HOOK_METHOD -> CApp::OnMouseMove -> Begin (TouchUI.cpp)\n")
    // The cursor still tracks the finger so hover highlights and tooltips keep working.
    super(x, y, xdiff, ydiff, holdingLMB, holdingRMB, holdingMMB);
    if (touchActive) RouteTouch(this, TouchAction::TOUCH_MOVE, ToGamePos(this, x, y));
}

HOOK_METHOD(CApp, OnLButtonUp, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD -> CApp::OnLButtonUp -> Begin (TouchUI.cpp)\n")
    if (!touchActive) return super(x, y);

    touchActive = false;
    RouteTouch(this, TouchAction::TOUCH_UP, ToGamePos(this, x, y));
}

#endif // _WIN32
