
#include "platform/X11Window.h"
#include "input/Input.h"
#include "terminal/TerminalConfig.h"
#include "terminal/Terminal.h"
#include "platform/Clipboard.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <sys/select.h>
#include <unistd.h>
#include <vector>
#include <utility>

using namespace std;

bool cursorVisible = true;

struct TextPosition {
    int line = 0;
    std::size_t column = 0;
};

struct TextSelection {
    TextPosition anchor;
    TextPosition focus;
    bool dragging = false;
};

// State used while dragging the scrollbar thumb.
bool draggingScrollbar = false;
int dragOffsetY = 0;

// Compare positions in terminal reading order.
bool isPositionBefore(
    const TextPosition& first,
    const TextPosition& second
) {
    if (first.line != second.line) {
        return first.line < second.line;
    }

    return first.column < second.column;
}

// Return selection endpoints in reading order.
std::pair<TextPosition, TextPosition> getOrderedSelection(
    const TextSelection& selection
) {
    if (isPositionBefore(selection.focus, selection.anchor)) {
        return {selection.focus, selection.anchor};
    }

    return {selection.anchor, selection.focus};
}

bool hasTextSelection(const TextSelection& selection) {
    return (selection.anchor.line != selection.focus.line ||
            selection.anchor.column != selection.focus.column);
}

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

void clearSelection(TextSelection& selection, bool& draggingScrollbar) {
    selection.anchor = {};
    selection.focus = {};
    selection.dragging = false;
    draggingScrollbar = false;
}

TextPosition getTextPositionAt(
    X11Window& window,
    const Terminal& terminal,
    int mouseX,
    int mouseY
) {
    const auto& lines = terminal.getBuffer().getLines();

    if (lines.empty()) {
        return {};
    }

    // Restrict selection to the terminal content area.
    const int contentWidth =
        WINDOW_WIDTH - window.getScrollbarWidth();

    mouseX = std::clamp(mouseX, 0, contentWidth - 1);

    // START_Y is the XDrawString baseline, not the top of the first row.
    // Use the font ascent so the mouse hit area matches the rendered glyphs.
    const int firstLineTop = START_Y - window.getFontAscent();
    const int visibleRow = std::clamp(
        (mouseY - firstLineTop) / LINE_HEIGHT,
        0,
        getVisibleLines() - 1
    );

    const int logicalLine = std::clamp(
        terminal.getFirstVisibleLine() + visibleRow,
        0,
        static_cast<int>(lines.size()) - 1
    );

    // Convert horizontal pixels into the nearest character boundary.
    const std::size_t column = window.getTextColumnAtX(
        lines[logicalLine],
        mouseX
    );

    return {
        logicalLine,
        column
    };
}

std::string getSelectedText(
    const Terminal& terminal,
    const TextSelection& selection
) {
    if (!hasTextSelection(selection)) {
        return "";
    }

    const auto& lines = terminal.getBuffer().getLines();

    const auto [start, end] = getOrderedSelection(selection);

    std::string result;

    for (int lineIndex = start.line;
         lineIndex <= end.line;
         ++lineIndex) {

        if (lineIndex < 0 ||
            lineIndex >= static_cast<int>(lines.size())) {
            break;
        }

        const std::string& currentLine = lines[lineIndex];

        if (start.line == end.line) {
            // Selection is entirely within one line.
            const std::size_t begin = std::min(
                start.column, currentLine.size()
            );

            const std::size_t finish = std::min(
                end.column, currentLine.size()
            );

            if (finish > begin) {
                result += currentLine.substr(
                    begin, finish - begin
                );
            }

            break;
        }

        if (lineIndex == start.line) {
            // First line: from the starting column to its end.
            const std::size_t begin = std::min(
                start.column, currentLine.size()
            );

            result += currentLine.substr(begin);
        } else if (lineIndex == end.line) {
            // Final line: from the beginning to the ending column.
            const std::size_t finish = std::min(
                end.column, currentLine.size()
            );

            result += currentLine.substr(0, finish);
        } else {
            // Intermediate lines are copied completely.
            result += currentLine;
        }

        // Preserve line boundaries in the copied text.
        if (lineIndex < end.line) {
            result += '\n';
        }
    }

    return result;
}

