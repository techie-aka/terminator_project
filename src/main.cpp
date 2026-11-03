
#include "platform/X11Window.h"
#include "input/Input.h"
#include "terminal/TerminalConfig.h"
#include "terminal/Terminal.h"
#include "platform/Clipboard.h"

#include "process/PtySession.h"
#include "thread/ThreadSession.h"

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

#include <csignal>

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
        max(24, windowHeight * min(visibleLines, totalLines) / totalLines)
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

struct VisualRow {
    int logicalLine = 0;
    std::size_t startColumn = 0;
    std::size_t endColumn = 0;
    bool continuation = false;
};

constexpr int CONTENT_LEFT = 16;
constexpr int CONTENT_RIGHT = 12;
constexpr int TAB_BAR_HEIGHT = 44;
const std::string CONTINUATION_MARKER = "> ";

int getVisibleLines(const X11Window& window) {
    const int firstLineTop = START_Y - window.getFontAscent();
    const int usableHeight = window.getHeight() - firstLineTop - 8;
    return max(1, usableHeight / LINE_HEIGHT);
}

// Convert each logical buffer line into the rows that are actually visible.
// The source buffer is never changed: wrapping is purely a rendering concern.
std::vector<VisualRow> buildVisualRows(
    X11Window& window,
    const std::vector<std::string>& lines
) {
    std::vector<VisualRow> rows;

    const int contentWidth = max(
        1,
        window.getWidth() - window.getScrollbarWidth() -
        CONTENT_LEFT - CONTENT_RIGHT
    );

    for (int logicalLine = 0;
         logicalLine < static_cast<int>(lines.size());
         ++logicalLine) {

        const std::string& line = lines[logicalLine];

        if (line.empty()) {
            rows.push_back({logicalLine, 0, 0, false});
            continue;
        }

        std::size_t startColumn = 0;
        bool continuation = false;

        while (startColumn < line.size()) {
            const int markerWidth = continuation
                ? window.getTextWidth(CONTINUATION_MARKER) + 8
                : 0;

            const int availableWidth = max(1, contentWidth - markerWidth);

            // Find the longest substring that fits in this visual row.
            std::size_t low = startColumn + 1;
            std::size_t high = line.size();
            std::size_t best = startColumn + 1;

            while (low <= high) {
                const std::size_t middle = low + (high - low) / 2;
                const std::string candidate = line.substr(
                    startColumn, middle - startColumn
                );

                if (window.getTextWidth(candidate) <= availableWidth) {
                    best = middle;
                    low = middle + 1;
                } else {
                    if (middle == 0) {
                        break;
                    }
                    high = middle - 1;
                }
            }

            rows.push_back({
                logicalLine,
                startColumn,
                best,
                continuation
            });

            startColumn = best;
            continuation = true;
        }
    }

    return rows;
}

int getTextStartX(X11Window& window, const VisualRow& row) {
    if (!row.continuation) {
        return CONTENT_LEFT;
    }

    return CONTENT_LEFT + window.getTextWidth(CONTINUATION_MARKER) + 8;
}

// Keep the latest visual output visible when a tab is created or submitted to.
void scrollToBottom(Terminal& terminal, X11Window& window) {
    const auto rows = buildVisualRows(
        window, terminal.getBuffer().getLines()
    );

    terminal.setFirstVisibleLine(
        max(0, static_cast<int>(rows.size()) - getVisibleLines(window))
    );
}

