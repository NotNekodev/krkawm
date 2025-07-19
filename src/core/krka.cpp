#include <core/krka.hpp>

extern "C" {
#include <Imlib2.h>
#include <X11/X.h>
#include <string.h>
#include <unistd.h>
}
#include <iostream>

using ::std::unique_ptr;

bool KrkaWM::wm_detected_ = false;
std::unordered_map<Window, Window> KrkaWM::clients_;
Logger KrkaWM::logger_("krka.log");

std::string windowToString(Window w) {
    char name[64];
    snprintf(name, sizeof(name), "0x%lx", w);
    return std::string(name);
}

unique_ptr<KrkaWM> KrkaWM::Create() {
    Display *display = XOpenDisplay(nullptr);
    if (!display) {
        logger_.fatal() << "Failed to open X display " << XDisplayName(nullptr)
                        << std::endl;
        return nullptr;
    }
    return unique_ptr<KrkaWM>(new KrkaWM(display));
}

KrkaWM::KrkaWM(Display *display)
    : display_(display), root_(DefaultRootWindow(display_)),
      focused_window_(None), dragging_(false), resizing_(false),
      drag_window_(None) {
    if (!InitializeDisplay()) {
        logger_.fatal() << "Failed to initialize display" << std::endl;
    }
}

bool KrkaWM::InitializeDisplay() {
    display_manager_ = DisplayManager::Create(this, display_);
    if (!display_manager_) {
        logger_.fatal() << "Failed to create display manager" << std::endl;
        return false;
    }

    int randr_event_base, randr_error_base;
    int randr_major, randr_minor;

    if (!XRRQueryExtension(display_, &randr_event_base, &randr_error_base)) {
        logger_.warn() << "Xrandr extension not available" << std::endl;
        return false;
    }

    if (!XRRQueryVersion(display_, &randr_major, &randr_minor)) {
        logger_.warn() << "Failed to query Xrandr version" << std::endl;
        return false;
    }

    logger_.info() << "Xrandr version: " << randr_major << "." << randr_minor
                   << std::endl;

    XRRSelectInput(display_, root_,
                   RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask |
                       RROutputChangeNotifyMask);

    randr_event_base_ = randr_event_base;

    display_manager_->RegisterConfigChangeCallback(
        [this](const DisplayConfiguration &config) {
            OnDisplayConfigurationChange(config);
        });

    display_manager_->SetWallpaper(WALLPAPER_PATH_JPG);

    return true;
}
void KrkaWM::OnDisplayConfigurationChange(const DisplayConfiguration &config) {
    (void)config;
    logger_.info() << "Display configuration changed, updating window positions"
                   << std::endl;

    for (const auto &client_pair : clients_) {
        EnsureWindowVisible(client_pair.second);
    }

    UpdateWindowBorders(focused_window_);
}

KrkaWM::~KrkaWM() {
    Window parent, *children;
    unsigned int nchildren;
    XWindowAttributes x_window_attrs;

    XGrabServer(display_);
    if (!XQueryTree(display_, root_, &parent, &parent, &children, &nchildren)) {
        logger_.err() << "Failed to query tree in destructor" << std::endl;
        for (auto it = clients_.begin(); it != clients_.end();) {
            if (XGetWindowAttributes(display_, it->second, &x_window_attrs)) {
                Unframe(it->first);
                XDestroyWindow(display_, it->second);
            }
            it = clients_.erase(it);
        }
    } else {
        for (unsigned int i = 0; i < nchildren; i++) {
            Window client_window = children[i];
            if (clients_.count(client_window)) {
                if (XGetWindowAttributes(display_, clients_[client_window],
                                         &x_window_attrs)) {
                    Unframe(client_window);
                    XDestroyWindow(display_, clients_[client_window]);
                }
                clients_.erase(client_window);
            } else if (XGetWindowAttributes(display_, client_window,
                                            &x_window_attrs)) {
                XDestroyWindow(display_, client_window);
            }
        }
        XFree(children);
    }
    XUngrabServer(display_);
    XFlush(display_);
}

