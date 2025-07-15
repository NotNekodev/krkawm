#include <core/krka.hpp>

#include <X11/X.h>
#include <functional>
#include <iostream>
#include <unistd.h>
#include <vector>
using ::std::unique_ptr;

bool KrkaWM::wm_detected_ = false;
std::unordered_map<Window, Window> KrkaWM::clients_;

void KrkaWM::TileClients() {
    const int screen_width  = DisplayWidth(display_, DefaultScreen(display_));
    const int screen_height = DisplayHeight(display_, DefaultScreen(display_));

    const int border = 3;
    int count        = clients_.size();
    if (count == 0)
        return;

    std::vector<Window> clientOrder;
    for (const auto &[client, _] : clients_) {
        clientOrder.push_back(client);
    }

    std::function<void(int, int, int, int, int)> place =
        [&](int index, int x, int y, int w, int h) {
            if (index >= count)
                return;

            Window client = clientOrder[index];
            Window frame  = clients_[client];

            int fx = x, fy = y, fw = w, fh = h;

            int client_w = std::max(50, fw - 2 * border);
            int client_h = std::max(50, fh - 2 * border);

            XMoveResizeWindow(display_, frame, fx, fy, fw, fh);

            XWindowChanges changes;
            changes.x      = border;
            changes.y      = border;
            changes.width  = client_w;
            changes.height = client_h;
            XConfigureWindow(display_, client, CWX | CWY | CWWidth | CWHeight,
                             &changes);

            XMapWindow(display_, client);
            XMapWindow(display_, frame);
            XRaiseWindow(display_, frame);

            if (index + 1 < count) {
                if (index % 2 == 0) {
                    place(index + 1, x + w / 2, y, w / 2, h);
                    fw = w / 2;
                } else {
                    place(index + 1, x, y + h / 2, w, h / 2);
                    fh = h / 2;
                }
            }

            std::cout << "Tiled window " << index << ": client=" << client
                      << ", frame=" << frame << ", x=" << fx << ", y=" << fy
                      << ", w=" << fw << ", h=" << fh << std::endl;
        };

    place(0, 0, 0, screen_width, screen_height);
    XFlush(display_);
}

unique_ptr<KrkaWM> KrkaWM::Create() {
    Display *display = XOpenDisplay(nullptr);
    if (!display) {
        std::cerr << "Failed to open X display!" << std::endl;
        return nullptr;
    }
    return unique_ptr<KrkaWM>(new KrkaWM(display));
}

KrkaWM::KrkaWM(Display *display)
    : display_(display), root_(DefaultRootWindow(display_)),
      focused_window_(None), dragging_(false), resizing_(false),
      drag_window_(None) {
}

KrkaWM::~KrkaWM() {
    for (const auto &pair : clients_) {
        Unframe(pair.first);
    }
    XCloseDisplay(display_);
}

void KrkaWM::UpdateWindowBorders(Window new_focus_client) {
    const unsigned long ACTIVE_BORDER_COLOR   = 0xff0000;
    const unsigned long INACTIVE_BORDER_COLOR = 0x000000;

    if (focused_window_ != None && clients_.count(focused_window_)) {
        Window old_frame = clients_[focused_window_];
        XSetWindowBorder(display_, old_frame, INACTIVE_BORDER_COLOR);
    }

    if (new_focus_client != None && clients_.count(new_focus_client)) {
        Window new_frame = clients_[new_focus_client];
        XSetWindowBorder(display_, new_frame, ACTIVE_BORDER_COLOR);
    }
}

void KrkaWM::GrabGlobalInput() {
    XGrabKey(display_, XKeysymToKeycode(display_, WM_TERMINATE_KEY), MASTER_KEY,
             root_, False, GrabModeAsync, GrabModeAsync);
    XGrabKey(display_, XKeysymToKeycode(display_, TERMINAL_OPEN_KEY),
             MASTER_KEY, root_, False, GrabModeAsync, GrabModeAsync);
}

