#include "psp/input/PspControls.h"

namespace PspControls
{

float axis(unsigned char raw, float deadzone)
{
	float v = (static_cast<int>(raw) - 128) / 127.0f;
	if (v > 1.0f) v = 1.0f;
	if (v < -1.0f) v = -1.0f;
	const float mag = v < 0.0f ? -v : v;
	if (mag <= deadzone)
		return 0.0f;
	const float scaled = (mag - deadzone) / (1.0f - deadzone);
	return v < 0.0f ? -scaled : scaled;
}

static float clamp1(float v)
{
	return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
}

Actions map(const RawPad &pad, const Config &cfg, State &st, unsigned nowMs)
{
	Actions a;
	const unsigned b = pad.buttons;
	const unsigned pressed = b & ~st.prevButtons; // rising edges
	auto down = [&](unsigned m) { return (b & m) != 0; };
	auto edge = [&](unsigned m) { return (pressed & m) != 0; };

	const float nubX = axis(pad.lx, cfg.deadzone);
	const float nubY = -axis(pad.ly, cfg.deadzone); // nub up = negative raw

	if (cfg.layout == Layout::LookButtons)
	{
		a.moveX = nubX;
		a.moveZ = nubY;
		a.lookYaw   = (down(BTN_CIRCLE) ? 1.f : 0.f) - (down(BTN_SQUARE) ? 1.f : 0.f);
		a.lookPitch = (down(BTN_TRIANGLE) ? 1.f : 0.f) - (down(BTN_CROSS) ? 1.f : 0.f);
		a.jump = down(BTN_UP);
		const bool jumpEdge = edge(BTN_UP);
		if (edge(BTN_DOWN)) st.sneaking = !st.sneaking;
		if (edge(BTN_LEFT))  a.hotbarDelta = -1;
		if (edge(BTN_RIGHT)) a.hotbarDelta = +1;
		a.openInventory = edge(BTN_SELECT);
		if (jumpEdge)
		{
			if (st.hasLastJump && nowMs - st.lastJumpPressMs <= cfg.doubleTapMs)
			{
				st.flying = !st.flying;
				a.toggleFly = true;
				st.hasLastJump = false;
			}
			else
			{
				st.lastJumpPressMs = nowMs;
				st.hasLastJump = true;
			}
		}
	}
	else // Layout::LookNub
	{
		a.lookYaw = nubX;
		a.lookPitch = nubY;
		a.moveX = (down(BTN_RIGHT) ? 1.f : 0.f) - (down(BTN_LEFT) ? 1.f : 0.f);
		a.moveZ = (down(BTN_UP) ? 1.f : 0.f) - (down(BTN_DOWN) ? 1.f : 0.f);
		a.jump = down(BTN_CROSS);
		if (edge(BTN_CIRCLE)) st.sneaking = !st.sneaking;
		a.openInventory = edge(BTN_TRIANGLE);
		// Hotbar moves to the face-button leftovers: square = next slot.
		if (edge(BTN_SQUARE)) a.hotbarDelta = +1;
		if (edge(BTN_SELECT)) a.hotbarDelta = -1;
		if (edge(BTN_CROSS))
		{
			if (st.hasLastJump && nowMs - st.lastJumpPressMs <= cfg.doubleTapMs)
			{
				st.flying = !st.flying;
				a.toggleFly = true;
				st.hasLastJump = false;
			}
			else
			{
				st.lastJumpPressMs = nowMs;
				st.hasLastJump = true;
			}
		}
	}

	// Shared in both layouts.
	a.breakBlock = down(BTN_R) && !down(BTN_L);
	a.placeOrUse = down(BTN_L) && !down(BTN_R);
	if (down(BTN_L) && down(BTN_R) && (edge(BTN_L) || edge(BTN_R)))
		st.sprinting = !st.sprinting; // L+R chord toggles sprint (neither break nor place)
	a.pause = edge(BTN_START);

	a.sneak = st.sneaking;
	a.sprint = st.sprinting && (a.moveZ > 0.0f);

	a.lookYaw = clamp1(a.lookYaw * cfg.lookSpeed);
	a.lookPitch = clamp1(a.lookPitch * cfg.lookSpeed);
	st.prevButtons = b;
	return a;
}

}
