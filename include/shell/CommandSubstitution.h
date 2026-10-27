#ifndef COMMANDSUBSTITUTION_H
#define COMMANDSUBSTITUTION_H

#include <functional>
#include <string>

// Handles $(...) parsing and output substitution separately from the shell's
// parser and command executors. The shell supplies a callback for executing
// the inner command; this class captures the callback's standard output.
class CommandSubstitution {
public:
    using CommandRunner = std::function<int(const std::string& command)>;

    // Expands $(...) occurrences in input. Nested $(...) works by calling the
    // supplied runner recursively through the shell's normal executeLine path.
    // Returns false for malformed syntax or an I/O/capture failure.
    bool expand(
        const std::string& input,
        const CommandRunner& runCommand,
        std::string& expandedCommand,
        std::string& errorMessage
    ) const;
};

#endif // COMMANDSUBSTITUTION_H