
#include "shell/LineEditor.h"
#include "shell/History.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <iostream>
#include <string>
#include <vector>

#include <termios.h>
#include <unistd.h>

namespace {

enum class InputStatus {
    Complete,
    Incomplete,
    BackslashContinuation
};

enum class ContextKind {
    Normal,
    CommandSubstitution,
    Backtick
};

struct ScanContext {
    ContextKind kind;
    char quote;
    int parenthesisDepth;
};

// Inspect input without executing it.
// Separate contexts allow quotes inside command substitutions to be
// tracked independently from the surrounding command.
InputStatus analyzeInput(const std::string& input) {
    std::vector<ScanContext> contexts;
    contexts.push_back({ContextKind::Normal, '\0', 0});

    bool escaped = false;

    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        ScanContext& context = contexts.back();

        if (escaped) {
            escaped = false;
            continue;
        }

        // Single-quoted content is literal.
        if (context.quote == '\'') {
            if (c == '\'') {
                context.quote = '\0';
            }
            continue;
        }

        if (c == '\\') {
            if (i + 1 == input.size()) {
                return InputStatus::BackslashContinuation;
            }

            const char next = input[i + 1];

            if (context.quote == '"') {
                if (next == '$' || next == '`' || next == '"' ||
                    next == '\\' || next == '\n') {
                    escaped = true;
                    continue;
                }
            } else {
                escaped = true;
                continue;
            }
        }

        if (context.kind == ContextKind::Backtick && c == '`') {
            contexts.pop_back();
            continue;
        }

        if (context.quote == '"') {
            if (c == '"') {
                context.quote = '\0';
                continue;
            }

            if (c == '$' && i + 1 < input.size() &&
                input[i + 1] == '(') {
                contexts.push_back({
                    ContextKind::CommandSubstitution, '\0', 1
                });
                ++i;
                continue;
            }

            if (c == '`') {
                contexts.push_back({
                    ContextKind::Backtick, '\0', 0
                });
                continue;
            }

            continue;
        }

        if (c == '\'') {
            context.quote = '\'';
            continue;
        }

        if (c == '"') {
            context.quote = '"';
            continue;
        }

        if (c == '$' && i + 1 < input.size() &&
            input[i + 1] == '(') {
            contexts.push_back({
                ContextKind::CommandSubstitution, '\0', 1
            });
            ++i;
            continue;
        }

        if (c == '`') {
            contexts.push_back({
                ContextKind::Backtick, '\0', 0
            });
            continue;
        }

        if (context.kind == ContextKind::CommandSubstitution) {
            if (c == '(') {
                ++context.parenthesisDepth;
            } else if (c == ')') {
                --context.parenthesisDepth;

                if (context.parenthesisDepth == 0) {
                    contexts.pop_back();
                }
            }
        }
    }

    for (const ScanContext& context : contexts) {
        if (context.quote != '\0') {
            return InputStatus::Incomplete;
        }
    }

    if (contexts.size() > 1) {
        return InputStatus::Incomplete;
    }

    return InputStatus::Complete;
}

// Read one byte, retrying if the system call is interrupted.
bool readByte(char& byte) {
    while (true) {
        const ssize_t count = ::read(STDIN_FILENO, &byte, 1);

        if (count == 1) {
            return true;
        }

        if (count == -1 && errno == EINTR) {
            continue;
        }

        return false;
    }
}

// Read complete CSI/SS3 sequences:
// ESC [ D, ESC [ 3 ~, ESC O H, etc.
bool readEscapeSequence(std::string& sequence) {
    sequence.clear();

    char introducer = '\0';

    if (!readByte(introducer)) {
        return false;
    }

    if (introducer == '[') {
        while (sequence.size() < 16) {
            char c = '\0';

            if (!readByte(c)) {
                return false;
            }

            sequence += c;

            // CSI final bytes are in the range 0x40-0x7e.
            if (c >= '@' && c <= '~') {
                return true;
            }
        }

        return false;
    }

    if (introducer == 'O') {
        char finalByte = '\0';

        if (!readByte(finalByte)) {
            return false;
        }

        sequence = "O";
        sequence += finalByte;
        return true;
    }

    return false;
}

} // namespace

