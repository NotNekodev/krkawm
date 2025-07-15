#include <core/krka.hpp>

#include <Imlib2.h>
#include <X11/X.h>
#include <functional>
#include <iostream>
#include <unistd.h>
#include <vector>
using ::std::unique_ptr;

bool KrkaWM::wm_detected_ = false;
std::unordered_map<Window, Window> KrkaWM::clients_;
Logger KrkaWM::logger_("krka.log");

#define MARGIN                5
#define BG_COLOR              0x40d190
#define BORDER_COLOR_INACTIVE 0x000000
#define BORDER_COLOR_ACTIVE   0xff0000
#define BORDER_WIDTH          2
#define WALLPAPER_PATH_JPG    "./wallpaper.jpg"

std::string windowToString(Window w) {
    char name[64];
    snprintf(name, sizeof(name), "0x%lx", w);
    return std::string(name);
}

void KrkaWM::TileClients() {
    const int screenwidth  = DisplayWidth(display_, DefaultScreen(display_));
    const int screenheight = DisplayHeight(display_, DefaultScreen(display_));
    const int margin       = MARGIN;
    int count              = clients_.size();
    if (count == 0)
        return;

    std::vector<Window> clientOrder;
    for (const auto &[client, ph] : clients_) {
        clientOrder.push_back(client);
    }

    std::function<void(int, int, int, int, int)> place =
        [&](int index, int x, int y, int w, int h) {
            if (index >= count)
                return;

            int fw = w;
            int fh = h;
            int fx = x;
            int fy = y;

            if (index + 1 < count) {
                if (index % 2 == 0) {
                    // Split horizontally - leave margin between halves
                    fw = (w - margin) / 2;
                    place(index + 1, x + fw + margin, y, w - fw - margin, h);
                } else {
                    // Split vertically - leave margin between halves
                    fh = (h - margin) / 2;
                    place(index + 1, x, y + fh + margin, w, h - fh - margin);
                }
            }

            Window client = clientOrder[index];
            Window frame  = clients_[client];

            // Frame takes the allocated space
            XMoveResizeWindow(display_, frame, fx - (2 * BORDER_WIDTH),
                              fy - (2 * BORDER_WIDTH), fw + (2 * BORDER_WIDTH),
                              fh + (2 * BORDER_WIDTH));

            // Client window needs to leave space for border
            int client_w = std::max(50, fw - 2 * BORDER_WIDTH);
            int client_h = std::max(50, fh - 2 * BORDER_WIDTH);

            XWindowChanges changes;
            changes.x      = 2 * BORDER_WIDTH; // Offset by border width
            changes.y      = 2 * BORDER_WIDTH; // Offset by border width
            changes.width  = client_w;
            changes.height = client_h;

            XConfigureWindow(display_, client, CWX | CWY | CWWidth | CWHeight,
                             &changes);
            XMapWindow(display_, client);
            XMapWindow(display_, frame);
            XRaiseWindow(display_, frame);

            logger_.debug()
                << "Tiled window " << index
                << ": client=" << windowToString(client)
                << ", frame=" << windowToString(frame) << ", x=" << fx
                << ", y=" << fy << ", w=" << fw << ", h=" << fh << std::endl;
        };

    // Start tiling with margin around screen edges
    place(0, margin, margin, screenwidth - 2 * margin,
          screenheight - 2 * margin);

    XClearWindow(display_, root_);
    XFlush(display_);
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
    imlib_context_set_display(display_);
    imlib_context_set_drawable(root_);

    Imlib_Image wallpaper = imlib_load_image(WALLPAPER_PATH_JPG);
    if (!wallpaper) {
        logger_.err() << "Failed to load wallpaper image from "
                      << WALLPAPER_PATH_JPG << std::endl;
        XSetWindowBackground(display_, root_, BG_COLOR);
        XClearWindow(display_, root_);
        XFlush(display_);
        return;
    }

    imlib_context_set_image(wallpaper);

    int screen        = DefaultScreen(display_);
    int width         = DisplayWidth(display_, screen);
    int height        = DisplayHeight(display_, screen);
    Visual *visual    = DefaultVisual(display_, screen);
    Colormap colormap = DefaultColormap(display_, screen);

    imlib_context_set_visual(visual);
    imlib_context_set_colormap(colormap);

    Imlib_Image scaled = imlib_create_cropped_scaled_image(
        0, 0, imlib_image_get_width(), imlib_image_get_height(), width, height);

    imlib_free_image(); // Free original wallpaper image

    if (!scaled) {
        logger_.err() << "Failed to scale wallpaper image." << std::endl;
        XClearWindow(display_, root_);
        XFlush(display_);
        return;
    }

    // Set context to scaled image
    imlib_context_set_image(scaled);

    // Draw directly to the root window first
    imlib_context_set_drawable(root_);
    imlib_render_image_on_drawable(0, 0);

    // Create a pixmap copy for background
    Pixmap pix = XCreatePixmap(display_, root_, width, height,
                               DefaultDepth(display_, screen));
    imlib_context_set_drawable(pix);
    imlib_render_image_on_drawable(0, 0);

    // Set background pixmap
    XSetWindowBackgroundPixmap(display_, root_, pix);
    XClearWindow(display_, root_);
    XFlush(display_);

    imlib_free_image(); // Free scaled image
}

