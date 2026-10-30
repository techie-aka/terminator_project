
#include "platform/Clipboard.h"

#include <X11/Xatom.h>

#include <limits>

Clipboard::Clipboard(Display* display, Window window)
    : display(display),
      window(window),
      clipboardAtom(None),
      utf8StringAtom(None),
      targetsAtom(None),
      textAtom(None),
      incrAtom(None),
      propertyAtom(None),
      pasteTarget(None),
      pastePending(false),
      incrementalPaste(false),
      pasteReady(false),
      triedStringFallback(false) {

    if (this->display == nullptr || this->window == None) {
        return;
    }

    clipboardAtom = XInternAtom(
        this->display, "CLIPBOARD", False
    );

    utf8StringAtom = XInternAtom(
        this->display, "UTF8_STRING", False
    );

    targetsAtom = XInternAtom(
        this->display, "TARGETS", False
    );

    textAtom = XInternAtom(
        this->display, "TEXT", False
    );

    incrAtom = XInternAtom(
        this->display, "INCR", False
    );

    propertyAtom = XInternAtom(
        this->display, "TERMINATOR_CLIPBOARD_PROPERTY", False
    );
}

bool Clipboard::copy(const std::string& text) {
    if (display == nullptr || clipboardAtom == None) {
        return false;
    }

    // Keep the copied contents alive while this window owns CLIPBOARD.
    ownedText = text;

    XSetSelectionOwner(
        display,
        clipboardAtom,
        window,
        CurrentTime
    );

    XSync(display, False);

    // Confirm that X11 accepted our selection ownership.
    return XGetSelectionOwner(display, clipboardAtom) == window;
}

void Clipboard::requestPaste() {
    if (display == nullptr ||
        clipboardAtom == None ||
        propertyAtom == None) {
        return;
    }

    receivedText.clear();

    pastePending = true;
    incrementalPaste = false;
    pasteReady = false;
    triedStringFallback = false;

    // UTF8_STRING is preferred because modern applications normally
    // provide clipboard text in UTF-8.
    pasteTarget = utf8StringAtom;

    XDeleteProperty(display, window, propertyAtom);

    XConvertSelection(
        display,
        clipboardAtom,
        pasteTarget,
        propertyAtom,
        window,
        CurrentTime
    );

    XFlush(display);
}

void Clipboard::finishPaste() {
    pastePending = false;
    incrementalPaste = false;
    pasteReady = true;
}

bool Clipboard::handleSelectionRequest(
    const XSelectionRequestEvent& request
) {
    XEvent response{};

    response.xselection.type = SelectionNotify;
    response.xselection.display = display;
    response.xselection.requestor = request.requestor;
    response.xselection.selection = request.selection;
    response.xselection.target = request.target;
    response.xselection.time = request.time;
    response.xselection.property = None;

    if (request.selection == clipboardAtom &&
        XGetSelectionOwner(display, clipboardAtom) == window) {

        // ICCCM permits a requestor to use the target itself as
        // the property when request.property is None.
        const Atom property = request.property == None
            ? request.target
            : request.property;

        if (request.target == targetsAtom) {
            Atom supportedTargets[] = {
                targetsAtom,
                utf8StringAtom,
                XA_STRING,
                textAtom
            };

            XChangeProperty(
                display,
                request.requestor,
                property,
                XA_ATOM,
                32,
                PropModeReplace,
                reinterpret_cast<unsigned char*>(supportedTargets),
                4
            );

            response.xselection.property = property;
        } else if (
            request.target == utf8StringAtom ||
            request.target == XA_STRING ||
            request.target == textAtom
        ) {
            // TEXT requests are returned using UTF8_STRING.
            const Atom propertyType =
                request.target == XA_STRING
                    ? XA_STRING
                    : utf8StringAtom;

            if (ownedText.size() <=
                static_cast<std::size_t>(
                    std::numeric_limits<int>::max()
                )) {

                XChangeProperty(
                    display,
                    request.requestor,
                    property,
                    propertyType,
                    8,
                    PropModeReplace,
                    reinterpret_cast<const unsigned char*>(
                        ownedText.data()
                    ),
                    static_cast<int>(ownedText.size())
                );

                response.xselection.property = property;
            }
        }
    }

    // Tell the requesting application whether the conversion succeeded.
    XSendEvent(
        display,
        request.requestor,
        False,
        0,
        &response
    );

    XFlush(display);
    return true;
}

