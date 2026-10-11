#ifndef INPUT_H
#define INPUT_H

#include <X11/Xlib.h>
#include <string>

class Input {
private:
    std::string text;

public:
    Input();

    // Returns true when the user presses Enter
    bool handleKeyPress(XKeyEvent& keyEvent);

    std::string getText() const;
    std::string translateKeyPress(XKeyEvent& keyEvent);

    void clear();
};

#endif