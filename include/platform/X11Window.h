#ifndef X11WINDOW_H
#define X11WINDOW_H

#include <X11/Xlib.h>
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
    int getTextWidth(const std::string& text);

    void drawScrollbar(int totalLines, int visibleLines, int firstVisibleLine);

    int getScrollbarWidth() const;

    Display* getDisplay();
    Window getWindow();
};

#endif