void KrkaWM::HandleCrossMonitorDrag(Window window, int new_x, int new_y) {
    if (!display_manager_) {
        XMoveWindow(display_, window, new_x, new_y);
        return;
    }

    Window root_return;
    int x, y;
    unsigned int width, height, border, depth;
    if (!XGetGeometry(display_, window, &root_return, &x, &y, &width, &height,
                      &border, &depth)) {
        logger_.warn()
            << "Failed to get geometry for window in HandleCrossMonitorDrag: "
            << windowToString(window) << std::endl;
        return;
    }

    const DisplayConfiguration &config = display_manager_->GetConfiguration();
    int virtual_x = 0, virtual_y = 0;
    int virtual_width = 0, virtual_height = 0;

    for (const auto &monitor : config.monitors) {
        virtual_x      = std::min(virtual_x, monitor.x);
        virtual_y      = std::min(virtual_y, monitor.y);
        virtual_width  = std::max(virtual_width, monitor.x + monitor.width);
        virtual_height = std::max(virtual_height, monitor.y + monitor.height);
    }

    new_x = std::max(virtual_x - static_cast<int>(width) + 50,
                     std::min(new_x, virtual_width - 50));
    new_y = std::max(virtual_y - static_cast<int>(height) + 50,
                     std::min(new_y, virtual_height - 50));

    logger_.debug() << "Cross-monitor drag: window " << windowToString(window)
                    << " to position " << new_x << "," << new_y << std::endl;

    XMoveWindow(display_, window, new_x, new_y);
}

void KrkaWM::SmartPositionWindow(Window frame, int preferred_x, int preferred_y,
                                 int width, int height) {
    if (!display_manager_) {
        XMoveWindow(display_, frame, preferred_x, preferred_y);
        return;
    }

    if (preferred_x == 0 && preferred_y == 0) {
        MonitorInfo primary = display_manager_->GetPrimaryMonitor();
        preferred_x         = primary.x + (primary.width - width) / 2;
        preferred_y         = primary.y + (primary.height - height) / 2;
    }

    const DisplayConfiguration &config = display_manager_->GetConfiguration();
    int virtual_x = 0, virtual_y = 0;
    int virtual_width = 0, virtual_height = 0;

    for (const auto &monitor : config.monitors) {
        virtual_x      = std::min(virtual_x, monitor.x);
        virtual_y      = std::min(virtual_y, monitor.y);
        virtual_width  = std::max(virtual_width, monitor.x + monitor.width);
        virtual_height = std::max(virtual_height, monitor.y + monitor.height);
    }

    if (preferred_x + width < virtual_x || preferred_x > virtual_width ||
        preferred_y + height < virtual_y || preferred_y > virtual_height) {
        MonitorInfo primary = display_manager_->GetPrimaryMonitor();
        preferred_x         = primary.x + 50;
        preferred_y         = primary.y + 50;
    }

    logger_.debug() << "Smart positioning window " << windowToString(frame)
                    << " at " << preferred_x << "," << preferred_y
                    << " (spanning allowed)" << std::endl;

    XMoveWindow(display_, frame, preferred_x, preferred_y);
}

void KrkaWM::EnsureWindowVisible(Window frame) {
    if (!display_manager_) {
        return;
    }

    Window root_return;
    int x, y;
    unsigned int width, height, border, depth;
    if (!XGetGeometry(display_, frame, &root_return, &x, &y, &width, &height,
                      &border, &depth)) {
        logger_.warn()
            << "Failed to get geometry for window in EnsureWindowVisible: "
            << windowToString(frame) << std::endl;
        return;
    }

    const DisplayConfiguration &config = display_manager_->GetConfiguration();

    int virtual_x = 0, virtual_y = 0;
    int virtual_width = 0, virtual_height = 0;

    for (const auto &monitor : config.monitors) {
        virtual_x      = std::min(virtual_x, monitor.x);
        virtual_y      = std::min(virtual_y, monitor.y);
        virtual_width  = std::max(virtual_width, monitor.x + monitor.width);
        virtual_height = std::max(virtual_height, monitor.y + monitor.height);
    }

    bool completely_outside = (x + static_cast<int>(width) < virtual_x) ||
                              (y + static_cast<int>(height) < virtual_y) ||
                              (x > virtual_width) || (y > virtual_height);

    if (completely_outside) {
        logger_.info() << "Window " << windowToString(frame)
                       << " is completely outside virtual screen, repositioning"
                       << std::endl;

        MonitorInfo primary = display_manager_->GetPrimaryMonitor();
        int new_x           = primary.x + 50;
        int new_y           = primary.y + 50;

        XMoveWindow(display_, frame, new_x, new_y);
        logger_.info() << "Moved window to " << new_x << "," << new_y
                       << " on monitor " << primary.name << std::endl;
    }
}

