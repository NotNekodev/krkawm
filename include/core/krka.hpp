#ifndef WM_HPP
#define WM_HPP

#include <unordered_map>
extern "C" {
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
}
#include <core/log.hpp>
#include <memory>
#include <string.h>
#include <util/checks.hpp>

#define MASTER_KEY        Mod5Mask // Super key
#define WINDOW_CLOSE_KEY  XK_q
#define WINDOW_CYCLE_KEY  XK_Tab
#define WM_TERMINATE_KEY  XK_Escape
#define TERMINAL_OPEN_KEY XK_t

class KrkaWM {
public:
    static ::std::unique_ptr<KrkaWM> Create();
    ~KrkaWM();
    void Run();

    static Logger logger_;

private:
    KrkaWM(Display *display);
    Display *display_;
    const Window root_;
    Window focused_window_ = None;

    static std::unordered_map<Window, Window> clients_;
    static bool wm_detected_;

    bool dragging_      = false;
    bool resizing_      = false;
    Window drag_window_ = None;
    Vec2 drag_start_pos_;
    Vec2 frame_start_pos_;
    Vec2 frame_start_size_;

    Atom ATOM_WM_PROTOCOLS;
    Atom ATOM_WM_DELETE_WINDOW;

    void TileClients();

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
    void OnKeyPress(const XKeyEvent &e);
    void Frame(Window w, bool was_created_before_window_manager);
    void Unframe(Window w);
    void UpdateWindowBorders(Window new_focus);
    void GrabWindowInput(Window frame);
    void GrabGlobalInput();

    static int OnXError(Display *display, XErrorEvent *event);
    static int OnWMDetected(Display *display, XErrorEvent *event);
};

#endif // WM_HPP
