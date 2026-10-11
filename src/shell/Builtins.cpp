#include "shell/Builtins.h"
#include "shell/History.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

bool Builtins::execute(
    const std::vector<std::string>& arguments,
    int& exitStatus,
    bool& shouldExit,
    const LineExecutor& executeLine,
    History& history
) const {
    if (arguments.empty()) {
        return false;
    }

    const std::string& command = arguments[0];

    // Exit the current shell process.
    if (command == "exit") {
        shouldExit = true;
        return true;
    }

    // Display the history shared by all shell processes.
    if (command == "history") {
        if (arguments.size() != 1) {
            std::cerr << "terminator: history: "
                      << "this command takes no arguments\n";
            exitStatus = 2;
            return true;
        }

        if (!history.print(std::cout)) {
            std::cerr << "terminator: history: "
                      << "shared history is unavailable\n";
            exitStatus = 1;
            return true;
        }

        exitStatus = 0;
        return true;
    }

    // Execute commands from a file in the current shell.
    if (command == "source") {
        if (arguments.size() != 2) {
            std::cerr << "terminator: source: usage: source filename\n";
            exitStatus = 2;
            return true;
        }

        std::string filePath = arguments[1];

        // Support ~/filename for the source command.
        if (filePath == "~" ||
            filePath.rfind("~/", 0) == 0) {
            const char* homeDirectory = std::getenv("HOME");

            if (homeDirectory == nullptr) {
                std::cerr << "terminator: source: HOME not set\n";
                exitStatus = 1;
                return true;
            }

            if (filePath == "~") {
                filePath = homeDirectory;
            } else {
                filePath =
                    std::string(homeDirectory) + filePath.substr(1);
            }
        }

        std::ifstream script(filePath);

        if (!script.is_open()) {
            std::cerr << "terminator: source: "
                      << filePath << ": cannot open file\n";
            exitStatus = 1;
            return true;
        }

        std::string scriptLine;
        int scriptStatus = 0;

        while (std::getline(script, scriptLine)) {
            // Ignore blank lines and whole-line comments for now.
            const std::size_t firstCharacter =
                scriptLine.find_first_not_of(" \t\r");

            if (firstCharacter == std::string::npos ||
                scriptLine[firstCharacter] == '#') {
                continue;
            }

            // Reuse the same parser, built-ins, and executor.
            scriptStatus = executeLine(scriptLine);

            // An exit command in the sourced file exits this shell.
            if (shouldExit) {
                break;
            }
        }

        if (script.bad()) {
            std::cerr << "terminator: source: "
                      << filePath << ": error reading file\n";
            exitStatus = 1;
        } else {
            // source returns the status of the last executed command.
            exitStatus = scriptStatus;
        }

        return true;
    }

    if (command != "cd") {
        return false;
    }

    // cd accepts zero or one directory argument.
    if (arguments.size() > 2) {
        std::cerr << "terminator: cd: too many arguments\n";
        exitStatus = 1;
        return true;
    }

    const char* homeDirectory = std::getenv("HOME");
    std::string targetDirectory;

    if (arguments.size() == 1 || arguments[1] == "~") {
        if (homeDirectory == nullptr) {
            std::cerr << "terminator: cd: HOME not set\n";
            exitStatus = 1;
            return true;
        }

        targetDirectory = homeDirectory;
    } else {
        targetDirectory = arguments[1];

        if (targetDirectory.rfind("~/", 0) == 0) {
            if (homeDirectory == nullptr) {
                std::cerr << "terminator: cd: HOME not set\n";
                exitStatus = 1;
                return true;
            }

            targetDirectory =
                std::string(homeDirectory) + targetDirectory.substr(1);
        }
    }

    // chdir() must run in the shell process.
    if (chdir(targetDirectory.c_str()) == -1) {
        std::cerr << "terminator: cd: "
                  << targetDirectory << ": "
                  << std::strerror(errno) << '\n';

        exitStatus = 1;
        return true;
    }

    char currentDirectory[4096];

    if (getcwd(currentDirectory, sizeof(currentDirectory)) != nullptr) {
        setenv("PWD", currentDirectory, 1);
    }

    exitStatus = 0;
    return true;
}