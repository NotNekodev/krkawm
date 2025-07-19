#pragma once

#include <functional>
#include <memory>
#include <vector>
extern "C" {
#include <Imlib2.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
}
#include <core/log.hpp>
#include <util/checks.hpp>

class KrkaWM;

struct MonitorInfo {
    int x, y;
    int width, height;
    int mm_width, mm_height;
    std::string name;
    RROutput output;
    RRCrtc crtc;
    bool is_primary;
    bool is_connected;
    float scale_factor;

    MonitorInfo()
        : x(0), y(0), width(0), height(0), mm_width(0), mm_height(0),
          output(None), crtc(None), is_primary(false), is_connected(false),
          scale_factor(1.0f) {
    }
};

struct DisplayConfiguration {
    std::vector<MonitorInfo> monitors;
    int total_width;
    int total_height;
    int primary_monitor_index;

    DisplayConfiguration()
        : total_width(0), total_height(0), primary_monitor_index(0) {
    }
};

class DisplayManager {
public:
    using ConfigChangeCallback =
        std::function<void(const DisplayConfiguration &)>;

    static std::unique_ptr<DisplayManager> Create(KrkaWM *wm, Display *display);
    ~DisplayManager();

    bool Initialize();
    void Refresh();
    void HandleRandrEvent(const XRRScreenChangeNotifyEvent &event);

    const DisplayConfiguration &GetConfiguration() const {
        return config_;
    }
    MonitorInfo GetMonitorAt(int x, int y) const;
    MonitorInfo GetMonitorContaining(int x, int y, int width, int height) const;
    MonitorInfo GetPrimaryMonitor() const;
    std::vector<MonitorInfo> GetMonitorsIntersecting(int x, int y, int width,
                                                     int height) const;

    void ConstrainToMonitor(int &x, int &y, int width, int height,
                            const MonitorInfo &monitor) const;
    void ConstrainToAnyMonitor(int &x, int &y, int width, int height) const;
    bool IsPositionValid(int x, int y) const;

    void SetWallpaper(const std::string &path);
    void SetWallpaperColor(unsigned long color);
    void RefreshWallpaper();

    void RegisterConfigChangeCallback(ConfigChangeCallback callback);
    int GetRandrEventBase() const {
        return randr_event_base_;
    }

    bool IsRandrAvailable() const {
        return randr_available_;
    }
    int GetRandrMajorVersion() const {
        return randr_major_version_;
    }
    int GetRandrMinorVersion() const {
        return randr_minor_version_;
    }

private:
    explicit DisplayManager(KrkaWM *wm, Display *display);

    bool InitializeRandr();
    void QueryMonitors();
    void UpdateConfiguration();

    void DetectRandrMonitors();
    void DetectFallbackMonitor();

    void SetupWallpaperAcrossMonitors(Imlib_Image image);
    void SetupSolidColorBackground(unsigned long color);

    void LogMonitorConfiguration() const;
    void CalculateTotalDisplaySize();
    void FindPrimaryMonitor();

    Display *display_;
    Window root_;
    DisplayConfiguration config_;
    KrkaWM *wm_;

    bool randr_available_;
    int randr_event_base_;
    int randr_error_base_;
    int randr_major_version_;
    int randr_minor_version_;

    std::string current_wallpaper_path_;
    unsigned long current_bg_color_;
    bool has_wallpaper_;

    std::vector<ConfigChangeCallback> config_callbacks_;
};
