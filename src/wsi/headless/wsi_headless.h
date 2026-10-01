#pragma once

#include <cstdint>

// Headless WSI for DXVK Native: one fake monitor, opaque HWNDs whose size is
// registered by the test harness, surfaces via VK_EXT_headless_surface.
extern "C" {
  void vkdawn_wsi_register_window(void* hwnd, uint32_t width, uint32_t height);
  void vkdawn_wsi_unregister_window(void* hwnd);
}
