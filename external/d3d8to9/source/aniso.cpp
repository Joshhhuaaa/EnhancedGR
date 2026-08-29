#include "aniso.hpp"
#include "ini.hpp"

#include <fstream>

#ifndef D3D8TO9NOLOG
extern std::ofstream LOG;
#define ANISO_LOG(Message) do { LOG << "> ANISO: " << Message << std::endl; } while (false)
#else
#define ANISO_LOG(Message) do { } while (false)
#endif

namespace
{
	constexpr DWORD Stages = 8;

	DWORD Level = 0;
	DWORD CapsMax = 1;
	DWORD GameMinFilter[Stages] = {};
	DWORD GameMaxAnisotropy[Stages] = {};
	bool  Configured = false;
}

void Aniso::OnDeviceReady(IDirect3DDevice9 *Device)
{
	if (!Configured)
	{
		Configured = true;

		// A Reset cannot change the cap, so query it once
		D3DCAPS9 Caps = {};
		if (SUCCEEDED(Device->GetDeviceCaps(&Caps)) &&
			(Caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC) != 0 && Caps.MaxAnisotropy > 1)
		{
			CapsMax = Caps.MaxAnisotropy;
		}

		Level = static_cast<DWORD>(Ini::ReadInt(L"Graphics", L"AnisotropicFiltering", 16));
		if (Level > CapsMax)
			Level = CapsMax;
		if (Level <= 1)
			Level = 0;

		ANISO_LOG("level " << Level << "x, device cap " << CapsMax << "x");
	}

	// Create and Reset both wipe the sampler state on the proxy, so restart from the D3D8 defaults
	for (DWORD Stage = 0; Stage < Stages; ++Stage)
	{
		GameMinFilter[Stage] = D3DTEXF_POINT;
		GameMaxAnisotropy[Stage] = 1;

		if (Level > 1)
			Device->SetSamplerState(Stage, D3DSAMP_MAXANISOTROPY, Level);
	}
}

DWORD Aniso::OnSetMinFilter(DWORD Stage, DWORD Value)
{
	if (Stage >= Stages)
		return Value;

	GameMinFilter[Stage] = Value;

	// POINT is an intentional look, so only LINEAR is upgraded
	if (Level > 1 && Value == D3DTEXF_LINEAR)
		return D3DTEXF_ANISOTROPIC;

	return Value;
}

DWORD Aniso::OnSetMaxAnisotropy(DWORD Stage, DWORD Value)
{
	if (Stage >= Stages)
		return Value;

	GameMaxAnisotropy[Stage] = Value;

	// Never let the game lower the forced level
	return (Value < Level) ? Level : Value;
}

bool Aniso::OnGetMinFilter(DWORD Stage, DWORD *Value)
{
	// Report what the game last set, so its own redundant-set filtering still works
	if (Level <= 1 || Stage >= Stages || Value == nullptr)
		return false;

	*Value = GameMinFilter[Stage];
	return true;
}

bool Aniso::OnGetMaxAnisotropy(DWORD Stage, DWORD *Value)
{
	if (Level <= 1 || Stage >= Stages || Value == nullptr)
		return false;

	*Value = GameMaxAnisotropy[Stage];
	return true;
}
