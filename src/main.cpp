
#include "platform/X11Window.h"
#include "input/Input.h"
#include "terminal/TerminalConfig.h"
#include "terminal/Terminal.h"

#include <X11/keysym.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <sys/select.h>
#include <unistd.h>
#include <vector>

using namespace std;

bool cursorVisible = true;

// State used while dragging the scrollbar thumb.
bool draggingScrollbar = false;
int dragOffsetY = 0;

// Calculate the thumb height using the same formula as X11Window.
int getThumbHeight(int totalLines, int visibleLines, int windowHeight) {
    if (totalLines <= 0) {
        return windowHeight;
    }

    return min(
        windowHeight,
        max(20, windowHeight * min(visibleLines, totalLines) / totalLines)
    );
}

// Convert a thumb position into the first visible line.
int getFirstLineFromThumbY(int thumbY, int totalLines, int visibleLines,
                           int windowHeight, int thumbHeight) {
    const int maxFirstLine = max(0, totalLines - visibleLines);
    const int maxThumbY = windowHeight - thumbHeight;

    if (maxFirstLine == 0 || maxThumbY <= 0) {
        return 0;
    }

    thumbY = clamp(thumbY, 0, maxThumbY);

    return thumbY * maxFirstLine / maxThumbY;
}

int getVisibleLines() {
    return max(1, (WINDOW_HEIGHT - START_Y) / LINE_HEIGHT);
}

// Keep the latest output visible when a terminal is created or submitted to.
void scrollToBottom(Terminal& terminal) {
    const int totalLines = static_cast<int>(
        terminal.getBuffer().getLines().size()
    );

    terminal.setFirstVisibleLine(
        max(0, totalLines - getVisibleLines())
    );
}

// Draw a simple tab bar without adding tab-specific methods to X11Window yet.
void drawTabs(X11Window& window,
              const vector<unique_ptr<Terminal>>& terminals,
              int activeTab) {
    const int tabCount = static_cast<int>(terminals.size());

    if (tabCount == 0) {
        return;
    }

    const int availableWidth = WINDOW_WIDTH - window.getScrollbarWidth();
    const int tabWidth = max(1, availableWidth / tabCount);
    const bool showCloseButtons = tabCount > 1;

    for (int i = 0; i < tabCount; ++i) {
        const int x = i * tabWidth;

        const string label =
            (i == activeTab ? "[*] bash " : "[ ] bash ") +
            to_string(terminals[i]->getSession().getPid());

        window.drawText(label, x + 5, 20);

        // Show a close icon on every tab only when multiple tabs exist.
        if (showCloseButtons) {
            const int closeX = x + tabWidth - 18;
            window.drawText("x", closeX, 20);
        }
    }
}

// Draw the active terminal's own buffer and scrollbar position.
void drawTerminal(X11Window& window,
                  const Terminal& terminal,
                  const vector<unique_ptr<Terminal>>& terminals,
                  int activeTab) {
    window.clear();

    drawTabs(window, terminals, activeTab);

    const vector<string>& lines = terminal.getBuffer().getLines();
    const int totalLines = static_cast<int>(lines.size());
    const int visibleLines = getVisibleLines();
    const int maxFirstLine = max(0, totalLines - visibleLines);

    const int firstVisibleLine = clamp(
        terminal.getFirstVisibleLine(), 0, maxFirstLine
    );

    const int lastVisibleLine = min(
        totalLines, firstVisibleLine + visibleLines
    );

    int y = START_Y;

    // Draw only the lines inside this terminal's viewport.
    for (int line = firstVisibleLine; line < lastVisibleLine; ++line) {
        window.drawText(lines[line], 0, y);

        // Show the cursor at the current end of the terminal buffer.
        if (cursorVisible && line == totalLines - 1) {
            const size_t cursorColumn = min(
                terminal.getBuffer().getCursorColumn(),
                lines[line].size()
            );

            const string textBeforeCursor = lines[line].substr(0, cursorColumn);
            const int cursorX = window.getTextWidth(textBeforeCursor);

            window.drawText("_", cursorX, y);
        }

        y += LINE_HEIGHT;
    }

    // Keep the scrollbar synchronized with this terminal's viewport.
    window.drawScrollbar(totalLines, visibleLines, firstVisibleLine);
}

