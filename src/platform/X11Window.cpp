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
      font(nullptr),
      colormap(0),
      backgroundColor(0),
      foregroundColor(0),
      promptColor(0),
      mutedColor(0),
      accentColor(0),
      activeTabBackground(0),
      inactiveTabBackground(0),
      activeTabForeground(0),
      inactiveTabForeground(0),
      selectionBackground(0),
      selectionForeground(0),
      cursorColor(0),
      scrollbarTrackColor(0),
      scrollbarThumbColor(0),
      borderColor(0) {
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

unsigned long X11Window::allocateColor(
    const char* name,
    unsigned long fallback
) {
    XColor screenColor{};
    XColor exactColor{};

    if (XAllocNamedColor(
            display, colormap, name, &screenColor, &exactColor) == 0) {
        return fallback;
    }

    return screenColor.pixel;
}

unsigned long X11Window::colorForStyle(TextStyle style) const {
    switch (style) {
        case TextStyle::Prompt:
            return promptColor;
        case TextStyle::Muted:
            return mutedColor;
        case TextStyle::Accent:
            return accentColor;
        case TextStyle::ActiveTab:
            return activeTabForeground;
        case TextStyle::InactiveTab:
            return inactiveTabForeground;
        case TextStyle::Normal:
        default:
            return foregroundColor;
    }
}

bool X11Window::initialize() {
    display = XOpenDisplay(nullptr);

    if (display == nullptr) {
        std::cerr << "Unable to open X11 display\n";
        return false;
    }

    screen = DefaultScreen(display);
    colormap = DefaultColormap(display, screen);

    const unsigned long fallbackBackground = BlackPixel(display, screen);
    const unsigned long fallbackForeground = WhitePixel(display, screen);

    // High-contrast dark navy palette. Every color has a safe fallback.
    backgroundColor = allocateColor("#101827", fallbackBackground);
    foregroundColor = allocateColor("#E5EDF7", fallbackForeground);
    promptColor = allocateColor("#5EEAD4", foregroundColor);
    mutedColor = allocateColor("#94A3B8", foregroundColor);
    accentColor = allocateColor("#60A5FA", foregroundColor);
    activeTabBackground = allocateColor("#243449", fallbackBackground);
    inactiveTabBackground = allocateColor("#172235", fallbackBackground);
    activeTabForeground = allocateColor("#F8FAFC", foregroundColor);
    inactiveTabForeground = allocateColor("#AAB8CA", foregroundColor);
    selectionBackground = allocateColor("#9eeefe", fallbackForeground);
    selectionForeground = allocateColor("#000000", fallbackBackground);
    cursorColor = allocateColor("#FBBF24", foregroundColor);
    scrollbarTrackColor = allocateColor("#172235", fallbackBackground);
    scrollbarThumbColor = allocateColor("#64748B", foregroundColor);
    borderColor = allocateColor("#334155", fallbackForeground);

    // Prefer a more readable font, but retain a portable fallback.
    // Try loading a larger Courier font using wildcards
    font = XLoadQueryFont(display, "-adobe-courier-medium-r-normal*-20-*");
    if (font == nullptr) {
        // Try a Terminus wildcard
        font = XLoadQueryFont(display, "-xos4-terminus-bold-r-normal*-18-*");
    }
    if (font == nullptr) {
        font = XLoadQueryFont(display, "fixed");
    }

    if (font == nullptr) {
        std::cerr << "Unable to load X11 font: special or fixed\n";
        XCloseDisplay(display);
        display = nullptr;
        return false;
    }

    window = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        80,
        60,
        static_cast<unsigned int>(width),
        static_cast<unsigned int>(height),
        1,
        borderColor,
        backgroundColor
    );

    if (window == 0) {
        std::cerr << "Unable to create X11 window\n";
        XFreeFont(display, font);
        font = nullptr;
        XCloseDisplay(display);
        display = nullptr;
        return false;
    }

    XStoreName(display, window, "Terminator - Custom Terminal Emulator");

    // Set a practical minimum size so the tab bar and terminal remain usable.
    XSizeHints sizeHints{};
    sizeHints.flags = PMinSize;
    sizeHints.min_width = 640;
    sizeHints.min_height = 420;
    XSetNormalHints(display, window, &sizeHints);

    XSelectInput(
        display,
        window,
        ExposureMask | KeyPressMask | ButtonPressMask |
        ButtonReleaseMask | Button1MotionMask | PropertyChangeMask |
        StructureNotifyMask
    );

    GC gc = DefaultGC(display, screen);
    XSetFont(display, gc, font->fid);
    XSetForeground(display, gc, foregroundColor);
    XSetBackground(display, gc, backgroundColor);

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

void X11Window::updateDimensions(int newWidth, int newHeight) {
    if (newWidth > 0) {
        width = newWidth;
    }

    if (newHeight > 0) {
        height = newHeight;
    }
}

void X11Window::drawText(
    const std::string& text,
    int x,
    int y,
    TextStyle style
) {
    if (display == nullptr || window == 0 || font == nullptr || text.empty()) {
        return;
    }

    GC gc = DefaultGC(display, screen);
    XSetForeground(display, gc, colorForStyle(style));
    XSetFont(display, gc, font->fid);

    XDrawString(
        display,
        window,
        gc,
        x,
        y,
        text.c_str(),
        static_cast<int>(text.length())
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

    drawText(prefix, x, y);

    if (!selectedText.empty() && selectedWidth > 0) {
        XSetForeground(display, gc, selectionBackground);

        // Add vertical padding to ensure underscores and descenders are fully covered
        const int paddingTop = 1;
        const int paddingBottom = 2;
        const int rectY = (y - font->ascent) - paddingTop;
        const int rectHeight = (font->ascent + font->descent) + paddingTop + paddingBottom;

        XFillRectangle(
            display,
            window,
            gc,
            selectedX,
            rectY,
            static_cast<unsigned int>(selectedWidth),
            static_cast<unsigned int>(rectHeight)
        );

        XSetForeground(display, gc, selectionForeground);
        XDrawString(
            display,
            window,
            gc,
            selectedX,
            y,
            selectedText.c_str(),
            static_cast<int>(selectedText.length())
        );
    }

    drawText(suffix, selectedX + selectedWidth, y);
}

void X11Window::drawCursor(int x, int baseline) {
    if (display == nullptr || window == 0 || font == nullptr) {
        return;
    }

    GC gc = DefaultGC(display, screen);
    XSetForeground(display, gc, cursorColor);
    XSetLineAttributes(display, gc, 2, LineSolid, CapButt, JoinMiter);

    const int top = baseline - font->ascent;
    const int bottom = baseline + font->descent - 1;
    XDrawLine(display, window, gc, x, top, x, bottom);

    XSetLineAttributes(display, gc, 1, LineSolid, CapButt, JoinMiter);
}

void X11Window::drawTab(
    int x,
    int tabWidth,
    const std::string& label,
    bool active,
    bool showCloseButton
) {
    if (display == nullptr || window == 0 || tabWidth <= 0) {
        return;
    }

    GC gc = DefaultGC(display, screen);
    const unsigned long tabBackground =
        active ? activeTabBackground : inactiveTabBackground;

    XSetForeground(display, gc, tabBackground);
    XFillRectangle(display, window, gc, x, 0,
                   static_cast<unsigned int>(tabWidth), 44);

    drawText(
        label,
        x + 16,
        28,
        active ? TextStyle::ActiveTab : TextStyle::InactiveTab
    );

    if (showCloseButton) {
        drawText("x", x + tabWidth - 22, 28, TextStyle::Muted);
    }

    if (active) {
        XSetForeground(display, gc, accentColor);
        XFillRectangle(display, window, gc, x + 8, 40,
                       static_cast<unsigned int>(std::max(1, tabWidth - 16)), 3);
    }
}

void X11Window::drawSeparator(int y) {
    if (display == nullptr || window == 0) {
        return;
    }

    GC gc = DefaultGC(display, screen);
    XSetForeground(display, gc, borderColor);
    XDrawLine(display, window, gc, 0, y, width, y);
}

int X11Window::getTextWidth(const std::string& text) {
    if (font == nullptr || text.empty()) {
        return 0;
    }

    return XTextWidth(font, text.c_str(), static_cast<int>(text.length()));
}

int X11Window::getFontAscent() const {
    return font == nullptr ? 0 : font->ascent;
}

int X11Window::getFontHeight() const {
    return font == nullptr ? 0 : font->ascent + font->descent;
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

        if (boundary > pixelX && distance > bestDistance) {
            break;
        }
    }

    return bestColumn;
}

int X11Window::getScrollbarWidth() const {
    return 14;
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

    XSetForeground(display, gc, scrollbarTrackColor);
    XFillRectangle(display, window, gc, barX, 0,
                   static_cast<unsigned int>(barWidth),
                   static_cast<unsigned int>(trackHeight));

    int thumbHeight = std::max(
        24,
        trackHeight * std::min(visibleLines, totalLines) / totalLines
    );
    thumbHeight = std::min(thumbHeight, trackHeight);

    const int maxFirstLine = std::max(0, totalLines - visibleLines);
    firstVisibleLine = std::clamp(firstVisibleLine, 0, maxFirstLine);

    int thumbY = 0;
    if (maxFirstLine > 0) {
        thumbY = (trackHeight - thumbHeight) * firstVisibleLine / maxFirstLine;
    }

    XSetForeground(display, gc, scrollbarThumbColor);
    XFillRectangle(
        display,
        window,
        gc,
        barX + 3,
        thumbY + 2,
        static_cast<unsigned int>(barWidth - 6),
        static_cast<unsigned int>(std::max(1, thumbHeight - 4))
    );
}

int X11Window::getWidth() const {
    return width;
}

int X11Window::getHeight() const {
    return height;
}

Display* X11Window::getDisplay() {
    return display;
}

Window X11Window::getWindow() {
    return window;
}
