// Linux TSan build only: stands in for the few Windows calls in Server/ without touching server code (force-included).
#pragma once
#ifndef _WIN32
#include <cstdint>
#include <cstdio>
#include <ctime>
struct FILETIME { uint32_t dwLowDateTime; uint32_t dwHighDateTime; };
inline void* GetCurrentThread() { return nullptr; }
inline void* GetCurrentProcess() { return nullptr; }
inline bool ShimTimes(clockid_t id, FILETIME* user)
{
	timespec t{};
	clock_gettime(id, &t);
	const uint64_t ticks = (uint64_t)t.tv_sec * 10000000ull + (uint64_t)t.tv_nsec / 100ull;
	user->dwLowDateTime = (uint32_t)ticks;
	user->dwHighDateTime = (uint32_t)(ticks >> 32);
	return true;
}
inline bool GetThreadTimes(void*, FILETIME* c, FILETIME* e, FILETIME* k, FILETIME* u) { *c = *e = *k = FILETIME{}; return ShimTimes(CLOCK_THREAD_CPUTIME_ID, u); }
inline bool GetProcessTimes(void*, FILETIME* c, FILETIME* e, FILETIME* k, FILETIME* u) { *c = *e = *k = FILETIME{}; return ShimTimes(CLOCK_PROCESS_CPUTIME_ID, u); }
inline void timeBeginPeriod(unsigned) {}
inline void timeEndPeriod(unsigned) {}
inline int fopen_s(FILE** f, const char* path, const char* mode) { *f = std::fopen(path, mode); return *f ? 0 : 1; }
#endif
