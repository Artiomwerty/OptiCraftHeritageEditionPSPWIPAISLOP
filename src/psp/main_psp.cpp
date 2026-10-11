// main_psp.cpp — PSP entry point for the full game.
#ifdef PSP_PLATFORM

#include <pspkernel.h>
#include <psppower.h>
#include <unistd.h>
#include <cstring>

#include "client/Minecraft.h"
#include "java/String.h"
#include "lwjgl/GLContext.h"
#include "net/minecraft/src/Tessellator.h"
#include "psp/system/PspSystem.h"
#include "platform/storage/AssetPak.h"

// Runs before every other global constructor: proves the program got this far and records what libc
// believes the current directory is (empty means path handling would crash).
extern "C" char __cwd[256];
__attribute__((constructor(101))) static void earlyTrace()
{
	if (__cwd[0] == 0)
	{
		strncpy(__cwd, "ms0:/PSP/GAME/OptiCraft", sizeof(__cwd) - 1);
	}
	char cwd[256] = "(getcwd failed)";
	if (!getcwd(cwd, sizeof(cwd)))
		cwd[0] = 0;
	PspSystem::bootTrace("early: static init, cwd='%s'\n", cwd);
}

PSP_MODULE_INFO("OptiCraft", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
// Leave 1 MB for the system and thread stacks; the rest is the game's heap.
PSP_HEAP_SIZE_KB(-1024);

int main(int argc, char **argv)
{
	PspSystem::setAppDirFromArgv0(argc > 0 ? argv[0] : nullptr);
	scePowerSetClockFrequency(333, 333, 166);
	PspSystem::bootTrace("main: argv0=%s appDir=%s\n", (argc > 0 && argv[0]) ? argv[0] : "(null)", PspSystem::appDir().c_str());

	lwjgl::GLContext::instantiate();
	Tessellator::convertQuadsToTriangles = true;

	PspSystem::bootTrace("main: assets.pak mounted=%d path=%s\n", AssetPak::mountFrom(PspSystem::appDir()) ? 1 : 0, AssetPak::archivePath().c_str());
	PspSystem::bootTrace("main: before Minecraft::start\n");
	jstring username = "Player";
	jstring auth = "-";
	Minecraft::start(&username, &auth);

	PspSystem::exitGame();
	return 0;
}
#endif