void KrkaWM::UpdateWindowBorders(Window new_focus_client) {
    logger_.debug() << "UpdateWindowBorders called with new_focus_client: "
                    << windowToString(new_focus_client) << std::endl;

    if (new_focus_client != None && clients_.count(new_focus_client) == 0) {
        logger_.warn() << "Invalid new_focus_client: "
                       << windowToString(new_focus_client)
                       << " not found in clients_" << std::endl;
        new_focus_client = None;
    }

    focused_window_ = new_focus_client;

    XGrabServer(display_);

    if (new_focus_client != None) {
        Window frame = clients_[new_focus_client];
        XWindowAttributes attrs;
        if (!XGetWindowAttributes(display_, frame, &attrs)) {
            logger_.warn() << "Invalid frame window for client: "
                           << windowToString(new_focus_client) << std::endl;
            new_focus_client = None;
        } else {
            XRaiseWindow(display_, frame);
            XSetInputFocus(display_, new_focus_client, RevertToPointerRoot,
                           CurrentTime);
        }
    } else {
        XSetInputFocus(display_, root_, RevertToPointerRoot, CurrentTime);
    }

    for (auto it = clients_.begin(); it != clients_.end();) {
        Window client = it->first;
        Window frame  = it->second;
        XWindowAttributes attrs;
        if (!XGetWindowAttributes(display_, frame, &attrs)) {
            logger_.warn() << "Invalid frame window: " << windowToString(frame)
                           << " for client: " << windowToString(client)
                           << ", removing from clients_" << std::endl;
            it = clients_.erase(it);
            continue;
        }
        XSetWindowBorder(display_, frame,
                         client == new_focus_client ? BORDER_COLOR_ACTIVE
                                                    : BORDER_COLOR_INACTIVE);
        ++it;
    }

    XUngrabServer(display_);
    XFlush(display_);

    Atom net_active_window = XInternAtom(display_, "_NET_ACTIVE_WINDOW", False);
    Window active_window   = new_focus_client != None ? new_focus_client : None;
    XChangeProperty(display_, root_, net_active_window, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&active_window, 1);

    logger_.debug() << "UpdateWindowBorders completed, focused_window_: "
                    << windowToString(focused_window_) << std::endl;
}

void KrkaWM::GrabGlobalInput() {
    XGrabKey(display_, XKeysymToKeycode(display_, WM_TERMINATE_KEY), MASTER_KEY,
             root_, False, GrabModeAsync, GrabModeAsync);
    XGrabKey(display_, XKeysymToKeycode(display_, TERMINAL_OPEN_KEY),
             MASTER_KEY, root_, False, GrabModeAsync, GrabModeAsync);

    XSelectInput(display_, root_,
                 SubstructureRedirectMask | SubstructureNotifyMask |
                     KeyPressMask | FocusChangeMask);
}

void KrkaWM::GrabWindowInput(Window frame) {

    XGrabButton(display_, Button1, MASTER_KEY, frame, False,
                ButtonPressMask | ButtonReleaseMask, GrabModeAsync,
                GrabModeAsync, None, None);
    XGrabButton(display_, Button3, MASTER_KEY, frame, False,
                ButtonPressMask | ButtonReleaseMask, GrabModeAsync,
                GrabModeAsync, None, None);
    XGrabKey(display_, XKeysymToKeycode(display_, WINDOW_CLOSE_KEY), MASTER_KEY,
             frame, False, GrabModeAsync, GrabModeAsync);
    XGrabKey(display_, XKeysymToKeycode(display_, WINDOW_CYCLE_KEY), MASTER_KEY,
             frame, False, GrabModeAsync, GrabModeAsync);
}