void drawTabs(
    X11Window& window,
    const vector<unique_ptr<Terminal>>& terminals,
    int activeTab
) {
    const int tabCount = static_cast<int>(terminals.size());

    if (tabCount == 0) {
        return;
    }

    const int availableWidth = window.getWidth() - window.getScrollbarWidth();
    const int tabWidth = max(1, availableWidth / tabCount);
    const bool showCloseButtons = tabCount > 1;

    for (int i = 0; i < tabCount; ++i) {
        const int x = i * tabWidth;
        const string label = "bash  " +
            to_string(terminals[i]->getSession().getPid());

        window.drawTab(
            x,
            tabWidth,
            label,
            i == activeTab,
            showCloseButtons
        );
    }

    window.drawSeparator(TAB_BAR_HEIGHT);
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
    const auto rows = buildVisualRows(
        window, terminal.getBuffer().getLines()
    );

    if (rows.empty()) {
        return {};
    }

    const int contentWidth = window.getWidth() - window.getScrollbarWidth();
    mouseX = std::clamp(mouseX, 0, contentWidth - 1);

    const int firstLineTop = START_Y - window.getFontAscent();
    const int visibleRow = std::clamp(
        (mouseY - firstLineTop) / LINE_HEIGHT,
        0,
        getVisibleLines(window) - 1
    );

    const int visualIndex = std::clamp(
        terminal.getFirstVisibleLine() + visibleRow,
        0,
        static_cast<int>(rows.size()) - 1
    );

    const VisualRow& row = rows[visualIndex];
    const std::string& logicalText =
        terminal.getBuffer().getLines()[row.logicalLine];
    const std::string visibleText = logicalText.substr(
        row.startColumn, row.endColumn - row.startColumn
    );

    const int textStartX = getTextStartX(window, row);
    const std::size_t localColumn = mouseX <= textStartX
        ? 0
        : window.getTextColumnAtX(visibleText, mouseX - textStartX);

    return {
        row.logicalLine,
        min(row.endColumn, row.startColumn + localColumn)
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

// Draw one visual row, highlighting the prompt separately from command text.
void drawVisualText(
    X11Window& window,
    const std::string& logicalText,
    const VisualRow& row,
    int x,
    int baseline
) {
    const std::string visibleText = logicalText.substr(
        row.startColumn, row.endColumn - row.startColumn
    );

    if (row.continuation) {
        window.drawText(
            CONTINUATION_MARKER,
            CONTENT_LEFT,
            baseline,
            TextStyle::Accent
        );
    }

    if (row.startColumn == 0) {
        const std::string prompts[] = {
            "user@terminator> ",
            "continuation> "
        };

        for (const std::string& prompt : prompts) {
            if (logicalText.rfind(prompt, 0) == 0) {
                const std::size_t promptLength = min(
                    prompt.size(), visibleText.size()
                );

                window.drawText(
                    visibleText.substr(0, promptLength),
                    x,
                    baseline,
                    TextStyle::Prompt
                );

                window.drawText(
                    visibleText.substr(promptLength),
                    x + window.getTextWidth(visibleText.substr(0, promptLength)),
                    baseline,
                    TextStyle::Normal
                );
                return;
            }
        }
    }

    window.drawText(visibleText, x, baseline);
}

// Draw the active terminal's buffer using wrapped visual rows.
void drawTerminal(
    X11Window& window,
    const Terminal& terminal,
    const vector<unique_ptr<Terminal>>& terminals,
    int activeTab,
    const TextSelection& selection
) {
    window.clear();
    drawTabs(window, terminals, activeTab);

    const vector<string>& lines = terminal.getBuffer().getLines();
    const vector<VisualRow> rows = buildVisualRows(window, lines);
    const int totalRows = static_cast<int>(rows.size());
    const int visibleRows = getVisibleLines(window);
    const int maxFirstRow = max(0, totalRows - visibleRows);

    const int firstVisibleRow = clamp(
        terminal.getFirstVisibleLine(), 0, maxFirstRow
    );
    const int lastVisibleRow = min(
        totalRows, firstVisibleRow + visibleRows
    );

    const auto [selectionStart, selectionEnd] =
        getOrderedSelection(selection);

    for (int visualIndex = firstVisibleRow;
         visualIndex < lastVisibleRow;
         ++visualIndex) {

        const VisualRow& row = rows[visualIndex];
        const string& logicalText = lines[row.logicalLine];
        const int y = START_Y +
            (visualIndex - firstVisibleRow) * LINE_HEIGHT;
        const int textX = getTextStartX(window, row);
        const string visibleText = logicalText.substr(
            row.startColumn, row.endColumn - row.startColumn
        );

        bool selectedOnThisRow = false;
        std::size_t selectedStart = row.startColumn;
        std::size_t selectedEnd = row.endColumn;

        if (hasTextSelection(selection) &&
            row.logicalLine >= selectionStart.line &&
            row.logicalLine <= selectionEnd.line) {

            if (row.logicalLine == selectionStart.line) {
                selectedStart = max(selectedStart, selectionStart.column);
            }

            if (row.logicalLine == selectionEnd.line) {
                selectedEnd = min(selectedEnd, selectionEnd.column);
            }

            selectedStart = min(selectedStart, row.endColumn);
            selectedEnd = max(selectedStart, min(selectedEnd, row.endColumn));
            selectedOnThisRow = selectedStart < selectedEnd;
        }

        if (selectedOnThisRow) {
            window.drawTextWithSelection(
                visibleText,
                textX,
                y,
                selectedStart - row.startColumn,
                selectedEnd - row.startColumn
            );

            if (row.continuation) {
                window.drawText(
                    CONTINUATION_MARKER,
                    CONTENT_LEFT,
                    y,
                    TextStyle::Accent
                );
            }
        } else {
            drawVisualText(window, logicalText, row, textX, y);
        }

        // Cursor column is stored against the original logical buffer line.
        if (cursorVisible && row.logicalLine == static_cast<int>(lines.size()) - 1) {
            const std::size_t cursorColumn = min(
                terminal.getBuffer().getCursorColumn(), logicalText.size()
            );

            const bool lastVisualSegment =
                visualIndex + 1 == totalRows ||
                rows[visualIndex + 1].logicalLine != row.logicalLine;

            const bool cursorBelongsHere =
                cursorColumn >= row.startColumn &&
                (cursorColumn < row.endColumn ||
                 (lastVisualSegment && cursorColumn <= row.endColumn));

            if (cursorBelongsHere) {
                const std::string beforeCursor = logicalText.substr(
                    row.startColumn, cursorColumn - row.startColumn
                );

                window.drawCursor(
                    textX + window.getTextWidth(beforeCursor),
                    y
                );
            }
        }
    }

    window.drawScrollbar(totalRows, visibleRows, firstVisibleRow);
}

int main(int argc, char* argv[]) {
    // Thread mode is the default.
    bool useProcess = false;

    if (argc == 2 &&
        std::string(argv[1]) == "useProcess=True") {

        useProcess = true;

    } else if (argc != 1) {

        std::cerr << "Usage: " << argv[0]
                  << " [useProcess=True]\n";

        return 2;
    }

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

        std::unique_ptr<TabSession> session;

        if (useProcess) {
            session = std::make_unique<PtySession>();
        } else {
            session = std::make_unique<ThreadSession>();
        }

        auto terminal = std::make_unique<Terminal>(
            std::move(session)
        );

        if (!terminal->start()) {
            cerr << "Unable to start shell for the new tab\n";
            return false;
        }

        terminals.push_back(std::move(terminal));

        activeTab = static_cast<int>(terminals.size()) - 1;

        clearSelection(selection, draggingScrollbar);
        scrollToBottom(*terminals[activeTab], window);

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

            if (event.type == ConfigureNotify) {
                window.updateDimensions(
                    event.xconfigure.width,
                    event.xconfigure.height
                );
                redraw = true;
            } else if (event.type == Expose) {
                redraw = true;
            } else if (event.type == ButtonPress) {
                const int totalLines = static_cast<int>(buildVisualRows(
                    window,
                    terminals[activeTab]->getBuffer().getLines()
                ).size());

                const int visibleLines = getVisibleLines(window);
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
                    const int barX = window.getWidth() - window.getScrollbarWidth();
                    const int availableWidth = window.getWidth() - window.getScrollbarWidth();

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
                            totalLines, visibleLines, window.getHeight()
                        );

                        const int maxThumbY = window.getHeight() - thumbHeight;
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
                                    window.getHeight(), thumbHeight
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

                    const int totalLines = static_cast<int>(buildVisualRows(
                        window, terminal.getBuffer().getLines()
                    ).size());

                    const int visibleLines = getVisibleLines(window);
                    const int thumbHeight = getThumbHeight(
                        totalLines, visibleLines, window.getHeight()
                    );

                    // Dragging updates the viewport continuously.
                    terminal.setFirstVisibleLine(
                        getFirstLineFromThumbY(
                            event.xmotion.y - dragOffsetY,
                            totalLines, visibleLines,
                            window.getHeight(), thumbHeight
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

                    case InputAction::Interrupt: {
                        Terminal& terminal = *terminals[activeTab];

                        terminal.getSession().sendInterrupt();

                        redraw = true;
                        continue;
                    }

                    case InputAction::Suspend: {
                        Terminal& terminal = *terminals[activeTab];

                        terminal.getSession().sendSuspend();

                        redraw = true;
                        continue;
                    }

                    case InputAction::NoAction:
                        break;
                }

                // No application shortcut was detected.
                // Forward ordinary keyboard input to the active shell.
                Terminal& terminal = *terminals[activeTab];

                const int oldTotalLines = static_cast<int>(buildVisualRows(
                    window, terminal.getBuffer().getLines()
                ).size());

                const bool wasAtBottom =
                    terminal.getFirstVisibleLine() >=
                    max(0, oldTotalLines - getVisibleLines(window));

                const string keyInput = input.translateKeyPress(event.xkey);

                if (!keyInput.empty()) {
                    terminal.sendInput(keyInput);

                    cursorVisible = true;
                    lastBlink = chrono::steady_clock::now();

                    if (wasAtBottom) {
                        scrollToBottom(terminal, window);
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

                const int oldTotalLines = static_cast<int>(buildVisualRows(
                    window, terminal.getBuffer().getLines()
                ).size());

                const bool wasAtBottom =
                    terminal.getFirstVisibleLine() >=
                    max(0, oldTotalLines - getVisibleLines(window));

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
                    scrollToBottom(terminal, window);
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
