
#include "platform/X11Window.h"

#include <algorithm>
#include <iostream>

X11Window::X11Window(int width, int height)
    : display(nullptr),
      window(0),
      screen(0),
      width(width),
      height(height),
      font(nullptr) {
}

X11Window::~X11Window() {
    if (display != nullptr) {
        if (font != nullptr) {
            XFreeFont(display, font);
            font = nullptr;
        }

        if (window != 0) {
            XDestroyWindow(display, window);
            window = 0;
        }

        XCloseDisplay(display);
        display = nullptr;
    }
}

bool X11Window::initialize() {
    display = XOpenDisplay(nullptr);

    if (display == nullptr) {
        std::cerr << "Unable to open X11 display\n";
        return false;
    }

    screen = DefaultScreen(display);

    // Load the font once.
    font = XLoadQueryFont(display, "fixed");

    if (font == nullptr) {
        std::cerr << "Unable to load X11 font: fixed\n";
        XCloseDisplay(display);
        display = nullptr;
        return false;
    }

    window = XCreateSimpleWindow(
        display, RootWindow(display, screen),
        100, 100, width, height, 2,
        BlackPixel(display, screen),
        WhitePixel(display, screen)
    );

    if (window == 0) {
        std::cerr << "Unable to create X11 window\n";
        XFreeFont(display, font);
        font = nullptr;
        XCloseDisplay(display);
        display = nullptr;
        return false;
    }

    // Receive keyboard, mouse-wheel, scrollbar click, drag, and expose events.
    XSelectInput(
        display, window,
        ExposureMask | KeyPressMask | ButtonPressMask |
        ButtonReleaseMask | PointerMotionMask
    );

    // Use the same font for drawing and measuring.
    XSetFont(display, DefaultGC(display, screen), font->fid);

    return true;
}

void X11Window::show() {
    if (display == nullptr || window == 0) {
        return;
    }

    XMapWindow(display, window);
    XFlush(display);
}

void X11Window::clear() {
    if (display == nullptr || window == 0) {
        return;
    }

    XClearWindow(display, window);
}

void X11Window::drawText(const std::string& text, int x, int y) {
    if (display == nullptr || window == 0 || font == nullptr || text.empty()) {
        return;
    }

    // Restore black foreground because scrollbar drawing changes the GC color.
    XSetForeground(display, DefaultGC(display, screen), BlackPixel(display, screen));

    XDrawString(
        display, window, DefaultGC(display, screen),
        x, y, text.c_str(), static_cast<int>(text.length())
    );
}

int X11Window::getTextWidth(const std::string& text) {
    if (font == nullptr || text.empty()) {
        return 0;
    }

    return XTextWidth(font, text.c_str(), static_cast<int>(text.length()));
}

int X11Window::getScrollbarWidth() const {
    return 12;
}

void X11Window::drawScrollbar(int totalLines, int visibleLines, int firstVisibleLine) {
    if (display == nullptr || window == 0 || totalLines <= 0) {
        return;
    }

    const int barWidth = getScrollbarWidth();
    const int barX = width - barWidth;
    const int trackHeight = height;

    // Draw the scrollbar track.
    XSetForeground(display, DefaultGC(display, screen), WhitePixel(display, screen));
    XFillRectangle(display, window, DefaultGC(display, screen),
                   barX, 0, barWidth, trackHeight);

    // Calculate the thumb height based on the visible content.
    int thumbHeight = std::max(
        20, trackHeight * std::min(visibleLines, totalLines) / totalLines
    );
    thumbHeight = std::min(thumbHeight, trackHeight);

    // Clamp the viewport to the valid range.
    const int maxFirstLine = std::max(0, totalLines - visibleLines);
    firstVisibleLine = std::clamp(firstVisibleLine, 0, maxFirstLine);

    int thumbY = 0;

    if (maxFirstLine > 0) {
        thumbY = (trackHeight - thumbHeight) * firstVisibleLine / maxFirstLine;
    }

    // Draw the scrollbar thumb.
    XSetForeground(display, DefaultGC(display, screen), BlackPixel(display, screen));
    XFillRectangle(display, window, DefaultGC(display, screen),
                   barX + 2, thumbY, barWidth - 4, thumbHeight);
}

Display* X11Window::getDisplay() {
    return display;
}

Window X11Window::getWindow() {
    return window;
}
