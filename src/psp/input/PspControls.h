#pragma once

// PspControls — pure mapping from raw PSP pad state to game actions.
//
// No PSP SDK calls here, so it is unit-testable on a PC (tests/PspControlsTests.cpp).
// The sceCtrl poller (InputBackend_PSP.cpp) fills RawPad each frame and feeds the
// resulting Actions into the engine's key/mouse synthesis.
//
// Layout "LookButtons" (default; the PSP has one analog nub):
//   nub              move            face buttons  look (tri up, cross down, sq left, circ right)
//   R                break (hold)    L             place / use
//   D-pad left/right hotbar -/+      D-pad up      jump (double-tap: toggle fly)
//   D-pad down       sneak toggle    Start         pause
//   Select           inventory       L+R           sprint toggle
// Layout "LookNub" (alternative): D-pad moves, nub looks, face buttons keep
// jump (cross), sneak (circle), inventory (triangle), pause (Start).

namespace PspControls
{

// Same values as the PSP SDK's PspCtrlButtons (static_assert'ed in the backend).
enum Button : unsigned
{
	BTN_SELECT   = 0x000001,
	BTN_START    = 0x000008,
	BTN_UP       = 0x000010,
	BTN_RIGHT    = 0x000020,
	BTN_DOWN     = 0x000040,
	BTN_LEFT     = 0x000080,
	BTN_L        = 0x000100,
	BTN_R        = 0x000200,
	BTN_TRIANGLE = 0x001000,
	BTN_CIRCLE   = 0x002000,
	BTN_CROSS    = 0x004000,
	BTN_SQUARE   = 0x008000,
};

enum class Layout { LookButtons, LookNub };

struct RawPad
{
	unsigned buttons = 0;   // BTN_* bitmask
	unsigned char lx = 128; // analog nub, 0..255, 128 = centre
	unsigned char ly = 128;
};

struct Config
{
	Layout layout = Layout::LookButtons;
	float deadzone = 0.25f;          // fraction of full travel ignored (worn nubs drift)
	float lookSpeed = 1.0f;          // multiplier on look output
	unsigned doubleTapMs = 300;      // jump double-tap window for fly toggle
};

// Cross-frame state (edge detection, toggles, double-tap timing).
struct State
{
	unsigned prevButtons = 0;
	unsigned lastJumpPressMs = 0;
	bool hasLastJump = false;
	bool sneaking = false;
	bool sprinting = false;
	bool flying = false;
};

struct Actions
{
	float moveX = 0.f;      // strafe, -1 left .. +1 right
	float moveZ = 0.f;      // forward, -1 back .. +1 forward
	float lookYaw = 0.f;    // -1 left .. +1 right (per-frame rate, scale by dt)
	float lookPitch = 0.f;  // -1 down .. +1 up
	bool breakBlock = false;    // held
	bool placeOrUse = false;    // held
	bool jump = false;          // held
	bool sneak = false;         // toggled state
	bool sprint = false;        // toggled state
	bool toggleFly = false;     // edge: double-tapped jump this frame
	int  hotbarDelta = 0;       // edge: -1 / 0 / +1
	bool openInventory = false; // edge
	bool pause = false;         // edge
};

// Normalise one nub axis to -1..1 with a radial-free axial deadzone and rescale,
// so output starts at 0 right at the deadzone edge instead of jumping.
float axis(unsigned char raw, float deadzone);

// nowMs: monotonic milliseconds. Updates `state`, returns this frame's actions.
Actions map(const RawPad &pad, const Config &cfg, State &state, unsigned nowMs);

}
