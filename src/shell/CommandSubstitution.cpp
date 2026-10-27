#include "shell/CommandSubstitution.h"

#include <cerrno>
#include <cstdio>
#include <iostream>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

enum class ScanKind {
    Parentheses,
    Backtick
};

struct ScanFrame {
    ScanKind kind;
    char quote;
    bool escaped;
    std::size_t depth;
};

// Find the ')' matching an opening "$(". Nested $(), quotes, escapes and
// backtick-delimited regions must not make the scanner stop too early.
std::size_t findMatchingParen(
    const std::string& input,
    std::size_t start
) {
    if (start + 1 >= input.size() ||
        input[start] != '$' || input[start + 1] != '(') {
        return std::string::npos;
    }

    std::vector<ScanFrame> frames;
    frames.push_back({ScanKind::Parentheses, '\0', false, 1});

    for (std::size_t i = start + 2; i < input.size(); ++i) {
        ScanFrame& frame = frames.back();
        const char c = input[i];

        // Backtick contents are kept opaque for delimiter matching. Actual
        // expansion of backticks is intentionally a separate checkpoint.
        if (frame.kind == ScanKind::Backtick) {
            if (frame.escaped) {
                frame.escaped = false;
                continue;
            }

            if (c == '\\' && i + 1 < input.size() &&
                (input[i + 1] == '`' || input[i + 1] == '\\' ||
                 input[i + 1] == '$' || input[i + 1] == '\n')) {
                frame.escaped = true;
                continue;
            }

            if (c == '`') {
                frames.pop_back();
            }
            continue;
        }

        if (frame.escaped) {
            frame.escaped = false;
            continue;
        }

        if (frame.quote == '\'') {
            if (c == '\'') {
                frame.quote = '\0';
            }
            continue;
        }

        if (c == '\\') {
            bool escapesNext = frame.quote != '"';
            if (frame.quote == '"') {
                escapesNext = i + 1 < input.size() &&
                    (input[i + 1] == '"' || input[i + 1] == '\\' ||
                     input[i + 1] == '$' || input[i + 1] == '`' ||
                     input[i + 1] == '\n');
            }

            if (escapesNext) {
                frame.escaped = true;
                continue;
            }
        }

        if (frame.quote == '"') {
            if (c == '"') {
                frame.quote = '\0';
                continue;
            }

            // Substitution nesting is recognized even while double-quoted.
            if (c == '$' && i + 1 < input.size() && input[i + 1] == '(') {
                frames.push_back({ScanKind::Parentheses, '\0', false, 1});
                ++i;
                continue;
            }

            if (c == '`') {
                frames.push_back({ScanKind::Backtick, '\0', false, 0});
            }
            continue;
        }

        if (c == '\'') {
            frame.quote = '\'';
            continue;
        }

        if (c == '"') {
            frame.quote = '"';
            continue;
        }

        if (c == '$' && i + 1 < input.size() && input[i + 1] == '(') {
            frames.push_back({ScanKind::Parentheses, '\0', false, 1});
            ++i;
            continue;
        }

        if (c == '`') {
            frames.push_back({ScanKind::Backtick, '\0', false, 0});
            continue;
        }

        if (c == '(') {
            ++frame.depth;
        } else if (c == ')') {
            --frame.depth;
            if (frame.depth == 0) {
                if (frames.size() == 1) {
                    return i;
                }
                frames.pop_back();
            }
        }
    }

    return std::string::npos;
}

// Find the closing backtick for a backtick substitution.
// Quotes and escaped backticks inside the command must not
// prematurely terminate the substitution.
std::size_t findMatchingBacktick(
    const std::string& input,
    std::size_t start
) {
    if (start >= input.size() || input[start] != '`') {
        return std::string::npos;
    }

    char quote = '\0';
    bool escaped = false;

    for (std::size_t i = start + 1; i < input.size(); ++i) {
        const char c = input[i];

        if (escaped) {
            escaped = false;
            continue;
        }

        // Single-quoted content is literal.
        if (quote == '\'') {
            if (c == '\'') {
                quote = '\0';
            }
            continue;
        }

        // Ignore escaped delimiters while scanning the command.
        if (c == '\\' && i + 1 < input.size()) {
            const char next = input[i + 1];

            if (quote != '"' ||
                next == '"' || next == '\\' ||
                next == '$' || next == '`' ||
                next == '\n') {
                escaped = true;
                continue;
            }
        }

        if (quote == '"') {
            if (c == '"') {
                quote = '\0';
            }
            continue;
        }

        if (c == '\'') {
            quote = '\'';
            continue;
        }

        if (c == '"') {
            quote = '"';
            continue;
        }

        // An unquoted, unescaped backtick closes this substitution.
        if (c == '`') {
            return i;
        }
    }

    return std::string::npos;
}

