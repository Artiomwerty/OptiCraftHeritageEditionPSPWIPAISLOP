// Diagnostic: logs the return address (and type) of the first exceptions thrown, so a crash like
// "Invalid argument" can be mapped to a function with psp-addr2line. Linked via --wrap=__cxa_throw.
#ifdef PSP_PLATFORM
#include <typeinfo>
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
	}
	__real___cxa_throw(obj, tinfo, dest);
}
#endif
