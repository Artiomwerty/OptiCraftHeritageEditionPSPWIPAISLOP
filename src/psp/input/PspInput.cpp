// PspInput.cpp — sceCtrl -> engine key/mouse events.
//
// Gameplay uses PspControls (the approved layout). In menus the nub and D-pad move a cursor;
// Cross = left click, Square = right click, Circle/Start = back (Esc).
#ifdef PSP_PLATFORM

#include "psp/input/PspInput.h"
#include "psp/input/PspControls.h"
#include "psp/render/PspGuDevice.h"
#include "psp/system/PspSystem.h"

#include "lwjgl/Keyboard.h"
#include "lwjgl/Mouse.h"

#include <pspctrl.h>
#include <pspkernel.h>

namespace
{
volatile bool g_exit = false;

int exitCallback(int, int, void *)
{
	g_exit = true;
	return 0;
}

int callbackThread(SceSize, void *)
{
	int cb = sceKernelCreateCallback("exit_callback", exitCallback, nullptr);
	sceKernelRegisterExitCallback(cb);
	sceKernelSleepThreadCB();
	return 0;
}

PspControls::Config g_cfg;
PspControls::State g_state;

// Held-state mirror so we only emit edges.
struct Held
{
	bool w = false, a = false, s = false, d = false, space = false, shift = false, ctrl = false;
	bool lmb = false, rmb = false;
	bool menuL = false, menuR = false, menuBack = false;
} g_h;

int g_cx = 240, g_cy = 136; // menu cursor, top-left origin
unsigned g_prevBtn = 0;
bool g_wasMenu = true;

void key(bool &held, bool now, int k)
{
	if (held != now)
	{
		held = now;
		lwjgl::Keyboard::detail::pushKey(k, now);
	}
}

void pulse(int k)
{
	lwjgl::Keyboard::detail::pushKey(k, true);
	lwjgl::Keyboard::detail::pushKey(k, false);
}

void mouseBtn(bool &held, bool now, int button)
{
	if (held != now)
	{
		held = now;
		lwjgl::Mouse::detail::pushButton(button, now, g_cx, g_cy);
	}
}

void releaseAll()
{
	key(g_h.w, false, lwjgl::Keyboard::KEY_W);
	key(g_h.a, false, lwjgl::Keyboard::KEY_A);
	key(g_h.s, false, lwjgl::Keyboard::KEY_S);
	key(g_h.d, false, lwjgl::Keyboard::KEY_D);
	key(g_h.space, false, lwjgl::Keyboard::KEY_SPACE);
	key(g_h.shift, false, lwjgl::Keyboard::KEY_LSHIFT);
	key(g_h.ctrl, false, lwjgl::Keyboard::KEY_LCONTROL);
	mouseBtn(g_h.lmb, false, 0);
	mouseBtn(g_h.rmb, false, 1);
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
} // namespace

namespace PspInput
{

void init()
{
	sceCtrlSetSamplingCycle(0);
	sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
	int th = sceKernelCreateThread("exit_thread", callbackThread, 0x11, 0xFA0, 0, nullptr);
	if (th >= 0)
		sceKernelStartThread(th, 0, nullptr);
}

bool exitRequested() { return g_exit; }

void poll(bool inMenu)
{
	SceCtrlData pad;
	sceCtrlPeekBufferPositive(&pad, 1);
	PspControls::RawPad raw;
	raw.buttons = pad.Buttons;
	raw.lx = pad.Lx;
	raw.ly = pad.Ly;

	const unsigned pressed = raw.buttons & ~g_prevBtn;
	g_prevBtn = raw.buttons;

	if (inMenu)
	{
		if (!g_wasMenu)
		{
			releaseAll();
			g_state = PspControls::State();
		}
		g_wasMenu = true;

		const float dz = g_cfg.deadzone;
		float mx = PspControls::axis(raw.lx, dz) * 9.0f;
		float my = PspControls::axis(raw.ly, dz) * 9.0f;
		if (raw.buttons & PspControls::BTN_LEFT) mx -= 5.0f;
		if (raw.buttons & PspControls::BTN_RIGHT) mx += 5.0f;
		if (raw.buttons & PspControls::BTN_UP) my -= 5.0f;
		if (raw.buttons & PspControls::BTN_DOWN) my += 5.0f;
		const int ox = g_cx, oy = g_cy;
		g_cx = (int)clampf((float)g_cx + mx, 0.f, (float)(PspGu::kScreenWidth - 1));
		g_cy = (int)clampf((float)g_cy + my, 0.f, (float)(PspGu::kScreenHeight - 1));
		if (g_cx != ox || g_cy != oy)
			lwjgl::Mouse::detail::pushMotion(g_cx, g_cy, g_cx - ox, g_cy - oy);

		mouseBtn(g_h.menuL, (raw.buttons & PspControls::BTN_CROSS) != 0, 0);
		mouseBtn(g_h.menuR, (raw.buttons & PspControls::BTN_SQUARE) != 0, 1);
		if (pressed & (PspControls::BTN_CIRCLE | PspControls::BTN_START))
			pulse(lwjgl::Keyboard::KEY_ESCAPE);
		if (pressed & PspControls::BTN_L) lwjgl::Mouse::detail::pushWheel(1, g_cx, g_cy);
		if (pressed & PspControls::BTN_R) lwjgl::Mouse::detail::pushWheel(-1, g_cx, g_cy);
		return;
	}

	if (g_wasMenu)
	{
		g_wasMenu = false;
		g_h.menuL = g_h.menuR = false;
		g_prevBtn = raw.buttons; // don't treat the click that closed the menu as a game action
		g_state.prevButtons = raw.buttons;
		return;
	}

	const PspControls::Actions a = PspControls::map(raw, g_cfg, g_state, PspSystem::millis());

	key(g_h.w, a.moveZ > 0.3f, lwjgl::Keyboard::KEY_W);
	key(g_h.s, a.moveZ < -0.3f, lwjgl::Keyboard::KEY_S);
	key(g_h.a, a.moveX < -0.3f, lwjgl::Keyboard::KEY_A);
	key(g_h.d, a.moveX > 0.3f, lwjgl::Keyboard::KEY_D);
	key(g_h.space, a.jump, lwjgl::Keyboard::KEY_SPACE);
	key(g_h.shift, a.sneak, lwjgl::Keyboard::KEY_LSHIFT);
	key(g_h.ctrl, a.sprint, lwjgl::Keyboard::KEY_LCONTROL);
	mouseBtn(g_h.lmb, a.breakBlock, 0);
	mouseBtn(g_h.rmb, a.placeOrUse, 1);

	if (a.hotbarDelta != 0)
		lwjgl::Mouse::detail::pushWheel(-a.hotbarDelta, g_cx, g_cy);
	if (a.openInventory)
		pulse(lwjgl::Keyboard::KEY_E);
	if (a.pause)
		pulse(lwjgl::Keyboard::KEY_ESCAPE);

	const float lookPx = 14.0f;
	const int dx = (int)(a.lookYaw * lookPx);
	const int dy = (int)(-a.lookPitch * lookPx);
	if (dx != 0 || dy != 0)
		lwjgl::Mouse::detail::pushMotion(g_cx, g_cy, dx, dy);
}

} // namespace PspInput
#endif
