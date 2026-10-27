#ifndef COMMANDEXECUTOR_H
#define COMMANDEXECUTOR_H

#include <sys/types.h>
#include "shell/Redirection.h"

#include <string>
#include <vector>

class CommandExecutor {
public:
    int execute(
        const std::vector<std::string>& arguments,
        bool background = false,
        pid_t* startedProcess = nullptr,
        pid_t* startedProcessGroup = nullptr,
        bool* processStopped = nullptr,
        const RedirectionSpec* redirection = nullptr
    ) const;
};

#endif