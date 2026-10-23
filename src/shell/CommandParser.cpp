#include "shell/CommandParser.h"

#include <cctype>
#include <string>
#include <vector>

std::vector<std::string> CommandParser::parse(
    const std::string& input,
    int lastExitStatus
) const {
    std::vector<std::string> arguments;
    std::string currentArgument;

    // Track whether we are inside single or double quotes.
    char quote = '\0';

    // Distinguish an empty quoted argument from no argument.
    bool insideArgument = false;

    for (std::size_t i = 0; i < input.size(); ++i) {
        const char character = input[i];

        // Inside single quotes, every character is literal until
        // the matching single quote.
        if (quote == '\'') {
            if (character == '\'') {
                quote = '\0';
            } else {
                currentArgument += character;
            }

            continue;
        }

        // Inside double quotes, whitespace remains part of the argument.
        if (quote == '"') {
            if (character == '"') {
                quote = '\0';
                continue;
            }

            // Preserve the existing $? expansion inside double quotes.
            if (character == '$' &&
                i + 1 < input.size() &&
                input[i + 1] == '?') {
                currentArgument += std::to_string(lastExitStatus);
                ++i;
                continue;
            }

            // Basic escape handling inside double quotes.
            if (character == '\\' && i + 1 < input.size()) {
                const char nextCharacter = input[i + 1];

                if (nextCharacter == '"' ||
                    nextCharacter == '\\' ||
                    nextCharacter == '$' ||
                    nextCharacter == '`') {
                    currentArgument += nextCharacter;
                    ++i;
                } else {
                    currentArgument += character;
                }

                continue;
            }

            currentArgument += character;
            continue;
        }

        // Outside quotes, whitespace ends the current argument.
        if (std::isspace(static_cast<unsigned char>(character))) {
            if (insideArgument) {
                arguments.push_back(currentArgument);
                currentArgument.clear();
                insideArgument = false;
            }

            continue;
        }

        // Start a quoted argument. Empty quotes still create an argument.
        if (character == '\'' || character == '"') {
            quote = character;
            insideArgument = true;
            continue;
        }

        // Outside quotes, a backslash escapes the next character.
        if (character == '\\' && i + 1 < input.size()) {
            currentArgument += input[++i];
            insideArgument = true;
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

    // Preserve the final argument, including an empty quoted argument.
    if (insideArgument) {
        arguments.push_back(currentArgument);
    }

    return arguments;
}

std::vector<std::string> CommandParser::splitPipeline(
    const std::string& input
) const {
    std::vector<std::string> commands;
    std::string currentCommand;

    char quote = '\0';
    bool escaped = false;

    for (char character : input) {
        if (escaped) {
            currentCommand += character;
            escaped = false;
            continue;
        }

        if (character == '\\' && quote != '\'') {
            currentCommand += character;
            escaped = true;
            continue;
        }

        if (quote != '\0') {
            currentCommand += character;

            if (character == quote) {
                quote = '\0';
            }

            continue;
        }

        if (character == '\'' || character == '"') {
            quote = character;
            currentCommand += character;
        } else if (character == '|') {
            commands.push_back(currentCommand);
            currentCommand.clear();
        } else {
            currentCommand += character;
        }
    }

    commands.push_back(currentCommand);

    return commands;
}