void KrkaWM::Run() {
    wm_detected_ = false;
    XSetErrorHandler(&KrkaWM::OnWMDetected);

    if (display_manager_ && display_manager_->IsRandrAvailable()) {
        XSelectInput(display_, root_,
                     SubstructureRedirectMask | SubstructureNotifyMask |
                         KeyPressMask | ButtonPressMask | ButtonReleaseMask |
                         PointerMotionMask | FocusChangeMask);
    }

    XSelectInput(display_, root_,
                 SubstructureRedirectMask | SubstructureNotifyMask |
                     KeyPressMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask);
    XSync(display_, False);
    if (wm_detected_) {
        std::cerr << "Another window manager is already running on "
                  << XDisplayString(display_) << std::endl;
        return;
    }

    ATOM_WM_PROTOCOLS     = XInternAtom(display_, "WM_PROTOCOLS", False);
    ATOM_WM_DELETE_WINDOW = XInternAtom(display_, "WM_DELETE_WINDOW", False);

    Atom net_supported     = XInternAtom(display_, "_NET_SUPPORTED", False);
    Atom net_wm_name       = XInternAtom(display_, "_NET_WM_NAME", False);
    Atom net_active_window = XInternAtom(display_, "_NET_ACTIVE_WINDOW", False);

    Atom supported_atoms[] = {net_supported, net_wm_name, net_active_window};
    XChangeProperty(display_, root_, net_supported, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)supported_atoms,
                    sizeof(supported_atoms) / sizeof(Atom));

    net_active_window = XInternAtom(display_, "_NET_ACTIVE_WINDOW", False);
    Window none       = None;
    XChangeProperty(display_, root_, net_active_window, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&none, 1);

    XSetErrorHandler(&KrkaWM::OnXError);
    GrabGlobalInput();

    XGrabServer(display_);
    Window returned_root, returned_parent;
    Window *top_level_windows;
    unsigned int num_top_level_windows;
    XQueryTree(display_, root_, &returned_root, &returned_parent,
               &top_level_windows, &num_top_level_windows);
    if (root_ != returned_root) {
        logger_.fatal() << "Root window changed during query tree: "
                        << windowToString(root_)
                        << " != " << windowToString(returned_root) << std::endl;
        XUngrabServer(display_);
        return;
    }

    for (unsigned int i = 0; i < num_top_level_windows; ++i) {
        Frame(top_level_windows[i], true);
    }
    XFree(top_level_windows);
    XUngrabServer(display_);

    if (fork() == 0) {
        setsid();
        execlp("xterm", "xterm", nullptr);
        logger_.warn() << "Failed to launch xterm" << std::endl;
        _exit(1);
    }

    for (;;) {
        XEvent e;
        XNextEvent(display_, &e);

        if (e.type == randr_event_base_ + RRScreenChangeNotify) {
            XRRScreenChangeNotifyEvent *rr_event =
                (XRRScreenChangeNotifyEvent *)&e;
            OnRandrNotify(*rr_event);
            continue;
        }

        switch (e.type) {
        case CreateNotify:
            OnCreateNotify(e.xcreatewindow);
            break;
        case DestroyNotify:
            OnDestroyNotify(e.xdestroywindow);
            break;
        case ReparentNotify:
            OnReparentNotify(e.xreparent);
            break;
        case ConfigureRequest:
            OnConfigureRequest(e.xconfigurerequest);
            break;
        case MapRequest:
            OnMapRequest(e.xmaprequest);
            break;
        case MapNotify:
            OnMapNotify(e.xmap);
            break;
        case ConfigureNotify:
            OnConfigureNotify(e.xconfigure);
            break;
        case UnmapNotify:
            OnUnmapNotify(e.xunmap);
            break;
        case FocusIn:
            OnFocusIn(e.xfocus);
            break;
        case ButtonPress:
            OnButtonPress(e.xbutton);
            break;
        case ButtonRelease:
            OnButtonRelease(e.xbutton);
            break;
        case MotionNotify:
            OnMotionNotify(e.xmotion);
            break;
        case ClientMessage:
            OnClientMessage(e.xclient);
            break;
        case KeyPress:
            OnKeyPress(e.xkey);
            break;
        }
    }
}