bool LineEditor::readCommandLine(
    const History& history,
    std::string& line
) {
    // Save the terminal settings so they can be restored after input.
    termios originalSettings{};

    if (tcgetattr(STDIN_FILENO, &originalSettings) == -1) {
        return false;
    }

    // Disable canonical input and automatic echo.
    // We read and display every character ourselves.
    termios settings = originalSettings;
    settings.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    settings.c_cc[VMIN] = 1;
    settings.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &settings) == -1) {
        return false;
    }

    const auto restoreTerminal = [&]() {
        return tcsetattr(
            STDIN_FILENO, TCSANOW, &originalSettings
        ) != -1;
    };

    // History navigation state.
    std::vector<std::string> entries = history.getEntries();
    std::size_t historyPosition = entries.size();
    std::string draft;

    // physicalLine is the current line being edited.
    // logicalCommand accumulates all lines of a multiline command.
    std::string physicalLine;
    std::string logicalCommand;

    // Position where the next character will be inserted.
    std::size_t cursorPosition = 0;

    bool continuing = false;
    bool joinNextLine = false;

    line.clear();

    const std::string primaryPrompt = "user@terminator> ";
    const std::string continuationPrompt = "continuation> ";

    std::string currentPrompt = primaryPrompt;

    // Redraw the entire editable line and return the visual cursor
    // to its logical position using backspace characters.
    const auto redrawLine = [&]() {
        std::cout << '\r' << "\033[K"
                  << currentPrompt << physicalLine;

        for (std::size_t i = cursorPosition;
             i < physicalLine.size(); ++i) {
            std::cout << '\b';
        }

        std::cout << std::flush;
    };

    while (true) {
        char character = '\0';

        if (!readByte(character)) {
            restoreTerminal();
            return false;
        }

        // Enter submits the current physical line to the multiline scanner.
        if (character == '\r' || character == '\n') {
            // Display the complete line before submitting it, even when
            // the cursor is currently somewhere in the middle.
            std::cout << '\r' << "\033[K"
                      << currentPrompt << physicalLine
                      << '\n' << std::flush;

            if (!logicalCommand.empty() && !joinNextLine) {
                logicalCommand += '\n';
            }

            logicalCommand += physicalLine;

            joinNextLine = false;
            physicalLine.clear();
            cursorPosition = 0;

            const InputStatus status = analyzeInput(logicalCommand);

            if (status == InputStatus::BackslashContinuation) {
                // Remove the trailing backslash. The next physical line
                // will be joined directly to this one.
                logicalCommand.pop_back();

                joinNextLine = true;
                continuing = true;
                currentPrompt = continuationPrompt;

                std::cout << currentPrompt << std::flush;
                continue;
            }

            if (status == InputStatus::Incomplete) {
                continuing = true;
                currentPrompt = continuationPrompt;

                std::cout << currentPrompt << std::flush;
                continue;
            }

            line = logicalCommand;
            break;
        }

        // Ctrl+D exits the shell when the current physical line is empty.
        if (character == 4 && physicalLine.empty()) {
            restoreTerminal();
            return false;
        }

        // Backspace: delete the character immediately before the cursor.
        if (character == 127 || character == '\b') {
            if (cursorPosition > 0) {
                physicalLine.erase(cursorPosition - 1, 1);
                --cursorPosition;
                redrawLine();
            }

            continue;
        }

        // Ctrl+A: move to the beginning of the line.
        if (character == '\x01') {
            cursorPosition = 0;
            redrawLine();
            continue;
        }

        // Ctrl+E: move to the end of the line.
        if (character == '\x05') {
            cursorPosition = physicalLine.size();
            redrawLine();
            continue;
        }

        // Ctrl+U: delete from the beginning to the cursor.
        if (character == '\x15') {
            physicalLine.erase(0, cursorPosition);
            cursorPosition = 0;
            redrawLine();
            continue;
        }

        // Ctrl+K: delete from the cursor to the end.
        if (character == '\x0b') {
            physicalLine.erase(cursorPosition);
            redrawLine();
            continue;
        }

        // Ctrl+W: delete the previous word and its preceding whitespace.
        if (character == '\x17') {
            std::size_t wordStart = cursorPosition;

            while (wordStart > 0 &&
                   std::isspace(static_cast<unsigned char>(
                       physicalLine[wordStart - 1]))) {
                --wordStart;
            }

            while (wordStart > 0 &&
                   !std::isspace(static_cast<unsigned char>(
                       physicalLine[wordStart - 1]))) {
                --wordStart;
            }

            physicalLine.erase(
                wordStart, cursorPosition - wordStart
            );

            cursorPosition = wordStart;
            redrawLine();
            continue;
        }

        // Arrow, Home, End and Delete keys arrive as escape sequences.
        if (character == '\x1b') {
            std::string sequence;

            if (!readEscapeSequence(sequence)) {
                continue;
            }

            if (sequence == "D") {
                // Left arrow.
                if (cursorPosition > 0) {
                    --cursorPosition;
                    redrawLine();
                }
            } else if (sequence == "C") {
                // Right arrow.
                if (cursorPosition < physicalLine.size()) {
                    ++cursorPosition;
                    redrawLine();
                }
            } else if (sequence == "H" || sequence == "1~" ||
                       sequence == "7~" || sequence == "OH") {
                // Home.
                cursorPosition = 0;
                redrawLine();
            } else if (sequence == "F" || sequence == "4~" ||
                       sequence == "8~" || sequence == "OF") {
                // End.
                cursorPosition = physicalLine.size();
                redrawLine();
            } else if (sequence == "3~") {
                // Delete: remove the character at the cursor.
                if (cursorPosition < physicalLine.size()) {
                    physicalLine.erase(cursorPosition, 1);
                    redrawLine();
                }
            } else if (!continuing && sequence == "A") {
                // Up arrow: browse older history entries.
                if (historyPosition == entries.size()) {
                    draft = physicalLine;

                    // Refresh history so entries from other tabs appear.
                    entries = history.getEntries();
                    historyPosition = entries.size();
                }

                if (historyPosition > 0) {
                    --historyPosition;
                    physicalLine = entries[historyPosition];
                    cursorPosition = physicalLine.size();
                    redrawLine();
                }
            } else if (!continuing && sequence == "B") {
                // Down arrow: browse newer history entries.
                if (historyPosition < entries.size()) {
                    ++historyPosition;

                    if (historyPosition == entries.size()) {
                        physicalLine = draft;
                    } else {
                        physicalLine = entries[historyPosition];
                    }

                    cursorPosition = physicalLine.size();
                    redrawLine();
                }
            }

            continue;
        }

        // Insert printable ASCII characters at the current cursor position.
        if (character >= 32 && character <= 126) {
            physicalLine.insert(cursorPosition, 1, character);
            ++cursorPosition;
            redrawLine();
        }
    }

    if (!restoreTerminal()) {
        return false;
    }

    return true;
}
