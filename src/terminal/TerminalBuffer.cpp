
#include "terminal/TerminalBuffer.h"

#include <algorithm>
#include <cctype>

TerminalBuffer::TerminalBuffer()
    : lines(1, ""),
      cursorColumn(0),
      parseState(ParseState::Normal) {
}

bool TerminalBuffer::appendOutput(const std::string& output) {
    bool newTabRequested = false;

    for (char character : output) {
        switch (parseState) {
            case ParseState::Normal:
                if (character == '\x1b') {
                    parseState = ParseState::Escape;
                } else {
                    processCharacter(character);
                }
                break;

            case ParseState::Escape:
                controlSequence.clear();

                if (character == '[') {
                    parseState = ParseState::CSI;
                } else if (character == ']') {
                    parseState = ParseState::OSC;
                } else {
                    parseState = ParseState::Normal;
                }
                break;

            case ParseState::CSI:
                if (character >= 0x40 && character <= 0x7e) {
                    // Handle a few common terminal control sequences.
                    if (character == 'J' &&
                        (controlSequence == "2" || controlSequence == "3")) {
                        clear();
                    } else if (character == 'K') {
                        eraseLineFromCursor();
                    } else if (character == 'G') {
                        try {
                            const std::size_t column = controlSequence.empty()
                                ? 1
                                : static_cast<std::size_t>(
                                    std::stoul(controlSequence)
                                );

                            moveCursorToColumn(column > 0 ? column - 1 : 0);
                        } catch (...) {
                            moveCursorToColumn(0);
                        }
                    } else if (character == 'H' || character == 'f') {
                        // Full row/column cursor positioning comes later.
                        moveCursorToColumn(0);
                    }

                    // Other CSI commands, including color changes, are ignored
                    // by this basic renderer.
                    controlSequence.clear();
                    parseState = ParseState::Normal;
                } else if (controlSequence.size() < 64) {
                    controlSequence += character;
                }
                break;

            case ParseState::OSC:
                if (character == '\a') {
                    newTabRequested =
                        newTabRequested ||
                        controlSequence == "777;newtab";

                    controlSequence.clear();
                    parseState = ParseState::Normal;
                } else if (character == '\x1b') {
                    parseState = ParseState::OSCEscape;
                } else if (controlSequence.size() < 256) {
                    controlSequence += character;
                }
                break;

            case ParseState::OSCEscape:
                if (character == '\\') {
                    newTabRequested =
                        newTabRequested ||
                        controlSequence == "777;newtab";
                }

                controlSequence.clear();
                parseState = ParseState::Normal;
                break;
        }
    }

    return newTabRequested;
}

void TerminalBuffer::processCharacter(char character) {
    if (character == '\r') {
        cursorColumn = 0;
        return;
    }

    if (character == '\n') {
        lines.emplace_back("");
        cursorColumn = 0;
        return;
    }

    if (character == '\b' || character == '\x7f') {
        if (cursorColumn > 0) {
            --cursorColumn;
        }

        return;
    }

    if (character == '\t') {
        const std::size_t spaces = 8 - (cursorColumn % 8);

        for (std::size_t i = 0; i < spaces; ++i) {
            processCharacter(' ');
        }

        return;
    }

    if (!std::isprint(static_cast<unsigned char>(character))) {
        return;
    }

    std::string& line = lines.back();

    if (cursorColumn >= line.size()) {
        line.resize(cursorColumn + 1, ' ');
    }

    line[cursorColumn] = character;
    ++cursorColumn;
}

void TerminalBuffer::eraseLineFromCursor() {
    std::string& line = lines.back();

    if (cursorColumn < line.size()) {
        line.resize(cursorColumn);
    }
}

void TerminalBuffer::moveCursorToColumn(std::size_t column) {
    cursorColumn = column;

    std::string& line = lines.back();

    if (cursorColumn > line.size()) {
        line.resize(cursorColumn, ' ');
    }
}

const std::vector<std::string>& TerminalBuffer::getLines() const {
    return lines;
}

std::size_t TerminalBuffer::getCursorColumn() const {
    return cursorColumn;
}

void TerminalBuffer::clear() {
    lines.clear();
    lines.emplace_back("");
    cursorColumn = 0;
}