void KrkaWM::OnRandrNotify(const XRRScreenChangeNotifyEvent &e) {
    if (display_manager_) {
        display_manager_->HandleRandrEvent(e);
    }
}

int KrkaWM::OnWMDetected(Display *display, XErrorEvent *event) {
    (void)display;
    if (event->error_code == BadAccess) {
        wm_detected_ = true;
    }
    return 0;
}

int KrkaWM::OnXError(Display *display, XErrorEvent *event) {
    char error_text[256];
    XGetErrorText(display, event->error_code, error_text, sizeof(error_text));
    logger_.warn() << "X Error: code=" << event->error_code
                   << ", resourceid=" << windowToString(event->resourceid)
                   << ", request_code=" << static_cast<int>(event->request_code)
                   << ", minor_code=" << event->minor_code
                   << ", message=" << error_text << std::endl;
    return 0;
}

void KrkaWM::OnCreateNotify(const XCreateWindowEvent &e) {
    logger_.info() << "Created new Window: " << e.window << std::endl;
}

void KrkaWM::OnConfigureRequest(const XConfigureRequestEvent &e) {
    XWindowChanges changes;
    changes.x            = e.x;
    changes.y            = e.y;
    changes.width        = e.width;
    changes.height       = e.height;
    changes.border_width = e.border_width;
    changes.sibling      = e.above;
    changes.stack_mode   = e.detail;

    if (!clients_.count(e.window)) {
        XConfigureWindow(display_, e.window, e.value_mask, &changes);
    }
}

void KrkaWM::OnMapRequest(const XMapRequestEvent &e) {
    if (!clients_.count(e.window)) {
        Frame(e.window, false);
    }

    XMapWindow(display_, e.window);
    logger_.info() << "MapRequest for window: " << windowToString(e.window)
                   << ", tiled" << std::endl;
}

void KrkaWM::Frame(Window w, bool was_created_before_window_manager) {
    XWindowAttributes x_window_attrs;
    if (!XGetWindowAttributes(display_, w, &x_window_attrs)) {
        logger_.err() << "Failed to get attributes for window "
                      << windowToString(w) << std::endl;
        return;
    }

    if (was_created_before_window_manager) {
        if (x_window_attrs.override_redirect ||
            x_window_attrs.map_state != IsViewable) {
            return;
        }
    }

    float scale_factor = 1.0f;
    if (display_manager_) {
        MonitorInfo target_monitor =
            display_manager_->GetMonitorAt(x_window_attrs.x, x_window_attrs.y);
        scale_factor = target_monitor.scale_factor;
    }

    const int initial_width =
        std::min(static_cast<int>(x_window_attrs.width * scale_factor),
                 static_cast<int>(800 * scale_factor));
    const int initial_height =
        std::min(static_cast<int>(x_window_attrs.height * scale_factor),
                 static_cast<int>(600 * scale_factor));

    XGrabServer(display_);

    const Window frame = XCreateWindow(
        display_, root_, x_window_attrs.x, x_window_attrs.y,
        initial_width + 0 * BORDER_WIDTH, initial_height + 0 * BORDER_WIDTH,
        BORDER_WIDTH, CopyFromParent, InputOutput, CopyFromParent, 0, nullptr);

    if (frame == None) {
        logger_.err() << "Failed to create frame window for client: "
                      << windowToString(w) << std::endl;
        XUngrabServer(display_);
        return;
    }

    XSetWindowBackground(display_, frame, BORDER_COLOR_INACTIVE);

    XSelectInput(display_, frame,
                 SubstructureRedirectMask | SubstructureNotifyMask |
                     FocusChangeMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask);
    XSelectInput(display_, w,
                 FocusChangeMask | PropertyChangeMask | StructureNotifyMask);

    GrabWindowInput(frame);
    XAddToSaveSet(display_, w);

    XResizeWindow(display_, w, initial_width, initial_height);
    XReparentWindow(display_, w, frame, 0, 0);

    SmartPositionWindow(frame, x_window_attrs.x, x_window_attrs.y,
                        initial_width, initial_height);

    XMapWindow(display_, frame);

    Atom net_wm_name = XInternAtom(display_, "_NET_WM_NAME", False);
    const char *name = "KrkaWM Window";
    XChangeProperty(display_, frame, net_wm_name,
                    XInternAtom(display_, "UTF8_STRING", False), 8,
                    PropModeReplace, (unsigned char *)name, strlen(name));

    clients_[w] = frame;

    XSetWindowBorderWidth(display_, w, 0);
    XSetWindowBorderWidth(display_, frame, BORDER_WIDTH);
    XSetWindowBorder(display_, frame, BORDER_COLOR_INACTIVE);

    UpdateWindowBorders(w);
    XUngrabServer(display_);
    XFlush(display_);
}

