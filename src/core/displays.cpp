#include <core/displays.hpp>
#include <core/krka.hpp>

#include <algorithm>
#include <cstring>
#include <iostream>

std::unique_ptr<DisplayManager> DisplayManager::Create(KrkaWM *wm,
                                                       Display *display) {
    if (!display) {
        wm->logger_.fatal() << "Invalid display pointer" << std::endl;
        return nullptr;
    }

    auto manager =
        std::unique_ptr<DisplayManager>(new DisplayManager(wm, display));
    if (!manager->Initialize()) {
        wm->logger_.fatal()
            << "Failed to initialize display manager" << std::endl;
        return nullptr;
    }

    return manager;
}

DisplayManager::DisplayManager(KrkaWM *wm, Display *display)
    : display_(display), root_(DefaultRootWindow(display)),
      randr_available_(false), randr_event_base_(0), randr_error_base_(0),
      randr_major_version_(0), randr_minor_version_(0),
      current_bg_color_(0x40d190), has_wallpaper_(false) {
    wm_ = wm;
}

DisplayManager::~DisplayManager() {
}

bool DisplayManager::Initialize() {
    wm_->logger_.info() << "Initializing display manager..." << std::endl;

    imlib_context_set_display(display_);
    imlib_context_set_drawable(root_);

    if (!InitializeRandr()) {
        wm_->logger_.warn()
            << "Xrandr not available, falling back to single monitor"
            << std::endl;
    }

    QueryMonitors();
    UpdateConfiguration();
    LogMonitorConfiguration();

    return true;
}

bool DisplayManager::InitializeRandr() {
    if (!XRRQueryExtension(display_, &randr_event_base_, &randr_error_base_)) {
        wm_->logger_.warn() << "Xrandr extension not available" << std::endl;
        return false;
    }

    if (!XRRQueryVersion(display_, &randr_major_version_,
                         &randr_minor_version_)) {
        wm_->logger_.warn() << "Failed to query Xrandr version" << std::endl;
        return false;
    }

    wm_->logger_.info() << "Xrandr version: " << randr_major_version_ << "."
                        << randr_minor_version_ << std::endl;

    if (randr_major_version_ < 1 ||
        (randr_major_version_ == 1 && randr_minor_version_ < 2)) {
        wm_->logger_.warn()
            << "Xrandr version too old (need >= 1.2)" << std::endl;
        return false;
    }

    XRRSelectInput(display_, root_,
                   RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask |
                       RROutputChangeNotifyMask);

    randr_available_ = true;
    return true;
}

void DisplayManager::QueryMonitors() {
    config_.monitors.clear();

    if (randr_available_) {
        DetectRandrMonitors();
    } else {
        DetectFallbackMonitor();
    }
}

void DisplayManager::DetectRandrMonitors() {
    XRRScreenResources *screen_resources =
        XRRGetScreenResources(display_, root_);
    if (!screen_resources) {
        wm_->logger_.err() << "Failed to get screen resources" << std::endl;
        DetectFallbackMonitor();
        return;
    }

    wm_->logger_.debug() << "Found " << screen_resources->noutput << " outputs"
                         << std::endl;

    for (int i = 0; i < screen_resources->noutput; ++i) {
        RROutput output = screen_resources->outputs[i];
        XRROutputInfo *output_info =
            XRRGetOutputInfo(display_, screen_resources, output);

        if (!output_info) {
            wm_->logger_.warn()
                << "Failed to get output info for output " << i << std::endl;
            continue;
        }

        if (output_info->connection != RR_Connected) {
            wm_->logger_.debug() << "Output " << output_info->name
                                 << " is not connected" << std::endl;
            XRRFreeOutputInfo(output_info);
            continue;
        }

        if (output_info->crtc != None) {
            XRRCrtcInfo *crtc_info =
                XRRGetCrtcInfo(display_, screen_resources, output_info->crtc);
            if (crtc_info) {
                MonitorInfo monitor;
                monitor.x            = crtc_info->x;
                monitor.y            = crtc_info->y;
                monitor.width        = crtc_info->width;
                monitor.height       = crtc_info->height;
                monitor.mm_width     = output_info->mm_width;
                monitor.mm_height    = output_info->mm_height;
                monitor.name         = std::string(output_info->name);
                monitor.output       = output;
                monitor.crtc         = output_info->crtc;
                monitor.is_connected = true;

                if (monitor.mm_width > 0 && monitor.mm_height > 0) {
                    double dpi_x = (monitor.width * 25.4) / monitor.mm_width;
                    double dpi_y = (monitor.height * 25.4) / monitor.mm_height;
                    double avg_dpi       = (dpi_x + dpi_y) / 2.0;
                    monitor.scale_factor = static_cast<float>(avg_dpi / 96.0);
                } else {
                    monitor.scale_factor = 1.0f;
                }

                config_.monitors.push_back(monitor);

                wm_->logger_.info()
                    << "Monitor: " << monitor.name << " (" << monitor.width
                    << "x" << monitor.height << " at " << monitor.x << ","
                    << monitor.y << ", scale: " << monitor.scale_factor << ")"
                    << std::endl;

                XRRFreeCrtcInfo(crtc_info);
            }
        }

        XRRFreeOutputInfo(output_info);
    }

    RROutput primary = XRRGetOutputPrimary(display_, root_);
    if (primary != None) {
        for (size_t i = 0; i < config_.monitors.size(); ++i) {
            if (config_.monitors[i].output == primary) {
                config_.monitors[i].is_primary = true;
                config_.primary_monitor_index  = static_cast<int>(i);
                wm_->logger_.info()
                    << "Primary monitor: " << config_.monitors[i].name
                    << std::endl;
                break;
            }
        }
    }

    XRRFreeScreenResources(screen_resources);

    if (config_.monitors.empty()) {
        wm_->logger_.warn()
            << "No connected monitors found via Xrandr, using fallback"
            << std::endl;
        DetectFallbackMonitor();
    }
}