bool Clipboard::handleSelectionNotify(
    const XSelectionEvent& notification
) {
    if (!pastePending ||
        notification.selection != clipboardAtom) {
        return false;
    }

    // If UTF8_STRING is unavailable, retry with XA_STRING.
    if (notification.property == None) {
        if (!triedStringFallback &&
            pasteTarget == utf8StringAtom) {

            triedStringFallback = true;
            pasteTarget = XA_STRING;

            XConvertSelection(
                display,
                clipboardAtom,
                pasteTarget,
                propertyAtom,
                window,
                CurrentTime
            );

            XFlush(display);
        } else {
            finishPaste();
        }

        return true;
    }

    Atom actualType = None;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesAfter = 0;
    unsigned char* propertyData = nullptr;

    const int result = XGetWindowProperty(
        display,
        window,
        propertyAtom,
        0,
        1L << 20,
        False,
        AnyPropertyType,
        &actualType,
        &actualFormat,
        &itemCount,
        &bytesAfter,
        &propertyData
    );

    if (result != Success) {
        if (propertyData != nullptr) {
            XFree(propertyData);
        }

        XDeleteProperty(display, window, propertyAtom);
        finishPaste();
        return true;
    }

    if (actualType == incrAtom) {
        // The clipboard owner will transfer the data in chunks.
        // Deleting the initial INCR property acknowledges readiness.
        if (propertyData != nullptr) {
            XFree(propertyData);
        }

        XDeleteProperty(display, window, propertyAtom);

        incrementalPaste = true;
        XFlush(display);
        return true;
    }

    if (actualFormat == 8 && propertyData != nullptr) {
        receivedText.append(
            reinterpret_cast<const char*>(propertyData),
            itemCount
        );
    }

    if (propertyData != nullptr) {
        XFree(propertyData);
    }

    XDeleteProperty(display, window, propertyAtom);
    finishPaste();

    return true;
}

bool Clipboard::handlePropertyNotify(
    const XPropertyEvent& propertyEvent
) {
    if (!pastePending ||
        !incrementalPaste ||
        propertyEvent.window != window ||
        propertyEvent.atom != propertyAtom ||
        propertyEvent.state != PropertyNewValue) {
        return false;
    }

    Atom actualType = None;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesAfter = 0;
    unsigned char* propertyData = nullptr;

    const int result = XGetWindowProperty(
        display,
        window,
        propertyAtom,
        0,
        1L << 20,
        True,
        AnyPropertyType,
        &actualType,
        &actualFormat,
        &itemCount,
        &bytesAfter,
        &propertyData
    );

    if (result != Success) {
        if (propertyData != nullptr) {
            XFree(propertyData);
        }

        finishPaste();
        return true;
    }

    // An empty property terminates an incremental transfer.
    if (actualType == None || itemCount == 0) {
        if (propertyData != nullptr) {
            XFree(propertyData);
        }

        finishPaste();
        return true;
    }

    if (actualFormat == 8 && propertyData != nullptr) {
        receivedText.append(
            reinterpret_cast<const char*>(propertyData),
            itemCount
        );
    }

    if (propertyData != nullptr) {
        XFree(propertyData);
    }

    // XGetWindowProperty(delete=True) removes the property once
    // the complete chunk has been read. The owner can send the next one.
    XFlush(display);

    return true;
}

bool Clipboard::handleEvent(const XEvent& event) {
    if (display == nullptr) {
        return false;
    }

    switch (event.type) {
        case SelectionRequest:
            return handleSelectionRequest(event.xselectionrequest);

        case SelectionNotify:
            return handleSelectionNotify(event.xselection);

        case PropertyNotify:
            return handlePropertyNotify(event.xproperty);

        default:
            return false;
    }
}

bool Clipboard::takePastedText(std::string& text) {
    if (!pasteReady) {
        return false;
    }

    text = std::move(receivedText);
    receivedText.clear();
    pasteReady = false;

    return true;
}
