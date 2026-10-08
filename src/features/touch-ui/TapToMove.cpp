#include "Global.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <vector>

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

// Drag-to-aim beams: FTL sets a beam's start on one click and its end on the next; releasing the button does
// nothing. A finger naturally drags across the ship instead, so a release far enough from the press that started
// the beam counts as that second click.
static bool beamDragPending = false;
static Point beamDragStart;

// Double-tap to autofire: on a keyboard, shift-clicking a target sets that one weapon to autofire. A second tap on
// the target within a moment of aiming a weapon there does the same.
static int lastAimedSlot = -1;
static Point lastAimedPoint;
static std::chrono::steady_clock::time_point lastAimedTime;

// Several weapons at once: while a weapon is armed, a tap on another weapon adds it to the selection (a tap on a
// selected one drops it), and the next tap on a target aims every selected weapon there. Beams go last: one beam
// still takes its drag (or second tap) across the ship.
static std::vector<int> volley;      // selected weapon slots besides the armed one
static std::vector<int> highlighted; // boxes we marked selected

static ProjectileFactory *SlotWeapon(WeaponControl *control, int slot)
{
    ShipManager *ship = control->shipManager;
    if (ship == nullptr || ship->weaponSystem == nullptr || slot < 0 || slot >= (int)ship->weaponSystem->weapons.size()) return nullptr;
    return ship->weaponSystem->weapons[slot];
}

static bool IsBeam(ProjectileFactory *weapon)
{
    return weapon != nullptr && weapon->blueprint != nullptr && weapon->blueprint->type == 2;
}

// For the test harness: how many weapons are selected besides the armed one.
int TouchVolleySize() { return (int)volley.size(); }

HOOK_METHOD_PRIORITY(WeaponControl, LButton, -100, (int x, int y, bool holdingShift) -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> WeaponControl::LButton -> Begin (TapToMove.cpp)\n")
    if (!TapToMoveEnabled() || armedWeapon == nullptr) return super(x, y, holdingShift);
    int hover = -1;
    for (int i = 0; i < (int)boxes.size(); i++)
    {
        if (boxes[i] != nullptr && boxes[i]->mouseHover) hover = i;
    }
    if (hover < 0) return super(x, y, holdingShift);
    auto selected = std::find(volley.begin(), volley.end(), hover);
    if (selected != volley.end())
    {
        volley.erase(selected);
        return true;
    }
    if (hover == armedSlot)
    {
        if (volley.empty()) return super(x, y, holdingShift); // FTL: disarm
        int next = volley.back();
        volley.pop_back();
        SelectArmament(next);
        return true;
    }
    ProjectileFactory *weapon = SlotWeapon(this, hover);
    if (weapon == nullptr || !weapon->powered) return super(x, y, holdingShift);
    volley.push_back(hover);
    return true;
}

HOOK_METHOD_PRIORITY(WeaponControl, OnLoop, -100, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> WeaponControl::OnLoop -> Begin (TapToMove.cpp)\n")
    super();
    if (armedWeapon == nullptr) volley.clear();
    for (auto it = volley.begin(); it != volley.end();)
    {
        ProjectileFactory *weapon = SlotWeapon(this, *it);
        if (weapon == nullptr || !weapon->powered || *it == armedSlot || *it >= (int)boxes.size()) it = volley.erase(it);
        else ++it;
    }
    // The selected weapons light up like the armed one.
    for (int slot : highlighted)
    {
        if (slot < (int)boxes.size() && boxes[slot] != nullptr && slot != armedSlot) boxes[slot]->selected = false;
    }
    highlighted = volley;
    for (int slot : volley) boxes[slot]->selected = true;
}

HOOK_METHOD(CombatControl, MouseClick, (int mX, int mY, bool shift) -> void)
{
    LOG_HOOK("HOOK_METHOD -> CombatControl::MouseClick -> Begin (TapToMove.cpp)\n")
    if (TapToMoveEnabled() && !volley.empty() && weapControl.armedWeapon != nullptr && aimingPoints.empty())
    {
        // Aim the armed weapon and then each selected one at this point; stop at the first that does not take it
        // (not a target: the tap did whatever else it does, once).
        std::vector<int> order{weapControl.armedSlot};
        order.insert(order.end(), volley.begin(), volley.end());
        volley.clear();
        std::stable_partition(order.begin(), order.end(), [this](int slot) { return !IsBeam(SlotWeapon(&weapControl, slot)); });
        bool beamAimed = false;
        for (int slot : order)
        {
            ProjectileFactory *weapon = SlotWeapon(&weapControl, slot);
            if (weapon == nullptr || (IsBeam(weapon) && beamAimed)) continue;
            if (weapControl.armedWeapon == nullptr || weapControl.armedSlot != slot) weapControl.SelectArmament(slot);
            if (weapControl.armedWeapon == nullptr || weapControl.armedSlot != slot) continue; // could not arm it
            super(mX, mY, shift);
            if (IsBeam(weapon))
            {
                beamAimed = aimingPoints.size() == 1;
                if (!beamAimed) break;
            }
            else if (weapon->targets.empty()) break;
        }
        beamDragPending = beamAimed;
        beamDragStart = Point(mX, mY);
        lastAimedSlot = -1;
        return;
    }
    bool wasEmpty = aimingPoints.empty();
    int armedBefore = weapControl.armedWeapon != nullptr ? weapControl.armedSlot : -1;
    super(mX, mY, shift);
    beamDragPending = TapToMoveEnabled() && wasEmpty && aimingPoints.size() == 1;
    beamDragStart = Point(mX, mY);

    if (!TapToMoveEnabled()) return;
    auto now = std::chrono::steady_clock::now();
    if (armedBefore != -1 && weapControl.armedWeapon == nullptr)
    {
        // This tap aimed that weapon.
        lastAimedSlot = armedBefore;
        lastAimedPoint = Point(mX, mY);
        lastAimedTime = now;
        return;
    }
    int dx = mX - lastAimedPoint.x, dy = mY - lastAimedPoint.y;
    if (armedBefore == -1 && lastAimedSlot != -1 && dx * dx + dy * dy < 40 * 40 &&
        now - lastAimedTime < std::chrono::milliseconds(500) && shipManager != nullptr && shipManager->weaponSystem != nullptr &&
        lastAimedSlot < (int)shipManager->weaponSystem->weapons.size())
    {
        ProjectileFactory *weapon = shipManager->weaponSystem->weapons[lastAimedSlot];
        if (weapon != nullptr && !weapon->targets.empty()) weapon->autoFiring = true;
    }
    lastAimedSlot = -1;
}

HOOK_METHOD(CombatControl, MouseUp, (int mX, int mY) -> void)
{
    LOG_HOOK("HOOK_METHOD -> CombatControl::MouseUp -> Begin (TapToMove.cpp)\n")
    super(mX, mY);
    if (!beamDragPending) return;
    beamDragPending = false;
    int dx = mX - beamDragStart.x, dy = mY - beamDragStart.y;
    if (aimingPoints.size() != 1 || dx * dx + dy * dy < 15 * 15) return;
    MouseMove(mX, mY);
    SelectTarget();
}
