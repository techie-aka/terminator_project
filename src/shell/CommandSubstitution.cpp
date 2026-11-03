#include "shell/CommandSubstitution.h"
#include "shell/ShellHelper.h"

#include <vector>

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

// Run the inner command in a separate shell process. This is important in
// thread mode: we must not fork and then execute C++ shell code in the child
// of the GUI process. The helper's stdout is captured; stderr stays in this tab.
bool captureOutput(
    const std::string& command,
    std::string& output,
    std::string& errorMessage,
    int errorFd,
    ShellIO* owner
) {
    // Nested substitutions are handled by the helper shell's own execute path.
    return ShellHelper::captureOutput(
        command,
        errorFd,
        output,
        errorMessage,
        owner
    );
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
    std::string& expandedCommand,
    std::string& errorMessage,
    int errorFd,
    ShellIO* owner
) const {
    expandedCommand.clear();
    errorMessage.clear();

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
                        innerCommand, capturedOutput, errorMessage, errorFd, owner)) {
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
                        capturedOutput,
                        errorMessage,
                        errorFd,
                        owner)) {
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
                    innerCommand, capturedOutput, errorMessage, errorFd, owner)) {
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
                    capturedOutput,
                    errorMessage,
                    errorFd,
                    owner)) {
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