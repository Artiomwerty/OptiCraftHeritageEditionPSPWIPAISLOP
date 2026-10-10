// Display_psp.cpp — lwjgl::Display on the PSP: a fixed 480x272 screen driven by PspGu.
#ifdef PSP_PLATFORM

#include "lwjgl/Display.h"

#include <pspgu.h>
#include "client/Minecraft.h"
#include "net/minecraft/src/GuiScreen.h"
#include "psp/input/PspInput.h"
#include "psp/render/PspGuDevice.h"
#include "psp/system/PspSystem.h"

void pspRenderInvalidateState();

namespace lwjgl
{
namespace Display
{

void create()
{
	PspSystem::bootTrace("Display::create\n");
	PspGu::init();
	PspInput::init();
	PspSystem::bootTrace("Display::create done\n");
}

void setDisplayMode(const DisplayMode &) {}
DisplayMode getDisplayMode() { return DisplayMode(PspGu::kScreenWidth, PspGu::kScreenHeight); }
void setTitle(const jstring &) {}
void setFullscreen(bool) {}

bool isCloseRequested() { return PspInput::exitRequested(); }
bool isVisible() { return true; }
bool isActive() { return true; }

void processMessages()
{
	Minecraft *mc = Minecraft::getMinecraft();
	const bool inMenu = (mc != nullptr && mc->currentScreen != nullptr);

	// Re-grab the camera whenever no menu is open and a world exists (see Display_wii.cpp for why
	// the world check matters).
	if (!inMenu && mc != nullptr && mc->theWorld != nullptr && !mc->inGameHasFocus)
		mc->setIngameFocus();

	PspInput::poll(inMenu);
}

// Debug heartbeat: a small square in the top-left corner, drawn straight with sceGu in screen
// space. It changes colour every 15 frames, so a black screen can be told apart from a hang:
// if it blinks, the display and GE work and the problem is in what the game draws.
static void heartbeat()
{
	struct V { unsigned color; short x, y, z; };
	static unsigned frame = 0;
	++frame;
	if (frame > 1800) return; // first minute only
	V *v = static_cast<V *>(sceGuGetMemory(2 * sizeof(V)));
	const unsigned color = ((frame / 15) & 1) ? 0xFF00FF00u : 0xFF0000FFu; // green / red (ABGR)
	v[0] = {color, 4, 4, 0};
	v[1] = {color, 28, 28, 0};
	sceGuDisable(GU_TEXTURE_2D);
	sceGuDisable(GU_BLEND);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuDisable(GU_SCISSOR_TEST);
	sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, nullptr, v);
	sceGuEnable(GU_SCISSOR_TEST);
	pspRenderInvalidateState();
}

// Always-visible menu cursor (white cross with a dark outline), independent of the game's own.
static void menuCursor()
{
	int cx = 0, cy = 0;
	if (!PspInput::menuCursor(cx, cy)) return;
	struct V { unsigned color; short x, y, z; };
	V *v = static_cast<V *>(sceGuGetMemory(8 * sizeof(V)));
	const short x = static_cast<short>(cx), y = static_cast<short>(cy);
	const unsigned dark = 0xFF000000u, light = 0xFFFFFFFFu;
	// horizontal bar, vertical bar: dark (bigger) then light (smaller)
	v[0] = {dark, short(x - 7), short(y - 2), 0}; v[1] = {dark, short(x + 8), short(y + 3), 0};
	v[2] = {dark, short(x - 2), short(y - 7), 0}; v[3] = {dark, short(x + 3), short(y + 8), 0};
	v[4] = {light, short(x - 6), short(y - 1), 0}; v[5] = {light, short(x + 7), short(y + 2), 0};
	v[6] = {light, short(x - 1), short(y - 6), 0}; v[7] = {light, short(x + 2), short(y + 7), 0};
	sceGuDisable(GU_TEXTURE_2D);
	sceGuDisable(GU_BLEND);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuDisable(GU_SCISSOR_TEST);
	sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 8, nullptr, v);
	sceGuEnable(GU_SCISSOR_TEST);
	pspRenderInvalidateState();
}

void swapBuffers() { heartbeat(); menuCursor(); PspGu::present(); }

void update(bool doProcessMessages)
{
	swapBuffers();
	if (doProcessMessages)
		processMessages();
}

int_t getX() { return 0; }
int_t getY() { return 0; }
int_t getWidth() { return PspGu::kScreenWidth; }
int_t getHeight() { return PspGu::kScreenHeight; }

} // namespace Display
} // namespace lwjgl
#endif
