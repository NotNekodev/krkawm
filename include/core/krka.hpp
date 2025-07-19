#pragma once

#include <memory>
#include <unordered_map>
extern "C" {
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
}
#include <core/displays.hpp>
#include <core/log.hpp>
#include <util/checks.hpp>

#define MASTER_KEY        Mod5Mask // Super key
#define WINDOW_CLOSE_KEY  XK_q
#define WINDOW_CYCLE_KEY  XK_Tab
#define WM_TERMINATE_KEY  XK_Escape
#define TERMINAL_OPEN_KEY XK_t

#define BG_COLOR              0x40d190
#define BORDER_COLOR_INACTIVE 0x000000
#define BORDER_COLOR_ACTIVE   0xff0000
#define BORDER_WIDTH          2
#define WALLPAPER_PATH_JPG    "./wallpaper.jpg"

class KrkaWM {
public:
    static ::std::unique_ptr<KrkaWM> Create();
    ~KrkaWM();
    void Run();

    static Logger logger_;

private:
    KrkaWM(Display *display);

    // core x11 members
    Display *display_;
    const Window root_;
    Window focused_window_ = None;
    static std::unordered_map<Window, Window> clients_;
    static bool wm_detected_;
    bool InitializeDisplay();

    // display management
    std::unique_ptr<DisplayManager> display_manager_;

    // window interaction state stuff
    bool dragging_      = false;
    bool resizing_      = false;
    Window drag_window_ = None;
    Vec2 drag_start_pos_;
    Vec2 frame_start_pos_;
    Vec2 frame_start_size_;

    // atom (bombs :3)
    Atom ATOM_WM_PROTOCOLS;
    Atom ATOM_WM_DELETE_WINDOW;

    // xrandr support
    int randr_event_base_;
    int randr_error_base_;

    // events
    void OnCreateNotify(const XCreateWindowEvent &e);
    void OnConfigureRequest(const XConfigureRequestEvent &e);
    void OnMapRequest(const XMapRequestEvent &e);
    void OnReparentNotify(const XReparentEvent &e);
    void OnMapNotify(const XMapEvent &e);
    void OnConfigureNotify(const XConfigureEvent &e);
    void OnUnmapNotify(const XUnmapEvent &e);
    void OnDestroyNotify(const XDestroyWindowEvent &e);
    void OnFocusIn(const XFocusChangeEvent &e);
    void OnButtonPress(const XButtonEvent &e);
    void OnButtonRelease(const XButtonEvent &e);
    void OnMotionNotify(const XMotionEvent &e);
    void OnClientMessage(const XClientMessageEvent &e);
    void OnRandrNotify(const XRRScreenChangeNotifyEvent &e);
    void OnKeyPress(const XKeyEvent &e);

    // windowing
    void Frame(Window w, bool was_created_before_window_manager);
    void Unframe(Window w);
    void UpdateWindowBorders(Window new_focus);

    // multi-monitor window handling
    void HandleCrossMonitorDrag(Window window, int new_x, int new_y);
    void SmartPositionWindow(Window frame, int preferred_x, int preferred_y,
                             int width, int height);
    void EnsureWindowVisible(Window frame);

    // input
    void GrabWindowInput(Window frame);
    void GrabGlobalInput();

    void OnDisplayConfigurationChange(const DisplayConfiguration &config);

    // error handlers
    static int OnXError(Display *display, XErrorEvent *event);
    static int OnWMDetected(Display *display, XErrorEvent *event);
};
