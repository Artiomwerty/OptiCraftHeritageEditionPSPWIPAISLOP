// Diagnostic: logs the return address (and type) of the first exceptions thrown, so a crash like
// "Invalid argument" can be mapped to a function with psp-addr2line. Linked via --wrap=__cxa_throw.
#ifdef PSP_PLATFORM
#include <typeinfo>
#include <cstdio>
#include "psp/system/PspSystem.h"

extern "C" void __real___cxa_throw(void *obj, void *tinfo, void (*dest)(void *));

extern "C" void __wrap___cxa_throw(void *obj, void *tinfo, void (*dest)(void *))
{
	static int count = 0;
	if (count < 40)
	{
		++count;
		const std::type_info *ti = static_cast<const std::type_info *>(tinfo);
		PspSystem::bootTrace("throw #%d ra=%p type=%s", count, __builtin_return_address(0), ti ? ti->name() : "?");
		// Return addresses of the callers: scan the stack for words that point into the code.
		const unsigned *sp = static_cast<const unsigned *>(__builtin_frame_address(0));
		char buf[300];
		int n = 0, found = 0;
		for (int i = 0; i < 400 && found < 16; ++i)
		{
			const unsigned v = sp[i];
			if (v >= 0x08804000u && v < 0x09200000u && (v & 3) == 0)
				n += snprintf(buf + n, sizeof(buf) - n, " %x", v), ++found;
			if (n > (int)sizeof(buf) - 12) break;
		}
		PspSystem::bootTrace("  stack:%s", buf);
	}
	__real___cxa_throw(obj, tinfo, dest);
}
#endif
