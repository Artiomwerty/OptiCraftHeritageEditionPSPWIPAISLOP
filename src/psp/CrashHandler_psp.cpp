// CrashHandler_psp.cpp — shows the crash text on screen (never on the Memory Stick) and waits
// for a button, so it can be photographed and sent.
#ifdef PSP_PLATFORM

#include "pc/CrashHandler.h"
#include "psp/system/PspSystem.h"

#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspkernel.h>

namespace CrashHandler
{
void Crash(const std::string &message, const std::string &stackTrace)
{
	pspDebugScreenInit();
	pspDebugScreenSetBackColor(0x00000080);
	pspDebugScreenClear();
	pspDebugScreenPrintf("OptiCraft has crashed\n\n%s\n\n%s\n\nPress any button to exit.\n",
	                     message.c_str(), stackTrace.c_str());
	sceCtrlSetSamplingCycle(0);
	sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
	SceCtrlData pad;
	sceKernelDelayThread(500000);
	for (;;)
	{
		sceCtrlReadBufferPositive(&pad, 1);
		if (pad.Buttons)
			break;
		sceKernelDelayThread(16000);
	}
	PspSystem::exitGame();
}
}
#endif
