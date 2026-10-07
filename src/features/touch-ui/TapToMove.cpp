#include "Global.h"

#include <cstdlib>

// Tap-to-move: with crew selected, a left click on a room (not on a crew member, door or crew box) sends the
// selected crew there, like a right click does. On a touch screen that turns "hold to right click" into a plain tap.
// Set FTL_TAP_TO_MOVE=0 to keep the original behaviour.
static bool TapToMoveEnabled()
{
    static const bool enabled = []
    {
        const char *value = std::getenv("FTL_TAP_TO_MOVE");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

HOOK_METHOD(CrewControl, LButton, (int mX, int mY, int wX, int wY, bool shiftHeld) -> void)
{
    LOG_HOOK("HOOK_METHOD -> CrewControl::LButton -> Begin (TapToMove.cpp)\n")
    if (TapToMoveEnabled() && !shiftHeld && !doorControlMode && !selectedCrew.empty() &&
        potentialSelectedCrew.empty() && selectedDoor == nullptr && selectedRoom != -1 &&
        !IsPointInCrewBoxes(mX, mY))
    {
        RButton(mX, mY, false);
        return;
    }
    super(mX, mY, wX, wY, shiftHeld);
}
