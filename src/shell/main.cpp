#include "shell/Builtins.h"
#include "shell/CommandExecutor.h"
#include "shell/CommandParser.h"
#include "shell/History.h"
#include "shell/JobManager.h"
#include "shell/PipelineExecutor.h"

#include <csignal>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <algorithm>
#include <termios.h>
#include <unistd.h>

enum class InputStatus {
    Complete,
    Incomplete,
    BackslashContinuation
};

enum class ContextKind {
    Normal,
    CommandSubstitution,
    Backtick
};

struct ScanContext {
    ContextKind kind;
    char quote;
    int parenthesisDepth;
};

// Inspect the input without executing it.
// Separate contexts let quotes inside $(...) be tracked independently
// from quotes surrounding the command substitution.
InputStatus analyzeInput(const std::string& input) {
    std::vector<ScanContext> contexts;

    contexts.push_back({ContextKind::Normal, '\0', 0});

    bool escaped = false;

    for (std::size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        ScanContext& context = contexts.back();

        // A backslash outside single quotes can escape the next
        // character. If it is the final character, the user must
        // continue the command on the next physical line.
        if (escaped) {
            escaped = false;
            continue;
        }

        // Inside single quotes, everything is literal except another
        // single quote, which closes the quoted section.
        if (context.quote == '\'') {
            if (c == '\'') {
                context.quote = '\0';
            }

            continue;
        }

        if (c == '\\') {
            // A final unescaped backslash means line continuation.
            if (i + 1 == input.size()) {
                return InputStatus::BackslashContinuation;
            }

            const char next = input[i + 1];

            if (context.quote == '"') {
                // Inside double quotes, these characters can be
                // escaped by a backslash. For other characters,
                // the backslash itself remains literal.
                if (next == '$' || next == '`' ||
                    next == '"' || next == '\\' ||
                    next == '\n') {
                    escaped = true;
                    continue;
                }
            } else {
                // Outside double quotes, the backslash escapes
                // the next character.
                escaped = true;
                continue;
            }
        }

        // An unescaped backtick closes its corresponding substitution.
        if (context.kind == ContextKind::Backtick && c == '`') {
            contexts.pop_back();
            continue;
        }

        // Process the current context's double-quoted text.
        if (context.quote == '"') {
            if (c == '"') {
                context.quote = '\0';
                continue;
            }

            // Command substitutions can appear inside double quotes.
            if (c == '$' && i + 1 < input.size() &&
                input[i + 1] == '(') {
                contexts.push_back({
                    ContextKind::CommandSubstitution, '\0', 1
                });

                ++i;
                continue;
            }

            // Backtick substitutions can also appear inside
            // double-quoted text.
            if (c == '`') {
                contexts.push_back({
                    ContextKind::Backtick, '\0', 0
                });

                continue;
            }

            continue;
        }

        // Opening quotes start a quoted section in this context.
        if (c == '\'') {
            context.quote = '\'';
            continue;
        }

        if (c == '"') {
            context.quote = '"';
            continue;
        }

        // Start a new, independently scanned $(...) context.
        if (c == '$' && i + 1 < input.size() &&
            input[i + 1] == '(') {
            contexts.push_back({
                ContextKind::CommandSubstitution, '\0', 1
            });

            ++i;
            continue;
        }

        // Start a backtick substitution.
        if (c == '`') {
            contexts.push_back({
                ContextKind::Backtick, '\0', 0
            });

            continue;
        }

        // Count parentheses only inside a command substitution.
        // This allows nested parentheses and nested $(...) expressions.
        if (context.kind == ContextKind::CommandSubstitution) {
            if (c == '(') {
                ++context.parenthesisDepth;
            } else if (c == ')') {
                --context.parenthesisDepth;

                if (context.parenthesisDepth == 0) {
                    contexts.pop_back();
                }
            }
        }
    }

    // Any unclosed quote means that additional input is required.
    for (const ScanContext& context : contexts) {
        if (context.quote != '\0') {
            return InputStatus::Incomplete;
        }
    }

    // An unfinished $(...) or backtick substitution also needs input.
    if (contexts.size() > 1) {
        return InputStatus::Incomplete;
    }

    return InputStatus::Complete;
}

