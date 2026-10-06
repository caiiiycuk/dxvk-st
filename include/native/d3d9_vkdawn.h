#pragma once

// vkdawn additions to the D3D9 surface. Only <d3d9.h> is needed to use them.
//
// The vkdawn layer never blocks, so a synchronous readback (Lock of a
// render target, GetRenderTargetData, GetFrontBufferData) can only
// complete if the host gives DXVK a way to pump the layer while it
// waits. Without a pump those calls return D3DERR_NOTAVAILABLE /
// D3DERR_WASSTILLDRAWING and keep the surface marked for readback.

#include <d3d9.h>

extern "C" void dxvk_set_wait_pump(void (*fn)(void* user), void* user);

// Asynchronous surface readback: the pixels arrive in cb from a later
// vkdawn_poll() / Present, never inside RequestSurfaceData. data and pitch
// are valid for the duration of the callback only.
MIDL_INTERFACE("5f0d2a1c-9b6e-4c3a-8e71-2d4f6a9c0b11")
ID3D9VkdawnReadback : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE RequestSurfaceData(
          IDirect3DSurface9* pSurface,
          void (*cb)(const void* data, UINT pitch, void* user),
          void* user) = 0;
};

#ifndef _MSC_VER
__CRT_UUID_DECL(ID3D9VkdawnReadback, 0x5f0d2a1c, 0x9b6e, 0x4c3a, 0x8e, 0x71, 0x2d, 0x4f, 0x6a, 0x9c, 0x0b, 0x11);
#endif
