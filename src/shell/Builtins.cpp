#include "shell/Builtins.h"
#include "shell/History.h"
#include "shell/JobManager.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include <limits>
#include <stdexcept>

bool Builtins::execute(
    const std::vector<std::string>& arguments,
    int& exitStatus,
    bool& shouldExit,
    const LineExecutor& executeLine,
    History& history,
    JobManager& jobs
) const {
    if (arguments.empty()) {
        return false;
    }

    const std::string& command = arguments[0];

    // Display the current shell's background jobs.
    if (command == "jobs") {
        if (arguments.size() != 1) {
            std::cerr << "terminator: jobs: this command takes no arguments\n";
            exitStatus = 2;
            return true;
        }

        jobs.update();
        jobs.print(std::cout);

        exitStatus = 0;
        return true;
    }

    // Resume or foreground a tracked job.
    if (command == "fg" || command == "bg") {
        if (arguments.size() > 2) {
            std::cerr << "terminator: " << command
                    << ": expected at most one job ID\n";
            exitStatus = 2;
            return true;
        }

        // Without an argument, select the most recent active job.
        int jobId = 0;

        if (arguments.size() == 2) {
            std::string idText = arguments[1];

            // Support both "1" and "%1".
            if (!idText.empty() && idText[0] == '%') {
                idText.erase(0, 1);
            }

            try {
                std::size_t parsedLength = 0;
                const long parsedId = std::stol(idText, &parsedLength);

                if (parsedLength != idText.size() ||
                    parsedId <= 0 ||
                    parsedId > std::numeric_limits<int>::max()) {
                    throw std::invalid_argument("invalid job ID");
                }

                jobId = static_cast<int>(parsedId);
            } catch (const std::exception&) {
                std::cerr << "terminator: " << command
                        << ": invalid job ID\n";
                exitStatus = 2;
                return true;
            }
        } else {
            jobId = jobs.latestJobId();
        }

        if (jobId == 0) {
            std::cerr << "terminator: " << command
                    << ": no current job\n";
            exitStatus = 1;
            return true;
        }

        if (command == "fg") {
            if (!jobs.foreground(jobId, exitStatus)) {
                std::cerr << "terminator: fg: "
                        << "cannot foreground job %" << jobId << '\n';
                exitStatus = 1;
            }

            return true;
        }

        if (!jobs.background(jobId)) {
            std::cerr << "terminator: bg: "
                    << "cannot resume job %" << jobId << '\n';
            exitStatus = 1;
            return true;
        }

        jobs.update();
        std::cout << '[' << jobId << "] Running\n";
        exitStatus = 0;
        return true;
    }

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