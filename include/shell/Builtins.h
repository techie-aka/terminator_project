#ifndef BUILTINS_H
#define BUILTINS_H

#include <functional>
#include <string>
#include <vector>

class History;

class Builtins {
public:
    using LineExecutor = std::function<int(const std::string&)>;

    // Returns true when a built-in handled the command.
    bool execute(
        const std::vector<std::string>& arguments,
        int& exitStatus,
        bool& shouldExit,
        const LineExecutor& executeLine,
        History& history
    ) const;
};

#endif