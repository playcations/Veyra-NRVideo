"""Focus Veyra on KWin's Xwayland display without keyboard/mouse navigation."""
import ctypes as c
import sys
import time

x = c.CDLL('libX11.so.6')
x.XOpenDisplay.argtypes = [c.c_char_p]
x.XOpenDisplay.restype = c.c_void_p
x.XDefaultRootWindow.argtypes = [c.c_void_p]
x.XDefaultRootWindow.restype = c.c_ulong
x.XQueryTree.argtypes = [c.c_void_p, c.c_ulong, c.POINTER(c.c_ulong),
                        c.POINTER(c.c_ulong), c.POINTER(c.POINTER(c.c_ulong)),
                        c.POINTER(c.c_uint)]
x.XFetchName.argtypes = [c.c_void_p, c.c_ulong, c.POINTER(c.c_void_p)]
x.XFree.argtypes = [c.c_void_p]
x.XInternAtom.argtypes = [c.c_void_p, c.c_char_p, c.c_int]
x.XInternAtom.restype = c.c_ulong
x.XSendEvent.argtypes = [c.c_void_p, c.c_ulong, c.c_int, c.c_long, c.c_void_p]
x.XFlush.argtypes = [c.c_void_p]
x.XCloseDisplay.argtypes = [c.c_void_p]
x.XGetWindowProperty.argtypes = [c.c_void_p, c.c_ulong, c.c_ulong, c.c_long,
                                c.c_long, c.c_int, c.c_ulong,
                                c.POINTER(c.c_ulong), c.POINTER(c.c_int),
                                c.POINTER(c.c_ulong), c.POINTER(c.c_ulong),
                                c.POINTER(c.c_void_p)]
x.XKeysymToKeycode.argtypes = [c.c_void_p, c.c_ulong]
x.XKeysymToKeycode.restype = c.c_ubyte
x.XSetInputFocus.argtypes = [c.c_void_p, c.c_ulong, c.c_int, c.c_ulong]

class Data(c.Union):
    _fields_ = [('b', c.c_char * 20), ('s', c.c_short * 10), ('l', c.c_long * 5)]

class ClientMessage(c.Structure):
    _fields_ = [('type', c.c_int), ('serial', c.c_ulong), ('send_event', c.c_int),
                ('display', c.c_void_p), ('window', c.c_ulong),
                ('message_type', c.c_ulong), ('format', c.c_int), ('data', Data)]

class Event(c.Union):
    _fields_ = [('client', ClientMessage), ('padding', c.c_long * 24)]

display = x.XOpenDisplay(None)
if not display:
    sys.exit('Veyra: the streaming display is unavailable')
root = x.XDefaultRootWindow(display)

def windows():
    root_out, parent = c.c_ulong(), c.c_ulong()
    children, count = c.POINTER(c.c_ulong)(), c.c_uint()
    if not x.XQueryTree(display, root, c.byref(root_out), c.byref(parent),
                        c.byref(children), c.byref(count)):
        return []
    result = []
    for index in range(count.value):
        wid = children[index]
        name = c.c_void_p()
        if x.XFetchName(display, wid, c.byref(name)) and name:
            title = c.string_at(name).decode('utf-8', errors='replace')
            x.XFree(name)
            if title.startswith('Veyra ') and title[6:7].isdigit():
                result.append(wid)
    if children:
        x.XFree(children)
    return result

def message(wid, name, *values):
    event = Event()
    event.client.type = 33
    event.client.send_event = 1
    event.client.display = display
    event.client.window = wid
    event.client.message_type = x.XInternAtom(display, name, 0)
    event.client.format = 32
    for index, value in enumerate(values):
        event.client.data.l[index] = value
    x.XSendEvent(display, root, 0, (1 << 20) | (1 << 19), c.byref(event))

def property_values(wid, name):
    atom = x.XInternAtom(display, name, 0)
    actual, count, after = c.c_ulong(), c.c_ulong(), c.c_ulong()
    fmt, data = c.c_int(), c.c_void_p()
    status = x.XGetWindowProperty(display, wid, atom, 0, 64, 0, 0,
                                 c.byref(actual), c.byref(fmt), c.byref(count),
                                 c.byref(after), c.byref(data))
    result = []
    if status == 0 and fmt.value == 32 and data:
        values = c.cast(data, c.POINTER(c.c_ulong))
        result = [values[index] for index in range(count.value)]
    if data:
        x.XFree(data)
    return result

