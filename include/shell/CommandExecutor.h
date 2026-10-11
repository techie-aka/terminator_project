#ifndef COMMANDEXECUTOR_H
#define COMMANDEXECUTOR_H

#include <string>
#include <vector>

class CommandExecutor {
public:
    int execute(const std::vector<std::string>& arguments) const;
};

#endif