void KrkaWM::OnReparentNotify(const XReparentEvent &e) {
    logger_.debug() << "ReparentNotify: window=" << windowToString(e.window)
                    << ", parent=" << windowToString(e.parent) << std::endl;
}

void KrkaWM::OnMapNotify(const XMapEvent &e) {
    if (clients_.count(e.window)) {
        logger_.debug() << "MapNotify for client window: "
                        << windowToString(e.window) << std::endl;
        Window frame = clients_[e.window];
        XMapWindow(display_, frame);
    }
}

void KrkaWM::OnConfigureNotify(const XConfigureEvent &e) {
    (void)e;
}

void KrkaWM::OnUnmapNotify(const XUnmapEvent &e) {
    if (!clients_.count(e.window)) {
        logger_.warn() << "Ignore UnmapNotify for non-client window "
                       << e.window << std::endl;
        return;
    }

    if (e.event == root_) {
        logger_.warn()
            << "Ignore UnmapNotify for reparented pre-existing window "
            << e.window << std::endl;
        return;
    }

    Unframe(e.window);
}

void KrkaWM::Unframe(Window w) {
    if (!clients_.count(w)) {
        logger_.warn() << "Unframe called for non-client window: "
                       << windowToString(w) << std::endl;
        return;
    }

    Window frame = clients_[w];
    XWindowAttributes attrs;
    if (!XGetWindowAttributes(display_, frame, &attrs)) {
        logger_.warn() << "Frame window invalid: " << windowToString(frame)
                       << " for client: " << windowToString(w) << std::endl;
        clients_.erase(w);
        return;
    }

    XUnmapWindow(display_, frame);
    XReparentWindow(display_, w, root_, 0, 0);
    XRemoveFromSaveSet(display_, w);
    XDestroyWindow(display_, frame);
    clients_.erase(w);
    logger_.info() << "Unframed Window: " << windowToString(w) << std::endl;

    if (focused_window_ == w) {
        focused_window_ = None;
        if (!clients_.empty()) {
            Window next_client = clients_.begin()->first;
            XSetInputFocus(display_, next_client, RevertToPointerRoot,
                           CurrentTime);
            UpdateWindowBorders(next_client);
            logger_.info() << "Focused next client: "
                           << windowToString(next_client) << std::endl;
        } else {
            XSetInputFocus(display_, root_, RevertToPointerRoot, CurrentTime);
            UpdateWindowBorders(None);
            logger_.info() << "No clients left, focused root window"
                           << std::endl;
        }
    }
}

void KrkaWM::OnDestroyNotify(const XDestroyWindowEvent &e) {
    XGrabServer(display_);
    Window client_window = e.window;
    if (clients_.count(client_window)) {
        logger_.info() << "DestroyNotify for client window: "
                       << windowToString(client_window) << std::endl;
        Unframe(client_window);
    } else {
        for (auto it = clients_.begin(); it != clients_.end();) {
            if (it->second == client_window) {
                logger_.info()
                    << "DestroyNotify for frame window: "
                    << windowToString(client_window)
                    << ", unframing client: " << windowToString(it->first)
                    << std::endl;
                Unframe(it->first);
                break;
            } else {
                ++it;
            }
        }
    }
    XUngrabServer(display_);
    XFlush(display_);
}