int main() {
    X11Window window(WINDOW_WIDTH, WINDOW_HEIGHT);

    if (!window.initialize()) {
        return 1;
    }

    window.show();

    Input input;
    vector<unique_ptr<Terminal>> terminals;
    int activeTab = 0;

    // Each tab creates and owns an independent Bash process and buffer.
    auto createTab = [&]() -> bool {
        auto terminal = make_unique<Terminal>();

        if (!terminal->start()) {
            cerr << "Unable to start Bash for the new tab\n";
            return false;
        }

        terminals.push_back(move(terminal));
        activeTab = static_cast<int>(terminals.size()) - 1;

        scrollToBottom(*terminals[activeTab]);

        return true;
    };

    // Close the active tab and release its terminal resources.
    auto closeTab = [&]() -> bool {
        if (terminals.empty()) {
            return false;
        }

        terminals.erase(terminals.begin() + activeTab);

        // Closing the final tab exits the application.
        if (terminals.empty()) {
            return false;
        }

        // Select a valid remaining tab.
        activeTab = min(activeTab, static_cast<int>(terminals.size()) - 1);

        draggingScrollbar = false;
        cursorVisible = true;

        return true;
    };

    if (!createTab()) {
        return 1;
    }

    Display* display = window.getDisplay();
    const int xConnection = ConnectionNumber(display);
    XEvent event;

    auto lastBlink = chrono::steady_clock::now();

    while (true) {
        // Monitor X11 and every terminal's PTY without blocking the interface.
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(xConnection, &readSet);

        int maxFd = xConnection;

        for (const auto& terminal : terminals) {
            const int fd = terminal->getSession().getFileDescriptor();

            if (fd >= 0 && fd < FD_SETSIZE) {
                FD_SET(fd, &readSet);
                maxFd = max(maxFd, fd);
            }
        }

        const auto now = chrono::steady_clock::now();
        const auto elapsed = chrono::duration_cast<chrono::milliseconds>(
            now - lastBlink
        ).count();

        const long remaining = max(0L, CURSOR_BLINK_INTERVAL_MS - elapsed);

        timeval timeout;
        timeout.tv_sec = remaining / 1000;
        timeout.tv_usec = (remaining % 1000) * 1000;

        // Process already-buffered X11 events without waiting.
        if (XPending(display) > 0) {
            timeout.tv_sec = 0;
            timeout.tv_usec = 0;
        }

        const int ready = select(
            maxFd + 1, &readSet, nullptr, nullptr, &timeout
        );

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }

            cerr << "Error waiting for X11 or terminal events\n";
            return 1;
        }

        bool redraw = false;

        if (ready == 0 && XPending(display) == 0) {
            cursorVisible = !cursorVisible;
            lastBlink = chrono::steady_clock::now();
            redraw = true;
        }

        while (XPending(display) > 0) {
            XNextEvent(display, &event);

            if (event.type == Expose) {
                redraw = true;
            } else if (event.type == ButtonPress) {
                const int totalLines = static_cast<int>(
                    terminals[activeTab]->getBuffer().getLines().size()
                );

                const int visibleLines = getVisibleLines();
                const int maxFirstLine = max(0, totalLines - visibleLines);

                // Mouse-wheel up moves toward older lines.
                if (event.xbutton.button == Button4) {
                    Terminal& terminal = *terminals[activeTab];

                    terminal.setFirstVisibleLine(
                        max(0, terminal.getFirstVisibleLine() - 3)
                    );

                    redraw = true;
                } else if (event.xbutton.button == Button5) {
                    Terminal& terminal = *terminals[activeTab];

                    terminal.setFirstVisibleLine(
                        min(maxFirstLine, terminal.getFirstVisibleLine() + 3)
                    );

                    redraw = true;
                } else if (event.xbutton.button == Button1) {
                    const int barX = WINDOW_WIDTH - window.getScrollbarWidth();
                    const int availableWidth = WINDOW_WIDTH - window.getScrollbarWidth();

                    // A click in the tab-bar area selects or closes a tab.
                    if (event.xbutton.y < START_Y &&
                        event.xbutton.x >= 0 &&
                        event.xbutton.x < availableWidth) {

                        const int tabCount = static_cast<int>(terminals.size());
                        const int tabWidth = max(1, availableWidth / tabCount);
                        const int clickedTab = event.xbutton.x / tabWidth;
                        const int positionInTab = event.xbutton.x % tabWidth;

                        if (clickedTab >= 0 && clickedTab < tabCount) {
                            // The close icon occupies the rightmost 18 pixels of each tab.
                            const bool clickedCloseButton =
                                tabCount > 1 && positionInTab >= tabWidth - 18;

                            if (clickedCloseButton) {
                                activeTab = clickedTab;

                                if (!closeTab()) {
                                    return 0;
                                }
                            } else {
                                activeTab = clickedTab;
                            }

                            draggingScrollbar = false;
                            cursorVisible = true;
                            redraw = true;
                        }
                    } else if (event.xbutton.x >= barX) {
                        Terminal& terminal = *terminals[activeTab];

                        const int thumbHeight = getThumbHeight(
                            totalLines, visibleLines, WINDOW_HEIGHT
                        );

                        const int maxThumbY = WINDOW_HEIGHT - thumbHeight;
                        const int currentThumbY = maxFirstLine == 0
                            ? 0
                            : maxThumbY * terminal.getFirstVisibleLine()
                                / maxFirstLine;

                        if (event.xbutton.y >= currentThumbY &&
                            event.xbutton.y < currentThumbY + thumbHeight) {
                            // Start dragging while preserving the click position.
                            draggingScrollbar = true;
                            dragOffsetY = event.xbutton.y - currentThumbY;
                        } else {
                            // Clicking the track moves the thumb toward that position.
                            const int targetThumbY =
                                event.xbutton.y - thumbHeight / 2;

                            terminal.setFirstVisibleLine(
                                getFirstLineFromThumbY(
                                    targetThumbY, totalLines, visibleLines,
                                    WINDOW_HEIGHT, thumbHeight
                                )
                            );
                        }

                        redraw = true;
                    }
                }
            } else if (event.type == MotionNotify && draggingScrollbar) {
                Terminal& terminal = *terminals[activeTab];

                const int totalLines = static_cast<int>(
                    terminal.getBuffer().getLines().size()
                );

                const int visibleLines = getVisibleLines();
                const int thumbHeight = getThumbHeight(
                    totalLines, visibleLines, WINDOW_HEIGHT
                );

                // Dragging updates the viewport continuously.
                terminal.setFirstVisibleLine(
                    getFirstLineFromThumbY(
                        event.xmotion.y - dragOffsetY,
                        totalLines, visibleLines, WINDOW_HEIGHT, thumbHeight
                    )
                );

                redraw = true;
            } else if (event.type == ButtonRelease) {
                // Stop dragging when the mouse button is released.
                if (event.xbutton.button == Button1) {
                    draggingScrollbar = false;
                }
            } else if (event.type == KeyPress) {
                const KeySym keySymbol = XLookupKeysym(&event.xkey, 0);

                // Ctrl+Shift+T creates a new tab.
                if ((event.xkey.state & ControlMask) &&
                    (event.xkey.state & ShiftMask) &&
                    (keySymbol == XK_t || keySymbol == XK_T)) {
                    if (createTab()) {
                        redraw = true;
                    }

                    continue;
                }

                // Ctrl+Shift+W closes the active tab.
                if ((event.xkey.state & ControlMask) &&
                    (event.xkey.state & ShiftMask) &&
                    (keySymbol == XK_w || keySymbol == XK_W)) {
                    if (!closeTab()) {
                        return 0;
                    }

                    redraw = true;
                    continue;
                }

                Terminal& terminal = *terminals[activeTab];

                const int oldTotalLines = static_cast<int>(
                    terminal.getBuffer().getLines().size()
                );

                const bool wasAtBottom =
                    terminal.getFirstVisibleLine() >=
                    max(0, oldTotalLines - getVisibleLines());

                const string keyInput = input.translateKeyPress(event.xkey);

                if (!keyInput.empty()) {
                    terminal.sendInput(keyInput);

                    cursorVisible = true;
                    lastBlink = chrono::steady_clock::now();

                    // Keep following the prompt only when already at the bottom.
                    if (wasAtBottom) {
                        scrollToBottom(terminal);
                    }

                    redraw = true;
                }
            }
        }

        // Collect output from every tab, including inactive tabs.
        bool createRequestedTab = false;

        if (ready > 0) {
            for (auto& terminal : terminals) {
                const int fd = terminal->getSession().getFileDescriptor();

                if (fd < 0 || fd >= FD_SETSIZE || !FD_ISSET(fd, &readSet)) {
                    continue;
                }

                const int oldTotalLines = static_cast<int>(
                    terminal->getBuffer().getLines().size()
                );

                const bool wasAtBottom =
                    terminal->getFirstVisibleLine() >=
                    max(0, oldTotalLines - getVisibleLines());

                if (terminal->receiveOutput()) {
                    createRequestedTab = true;
                }

                if (wasAtBottom) {
                    scrollToBottom(*terminal);
                }

                redraw = true;
            }
        }

        // Both the keyboard shortcut and the shell's newtab command
        // use the same tab-creation function.
        if (createRequestedTab) {
            if (createTab()) {
                redraw = true;
            }
        }

        if (redraw) {
            drawTerminal(
                window, *terminals[activeTab], terminals, activeTab
            );

            XFlush(display);
        }
    }

    return 0;
}
