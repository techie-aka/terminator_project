#ifndef REDIRECTION_H
#define REDIRECTION_H

#include <string>
#include <vector>

// Redirection requested by one command.
struct RedirectionSpec {
    bool hasInput = false;
    std::string inputFile;

    bool hasOutput = false;
    std::string outputFile;
};

// One parsed command, including its arguments and redirections.
struct ParsedCommand {
    std::vector<std::string> arguments;
    RedirectionSpec redirection;
};

// Describes a file-opening or descriptor-duplication failure.
struct RedirectionFailure {
    int errorNumber = 0;
    std::string filename;
    bool input = false;
};

class Redirection {
public:
    // Called inside a child process before execvp().
    // Input is opened before output, avoiding output truncation when
    // the requested input file is missing.
    static bool applyInChild(
        const RedirectionSpec& specification,
        RedirectionFailure& failure
    );

    static void reportFailure(const RedirectionFailure& failure);
};

#endif