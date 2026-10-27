#include "shell/CommandParser.h"

#include <cctype>
#include <string>
#include <vector>
#include <cstdlib>

// Expand a variable such as $HOME or ${HOME}.
// Returns false when the '$' does not begin a supported variable name.
// An environment variable that is not set expands to an empty string.
bool appendEnvironmentVariable(
    const std::string& input,
    std::size_t& position,
    std::string& output
) {
    if (position >= input.size() || input[position] != '$') {
        return false;
    }

    const std::size_t nameStart = position + 1;
    if (nameStart >= input.size()) {
        return false;
    }

    std::string name;

    if (input[nameStart] == '{') {
        // Handle ${HOME} syntax.
        const std::size_t closingBrace = input.find('}', nameStart + 1);

        if (closingBrace == std::string::npos) {
            return false;
        }

        name = input.substr(
            nameStart + 1,
            closingBrace - nameStart - 1
        );

        position = closingBrace;
    } else {
        // Handle $HOME syntax.
        const auto isFirstCharacter = [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') ||
                   (c >= 'a' && c <= 'z') ||
                   c == '_';
        };

        const auto isNameCharacter = [&](unsigned char c) {
            return isFirstCharacter(c) ||
                   (c >= '0' && c <= '9');
        };

        if (!isFirstCharacter(
                static_cast<unsigned char>(input[nameStart]))) {
            return false;
        }

        std::size_t end = nameStart + 1;

        while (end < input.size() &&
               isNameCharacter(static_cast<unsigned char>(input[end]))) {
            ++end;
        }

        name = input.substr(nameStart, end - nameStart);
        position = end - 1;
    }

    // Reject unsupported variable-name syntax rather than partially
    // interpreting expressions such as ${HOME:-fallback}.
    if (name.empty()) {
        return false;
    }

    for (std::size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);

        const bool valid = i == 0
            ? ((c >= 'A' && c <= 'Z') ||
               (c >= 'a' && c <= 'z') || c == '_')
            : ((c >= 'A' && c <= 'Z') ||
               (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_');

        if (!valid) {
            return false;
        }
    }

    const char* value = std::getenv(name.c_str());

    // An undefined environment variable expands to an empty string.
    if (value != nullptr) {
        output += value;
    }

    return true;
}

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

            // Expand environment variables inside double quotes.
            if (character == '$' &&
                appendEnvironmentVariable(input, i, currentArgument)) {
                insideArgument = true;
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

        // Expand environment variables outside quotes as well.
        if (character == '$' &&
            appendEnvironmentVariable(input, i, currentArgument)) {
            insideArgument = true;
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