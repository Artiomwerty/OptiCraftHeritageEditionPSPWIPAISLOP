// Host-side tests for the PSP control mapper (no PSP SDK needed).
#include "psp/input/PspControls.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace PspControls;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

int main()
{
	Config cfg; State st; RawPad pad;

	// Centre and drift inside the deadzone give no movement.
	pad.lx = 128; pad.ly = 128;
	CHECK(near(map(pad, cfg, st, 0).moveX, 0.f));
	pad.lx = 128 + 20; // ~0.157 < 0.20
	CHECK(near(map(pad, cfg, st, 10).moveX, 0.f));

	// Full nub up = full forward; full right = full strafe right.
	pad.lx = 255; pad.ly = 0;
	Actions a = map(pad, cfg, st, 20);
	CHECK(near(a.moveX, 1.f) && near(a.moveZ, 1.f));
	CHECK(axis(128 + 26, 0.20f) > 0.f); // just past the deadzone edge starts near 0
	CHECK(axis(128 + 26, 0.20f) < 0.1f);

	// Face buttons look in LookButtons.
	pad = RawPad(); pad.buttons = BTN_TRIANGLE | BTN_CIRCLE;
	a = map(pad, cfg, st, 30);
	CHECK(near(a.lookPitch, 1.f) && near(a.lookYaw, 1.f));
	pad.buttons = BTN_CROSS | BTN_SQUARE;
	a = map(pad, cfg, st, 40);
	CHECK(near(a.lookPitch, -1.f) && near(a.lookYaw, -1.f));

	// R breaks, L places, together neither (they chord to sprint).
	pad = RawPad(); pad.buttons = BTN_R;
	CHECK(map(pad, cfg, st, 50).breakBlock);
	pad.buttons = BTN_L;
	a = map(pad, cfg, st, 60); CHECK(a.placeOrUse && !a.breakBlock);
	pad.buttons = BTN_L | BTN_R;
	a = map(pad, cfg, st, 70); CHECK(!a.placeOrUse && !a.breakBlock);

	// Hotbar and inventory are edges, not held repeats.
	st = State(); pad = RawPad(); pad.buttons = BTN_RIGHT;
	CHECK(map(pad, cfg, st, 0).hotbarDelta == 1);
	CHECK(map(pad, cfg, st, 16).hotbarDelta == 0); // still held
	pad.buttons = BTN_LEFT; CHECK(map(pad, cfg, st, 32).hotbarDelta == -1);
	pad.buttons = BTN_SELECT; CHECK(map(pad, cfg, st, 48).openInventory);
	CHECK(!map(pad, cfg, st, 64).openInventory);

	// Sneak toggles on each press.
	st = State(); pad = RawPad();
	pad.buttons = BTN_DOWN; CHECK(map(pad, cfg, st, 0).sneak);
	pad.buttons = 0;        CHECK(map(pad, cfg, st, 16).sneak);   // stays on
	pad.buttons = BTN_DOWN; CHECK(!map(pad, cfg, st, 32).sneak);  // toggles off

	// Double-tap jump toggles fly only inside the window.
	st = State(); pad = RawPad();
	pad.buttons = BTN_UP;   CHECK(!map(pad, cfg, st, 1000).toggleFly);
	pad.buttons = 0;        map(pad, cfg, st, 1100);
	pad.buttons = BTN_UP;   CHECK(map(pad, cfg, st, 1200).toggleFly);   // 200 ms
	st = State();
	pad.buttons = BTN_UP;   map(pad, cfg, st, 2000);
	pad.buttons = 0;        map(pad, cfg, st, 2100);
	pad.buttons = BTN_UP;   CHECK(!map(pad, cfg, st, 2500).toggleFly);  // 500 ms

	// Alternative layout: nub looks, D-pad moves.
	cfg.layout = Layout::LookNub; st = State(); pad = RawPad();
	pad.lx = 255; pad.ly = 0; pad.buttons = BTN_UP;
	a = map(pad, cfg, st, 0);
	CHECK(near(a.lookYaw, 1.f) && near(a.lookPitch, 1.f) && near(a.moveZ, 1.f));

	// Look speed scales and clamps.
	cfg = Config(); cfg.lookSpeed = 0.5f; st = State(); pad = RawPad(); pad.buttons = BTN_CIRCLE;
	CHECK(near(map(pad, cfg, st, 0).lookYaw, 0.5f));
	cfg.lookSpeed = 5.f; CHECK(near(map(pad, cfg, st, 16).lookYaw, 1.f));

	if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
	std::printf("PspControlsTests: all passed\n");
	return 0;
}
