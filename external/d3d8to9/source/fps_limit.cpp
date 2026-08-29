#include "fps_limit.hpp"
#include "ini.hpp"

#include <windows.h>
#include <timeapi.h>
#include <fstream>

#pragma comment(lib, "winmm.lib")

#ifndef D3D8TO9NOLOG
extern std::ofstream LOG;
#define FPSLIMIT_LOG(Message) do { LOG << "> FPSLIMIT: " << Message << std::endl; } while (false)
#else
#define FPSLIMIT_LOG(Message) do { } while (false)
#endif

namespace
{
	// Leaves headroom for present jitter without blocking Present or leaving the VRR range
	constexpr int RefreshMargin = 3;

	int      Limit = 0;
	LONGLONG Frequency = 0;
	LONGLONG SpinTicks = 0;
	HANDLE   Timer = nullptr;
	bool     Configured = false;

	int RefreshRate()
	{
		static int Cached = 0;
		static LONGLONG Next = 0;

		// Refreshed once a second to catch a monitor or mode change
		LARGE_INTEGER Now;
		QueryPerformanceCounter(&Now);
		if (Now.QuadPart < Next)
			return Cached;
		Next = Now.QuadPart + Frequency;

		MONITORINFOEXW Monitor = {};
		DEVMODEW Mode = {};
		Monitor.cbSize = sizeof(Monitor);
		Mode.dmSize = sizeof(Mode);

		if (GetMonitorInfoW(MonitorFromWindow(GetActiveWindow(), MONITOR_DEFAULTTOPRIMARY), &Monitor) &&
			EnumDisplaySettingsW(Monitor.szDevice, ENUM_CURRENT_SETTINGS, &Mode) &&
			Mode.dmDisplayFrequency > 1 && static_cast<int>(Mode.dmDisplayFrequency) != Cached)
		{
			Cached = Mode.dmDisplayFrequency;
			FPSLIMIT_LOG("monitor refresh rate is " << Cached << " Hz, capping at " << Cached - RefreshMargin);
		}

		return Cached;
	}

	int Cap()
	{
		if (Limit >= 0)
			return Limit;

		const int Refresh = RefreshRate() - RefreshMargin;

		return Refresh > 0 ? Refresh : 0;
	}

	void SleepUntil(LONGLONG Deadline)
	{
		for (;;)
		{
			LARGE_INTEGER Now;
			QueryPerformanceCounter(&Now);
			const LONGLONG Remaining = Deadline - Now.QuadPart;

			if (Remaining <= 0)
				return;

			if (Remaining <= SpinTicks)
			{
				YieldProcessor();
				continue;
			}

			LARGE_INTEGER Due;
			Due.QuadPart = -((Remaining - SpinTicks) * 10000000 / Frequency);
			if (SetWaitableTimer(Timer, &Due, 0, nullptr, nullptr, FALSE))
				WaitForSingleObject(Timer, INFINITE);
		}
	}
}

void FpsLimit::OnPresent()
{
	if (!Configured)
	{
		Configured = true;
		Limit = Ini::ReadInt(L"General", L"FPSLimit", 0);

		LARGE_INTEGER Counter;
		QueryPerformanceFrequency(&Counter);
		Frequency = Counter.QuadPart;
		SpinTicks = Frequency / 1000;

		// Before Windows 10, version 1803, fall back to a regular timer
		Timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
		if (Timer == nullptr)
		{
			timeBeginPeriod(1);
			Timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
			SpinTicks *= 2;
		}

		FPSLIMIT_LOG("limit " << Limit << ", timer " << Timer);
	}

	const int Frames = Cap();
	if (Frames <= 0 || Timer == nullptr)
		return;

	// The deadline is carried, not recomputed, so a long frame is not paid back by the next
	static LONGLONG Next = 0;
	LARGE_INTEGER Now;
	QueryPerformanceCounter(&Now);

	if (Now.QuadPart < Next)
		SleepUntil(Next);
	else
		Next = Now.QuadPart;

	Next += Frequency / Frames;
}