bool readCommandLine(
    const History& history,
    std::string& line
) {
    termios originalSettings{};

    if (tcgetattr(STDIN_FILENO, &originalSettings) == -1) {
        return false;
    }

    termios settings = originalSettings;
    settings.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    settings.c_cc[VMIN] = 1;
    settings.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &settings) == -1) {
        return false;
    }

    // History navigation applies to the initial input line.
    // Do not let an Up/Down press replace the partially entered
    // command while the shell is waiting for a continuation line.
    std::vector<std::string> entries = history.getEntries();
    std::size_t historyPosition = entries.size();
    std::string draft;

    // physicalLine is the line currently being edited.
    // logicalCommand accumulates all lines belonging to one command.
    std::string physicalLine;
    std::string logicalCommand;

    // True after the shell has detected incomplete input.
    bool continuing = false;

    // True when a trailing backslash removed the newline, meaning
    // the next physical line must be joined directly to this one.
    bool joinNextLine = false;

    line.clear();

    const std::string primaryPrompt = "user@terminator> ";
    const std::string continuationPrompt = "continuation> ";

    std::string currentPrompt = primaryPrompt;

    auto redrawLine = [&]() {
        std::cout << '\r' << "\033[K"
                  << currentPrompt << physicalLine
                  << std::flush;
    };

    while (true) {
        char character = '\0';

        if (::read(STDIN_FILENO, &character, 1) != 1) {
            tcsetattr(STDIN_FILENO, TCSANOW, &originalSettings);
            return false;
        }

        // Enter submits the current physical line to the
        // input-completeness scanner.
        if (character == '\r' || character == '\n') {
            std::cout << '\r' << "\033[K"
                      << currentPrompt << physicalLine
                      << '\n' << std::flush;

            // Normally, separate physical lines with a newline.
            // A continuation backslash suppresses this separator.
            if (!logicalCommand.empty() && !joinNextLine) {
                logicalCommand += '\n';
            }

            logicalCommand += physicalLine;

            // The previous line's joining decision has now been used.
            joinNextLine = false;
            physicalLine.clear();

            const InputStatus status = analyzeInput(logicalCommand);

            if (status == InputStatus::BackslashContinuation) {
                // The scanner guarantees that the final character
                // is the continuation backslash. Remove it, matching
                // shell-style backslash-newline continuation.
                logicalCommand.pop_back();

                joinNextLine = true;
                continuing = true;
                currentPrompt = continuationPrompt;

                std::cout << currentPrompt << std::flush;
                continue;
            }

            if (status == InputStatus::Incomplete) {
                // Keep the accumulated command and read another line.
                // The newline was preserved in logicalCommand.
                continuing = true;
                currentPrompt = continuationPrompt;

                std::cout << currentPrompt << std::flush;
                continue;
            }

            // The full logical command is ready for execution.
            line = logicalCommand;
            break;
        }

        if (character == 4 && physicalLine.empty()) {
            // Preserve the existing Ctrl+D behavior on an empty line.
            tcsetattr(STDIN_FILENO, TCSANOW, &originalSettings);
            return false;
        }

        if (character == 127 || character == '\b') {
            if (!physicalLine.empty()) {
                physicalLine.pop_back();
                redrawLine();
            }

            continue;
        }

        if (character == '\x1b') {
            char bracket = '\0';
            char direction = '\0';

            if (::read(STDIN_FILENO, &bracket, 1) != 1 ||
                bracket != '[' ||
                ::read(STDIN_FILENO, &direction, 1) != 1) {
                continue;
            }

            // Keep existing history navigation on the primary line.
            // Continuation lines are part of the current command and
            // must not be replaced by an unrelated history entry.
            if (!continuing && direction == 'A') {
                if (historyPosition == entries.size()) {
                    draft = physicalLine;

                    // Refresh history so other tabs' entries are visible.
                    entries = history.getEntries();
                    historyPosition = entries.size();
                }

                if (historyPosition > 0) {
                    --historyPosition;
                    physicalLine = entries[historyPosition];
                    redrawLine();
                }
            } else if (!continuing &&
                       direction == 'B') {
                if (historyPosition < entries.size()) {
                    ++historyPosition;

                    if (historyPosition == entries.size()) {
                        physicalLine = draft;
                    } else {
                        physicalLine = entries[historyPosition];
                    }

                    redrawLine();
                }
            }

            continue;
        }

        // Accept printable ASCII characters, as before.
        if (character >= 32 && character <= 126) {
            physicalLine += character;
            std::cout << character << std::flush;
        }
    }

    if (tcsetattr(STDIN_FILENO, TCSANOW, &originalSettings) == -1) {
        return false;
    }

    return true;
}