KrkaWM::~KrkaWM() {

    Window parent, *children;
    unsigned int nchildren;

    if (!XQueryTree(display_, root_, &parent, &parent, &children, &nchildren)) {
        for (auto &pair : clients_) {
            Unframe(pair.first);
            XDestroyWindow(display_, pair.second); // Destroy all frames
        }
    }

    for (unsigned int i = 0; i < nchildren; i++) {
        Window client_window = children[i];
        if (clients_.count(client_window)) {
            Unframe(client_window);
            XDestroyWindow(display_, clients_[client_window]); // Destroy frame
        } else {
            XDestroyWindow(display_, client_window);
        }
    }
}

void KrkaWM::UpdateWindowBorders(Window new_focus_client) {
    for (const auto &pair : clients_) {
        Window client = pair.first;
        XSetWindowBorder(display_, client, // Set border on client window
                         client == new_focus_client ? BORDER_COLOR_ACTIVE
                                                    : BORDER_COLOR_INACTIVE);
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
                   << ", resourceid=" << event->resourceid
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

    if (clients_.count(e.window)) {
        Window frame = clients_[e.window];
        TileClients();
        logger_.info() << "ConfigureRequest for framed window: "
                       << windowToString(e.window)
                       << ", frame=" << windowToString(frame)
                       << ", tiling applied" << std::endl;
    } else {
        XConfigureWindow(display_, e.window, e.value_mask, &changes);
        logger_.info() << "ConfigureRequest for new window: "
                       << windowToString(e.window) << ", x=" << e.x
                       << ", y=" << e.y << ", width=" << e.width
                       << ", height=" << e.height << std::endl;
    }
}

void KrkaWM::OnMapRequest(const XMapRequestEvent &e) {
    if (!clients_.count(e.window)) {
        Frame(e.window, false);
    }

    XMapWindow(display_, e.window);
    TileClients(); // Re-tile to ensure new window is placed correctly
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

    const int initial_width  = std::min(x_window_attrs.width, 800);
    const int initial_height = std::min(x_window_attrs.height, 600);
    const Window frame =
        XCreateSimpleWindow(display_, root_, x_window_attrs.x, x_window_attrs.y,
                            initial_width, initial_height, 0, 0, BG_COLOR);

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
    TileClients();
    logger_.info() << "Framed Window: " << windowToString(w) << " with frame "
                   << windowToString(frame) << std::endl;

    XSetWindowBorderWidth(display_, w, BORDER_WIDTH);
    XSetWindowBorder(display_, w, BORDER_COLOR_INACTIVE);

    XSetInputFocus(display_, w, RevertToPointerRoot, CurrentTime);
    focused_window_ = w;
    UpdateWindowBorders(w);
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
        TileClients();
    }
}

void KrkaWM::OnConfigureNotify(const XConfigureEvent &e) {
    if (clients_.count(e.window)) {
        logger_.debug() << "ConfigureNotify for client window: " << e.window
                        << ", x=" << e.x << ", y=" << e.y
                        << ", width=" << e.width << ", height=" << e.height
                        << std::endl;
    }
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
        return;
    }

    Window frame = clients_[w];
    XUnmapWindow(display_, frame);
    XReparentWindow(display_, w, root_, 0, 0);
    XRemoveFromSaveSet(display_, w);
    XDestroyWindow(display_, frame);
    clients_.erase(w);
    logger_.info() << "Unframed Window: " << windowToString(w) << std::endl;

    if (focused_window_ == w) {
        focused_window_ = None;
        if (!clients_.empty()) {
            auto next_client = clients_.begin()->first;
            XSetInputFocus(display_, next_client, RevertToPointerRoot,
                           CurrentTime);
            focused_window_ = next_client;
            UpdateWindowBorders(next_client);
            logger_.info() << "Focused next client: " << next_client
                           << std::endl;
        } else {
            XSetInputFocus(display_, root_, RevertToPointerRoot, CurrentTime);
            UpdateWindowBorders(None);
            logger_.info() << "No clients left, focused root window"
                           << std::endl;
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
                logger_.info()
                    << "DestroyNotify for frame window: " << client_window
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

        // Ensure the window is a client window (not a frame)
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
            XRaiseWindow(display_, clients_[next_client]);
            XSetInputFocus(display_, next_client, RevertToPointerRoot,
                           CurrentTime);
            focused_window_ = next_client;
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
