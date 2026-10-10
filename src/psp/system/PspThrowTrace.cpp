// Diagnostic: logs the return address (and type) of the first exceptions thrown, so a crash like
// "Invalid argument" can be mapped to a function with psp-addr2line. Linked via --wrap=__cxa_throw.
#ifdef PSP_PLATFORM
#include <typeinfo>
#include <cstdio>
#include <unwind.h>
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
		struct Ctx { char buf[300]; int n; int depth; } ctx = {{0}, 0, 0};
		_Unwind_Backtrace([](struct _Unwind_Context *uc, void *arg) -> _Unwind_Reason_Code {
			Ctx *c = static_cast<Ctx *>(arg);
			if (c->depth >= 14 || c->n > (int)sizeof(c->buf) - 12) return _URC_END_OF_STACK;
			c->n += snprintf(c->buf + c->n, sizeof(c->buf) - c->n, " %x", (unsigned)_Unwind_GetIP(uc));
			++c->depth;
			return _URC_NO_REASON;
		}, &ctx);
		PspSystem::bootTrace("  bt:%s", ctx.buf);
	}
	__real___cxa_throw(obj, tinfo, dest);
}
#endif
