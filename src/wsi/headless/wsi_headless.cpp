#if defined(DXVK_WSI_HEADLESS)

#include "wsi_headless.h"

#include "wsi_platform.h"
#include "../util/log/log.h"

#include <cstring>
#include <mutex>
#include <unordered_map>

namespace {
  constexpr uint32_t kMonitorWidth  = 1024;
  constexpr uint32_t kMonitorHeight = 768;
  constexpr uint32_t kMonitorHz     = 60;
  constexpr uint32_t kMonitorBpp    = 32;
  constexpr uint32_t kDefaultWidth  = 640;
  constexpr uint32_t kDefaultHeight = 480;

  struct Size { uint32_t w, h; };
  std::mutex g_lock;
  std::unordered_map<void*, Size> g_windows;

  Size lookup(void* hwnd) {
    std::lock_guard<std::mutex> g(g_lock);
    auto it = g_windows.find(hwnd);
    if (it == g_windows.end() || !it->second.w || !it->second.h)
      return Size{kDefaultWidth, kDefaultHeight};
    return it->second;
  }
}

extern "C" void vkwgpu_wsi_register_window(void* hwnd, uint32_t width, uint32_t height) {
  std::lock_guard<std::mutex> g(g_lock);
  g_windows[hwnd] = Size{width, height};
}

extern "C" void vkwgpu_wsi_unregister_window(void* hwnd) {
  std::lock_guard<std::mutex> g(g_lock);
  g_windows.erase(hwnd);
}

namespace dxvk::wsi {

  // The single monitor is HMONITOR 1; NULL stays invalid.
  static const HMONITOR kMonitor = reinterpret_cast<HMONITOR>(intptr_t(1));

  static void fillMode(WsiMode* pMode) {
    pMode->width = kMonitorWidth;
    pMode->height = kMonitorHeight;
    pMode->refreshRate = WsiRational{kMonitorHz * 1000, 1000};
    pMode->bitsPerPixel = kMonitorBpp;
    pMode->interlaced = false;
  }

  class HeadlessWsiDriver : public WsiDriver {
  public:
    std::vector<const char*> getInstanceExtensions() override {
      return { VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME };
    }

    HMONITOR getDefaultMonitor() override { return kMonitor; }

    HMONITOR enumMonitors(uint32_t index) override {
      return index == 0 ? kMonitor : nullptr;
    }

    HMONITOR enumMonitors(const LUID*[], uint32_t, uint32_t index) override {
      return enumMonitors(index);
    }

    bool getDisplayName(HMONITOR hMonitor, WCHAR (&Name)[32]) override {
      if (hMonitor != kMonitor)
        return false;
      const wchar_t name[] = L"\\\\.\\DISPLAY1";
      std::memset(Name, 0, sizeof(Name));
      std::memcpy(Name, name, sizeof(name));
      return true;
    }

    bool getDesktopCoordinates(HMONITOR hMonitor, RECT* pRect) override {
      if (hMonitor != kMonitor)
        return false;
      pRect->left = 0;
      pRect->top = 0;
      pRect->right = LONG(kMonitorWidth);
      pRect->bottom = LONG(kMonitorHeight);
      return true;
    }

    bool getDisplayMode(HMONITOR hMonitor, uint32_t modeNumber, WsiMode* pMode) override {
      if (hMonitor != kMonitor || modeNumber != 0)
        return false;
      fillMode(pMode);
      return true;
    }

    bool getCurrentDisplayMode(HMONITOR hMonitor, WsiMode* pMode) override {
      return getDisplayMode(hMonitor, 0, pMode);
    }

    bool getDesktopDisplayMode(HMONITOR hMonitor, WsiMode* pMode) override {
      return getDisplayMode(hMonitor, 0, pMode);
    }

    WsiEdidData getMonitorEdid(HMONITOR) override { return {}; }

    void getWindowSize(HWND hWindow, uint32_t* pWidth, uint32_t* pHeight) override {
      Size s = lookup(hWindow);
      if (pWidth)  *pWidth = s.w;
      if (pHeight) *pHeight = s.h;
    }

    void resizeWindow(HWND hWindow, DxvkWindowState*, uint32_t width, uint32_t height) override {
      vkwgpu_wsi_register_window(hWindow, width, height);
    }

    bool setWindowMode(HMONITOR hMonitor, HWND hWindow, DxvkWindowState*, const WsiMode& mode) override {
      if (hMonitor != kMonitor)
        return false;
      vkwgpu_wsi_register_window(hWindow, mode.width, mode.height);
      return true;
    }

    bool enterFullscreenMode(HMONITOR hMonitor, HWND hWindow, DxvkWindowState*, bool) override {
      if (hMonitor != kMonitor)
        return false;
      vkwgpu_wsi_register_window(hWindow, kMonitorWidth, kMonitorHeight);
      return true;
    }

    bool leaveFullscreenMode(HWND, DxvkWindowState*, bool) override { return true; }
    bool restoreDisplayMode() override { return true; }
    HMONITOR getWindowMonitor(HWND) override { return kMonitor; }
    bool isWindow(HWND hWindow) override { return hWindow != nullptr; }
    bool isMinimized(HWND) override { return false; }
    bool isOccluded(HWND) override { return false; }
    void updateFullscreenWindow(HMONITOR, HWND, bool) override {}

    VkResult createSurface(HWND, PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkSurfaceKHR* pSurface) override {
      auto create = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
        gipa(instance, "vkCreateHeadlessSurfaceEXT"));
      if (!create) {
        Logger::err("Headless WSI: vkCreateHeadlessSurfaceEXT not available");
        return VK_ERROR_EXTENSION_NOT_PRESENT;
      }
      VkHeadlessSurfaceCreateInfoEXT info = { VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT };
      return create(instance, &info, nullptr, pSurface);
    }
  };

  static bool createHeadlessWsiDriver(WsiDriver** driver) {
    *driver = new HeadlessWsiDriver();
    return true;
  }

  WsiBootstrap HeadlessWSI = {
    "Headless",
    createHeadlessWsiDriver
  };

}

#endif
