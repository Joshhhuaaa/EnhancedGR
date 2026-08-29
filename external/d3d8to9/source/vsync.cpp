#include "vsync.hpp"
#include "ini.hpp"

#include <fstream>

#ifndef D3D8TO9NOLOG
extern std::ofstream LOG;
#define VSYNC_LOG(Message) do { LOG << "> VSYNC: " << Message << std::endl; } while (false)
#else
#define VSYNC_LOG(Message) do { } while (false)
#endif

namespace
{
	DWORD Value = D3DPRESENT_INTERVAL_ONE;
	bool  Configured = false;
}

// D3D8 could only ever wait for vblank on a fullscreen device, so the conversion pins a windowed
// one to IMMEDIATE and the game asks for nothing but COPY_VSYNC either way. Both are replaced,
// which is the only way the setting can survive a borderless device
DWORD Vsync::Interval()
{
	if (!Configured)
	{
		Configured = true;

		if (Ini::ReadInt(L"Graphics", L"VSync", 1) == 0)
			Value = D3DPRESENT_INTERVAL_IMMEDIATE;

		VSYNC_LOG((Value == D3DPRESENT_INTERVAL_ONE ? "on" : "off"));
	}

	return Value;
}
