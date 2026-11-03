#ifndef SHELL_LINE_EDITOR_H
#define SHELL_LINE_EDITOR_H

#include <string>

class History;
class ShellIO;

class LineEditor {
public:
    bool readCommandLine(
        const History& history,
        std::string& line
    );

    bool readCommandLine(
        const History& history,
        std::string& line,
        ShellIO& io
    );
};

#endif