void DisplayManager::DetectFallbackMonitor() {
    MonitorInfo monitor;
    monitor.x            = 0;
    monitor.y            = 0;
    monitor.width        = DisplayWidth(display_, DefaultScreen(display_));
    monitor.height       = DisplayHeight(display_, DefaultScreen(display_));
    monitor.mm_width     = DisplayWidthMM(display_, DefaultScreen(display_));
    monitor.mm_height    = DisplayHeightMM(display_, DefaultScreen(display_));
    monitor.name         = "Default";
    monitor.output       = None;
    monitor.crtc         = None;
    monitor.is_primary   = true;
    monitor.is_connected = true;
    monitor.scale_factor = 1.0f;

    config_.monitors.push_back(monitor);
    config_.primary_monitor_index = 0;

    wm_->logger_.info() << "Fallback monitor: " << monitor.width << "x"
                        << monitor.height << std::endl;
}

void DisplayManager::UpdateConfiguration() {
    CalculateTotalDisplaySize();
    FindPrimaryMonitor();

    for (const auto &callback : config_callbacks_) {
        callback(config_);
    }
}

void DisplayManager::CalculateTotalDisplaySize() {
    if (config_.monitors.empty()) {
        config_.total_width  = 0;
        config_.total_height = 0;
        return;
    }

    int min_x = config_.monitors[0].x;
    int min_y = config_.monitors[0].y;
    int max_x = config_.monitors[0].x + config_.monitors[0].width;
    int max_y = config_.monitors[0].y + config_.monitors[0].height;

    for (const auto &monitor : config_.monitors) {
        min_x = std::min(min_x, monitor.x);
        min_y = std::min(min_y, monitor.y);
        max_x = std::max(max_x, monitor.x + monitor.width);
        max_y = std::max(max_y, monitor.y + monitor.height);
    }

    config_.total_width  = max_x - min_x;
    config_.total_height = max_y - min_y;

    wm_->logger_.debug() << "Total display size: " << config_.total_width << "x"
                         << config_.total_height << std::endl;
}

void DisplayManager::FindPrimaryMonitor() {
    for (size_t i = 0; i < config_.monitors.size(); ++i) {
        if (config_.monitors[i].is_primary) {
            config_.primary_monitor_index = static_cast<int>(i);
            return;
        }
    }

    if (!config_.monitors.empty()) {
        config_.monitors[0].is_primary = true;
        config_.primary_monitor_index  = 0;
    }
}

void DisplayManager::Refresh() {
    wm_->logger_.info() << "Refreshing display configuration..." << std::endl;
    QueryMonitors();
    UpdateConfiguration();
    LogMonitorConfiguration();
    RefreshWallpaper();
}

void DisplayManager::HandleRandrEvent(const XRRScreenChangeNotifyEvent &event) {
    (void)event;
    wm_->logger_.info() << "Received RandR screen change event" << std::endl;

    XSync(display_, False);

    Refresh();
}

MonitorInfo DisplayManager::GetMonitorAt(int x, int y) const {
    for (const auto &monitor : config_.monitors) {
        if (x >= monitor.x && x < monitor.x + monitor.width && y >= monitor.y &&
            y < monitor.y + monitor.height) {
            return monitor;
        }
    }

    return GetPrimaryMonitor();
}

MonitorInfo DisplayManager::GetMonitorContaining(int x, int y, int width,
                                                 int height) const {
    int center_x = x + width / 2;
    int center_y = y + height / 2;
    return GetMonitorAt(center_x, center_y);
}

MonitorInfo DisplayManager::GetPrimaryMonitor() const {
    if (config_.primary_monitor_index >= 0 &&
        config_.primary_monitor_index <
            static_cast<int>(config_.monitors.size())) {
        return config_.monitors[config_.primary_monitor_index];
    }

    return config_.monitors.empty() ? MonitorInfo() : config_.monitors[0];
}

