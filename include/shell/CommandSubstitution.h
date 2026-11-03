#ifndef COMMANDSUBSTITUTION_H
#define COMMANDSUBSTITUTION_H

#include <string>

class ShellIO;

// Expands $(...) and backtick substitutions by running each inner command
// in a separate terminator-shell helper process.
class CommandSubstitution {
public:
    bool expand(
        const std::string& input,
        std::string& expandedCommand,
        std::string& errorMessage,
        int errorFd = 2,
        ShellIO* owner = nullptr
    ) const;
};

#endif // COMMANDSUBSTITUTION_H