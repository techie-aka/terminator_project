
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

    // Enter
    if (keySymbol == XK_Return || keySymbol == XK_KP_Enter) {
        return true;
    }

    // Backspace
    if (keySymbol == XK_BackSpace) {
        if (!text.empty()) {
            text.pop_back();
        }

        return false;
    }

    // Normal character
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

string Input::translateKeyPress(XKeyEvent& keyEvent) {
    char buffer[32];
    KeySym keySymbol;

    const int length = XLookupString(
        &keyEvent, buffer, sizeof(buffer), &keySymbol, nullptr
    );

    // Translate Enter and editing/navigation keys into terminal input.
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

    // Preserve standard terminal signals for Ctrl+C and Ctrl+D.
    if (keyEvent.state & ControlMask) {
        if (keySymbol == XK_c || keySymbol == XK_C) {
            return "\x03";
        }

        if (keySymbol == XK_d || keySymbol == XK_D) {
            return "\x04";
        }

        if (keySymbol == XK_z || keySymbol == XK_Z) {
            return "\x1a";
        }

        // Ctrl+Shift+T is handled by the application controller.
        if (keyEvent.state & ShiftMask &&
            (keySymbol == XK_t || keySymbol == XK_T)) {
            return "";
        }
    }

    if (length > 0) {
        return string(buffer, static_cast<size_t>(length));
    }

    return "";
}
