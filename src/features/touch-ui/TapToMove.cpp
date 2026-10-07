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

// selectedRoom keeps the last room hovered even when the pointer has moved onto the HUD, so check that the click
// itself is on a room: of the player ship (wX/wY are in its coordinates) or inside the enemy target window.
static bool ClickIsOnShip(CrewControl *crew, int mX, int mY, int wX, int wY)
{
    if (crew->selectedPlayerShip)
    {
        ShipGraph *graph = ShipGraph::GetShipInfo(0);
        return graph != nullptr && graph->GetSelectedRoom(wX, wY, true) != -1;
    }
    CombatControl *combat = crew->combatControl;
    if (combat == nullptr || combat->currentTarget == nullptr) return false;
    return mX >= combat->position.x && mX < combat->position.x + 370 && mY >= combat->position.y && mY < combat->position.y + 510;
}

HOOK_METHOD(CrewControl, LButton, (int mX, int mY, int wX, int wY, bool shiftHeld) -> void)
{
    LOG_HOOK("HOOK_METHOD -> CrewControl::LButton -> Begin (TapToMove.cpp)\n")
    if (TapToMoveEnabled() && !shiftHeld && !doorControlMode && !selectedCrew.empty() &&
        potentialSelectedCrew.empty() && selectedDoor == nullptr && selectedRoom != -1 &&
        !IsPointInCrewBoxes(mX, mY) && ClickIsOnShip(this, mX, mY, wX, wY))
    {
        RButton(mX, mY, false);
        return;
    }
    super(mX, mY, wX, wY, shiftHeld);
}
