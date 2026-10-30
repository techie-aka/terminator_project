
#ifndef PLATFORM_CLIPBOARD_H
#define PLATFORM_CLIPBOARD_H

#include <X11/Xlib.h>

#include <string>

class Clipboard {
private:
    Display* display;
    Window window;

    Atom clipboardAtom;
    Atom utf8StringAtom;
    Atom targetsAtom;
    Atom textAtom;
    Atom incrAtom;
    Atom propertyAtom;

    std::string ownedText;
    std::string receivedText;

    Atom pasteTarget;

    bool pastePending;
    bool incrementalPaste;
    bool pasteReady;
    bool triedStringFallback;

    bool handleSelectionRequest(const XSelectionRequestEvent& request);
    bool handleSelectionNotify(const XSelectionEvent& notification);
    bool handlePropertyNotify(const XPropertyEvent& propertyEvent);

    void finishPaste();

public:
    Clipboard(Display* display, Window window);

    // Publish text through the X11 CLIPBOARD selection.
    bool copy(const std::string& text);

    // Request clipboard text asynchronously.
    void requestPaste();

    // Process X11 clipboard-related events.
    // Returns true if the event was handled.
    bool handleEvent(const XEvent& event);

    // Retrieve completed pasted text.
    bool takePastedText(std::string& text);
};

#endif // PLATFORM_CLIPBOARD_H
