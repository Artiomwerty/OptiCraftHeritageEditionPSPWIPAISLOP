// main_psp.cpp — PSP entry point for the full game.
#ifdef PSP_PLATFORM

#include <pspkernel.h>
#include <cerrno>
#include <cstdio>
#include <pspiofilemgr.h>
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
	PspSystem::bootTrace("build: test67 %s %s\n", __DATE__, __TIME__);
	const int clockRc = scePowerSetClockFrequency(333, 333, 166);
	PspSystem::bootTrace("clock: set rc=%d now cpu=%d bus=%d\n", clockRc, scePowerGetCpuClockFrequency(), scePowerGetBusClockFrequency());
	PspSystem::bootTrace("main: argv0=%s appDir=%s\n", (argc > 0 && argv[0]) ? argv[0] : "(null)", PspSystem::appDir().c_str());

	lwjgl::GLContext::instantiate();
	Tessellator::convertQuadsToTriangles = true;

	{
		// Raw probe of assets.pak so a missing / short / corrupt file is obvious in boot.log.
		const std::string pak = PspSystem::appDir() + "/assets.pak";
		SceUID fd = sceIoOpen(pak.c_str(), PSP_O_RDONLY, 0);
		if (fd < 0)
			PspSystem::bootTrace("pak probe: open('%s') failed 0x%08x\n", pak.c_str(), (unsigned)fd);
		else
		{
			char magic[8] = {0};
			sceIoRead(fd, magic, 4);
			const int size = sceIoLseek32(fd, 0, PSP_SEEK_END);
			sceIoClose(fd);
			PspSystem::bootTrace("pak probe: open ok, size=%d magic=%02x%02x%02x%02x ('%.4s')\n", size,
			                     (unsigned char)magic[0], (unsigned char)magic[1], (unsigned char)magic[2], (unsigned char)magic[3], magic);
		}
	}
	{
		// stdio probe, mirrors what PakArchive does.
		const std::string pak = PspSystem::appDir() + "/assets.pak";
		FILE *f = fopen(pak.c_str(), "rb");
		if (!f)
			PspSystem::bootTrace("stdio probe: fopen failed errno=%d\n", errno);
		else
		{
			unsigned char h[32] = {0};
			const size_t n = fread(h, 1, 32, f);
			PspSystem::bootTrace("stdio probe: fopen ok, read %u header bytes, magic=%.4s\n", (unsigned)n, (const char *)h);
			const int sk = fseek(f, 11360, SEEK_SET);
			unsigned char t[16] = {0};
			const size_t n2 = fread(t, 1, 16, f);
			PspSystem::bootTrace("stdio probe: fseek=%d read=%u\n", sk, (unsigned)n2);
			fclose(f);
		}
	}
	PspSystem::bootTrace("main: assets.pak mounted=%d path=%s\n", AssetPak::mountFrom(PspSystem::appDir()) ? 1 : 0, AssetPak::archivePath().c_str());
	PspSystem::bootTrace("main: before Minecraft::start\n");
	jstring username = "Player";
	jstring auth = "-";
	Minecraft::start(&username, &auth);

	PspSystem::exitGame();
	return 0;
}
#endif