int main() {
    // The interactive shell must survive terminal-generated signals.
    // External children restore default signal handling in CommandExecutor.
    std::signal(SIGINT, SIG_IGN);
    std::signal(SIGQUIT, SIG_IGN);
    std::signal(SIGTSTP, SIG_IGN);
    std::signal(SIGTTIN, SIG_IGN);
    std::signal(SIGTTOU, SIG_IGN);

    CommandParser parser;
    CommandExecutor executor;
    Builtins builtins;
    History history;
    JobManager jobs;
    PipelineExecutor pipelineExecutor;

    if (!history.isReady()) {
        std::cerr << "terminator: shared history is unavailable\n";
    }

    std::string line;
    int lastExitStatus = 0;
    bool shouldExit = false;

    // A reusable execution path for interactive and sourced commands.
    std::function<int(const std::string&)> executeLine;

    executeLine = [&](const std::string& commandLine) -> int {
        std::string commandToParse = commandLine;

        // Recognize a trailing '&' as background execution.
        // Quote-aware parsing will be added in Task 3.
        const std::size_t lastCharacter =
            commandToParse.find_last_not_of(" \t\r");

        bool background = false;

        if (lastCharacter != std::string::npos &&
            commandToParse[lastCharacter] == '&') {
            background = true;
            commandToParse.erase(lastCharacter, 1);
        }

        // First determine whether the input contains an outer pipeline.
        const std::vector<std::string> pipelineStages =
            parser.splitPipeline(commandToParse);

        if (pipelineStages.size() > 1) {
            // Background pipelines will be handled during later job-control work.
            if (background) {
                std::cerr
                    << "terminator: background pipelines are not supported yet\n";

                lastExitStatus = 2;
                return lastExitStatus;
            }

            std::vector<std::vector<std::string>> pipelineCommands;

            // Parse every stage using the existing quote-aware tokenizer.
            for (const std::string& stage : pipelineStages) {
                const std::vector<std::string> stageArguments =
                    parser.parse(stage, lastExitStatus);

                // A leading, trailing or consecutive pipe creates an empty stage.
                if (stageArguments.empty()) {
                    std::cerr
                        << "terminator: invalid empty pipeline stage\n";

                    lastExitStatus = 2;
                    return lastExitStatus;
                }

                pipelineCommands.push_back(stageArguments);
            }

            // Execute the pipeline and preserve its exit status for later $?
            // expansions.
            lastExitStatus = pipelineExecutor.executePipeline(pipelineCommands);

            return lastExitStatus;
        }

        const std::vector<std::string> arguments =
            parser.parse(commandToParse, lastExitStatus);

        // Do not change $? for an empty line.
        if (arguments.empty()) {
            return lastExitStatus;
        }

        jobs.update();

        // Preserve the existing new-tab feature.
        if (arguments[0] == "newtab" && arguments.size() == 1) {
            std::cout << "\033]777;newtab\a" << std::flush;
            lastExitStatus = 0;
            return lastExitStatus;
        }

        // Built-ins execute in the current shell process.
        if (builtins.execute(
                arguments,
                lastExitStatus,
                shouldExit,
                executeLine,
                history,
                jobs)) {
            return lastExitStatus;
        }

        pid_t startedProcess = -1;
        pid_t startedProcessGroup = -1;
        bool processStopped = false;

        lastExitStatus = executor.execute(
            arguments,
            background,
            &startedProcess,
            &startedProcessGroup,
            &processStopped
        );

        // Register background jobs and foreground jobs stopped with Ctrl+Z.
        if (startedProcess > 0 && (background || processStopped)) {
            const JobState initialState = background
                ? JobState::Running
                : JobState::Stopped;

            const int jobId = jobs.addJob(
                startedProcess,
                startedProcessGroup,
                commandLine,
                initialState
            );

            if (background) {
                std::cout << '[' << jobId << "] "
                        << startedProcess << '\n' << std::flush;

                lastExitStatus = 0;
            } else {
                std::cout << '[' << jobId << "] Stopped "
                        << commandLine << '\n' << std::flush;
            }
        }
        return lastExitStatus;
    };

    while (true) {
        std::cout << "user@terminator> " << std::flush;

        if (!readCommandLine(history, line)) {
            break;
        }

        // Ignore whitespace-only input.
        if (line.find_first_not_of(" \t\r") == std::string::npos) {
            continue;
        }

        // Record interactive input before executing it, so "history"
        // also appears in the displayed list.
        history.add(line);

        executeLine(line);

        if (shouldExit) {
            break;
        }
    }

    return lastExitStatus;
}