void KrkaWM::OnFocusIn(const XFocusChangeEvent &e) {
    if (e.mode != NotifyNormal && e.mode != NotifyWhileGrabbed) {
        return;
    }

    XGrabServer(display_);
    Window client_window = e.window;
    for (const auto &pair : clients_) {
        if (pair.second == e.window) {
            client_window = pair.first;
            break;
        }
    }

    if (focused_window_ == client_window || !clients_.count(client_window)) {
        XUngrabServer(display_);
        return;
    }

    XSetInputFocus(display_, client_window, RevertToPointerRoot, CurrentTime);

    Atom net_active_window = XInternAtom(display_, "_NET_ACTIVE_WINDOW", False);
    XChangeProperty(display_, root_, net_active_window, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&client_window, 1);

    UpdateWindowBorders(client_window);
    XUngrabServer(display_);
    XFlush(display_);
}

void KrkaWM::OnButtonPress(const XButtonEvent &e) {
    XGrabServer(display_);
    Window client_window = e.window;
    Window frame_window  = e.window;
    for (const auto &pair : clients_) {
        if (pair.second == e.window || pair.first == e.window) {
            client_window = pair.first;
            frame_window  = pair.second;
            break;
        }
    }

    if (!clients_.count(client_window)) {
        XSetInputFocus(display_, root_, RevertToPointerRoot, CurrentTime);
        focused_window_ = None;
        UpdateWindowBorders(None);
        XUngrabServer(display_);
        XFlush(display_);
        return;
    }

    drag_window_ = frame_window;

    Window root_return;
    int x, y;
    unsigned int width, height, border, depth;
    if (!XGetGeometry(display_, frame_window, &root_return, &x, &y, &width,
                      &height, &border, &depth)) {
        logger_.warn() << "Invalid frame window in ButtonPress: "
                       << windowToString(frame_window) << std::endl;
        XUngrabServer(display_);
        XFlush(display_);
        return;
    }
    frame_start_pos_  = {static_cast<float>(x), static_cast<float>(y)};
    frame_start_size_ = {static_cast<float>(width), static_cast<float>(height)};
    drag_start_pos_   = {static_cast<float>(e.x_root),
                         static_cast<float>(e.y_root)};

    if (e.button == Button1 && (e.state & MASTER_KEY)) {
        dragging_ = true;
    } else if (e.button == Button3 && (e.state & MASTER_KEY)) {
        resizing_ = true;
    }

    XSetInputFocus(display_, client_window, RevertToPointerRoot, CurrentTime);
    XRaiseWindow(display_, frame_window);
    UpdateWindowBorders(client_window);
    XUngrabServer(display_);
    XFlush(display_);
}

void KrkaWM::OnButtonRelease(const XButtonEvent &e) {
    (void)e;
    dragging_    = false;
    resizing_    = false;
    drag_window_ = None;
}

void KrkaWM::OnMotionNotify(const XMotionEvent &e) {
    if (!dragging_ && !resizing_) {
        return;
    }

    if (drag_window_ == None) {
        return;
    }

    float dx = e.x_root - drag_start_pos_.x;
    float dy = e.y_root - drag_start_pos_.y;

    Window client_window = None;
    for (const auto &pair : clients_) {
        if (pair.second == drag_window_) {
            client_window = pair.first;
            break;
        }
    }

    if (dragging_) {
        int new_x = static_cast<int>(frame_start_pos_.x + dx);
        int new_y = static_cast<int>(frame_start_pos_.y + dy);

        HandleCrossMonitorDrag(drag_window_, new_x, new_y);
    } else if (resizing_ && client_window != None) {
        int new_width = std::max(1, static_cast<int>(frame_start_size_.x + dx));
        int new_height =
            std::max(1, static_cast<int>(frame_start_size_.y + dy));

        MonitorInfo current_monitor = display_manager_->GetMonitorContaining(
            static_cast<int>(frame_start_pos_.x),
            static_cast<int>(frame_start_pos_.y), new_width, new_height);

        int min_width  = static_cast<int>(50 * current_monitor.scale_factor);
        int min_height = static_cast<int>(50 * current_monitor.scale_factor);

        int client_w = std::max(min_width, new_width - 0 * BORDER_WIDTH);
        int client_h = std::max(min_height, new_height - 0 * BORDER_WIDTH);

        XResizeWindow(display_, client_window, client_w, client_h);
        XResizeWindow(display_, drag_window_, new_width, new_height);
    }
}

