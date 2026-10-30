
#include "input/Input.h"

#include <X11/Xutil.h>
#include <X11/keysym.h>

using namespace std;

Input::Input()
    : text("") {
}

bool Input::handleKeyPress(XKeyEvent& keyEvent) {
    char buffer[32];
    KeySym keySymbol;

    int length = XLookupString(
        &keyEvent, buffer, sizeof(buffer), &keySymbol, nullptr
    );

    if (keySymbol == XK_Return || keySymbol == XK_KP_Enter) {
        return true;
    }

    if (keySymbol == XK_BackSpace) {
        if (!text.empty()) {
            text.pop_back();
        }

        return false;
    }

    if (length > 0) {
        text.append(buffer, length);
    }

    return false;
}

string Input::getText() const {
    return text;
}

void Input::clear() {
    text.clear();
}

// Recognise application-level shortcuts.
// These must be handled by the GUI rather than sent to the shell.
InputAction Input::getAction(XKeyEvent& keyEvent) const {
    const KeySym keySymbol = XLookupKeysym(&keyEvent, 0);
    const KeySym shiftedSymbol = XLookupKeysym(&keyEvent, 1);

    const bool controlPressed =
        (keyEvent.state & ControlMask) != 0;

    const bool shiftPressed =
        (keyEvent.state & ShiftMask) != 0;

    if (!controlPressed) {
        return InputAction::NoAction;
    }

    // Ctrl+Tab moves forward; Ctrl+Shift+Tab moves backward.
    // Some X11 keyboard mappings report Shift+Tab as ISO_Left_Tab.
    const bool isTabKey =
        keySymbol == XK_Tab ||
        keySymbol == XK_ISO_Left_Tab ||
        shiftedSymbol == XK_ISO_Left_Tab;

    if (isTabKey) {
        return shiftPressed
            ? InputAction::PreviousTab
            : InputAction::NextTab;
    }

    // Ctrl+Shift+T: create a tab.
    if (shiftPressed &&
        (keySymbol == XK_t || keySymbol == XK_T)) {
        return InputAction::NewTab;
    }

    // Ctrl+Shift+W: close the active tab.
    if (shiftPressed &&
        (keySymbol == XK_w || keySymbol == XK_W)) {
        return InputAction::CloseTab;
    }

    return InputAction::NoAction;
}

string Input::translateKeyPress(XKeyEvent& keyEvent) {
    char buffer[32];
    KeySym keySymbol;

    const int length = XLookupString(
        &keyEvent, buffer, sizeof(buffer), &keySymbol, nullptr
    );

    // Translate special keys into terminal escape sequences.
    switch (keySymbol) {
        case XK_Return:
        case XK_KP_Enter:
            return "\r";

        case XK_BackSpace:
            return "\x7f";

        case XK_Delete:
            return "\x1b[3~";

        case XK_Up:
            return "\x1b[A";

        case XK_Down:
            return "\x1b[B";

        case XK_Right:
            return "\x1b[C";

        case XK_Left:
            return "\x1b[D";

        case XK_Home:
            return "\x1b[H";

        case XK_End:
            return "\x1b[F";

        default:
            break;
    }

    const bool controlPressed =
        (keyEvent.state & ControlMask) != 0;

    const bool shiftPressed =
        (keyEvent.state & ShiftMask) != 0;

    if (controlPressed) {
        // Reserve Ctrl+Shift+C/V for the upcoming clipboard checkpoint.
        // In particular, Ctrl+Shift+C must never become Ctrl+C.
        if (shiftPressed &&
            (keySymbol == XK_c || keySymbol == XK_C ||
             keySymbol == XK_v || keySymbol == XK_V)) {
            return "";
        }

        switch (keySymbol) {
            case XK_a:
            case XK_A:
                return "\x01"; // Ctrl+A

            case XK_e:
            case XK_E:
                return "\x05"; // Ctrl+E

            case XK_u:
            case XK_U:
                return "\x15"; // Ctrl+U

            case XK_k:
            case XK_K:
                return "\x0b"; // Ctrl+K

            case XK_w:
            case XK_W:
                return "\x17"; // Ctrl+W

            case XK_c:
            case XK_C:
                return "\x03"; // Ctrl+C

            case XK_d:
            case XK_D:
                return "\x04"; // Ctrl+D

            case XK_z:
            case XK_Z:
                return "\x1a"; // Ctrl+Z

            default:
                break;
        }
    }

    if (length > 0) {
        return string(buffer, static_cast<size_t>(length));
    }

    return "";
}