try:
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        found = windows()
        if found:
            wid = found[-1]
            message(wid, b'_NET_WM_STATE', 0,
                    x.XInternAtom(display, b'_NET_WM_STATE_HIDDEN', 0), 0, 2)
            message(wid, b'_NET_WM_STATE', 1,
                    x.XInternAtom(display, b'_NET_WM_STATE_FULLSCREEN', 0), 0, 2)
            message(wid, b'_NET_ACTIVE_WINDOW', 2, 0, 0)
            x.XFlush(display)
            time.sleep(0.3)
            fullscreen = x.XInternAtom(display, b'_NET_WM_STATE_FULLSCREEN', 0)
            # Veyra owns fullscreen itself and can override a compositor-only
            # request. Use its configured default F shortcut when necessary.
            if fullscreen not in property_values(wid, b'_NET_WM_STATE'):
                if property_values(root, b'_NET_ACTIVE_WINDOW') != [wid]:
                    sys.exit('Veyra could not receive focus')
                test = c.CDLL('libXtst.so.6')
                test.XTestFakeKeyEvent.argtypes = [c.c_void_p, c.c_uint, c.c_int, c.c_ulong]
                x.XSetInputFocus(display, wid, 2, 0)
                key = x.XKeysymToKeycode(display, 0xFFC8)  # F11
                test.XTestFakeKeyEvent(display, key, 1, 0)
                x.XFlush(display)
                time.sleep(0.05)
                test.XTestFakeKeyEvent(display, key, 0, 0)
                x.XFlush(display)
                time.sleep(0.5)
            # Focus is maintained in-process by our Qt6Gui!QGuiApplication::focusWindow()
            # IAT hook in avcodec-63.dll (no synthetic mouse click needed).
            x.XSetInputFocus(display, wid, 2, 0)
            x.XFlush(display)
            if fullscreen not in property_values(wid, b'_NET_WM_STATE'):
                if property_values(root, b'_NET_ACTIVE_WINDOW') != [wid]:
                    sys.exit('Veyra could not receive focus')
                test = c.CDLL('libXtst.so.6')
                test.XTestFakeKeyEvent.argtypes = [c.c_void_p, c.c_uint, c.c_int, c.c_ulong]
                x.XSetInputFocus(display, wid, 2, 0)
                key = x.XKeysymToKeycode(display, 0xFFC8)  # F11
                test.XTestFakeKeyEvent(display, key, 1, 0)
                x.XFlush(display)
                time.sleep(0.05)
                test.XTestFakeKeyEvent(display, key, 0, 0)
                x.XFlush(display)
                time.sleep(0.5)
            # Wine/Qt on Xwayland can drop WM_ACTIVATE (active=false) on fullscreen
            # transition even when _NET_ACTIVE_WINDOW matches; a center click ensures
            # Veyra's Qt/SDL focus state becomes active=true so PS5 controller input forwards.
            test = c.CDLL('libXtst.so.6')
            test.XTestFakeMotionEvent.argtypes = [c.c_void_p, c.c_int, c.c_int, c.c_int, c.c_ulong]
            test.XTestFakeButtonEvent.argtypes = [c.c_void_p, c.c_uint, c.c_int, c.c_ulong]
            x.XDefaultScreen.argtypes = [c.c_void_p]
            x.XDefaultScreen.restype = c.c_int
            x.XDisplayWidth.argtypes = [c.c_void_p, c.c_int]
            x.XDisplayWidth.restype = c.c_int
            x.XDisplayHeight.argtypes = [c.c_void_p, c.c_int]
            x.XDisplayHeight.restype = c.c_int
            screen = x.XDefaultScreen(display)
            cx = x.XDisplayWidth(display, screen) // 2
            cy = x.XDisplayHeight(display, screen) // 2
            test.XTestFakeMotionEvent(display, -1, cx, cy, 0)
            test.XTestFakeButtonEvent(display, 1, 1, 0)
            x.XFlush(display)
            time.sleep(0.05)
            test.XTestFakeButtonEvent(display, 1, 0, 0)
            x.XFlush(display)
            if fullscreen not in property_values(wid, b'_NET_WM_STATE'):
                print('Veyra focused; fullscreen will be available during playback')
            else:
                print('Veyra fullscreen window ready')
            break
        time.sleep(0.25)
    else:
        sys.exit('Veyra did not open within 60 seconds; check Steam sign-in')
finally:
    x.XCloseDisplay(display)
