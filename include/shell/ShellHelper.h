#ifndef SHELL_HELPER_H
#define SHELL_HELPER_H

#include <string>

class ShellIO;

// Starts a separate terminator-shell process for a command.
// The helper's stdin is /dev/null; stdout and stderr go to the supplied FDs.
class ShellHelper {
public:
    static int runCommand(
        const std::string& command,
        int outputFd,
        int errorFd,
        ShellIO* owner = nullptr
    );

    // Runs a command in a helper shell and returns its stdout in output.
    // Error output is sent to errorFd instead of being captured.
    static bool captureOutput(
        const std::string& command,
        int errorFd,
        std::string& output,
        std::string& errorMessage,
        ShellIO* owner = nullptr
    );
};

#endif // SHELL_HELPER_H