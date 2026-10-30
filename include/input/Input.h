
#ifndef INPUT_H
#define INPUT_H

#include <string>
#include <X11/Xlib.h>

// Application-level keyboard shortcuts recognised by Input.
enum class InputAction {
    NoAction,
    NextTab,
    PreviousTab,
    NewTab,
    CloseTab,
    CopySelection,
    PasteClipboard
};

class Input {
private:
    std::string text;

public:
    Input();

    bool handleKeyPress(XKeyEvent& keyEvent);

    std::string getText() const;
    void clear();

    // Identify application shortcuts without sending them to the shell.
    InputAction getAction(XKeyEvent& keyEvent) const;

    // Convert ordinary keys into terminal input bytes/sequences.
    std::string translateKeyPress(XKeyEvent& keyEvent);
};

#endif // INPUT_H
