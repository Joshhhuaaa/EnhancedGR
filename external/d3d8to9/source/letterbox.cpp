#include "letterbox.hpp"

// The pillarbox bars keep whatever was last drawn there even though the engine clears the
// whole target every frame, so this is a workaround for now since I'm not sure why it happens.
void Letterbox::OnPresent(IDirect3DDevice9 *Device)
{
	IDirect3DSurface9 *Target = nullptr;
	if (FAILED(Device->GetRenderTarget(0, &Target)))
		return;

	D3DSURFACE_DESC Desc = {};
	Target->GetDesc(&Desc);
	Target->Release();

	D3DVIEWPORT9 Saved = {};
	if (FAILED(Device->GetViewport(&Saved)))
		return;

	// Clear is clipped to the current viewport
	const D3DVIEWPORT9 Full = { 0, 0, Desc.Width, Desc.Height, Saved.MinZ, Saved.MaxZ };

	Device->SetViewport(&Full);
	Device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 0.0f, 0);
	Device->SetViewport(&Saved);
}
