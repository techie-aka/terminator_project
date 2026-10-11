#include "shell/CommandParser.h"

#include <cctype>

std::vector<std::string> CommandParser::parse(
    const std::string& input,
    int lastExitStatus
) const {
    std::vector<std::string> arguments;
    std::string currentArgument;
    bool insideArgument = false;

    for (std::size_t i = 0; i < input.size(); ++i) {
        const char character = input[i];

        // Separate arguments using whitespace.
        if (std::isspace(static_cast<unsigned char>(character))) {
            if (insideArgument) {
                arguments.push_back(currentArgument);
                currentArgument.clear();
                insideArgument = false;
            }

            continue;
        }

        // Expand $? using the previous command's exit status.
        if (character == '$' &&
            i + 1 < input.size() &&
            input[i + 1] == '?') {
            currentArgument += std::to_string(lastExitStatus);
            insideArgument = true;
            ++i;
            continue;
        }

        currentArgument += character;
        insideArgument = true;
    }

    if (insideArgument) {
        arguments.push_back(currentArgument);
    }

    return arguments;
}