// Execute a command callback in a child and capture its stdout through a pipe.
// Stderr remains attached to the terminal, as it normally does for $(...).
bool captureOutput(
    const std::string& command,
    const CommandSubstitution::CommandRunner& runCommand,
    std::string& output,
    std::string& errorMessage
) {
    int outputPipe[2];
    if (::pipe(outputPipe) == -1) {
        errorMessage = "pipe for command substitution failed";
        return false;
    }

    // Avoid copying buffered prompt/output into the forked child.
    std::cout.flush();

    const pid_t childPid = ::fork();
    if (childPid == -1) {
        const int savedError = errno;
        ::close(outputPipe[0]);
        ::close(outputPipe[1]);
        errno = savedError;
        errorMessage = "fork for command substitution failed";
        return false;
    }

    if (childPid == 0) {
        ::close(outputPipe[0]);

        if (::dup2(outputPipe[1], STDOUT_FILENO) == -1) {
            std::perror("terminator: command substitution dup2");
            ::close(outputPipe[1]);
            _exit(126);
        }
        ::close(outputPipe[1]);

        // Calling the shell callback allows nested substitutions and inner
        // pipelines to reuse the shell's existing execution implementation.
        const int status = runCommand(command);
        std::cout.flush();
        std::fflush(nullptr);
        _exit(status & 0xff);
    }

    ::close(outputPipe[1]);

    char buffer[4096];
    bool readSucceeded = true;
    while (true) {
        const ssize_t bytesRead = ::read(outputPipe[0], buffer, sizeof(buffer));
        if (bytesRead > 0) {
            output.append(buffer, static_cast<std::size_t>(bytesRead));
        } else if (bytesRead == 0) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            errorMessage = "read from command-substitution pipe failed";
            readSucceeded = false;
            break;
        }
    }

    ::close(outputPipe[0]);

    int childStatus = 0;
    while (::waitpid(childPid, &childStatus, 0) == -1) {
        if (errno == EINTR) {
            continue;
        }
        errorMessage = "waitpid for command substitution failed";
        return false;
    }

    // A non-zero inner command status does not suppress its captured stdout.
    return readSucceeded;
}

// Keep substituted characters as argument data, not pipeline/background
// syntax. Unquoted whitespace is not escaped so the current tokenizer still
// performs its existing word-splitting behavior.
std::string protectOutput(
    const std::string& output,
    char surroundingQuote
) {
    std::string protectedText;
    protectedText.reserve(output.size());

    for (char c : output) {
        bool needsEscape = false;

        if (surroundingQuote == '"') {
            needsEscape = c == '"' || c == '\\' || c == '$' || c == '`';
        } else {
            needsEscape = c == '\\' || c == '\'' || c == '"' ||
                          c == '$' || c == '|' || c == '&' ||
                          c == '<' || c == '>' || c == ';' || c == '`';
        }

        if (needsEscape) {
            protectedText += '\\';
        }
        protectedText += c;
    }

    return protectedText;
}

void removeTrailingNewlines(std::string& output) {
    while (!output.empty() && output.back() == '\n') {
        output.pop_back();
    }
}

} // namespace

