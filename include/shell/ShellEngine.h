#ifndef SHELL_ENGINE_H
#define SHELL_ENGINE_H

#include <string>

class ShellIO;

class ShellEngine {
private:
    int runImpl(ShellIO* io, const std::string* singleCommand = nullptr);

public:
    int run();
    int run(ShellIO& io);
    int runCommand(const std::string& command);
};

#endif