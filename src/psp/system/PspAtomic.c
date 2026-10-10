// PspAtomic.c — 64-bit __atomic_* libcalls the Allegrex cannot inline (no LL/SC on 64 bits).
// The PSP CPU is single core, so masking interrupts around the access is both correct and cheap.
#ifdef PSP_PLATFORM
#include <pspkernel.h>
#include <pspintrman.h>
#include <stdint.h>
#include <stdbool.h>

uint64_t __atomic_load_8(const volatile void *ptr, int memorder)
{
	(void)memorder;
	int s = sceKernelCpuSuspendIntr();
	uint64_t v = *(const volatile uint64_t *)ptr;
	sceKernelCpuResumeIntr(s);
	return v;
}

bool __atomic_compare_exchange_8(volatile void *ptr, void *expected, uint64_t desired, bool weak, int success, int failure)
{
	(void)weak; (void)success; (void)failure;
	volatile uint64_t *p = (volatile uint64_t *)ptr;
	uint64_t *e = (uint64_t *)expected;
	int s = sceKernelCpuSuspendIntr();
	bool ok = (*p == *e);
	if (ok) *p = desired; else *e = *p;
	sceKernelCpuResumeIntr(s);
	return ok;
}

void __atomic_store_8(volatile void *ptr, uint64_t value, int memorder)
{
	(void)memorder;
	int s = sceKernelCpuSuspendIntr();
	*(volatile uint64_t *)ptr = value;
	sceKernelCpuResumeIntr(s);
}

uint64_t __atomic_fetch_add_8(volatile void *ptr, uint64_t value, int memorder)
{
	(void)memorder;
	int s = sceKernelCpuSuspendIntr();
	uint64_t old = *(volatile uint64_t *)ptr;
	*(volatile uint64_t *)ptr = old + value;
	sceKernelCpuResumeIntr(s);
	return old;
}

uint64_t __atomic_exchange_8(volatile void *ptr, uint64_t value, int memorder)
{
	(void)memorder;
	int s = sceKernelCpuSuspendIntr();
	uint64_t old = *(volatile uint64_t *)ptr;
	*(volatile uint64_t *)ptr = value;
	sceKernelCpuResumeIntr(s);
	return old;
}
#endif