bool CommandSubstitution::expand(
    const std::string& input,
    const CommandRunner& runCommand,
    std::string& expandedCommand,
    std::string& errorMessage
) const {
    expandedCommand.clear();
    errorMessage.clear();

    if (!runCommand) {
        errorMessage = "command substitution has no command runner";
        return false;
    }

    char quote = '\0';
    bool escaped = false;

    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];

        if (escaped) {
            expandedCommand += c;
            escaped = false;
            continue;
        }

        // Preserve escaped input. In double quotes, backslash only escapes
        // quote, slash, dollar, backtick, or newline.
        if (c == '\\' && quote != '\'') {
            bool escapesNext = quote != '"';
            if (quote == '"') {
                escapesNext = i + 1 < input.size() &&
                    (input[i + 1] == '"' || input[i + 1] == '\\' ||
                     input[i + 1] == '$' || input[i + 1] == '`' ||
                     input[i + 1] == '\n');
            }

            expandedCommand += c;
            if (escapesNext) {
                escaped = true;
            }
            continue;
        }

        if (quote == '\'') {
            expandedCommand += c;
            if (c == '\'') {
                quote = '\0';
            }
            continue;
        }

        if (quote == '"') {
            if (c == '"') {
                expandedCommand += c;
                quote = '\0';
                continue;
            }

            if (c == '$' && i + 1 < input.size() && input[i + 1] == '(') {
                const std::size_t end = findMatchingParen(input, i);
                if (end == std::string::npos) {
                    errorMessage = "unterminated $(...) command substitution";
                    return false;
                }

                const std::string innerCommand =
                    input.substr(i + 2, end - (i + 2));
                std::string capturedOutput;

                if (!captureOutput(
                        innerCommand, runCommand, capturedOutput, errorMessage)) {
                    return false;
                }

                removeTrailingNewlines(capturedOutput);
                expandedCommand += protectOutput(capturedOutput, '"');
                i = end;
                continue;
            }

            // Execute backtick substitution inside double quotes.
            if (c == '`') {
                const std::size_t end = findMatchingBacktick(input, i);

                if (end == std::string::npos) {
                    errorMessage = "unterminated backtick command substitution";
                    return false;
                }

                // Extract the command between the opening and closing backticks.
                const std::string innerCommand =
                    input.substr(i + 1, end - i - 1);

                std::string capturedOutput;

                if (!captureOutput(
                        innerCommand,
                        runCommand,
                        capturedOutput,
                        errorMessage)) {
                    return false;
                }

                // Shell command substitution removes trailing newline characters.
                removeTrailingNewlines(capturedOutput);

                // Keep substituted characters inside the current double-quoted
                // argument rather than letting them become shell syntax.
                expandedCommand += protectOutput(capturedOutput, '"');

                // Skip the entire substitution, including its closing backtick.
                i = end;
                continue;
            }

            expandedCommand += c;
            continue;
        }

        if (c == '\'') {
            quote = '\'';
            expandedCommand += c;
            continue;
        }

        if (c == '"') {
            quote = '"';
            expandedCommand += c;
            continue;
        }

        if (c == '$' && i + 1 < input.size() && input[i + 1] == '(') {
            const std::size_t end = findMatchingParen(input, i);
            if (end == std::string::npos) {
                errorMessage = "unterminated $(...) command substitution";
                return false;
            }

            const std::string innerCommand =
                input.substr(i + 2, end - (i + 2));
            std::string capturedOutput;

            if (!captureOutput(
                    innerCommand, runCommand, capturedOutput, errorMessage)) {
                return false;
            }

            removeTrailingNewlines(capturedOutput);
            expandedCommand += protectOutput(capturedOutput, '\0');
            i = end;
            continue;
        }

        // Execute unquoted backtick substitution.
        if (c == '`') {
            const std::size_t end = findMatchingBacktick(input, i);

            if (end == std::string::npos) {
                errorMessage = "unterminated backtick command substitution";
                return false;
            }

            // Extract the command between the backticks.
            const std::string innerCommand =
                input.substr(i + 1, end - i - 1);

            std::string capturedOutput;

            if (!captureOutput(
                    innerCommand,
                    runCommand,
                    capturedOutput,
                    errorMessage)) {
                return false;
            }

            // Remove trailing newline characters, as shell substitution does.
            removeTrailingNewlines(capturedOutput);

            // Preserve special characters in the result as data.
            // Unquoted whitespace remains available for word splitting.
            expandedCommand += protectOutput(capturedOutput, '\0');

            // Skip over the closing backtick.
            i = end;
            continue;
        }

        expandedCommand += c;
    }

    return true;
}
