// PspPthreadWrap.c — linked with -Wl,--wrap=pthread_create. libpthreadglue gives every pthread a
// 4 KB stack unless told otherwise, which std::thread never does; the game's worker threads
// (chunk saves, world generation) need far more.
#ifdef PSP_PLATFORM
#include <pthread.h>

int __real_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg);

int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
	pthread_attr_t at;
	pthread_attr_init(&at);
	if (a)
		at = *a;
	pthread_attr_setstacksize(&at, 256 * 1024);
	return __real_pthread_create(t, &at, fn, arg);
}
#endif
