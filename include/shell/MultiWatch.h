#ifndef MULTI_WATCH_H
#define MULTI_WATCH_H

#include <string>
#include <vector>

class MultiWatch {
public:
    // True only when the first shell word is exactly "multiWatch".
    static bool isInvocation(const std::string& line);

    // Parses: multiWatch ["command 1", "command 2"]
    static bool parseInvocation(
        const std::string& line,
        std::vector<std::string>& commands,
        std::string& errorMessage
    );

    // Runs one independent worker/process group per monitored command.
    // Each command is rerun two seconds after its previous run finishes.
    int execute(
        const std::vector<std::string>& commands,
        int lastExitStatus = 0
    ) const;
};

#endif // MULTI_WATCH_H