std::vector<MonitorInfo>
DisplayManager::GetMonitorsIntersecting(int x, int y, int width,
                                        int height) const {
    std::vector<MonitorInfo> intersecting;

    for (const auto &monitor : config_.monitors) {
        if (!(x >= monitor.x + monitor.width || x + width <= monitor.x ||
              y >= monitor.y + monitor.height || y + height <= monitor.y)) {
            intersecting.push_back(monitor);
        }
    }

    return intersecting;
}

void DisplayManager::ConstrainToMonitor(int &x, int &y, int width, int height,
                                        const MonitorInfo &monitor) const {
    x = std::max(monitor.x, std::min(x, monitor.x + monitor.width - width));
    y = std::max(monitor.y, std::min(y, monitor.y + monitor.height - height));
}

void DisplayManager::ConstrainToAnyMonitor(int &x, int &y, int width,
                                           int height) const {
    MonitorInfo best_monitor = GetMonitorAt(x + width / 2, y + height / 2);
    ConstrainToMonitor(x, y, width, height, best_monitor);
}

bool DisplayManager::IsPositionValid(int x, int y) const {
    for (const auto &monitor : config_.monitors) {
        if (x >= monitor.x && x < monitor.x + monitor.width && y >= monitor.y &&
            y < monitor.y + monitor.height) {
            return true;
        }
    }
    return false;
}

void DisplayManager::SetWallpaper(const std::string &path) {
    current_wallpaper_path_ = path;
    has_wallpaper_          = true;

    Imlib_Image image = imlib_load_image(path.c_str());
    if (!image) {
        wm_->logger_.err() << "Failed to load wallpaper: " << path << std::endl;
        SetWallpaperColor(current_bg_color_);
        return;
    }

    SetupWallpaperAcrossMonitors(image);
    imlib_free_image();
}

void DisplayManager::SetWallpaperColor(unsigned long color) {
    current_bg_color_ = color;
    has_wallpaper_    = false;
    SetupSolidColorBackground(color);
}

void DisplayManager::RefreshWallpaper() {
    if (has_wallpaper_ && !current_wallpaper_path_.empty()) {
        SetWallpaper(current_wallpaper_path_);
    } else {
        SetWallpaperColor(current_bg_color_);
    }
}

void DisplayManager::SetupWallpaperAcrossMonitors(Imlib_Image image) {
    imlib_context_set_image(image);

    int screen        = DefaultScreen(display_);
    Visual *visual    = DefaultVisual(display_, screen);
    Colormap colormap = DefaultColormap(display_, screen);

    imlib_context_set_visual(visual);
    imlib_context_set_colormap(colormap);

    Imlib_Image scaled = imlib_create_cropped_scaled_image(
        0, 0, imlib_image_get_width(), imlib_image_get_height(),
        config_.total_width, config_.total_height);

    if (!scaled) {
        wm_->logger_.err() << "Failed to scale wallpaper image" << std::endl;
        SetupSolidColorBackground(current_bg_color_);
        return;
    }

    imlib_context_set_image(scaled);
    imlib_context_set_drawable(root_);
    imlib_render_image_on_drawable(0, 0);

    Pixmap pixmap =
        XCreatePixmap(display_, root_, config_.total_width,
                      config_.total_height, DefaultDepth(display_, screen));
    imlib_context_set_drawable(pixmap);
    imlib_render_image_on_drawable(0, 0);

    XSetWindowBackgroundPixmap(display_, root_, pixmap);
    XClearWindow(display_, root_);
    XFlush(display_);

    XFreePixmap(display_, pixmap);
    imlib_free_image();

    wm_->logger_.info() << "Wallpaper set across " << config_.monitors.size()
                        << " monitors" << std::endl;
}

void DisplayManager::SetupSolidColorBackground(unsigned long color) {
    XSetWindowBackground(display_, root_, color);
    XClearWindow(display_, root_);
    XFlush(display_);

    wm_->logger_.info() << "Background color set to 0x" << std::hex << color
                        << std::dec << std::endl;
}

void DisplayManager::RegisterConfigChangeCallback(
    ConfigChangeCallback callback) {
    config_callbacks_.push_back(callback);
}

void DisplayManager::LogMonitorConfiguration() const {
    wm_->logger_.info() << "=== Monitor Configuration ===" << std::endl;
    wm_->logger_.info() << "Total monitors: " << config_.monitors.size()
                        << std::endl;
    wm_->logger_.info() << "Total desktop size: " << config_.total_width << "x"
                        << config_.total_height << std::endl;

    for (size_t i = 0; i < config_.monitors.size(); ++i) {
        const auto &monitor = config_.monitors[i];
        wm_->logger_.info()
            << "Monitor " << i << ": " << monitor.name << " (" << monitor.width
            << "x" << monitor.height << " at " << monitor.x << "," << monitor.y
            << ")" << (monitor.is_primary ? " [PRIMARY]" : "")
            << " scale=" << monitor.scale_factor << std::endl;
    }
    wm_->logger_.info() << "=============================" << std::endl;
}
