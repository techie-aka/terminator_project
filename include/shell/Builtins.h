#ifndef BUILTINS_H
#define BUILTINS_H

#include <functional>
#include <string>
#include <vector>

class History;
class JobManager;
class ShellIO;

class Builtins {
public:
    using LineExecutor = std::function<int(const std::string&)>;

    bool execute(
        const std::vector<std::string>& arguments,
        int& exitStatus,
        bool& shouldExit,
        const LineExecutor& executeLine,
        History& history,
        JobManager& jobs,
        ShellIO* io = nullptr
    ) const;
};

#endif