#include "platform/X11Window.h"

#include <X11/Xutil.h>

#include <algorithm>
#include <cstdlib>
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
    // Button1MotionMask requests MotionNotify specifically while Button1
    // (the left mouse button) is held. No explicit pointer grab is needed.
    XSelectInput(
        display, window,
        ExposureMask | KeyPressMask | ButtonPressMask |
        ButtonReleaseMask | Button1MotionMask | PropertyChangeMask
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

    GC gc = DefaultGC(display, screen);

    // Restore black foreground because scrollbar drawing changes the GC color.
    XSetForeground(display, gc, BlackPixel(display, screen));

    XDrawString(
        display, window, gc,
        x, y, text.c_str(), static_cast<int>(text.length())
    );
}

void X11Window::drawTextWithSelection(
    const std::string& text,
    int x,
    int y,
    std::size_t selectionStart,
    std::size_t selectionEnd
) {
    if (display == nullptr || window == 0 || font == nullptr) {
        return;
    }

    selectionStart = std::min(selectionStart, text.size());
    selectionEnd = std::min(selectionEnd, text.size());

    if (selectionStart >= selectionEnd) {
        drawText(text, x, y);
        return;
    }

    const std::string prefix = text.substr(0, selectionStart);
    const std::string selectedText =
        text.substr(selectionStart, selectionEnd - selectionStart);
    const std::string suffix = text.substr(selectionEnd);

    const int selectedX = x + getTextWidth(prefix);
    const int selectedWidth = getTextWidth(selectedText);

    GC gc = DefaultGC(display, screen);

    // Draw the unselected prefix normally.
    drawText(prefix, x, y);

    if (!selectedText.empty() && selectedWidth > 0) {
        // Black rectangle forms the selection background.
        XSetForeground(display, gc, BlackPixel(display, screen));

        XFillRectangle(display, window, gc,
            selectedX, y - font->ascent,
            static_cast<unsigned int>(selectedWidth),
            static_cast<unsigned int>(font->ascent + font->descent)
        );

        // Draw selected characters in white.
        XSetForeground(display, gc, WhitePixel(display, screen));

        XDrawString(display, window, gc, selectedX,
            y, selectedText.c_str(),
            static_cast<int>(selectedText.length())
        );
    }

    // Draw the remaining suffix normally.
    drawText(suffix, selectedX + selectedWidth, y);
}

int X11Window::getTextWidth(const std::string& text) {
    if (font == nullptr || text.empty()) {
        return 0;
    }

    return XTextWidth(font, text.c_str(), static_cast<int>(text.length()));
}

std::size_t X11Window::getTextColumnAtX(
    const std::string& text,
    int pixelX
) {
    if (pixelX <= 0 || text.empty()) {
        return 0;
    }

    std::size_t bestColumn = 0;
    int bestDistance = std::abs(pixelX);

    for (std::size_t column = 1; column <= text.size(); ++column) {
        const int boundary = getTextWidth(text.substr(0, column));
        const int distance = std::abs(pixelX - boundary);

        if (distance < bestDistance) {
            bestDistance = distance;
            bestColumn = column;
        }

        // Character boundaries are ordered from left to right.
        if (boundary > pixelX && distance > bestDistance) {
            break;
        }
    }

    return bestColumn;
}

int X11Window::getFontAscent() const {
    return font == nullptr ? 0 : font->ascent;
}

int X11Window::getScrollbarWidth() const {
    return 12;
}

void X11Window::drawScrollbar(
    int totalLines,
    int visibleLines,
    int firstVisibleLine
) {
    if (display == nullptr || window == 0 || totalLines <= 0) {
        return;
    }

    const int barWidth = getScrollbarWidth();
    const int barX = width - barWidth;
    const int trackHeight = height;

    GC gc = DefaultGC(display, screen);

    // Draw the scrollbar track.
    XSetForeground(display, gc, WhitePixel(display, screen));
    XFillRectangle(display, window, gc, barX, 0, barWidth, trackHeight);

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
    XSetForeground(display, gc, BlackPixel(display, screen));

    XFillRectangle(
        display, window, gc, barX + 2, thumbY,
        barWidth - 4, thumbHeight
    );
}

Display* X11Window::getDisplay() {
    return display;
}

Window X11Window::getWindow() {
    return window;
}
