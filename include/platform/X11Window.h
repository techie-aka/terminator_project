#ifndef X11WINDOW_H
#define X11WINDOW_H

#include <X11/Xlib.h>
#include <cstddef>
#include <string>

class X11Window {
private:
    Display* display;
    Window window;
    int screen;

    int width;
    int height;

    // Load once and reuse for drawing and text measurement.
    XFontStruct* font;

public:
    X11Window(int width, int height);
    ~X11Window();

    bool initialize();
    void show();
    void clear();

    void drawText(const std::string& text, int x, int y);

    // Draw a line with a selected character range highlighted.
    void drawTextWithSelection(
        const std::string& text,
        int x,
        int y,
        std::size_t selectionStart,
        std::size_t selectionEnd
    );

    int getTextWidth(const std::string& text);

    // Font metrics: drawText's y-coordinate is the baseline.
    int getFontAscent() const;

    // Convert an X coordinate into the closest character position.
    std::size_t getTextColumnAtX(
        const std::string& text,
        int pixelX
    );

    void drawScrollbar(int totalLines, int visibleLines, int firstVisibleLine);

    int getScrollbarWidth() const;

    Display* getDisplay();
    Window getWindow();
};

#endif