void KrkaWM::OnClientMessage(const XClientMessageEvent &e) {
    if (e.message_type == ATOM_WM_PROTOCOLS &&
        (unsigned long)e.data.l[0] == ATOM_WM_DELETE_WINDOW) {
        Window client_window = e.window;
        if (clients_.count(client_window)) {
            logger_.info() << "Received WM_DELETE_WINDOW for window: "
                           << client_window << std::endl;
            Unframe(client_window);
        } else {
            logger_.warn() << "Ignoring WM_DELETE_WINDOW for unmanaged window: "
                           << client_window << std::endl;
        }
    }
}

void KrkaWM::OnKeyPress(const XKeyEvent &e) {
    if (e.state & MASTER_KEY &&
        e.keycode == XKeysymToKeycode(display_, WINDOW_CLOSE_KEY)) {
        Window client_window = e.window;

        for (const auto &pair : clients_) {
            if (pair.second == e.window) {
                client_window = pair.first;
                break;
            }
        }

        if (!clients_.count(client_window)) {
            logger_.warn() << "Ignoring close request for unmanaged window: "
                           << client_window << std::endl;
            return;
        }

        Atom *protocols;
        int num_protocols;
        Bool supports_delete = False;
        if (XGetWMProtocols(display_, client_window, &protocols,
                            &num_protocols)) {
            for (int i = 0; i < num_protocols; i++) {
                if (protocols[i] == ATOM_WM_DELETE_WINDOW) {
                    supports_delete = True;
                    break;
                }
            }
            XFree(protocols);
        }

        if (supports_delete) {
            XEvent msg;
            memset(&msg, 0, sizeof(msg));
            msg.xclient.type         = ClientMessage;
            msg.xclient.window       = client_window;
            msg.xclient.message_type = ATOM_WM_PROTOCOLS;
            msg.xclient.format       = 32;
            msg.xclient.data.l[0]    = ATOM_WM_DELETE_WINDOW;
            msg.xclient.data.l[1]    = CurrentTime;
            XSendEvent(display_, client_window, False, NoEventMask, &msg);
            XFlush(display_);
        } else {
            logger_.err()
                << "Window " << client_window
                << " does not support WM_DELETE_WINDOW, forcefully destroying"
                << std::endl;
            Unframe(client_window);
            XDestroyWindow(display_, client_window);
        }
    } else if (e.state & MASTER_KEY &&
               e.keycode == XKeysymToKeycode(display_, WINDOW_CYCLE_KEY)) {
        Window next_client = None;
        bool found         = false;
        for (const auto &pair : clients_) {
            if (found) {
                next_client = pair.first;
                break;
            }
            if (pair.first == e.window || pair.second == e.window) {
                found = true;
            }
        }
        if (!found || next_client == None) {
            for (const auto &pair : clients_) {
                next_client = pair.first;
                break;
            }
        }
        if (next_client != None) {
            UpdateWindowBorders(next_client);
        }
    } else if (e.state & MASTER_KEY &&
               e.keycode == XKeysymToKeycode(display_, WM_TERMINATE_KEY)) {
        XCloseDisplay(display_);
        exit(0);
    } else if (e.state & MASTER_KEY &&
               e.keycode == XKeysymToKeycode(display_, TERMINAL_OPEN_KEY)) {
        if (fork() == 0) {
            setsid();
            execlp("xterm", "xterm", nullptr);
            logger_.err() << "Failed to launch xterm" << std::endl;
            _exit(1);
        }
    }
}
