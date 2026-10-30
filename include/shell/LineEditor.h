
#ifndef SHELL_LINE_EDITOR_H
#define SHELL_LINE_EDITOR_H

#include <string>

class History;

class LineEditor {
public:
    // Read one logical command, preserving multiline and history behavior.
    // Returns false when input reaches EOF or Ctrl+D requests shell exit.
    bool readCommandLine(const History& history, std::string& line);
};

#endif // SHELL_LINE_EDITOR_H
