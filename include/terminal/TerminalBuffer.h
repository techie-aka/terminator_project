
#ifndef TERMINALBUFFER_H
#define TERMINALBUFFER_H

#include <cstddef>
#include <string>
#include <vector>

class TerminalBuffer {
private:
    std::vector<std::string> lines;
    std::size_t cursorColumn;

    enum class ParseState {
        Normal,
        Escape,
        CSI,
        OSC,
        OSCEscape
    };

    ParseState parseState;
    std::string controlSequence;

    void processCharacter(char character);
    void eraseLineFromCursor();
    void moveCursorToColumn(std::size_t column);

public:
    TerminalBuffer();

    // Append shell output and report whether it requested a new tab.
    bool appendOutput(const std::string& output);

    const std::vector<std::string>& getLines() const;
    std::size_t getCursorColumn() const;

    void clear();
};

#endif