// Draw the active terminal's own buffer and scrollbar position.
void drawTerminal(X11Window& window,
                  const Terminal& terminal,
                  const vector<unique_ptr<Terminal>>& terminals,
                  int activeTab,
                const TextSelection& selection) {
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

        // Draw this line with selection highlighting when it intersects
        // the selected range.
        if (hasTextSelection(selection)) {
            const auto [selectionStart, selectionEnd] =
                getOrderedSelection(selection);

            if (line >= selectionStart.line &&
                line <= selectionEnd.line) {

                std::size_t highlightStart = 0;
                std::size_t highlightEnd = lines[line].size();

                if (line == selectionStart.line) {
                    highlightStart = selectionStart.column;
                }

                if (line == selectionEnd.line) {
                    highlightEnd = selectionEnd.column;
                }

                highlightStart = std::min(
                    highlightStart, lines[line].size()
                );

                highlightEnd = std::min(
                    highlightEnd, lines[line].size()
                );

                window.drawTextWithSelection(
                    lines[line],
                    0,
                    y,
                    highlightStart,
                    highlightEnd
                );
            } else {
                window.drawText(lines[line], 0, y);
            }
        } else {
            window.drawText(lines[line], 0, y);
        }

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

    TextSelection selection;

    // Each tab creates and owns an independent Bash process and buffer.
    auto createTab = [&]() -> bool {
        auto terminal = make_unique<Terminal>();

        if (!terminal->start()) {
            cerr << "Unable to start Bash for the new tab\n";
            return false;
        }

        terminals.push_back(move(terminal));
        activeTab = static_cast<int>(terminals.size()) - 1;

        // A selection belongs to the tab where it was made.
        clearSelection(selection, draggingScrollbar);

        scrollToBottom(*terminals[activeTab]);

        return true;
    };

    // Close a particular tab and keep the active-tab index valid.
    auto closeTabAt = [&](int tabIndex) -> bool {
        if (tabIndex < 0 ||
            tabIndex >= static_cast<int>(terminals.size())) {
            return !terminals.empty();
        }

        terminals.erase(terminals.begin() + tabIndex);

        // No tabs remain, so the application can exit.
        if (terminals.empty()) {
            return false;
        }

        if (tabIndex < activeTab) {
            --activeTab;
        } else if (tabIndex == activeTab) {
            activeTab = min(
                activeTab,
                static_cast<int>(terminals.size()) - 1
            );
        }

        clearSelection(selection, draggingScrollbar);
        cursorVisible = true;

        return true;
    };

    // Close whichever tab is currently active.
    auto closeTab = [&]() -> bool {
        return closeTabAt(activeTab);
    };

    if (!createTab()) {
        return 1;
    }

    Display* display = window.getDisplay();
    const int xConnection = ConnectionNumber(display);

    // Manage X11 clipboard ownership and asynchronous paste requests.
    Clipboard clipboard(display, window.getWindow());

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

            // Clipboard operations use asynchronous X11 selection events.
            // Process them before the ordinary mouse/keyboard event handling.
            if (clipboard.handleEvent(event)) {
                std::string pastedText;

                if (clipboard.takePastedText(pastedText)) {
                    if (!pastedText.empty() && !terminals.empty()) {
                        // Send the pasted data to the currently active tab's PTY.
                        terminals[activeTab]->sendInput(pastedText);

                        cursorVisible = true;
                        lastBlink = chrono::steady_clock::now();

                        redraw = true;
                    }
                }

                continue;
            }

            if (event.type == Expose) {
                redraw = true;
            } else if (event.type == ButtonPress) {
                const int totalLines = static_cast<int>(
                    terminals[activeTab]->getBuffer().getLines().size()
                );

                const int visibleLines = getVisibleLines();
                const int maxFirstLine = max(0, totalLines - visibleLines);
                const int terminalContentTop = START_Y - window.getFontAscent();

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
                    if (event.xbutton.y < terminalContentTop &&
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

                            clearSelection(selection, draggingScrollbar);
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
                    } else if (event.xbutton.y >= terminalContentTop &&
                               event.xbutton.x < barX) {
                        // Begin a normal left-button text drag. X11 is configured
                        // for Button1MotionMask, so no explicit pointer grab is needed.
                        Terminal& terminal = *terminals[activeTab];

                        selection.anchor = getTextPositionAt(
                            window,
                            terminal,
                            event.xbutton.x,
                            event.xbutton.y
                        );

                        selection.focus = selection.anchor;
                        selection.dragging = true;
                        draggingScrollbar = false;

                        redraw = true;
                    }
                }
            } else if (event.type == MotionNotify) {
                if (draggingScrollbar) {
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
                            totalLines, visibleLines,
                            WINDOW_HEIGHT, thumbHeight
                        )
                    );

                    redraw = true;
                } else if (selection.dragging &&
                           (event.xmotion.state & Button1Mask)) {
                    // Extend the selection only while the left button is held.
                    Terminal& terminal = *terminals[activeTab];

                    selection.focus = getTextPositionAt(
                        window,
                        terminal,
                        event.xmotion.x,
                        event.xmotion.y
                    );
                    redraw = true;
                }
            } else if (event.type == ButtonRelease) {
                if (event.xbutton.button == Button1) {
                    // Keep the selected range visible after the drag ends.
                    draggingScrollbar = false;
                    selection.dragging = false;
                }
            } else if (event.type == KeyPress) {

                // Ask Input to identify application-level keyboard shortcuts.
                const InputAction action = input.getAction(event.xkey);

                switch (action) {

                    case InputAction::NewTab:
                        if (createTab()) {
                            redraw = true;
                        }
                        continue;

                    case InputAction::CloseTab:
                        if (!closeTab()) {
                            return 0;
                        }

                        redraw = true;
                        continue;

                    case InputAction::NextTab:
                        if (terminals.size() > 1) {
                            // Move forward and wrap around to the first tab.
                            activeTab = (activeTab + 1) %
                                static_cast<int>(terminals.size());

                            clearSelection(selection, draggingScrollbar);
                            cursorVisible = true;
                            lastBlink = chrono::steady_clock::now();

                            redraw = true;
                        }
                        continue;

                    case InputAction::PreviousTab:
                        if (terminals.size() > 1) {
                            // Move backward and wrap around to the last tab.
                            activeTab =
                                (activeTab - 1 + static_cast<int>(terminals.size())) %
                                static_cast<int>(terminals.size());

                            clearSelection(selection, draggingScrollbar);
                            cursorVisible = true;
                            lastBlink = chrono::steady_clock::now();

                            redraw = true;
                        }
                        continue;

                    case InputAction::CopySelection: {
                        // Copy only when the user has selected a non-empty range.
                        const Terminal& terminal = *terminals[activeTab];

                        if (hasTextSelection(selection)) {
                            const std::string selectedText =
                                getSelectedText(terminal, selection);

                            if (!clipboard.copy(selectedText)) {
                                cerr << "Unable to acquire the X11 CLIPBOARD selection\n";
                            }
                        }
                        continue;
                    }

                    case InputAction::PasteClipboard:
                        // Clipboard contents arrive later through SelectionNotify.
                        clipboard.requestPaste();
                        continue;

                    case InputAction::NoAction:
                        break;
                }

                // No application shortcut was detected.
                // Forward ordinary keyboard input to the active shell.
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

                    if (wasAtBottom) {
                        scrollToBottom(terminal);
                    }

                    redraw = true;
                }
            }
        }

        // Collect output from every tab, including inactive tabs.
        bool createRequestedTab = false;
        vector<int> terminatedTabs;

        if (ready > 0) {
            for (int i = 0; i < static_cast<int>(terminals.size()); ++i) {
                Terminal& terminal = *terminals[i];
                const int fd = terminal.getSession().getFileDescriptor();

                if (fd < 0 || fd >= FD_SETSIZE || !FD_ISSET(fd, &readSet)) {
                    continue;
                }

                const int oldTotalLines = static_cast<int>(
                    terminal.getBuffer().getLines().size()
                );

                const bool wasAtBottom =
                    terminal.getFirstVisibleLine() >=
                    max(0, oldTotalLines - getVisibleLines());

                // Read pending output. This may detect that the shell has exited.
                if (terminal.receiveOutput()) {
                    createRequestedTab = true;
                }

                // PtySession::readOutput() sets the descriptor to -1 on PTY closure.
                if (terminal.getSession().getFileDescriptor() == -1) {
                    terminatedTabs.push_back(i);
                    redraw = true;
                    continue;
                }

                if (wasAtBottom) {
                    scrollToBottom(terminal);
                }

                redraw = true;
            }
        }

        // Remove terminated tabs in reverse order to preserve the remaining indices.
        for (auto it = terminatedTabs.rbegin();
            it != terminatedTabs.rend();
            ++it) {
            closeTabAt(*it);
        }

        // Preserve tab creation requests received from shell output.
        if (createRequestedTab) {
            if (createTab()) {
                redraw = true;
            }
        }

        // If every shell has exited, close the application.
        if (terminals.empty()) {
            return 0;
        }

        if (redraw) {
            drawTerminal(
                window,
                *terminals[activeTab],
                terminals,
                activeTab,
                selection
            );

            XFlush(display);
        }
    }

    return 0;
}
