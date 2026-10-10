// Display_psp.cpp — lwjgl::Display on the PSP: a fixed 480x272 screen driven by PspGu.
#ifdef PSP_PLATFORM

#include "lwjgl/Display.h"

#include "client/Minecraft.h"
#include "net/minecraft/src/GuiScreen.h"
#include "psp/input/PspInput.h"
#include "psp/render/PspGuDevice.h"
#include "psp/system/PspSystem.h"

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

void swapBuffers() { PspGu::present(); }

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