void KrkaWM::GrabWindowInput(Window frame) {
    XGrabButton(display_, Button1, MASTER_KEY, frame, False,
                ButtonPressMask | ButtonReleaseMask | ButtonMotionMask,
                GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(display_, Button3, MASTER_KEY, frame, False,
                ButtonPressMask | ButtonReleaseMask | ButtonMotionMask,
                GrabModeAsync, GrabModeAsync, None, None);
    XGrabKey(display_, XKeysymToKeycode(display_, WINDOW_CLOSE_KEY), MASTER_KEY,
             frame, False, GrabModeAsync, GrabModeAsync);
    XGrabKey(display_, XKeysymToKeycode(display_, WINDOW_CYCLE_KEY), MASTER_KEY,
             frame, False, GrabModeAsync, GrabModeAsync);
}

void KrkaWM::Run() {
    wm_detected_ = false;
    XSetErrorHandler(&KrkaWM::OnWMDetected);
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
        std::cerr << "Failed to query root window!" << std::endl;
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
        std::cerr << "Failed to launch xterm" << std::endl;
        _exit(1);
    }

    for (;;) {
        XEvent e;
        XNextEvent(display_, &e);

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

int KrkaWM::OnWMDetected(Display *display, XErrorEvent *event) {
    if (event->error_code == BadAccess) {
        wm_detected_ = true;
    }
    return 0;
}

int KrkaWM::OnXError(Display *display, XErrorEvent *event) {
    char error_text[256];
    XGetErrorText(display, event->error_code, error_text, sizeof(error_text));
    std::cerr << "X Error: code=" << event->error_code
              << ", resourceid=" << event->resourceid
              << ", message=" << error_text << std::endl;
    return 0;
}

void KrkaWM::OnCreateNotify(const XCreateWindowEvent &e) {
    std::cout << "Created new Window: " << e.window << std::endl;
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

    if (clients_.count(e.window)) {
        // For already framed windows, apply tiling instead of requested size
        Window frame = clients_[e.window];
        TileClients(); // Re-tile to enforce dwindle layout
        std::cout << "ConfigureRequest for framed window: " << e.window
                  << ", frame=" << frame << ", tiling applied" << std::endl;
    } else {
        // For new windows, apply the requested size temporarily, then frame and
        // tile
        XConfigureWindow(display_, e.window, e.value_mask, &changes);
        std::cout << "ConfigureRequest for new window: " << e.window
                  << ", x=" << e.x << ", y=" << e.y << ", width=" << e.width
                  << ", height=" << e.height << std::endl;
    }
}

void KrkaWM::OnMapRequest(const XMapRequestEvent &e) {
    if (!clients_.count(e.window)) {
        Frame(e.window, false);
    }
    // Map the window and ensure tiling is applied
    XMapWindow(display_, e.window);
    TileClients(); // Re-tile to ensure new window is placed correctly
    std::cout << "MapRequest for window: " << e.window << ", tiled"
              << std::endl;
}

void KrkaWM::Frame(Window w, bool was_created_before_window_manager) {
    const unsigned int BORDER_WIDTH  = 3;
    const unsigned long BORDER_COLOR = 0x000000;
    const unsigned long BG_COLOR     = 0x0000ff;

    XWindowAttributes x_window_attrs;
    if (!XGetWindowAttributes(display_, w, &x_window_attrs)) {
        std::cerr << "Failed to get attributes for window " << w << std::endl;
        return;
    }

    if (was_created_before_window_manager) {
        if (x_window_attrs.override_redirect ||
            x_window_attrs.map_state != IsViewable) {
            return;
        }
    }

    // Create frame with reasonable initial size (not full screen)
    const int initial_width  = std::min(x_window_attrs.width, 800);
    const int initial_height = std::min(x_window_attrs.height, 600);
    const Window frame       = XCreateSimpleWindow(
        display_, root_, x_window_attrs.x, x_window_attrs.y, initial_width,
        initial_height, BORDER_WIDTH, BORDER_COLOR, BG_COLOR);

    XSelectInput(display_, frame,
                 SubstructureRedirectMask | SubstructureNotifyMask |
                     FocusChangeMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask);
    XSelectInput(display_, w,
                 FocusChangeMask | PropertyChangeMask | StructureNotifyMask);

    GrabWindowInput(frame);
    XAddToSaveSet(display_, w);
    XReparentWindow(display_, w, frame, 0, 0);
    XMapWindow(display_, frame);

    Atom net_wm_name = XInternAtom(display_, "_NET_WM_NAME", False);
    const char *name = "KrkaWM Window";
    XChangeProperty(display_, frame, net_wm_name,
                    XInternAtom(display_, "UTF8_STRING", False), 8,
                    PropModeReplace, (unsigned char *)name, strlen(name));

    clients_[w] = frame;
    TileClients(); // Immediately tile to apply dwindle layout
    std::cout << "Framed Window: " << w << " with frame " << frame << std::endl;

    if (focused_window_ == None) {
        XSetInputFocus(display_, w, RevertToPointerRoot, CurrentTime);
        focused_window_ = w;
        UpdateWindowBorders(w);
    }
}

// Rest of the code remains unchanged
void KrkaWM::OnReparentNotify(const XReparentEvent &e) {
    std::cout << "ReparentNotify: window=" << e.window
              << ", parent=" << e.parent << std::endl;
}

void KrkaWM::OnMapNotify(const XMapEvent &e) {
    if (clients_.count(e.window)) {
        std::cout << "MapNotify for client window: " << e.window << std::endl;
        Window frame = clients_[e.window];
        XMapWindow(display_, frame);
        TileClients(); // Ensure tiling is applied after mapping
    }
}

void KrkaWM::OnConfigureNotify(const XConfigureEvent &e) {
    if (clients_.count(e.window)) {
        std::cout << "ConfigureNotify for client window: " << e.window
                  << ", x=" << e.x << ", y=" << e.y << ", width=" << e.width
                  << ", height=" << e.height << std::endl;
    }
}

void KrkaWM::OnUnmapNotify(const XUnmapEvent &e) {
    if (!clients_.count(e.window)) {
        std::cout << "Ignore UnmapNotify for non-client window " << e.window
                  << std::endl;
        return;
    }

    if (e.event == root_) {
        std::cout << "Ignore UnmapNotify for reparented pre-existing window "
                  << e.window << std::endl;
        return;
    }

    Unframe(e.window);
}

void KrkaWM::Unframe(Window w) {
    if (!clients_.count(w)) {
        return;
    }

    Window frame = clients_[w];
    XUnmapWindow(display_, frame);
    XReparentWindow(display_, w, root_, 0, 0);
    XRemoveFromSaveSet(display_, w);
    XDestroyWindow(display_, frame);
    clients_.erase(w);
    std::cout << "Unframed Window: " << w << std::endl;

    if (focused_window_ == w) {
        focused_window_ = None;
        if (!clients_.empty()) {
            auto next_client = clients_.begin()->first;
            XSetInputFocus(display_, next_client, RevertToPointerRoot,
                           CurrentTime);
            focused_window_ = next_client;
            UpdateWindowBorders(next_client);
            std::cout << "Focused next client: " << next_client << std::endl;
        } else {
            XSetInputFocus(display_, root_, RevertToPointerRoot, CurrentTime);
            UpdateWindowBorders(None);
            std::cout << "No clients left, focused root window" << std::endl;
        }
    }
    TileClients();
}

void KrkaWM::OnDestroyNotify(const XDestroyWindowEvent &e) {
    Window client_window = e.window;
    if (clients_.count(client_window)) {
        std::cout << "DestroyNotify for client window: " << client_window
                  << std::endl;
        Unframe(client_window);
    } else {
        for (auto it = clients_.begin(); it != clients_.end(); ++it) {
            if (it->second == client_window) {
                std::cout << "DestroyNotify for frame window: " << client_window
                          << ", unframing client: " << it->first << std::endl;
                Unframe(it->first);
                break;
            }
        }
    }
}

void KrkaWM::OnFocusIn(const XFocusChangeEvent &e) {
    if (e.mode != NotifyNormal && e.mode != NotifyWhileGrabbed) {
        return;
    }

    Window client_window = e.window;
    for (const auto &pair : clients_) {
        if (pair.second == e.window) {
            client_window = pair.first;
            break;
        }
    }

    if (focused_window_ == client_window) {
        return;
    }

    focused_window_ = client_window;
    XSetInputFocus(display_, client_window, RevertToPointerRoot, CurrentTime);

    Atom net_active_window = XInternAtom(display_, "_NET_ACTIVE_WINDOW", False);
    XChangeProperty(display_, root_, net_active_window, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&client_window, 1);

    UpdateWindowBorders(client_window);
}

void KrkaWM::OnButtonPress(const XButtonEvent &e) {
    Window client_window = e.window;
    Window frame_window  = e.window;
    for (const auto &pair : clients_) {
        if (pair.second == e.window) {
            client_window = pair.first;
            frame_window  = pair.second;
            break;
        } else if (pair.first == e.window) {
            client_window = pair.first;
            frame_window  = pair.second;
            break;
        }
    }

    if (!clients_.count(client_window)) {
        XSetInputFocus(display_, root_, RevertToPointerRoot, CurrentTime);
        focused_window_ = None;
        UpdateWindowBorders(None);
        return;
    }

    drag_window_ = frame_window;

    Window root_return;
    int x, y;
    unsigned int width, height, border, depth;
    XGetGeometry(display_, frame_window, &root_return, &x, &y, &width, &height,
                 &border, &depth);
    frame_start_pos_  = {static_cast<float>(x), static_cast<float>(y)};
    frame_start_size_ = {static_cast<float>(width), static_cast<float>(height)};
    drag_start_pos_   = {static_cast<float>(e.x_root),
                         static_cast<float>(e.y_root)};

    if (e.state & Button1Mask) {
        dragging_ = true;
    } else if (e.state & Button3Mask) {
        resizing_ = true;
    }

    XSetInputFocus(display_, client_window, RevertToPointerRoot, CurrentTime);
    XRaiseWindow(display_, frame_window);
    focused_window_ = client_window;
    UpdateWindowBorders(client_window);
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

    if (dragging_) {
        int new_x = static_cast<int>(frame_start_pos_.x + dx);
        int new_y = static_cast<int>(frame_start_pos_.y + dy);
        XMoveWindow(display_, drag_window_, new_x, new_y);
    } else if (resizing_) {
        int new_width = std::max(1, static_cast<int>(frame_start_size_.x + dx));
        int new_height =
            std::max(1, static_cast<int>(frame_start_size_.y + dy));
        XResizeWindow(display_, drag_window_, new_width, new_height);
    }
}

void KrkaWM::OnClientMessage(const XClientMessageEvent &e) {
    if (e.message_type == ATOM_WM_PROTOCOLS &&
        e.data.l[0] == ATOM_WM_DELETE_WINDOW) {
        Window client_window = e.window;
        if (clients_.count(client_window)) {
            std::cout << "Received WM_DELETE_WINDOW for window: "
                      << client_window << std::endl;
            Unframe(client_window);
        } else {
            std::cout << "Ignoring WM_DELETE_WINDOW for unmanaged window: "
                      << client_window << std::endl;
        }
    }
}

void KrkaWM::OnKeyPress(const XKeyEvent &e) {
    if (e.state & MASTER_KEY &&
        e.keycode == XKeysymToKeycode(display_, WINDOW_CLOSE_KEY)) {
        Window client_window = e.window;

        // Ensure the window is a client window (not a frame)
        for (const auto &pair : clients_) {
            if (pair.second == e.window) {
                client_window = pair.first;
                break;
            }
        }

        if (!clients_.count(client_window)) {
            std::cout << "Ignoring close request for unmanaged window: "
                      << client_window << std::endl;
            return;
        }

        // Check if the window supports WM_DELETE_WINDOW
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
            // Send WM_DELETE_WINDOW message
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
            std::cout << "Sent WM_DELETE_WINDOW to window: " << client_window
                      << std::endl;
        } else {
            // Forcefully destroy the window
            std::cout
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
            XRaiseWindow(display_, clients_[next_client]);
            XSetInputFocus(display_, next_client, RevertToPointerRoot,
                           CurrentTime);
            focused_window_ = next_client;
            UpdateWindowBorders(next_client);
            std::cout << "Cycled to window: " << next_client << std::endl;
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
            std::cerr << "Failed to launch xterm" << std::endl;
            _exit(1);
        }
    }
}
