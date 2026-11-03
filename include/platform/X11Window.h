#ifndef X11WINDOW_H
#define X11WINDOW_H

#include <X11/Xlib.h>
#include <cstddef>
#include <string>

enum class TextStyle {
    Normal,
    Prompt,
    Muted,
    Accent,
    ActiveTab,
    InactiveTab
};

class X11Window {
private:
    Display* display;
    Window window;
    int screen;

    int width;
    int height;

    XFontStruct* font;
    Colormap colormap;

    unsigned long backgroundColor;
    unsigned long foregroundColor;
    unsigned long promptColor;
    unsigned long mutedColor;
    unsigned long accentColor;
    unsigned long activeTabBackground;
    unsigned long inactiveTabBackground;
    unsigned long activeTabForeground;
    unsigned long inactiveTabForeground;
    unsigned long selectionBackground;
    unsigned long selectionForeground;
    unsigned long cursorColor;
    unsigned long scrollbarTrackColor;
    unsigned long scrollbarThumbColor;
    unsigned long borderColor;

    unsigned long allocateColor(const char* name, unsigned long fallback);
    unsigned long colorForStyle(TextStyle style) const;

public:
    X11Window(int width, int height);
    ~X11Window();

    bool initialize();
    void show();
    void clear();
    void updateDimensions(int width, int height);

    void drawText(
        const std::string& text,
        int x,
        int y,
        TextStyle style = TextStyle::Normal
    );

    void drawTextWithSelection(
        const std::string& text,
        int x,
        int y,
        std::size_t selectionStart,
        std::size_t selectionEnd
    );

    void drawCursor(int x, int baseline);
    void drawTab(int x, int tabWidth, const std::string& label,
                 bool active, bool showCloseButton);
    void drawSeparator(int y);

    int getTextWidth(const std::string& text);
    int getFontAscent() const;
    int getFontHeight() const;

    std::size_t getTextColumnAtX(const std::string& text, int pixelX);

    void drawScrollbar(int totalLines, int visibleLines, int firstVisibleLine);

    int getScrollbarWidth() const;
    int getWidth() const;
    int getHeight() const;

    Display* getDisplay();
    Window getWindow();
};

#endif
