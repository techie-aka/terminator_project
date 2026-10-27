#ifndef COMMANDPARSER_H
#define COMMANDPARSER_H

#include <string>
#include <vector>

#include "shell/Redirection.h"

class CommandParser {
public:
    std::vector<std::string> parse(
        const std::string& input,
        int lastExitStatus
    ) const;

    std::vector<std::string> splitPipeline(
        const std::string& input
    ) const;

    // Parse one command, extracting redirection operators and filenames.
    // Returns false and fills errorMessage when the syntax is invalid.
    bool parseCommand(
        const std::string& input,
        int lastExitStatus,
        ParsedCommand& result,
        std::string& errorMessage
    ) const;
};

#endif