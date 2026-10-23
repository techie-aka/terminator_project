#include "shell/Builtins.h"
#include "shell/CommandExecutor.h"
#include "shell/CommandParser.h"
#include "shell/History.h"
#include "shell/JobManager.h"

#include <csignal>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <algorithm>
#include <termios.h>
#include <unistd.h>

bool readCommandLine(
    const History& history,
    std::string& line
) {
    termios originalSettings {};

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

    std::vector<std::string> entries = history.getEntries();
    std::size_t historyPosition = entries.size();
    std::string draft;
    line.clear();

    const std::string prompt = "user@terminator> ";

    auto redrawLine = [&]() {
        std::cout << '\r' << "\033[K" << prompt << line << std::flush;
    };

    while (true) {
        char character = '\0';

        if (::read(STDIN_FILENO, &character, 1) != 1) {
            tcsetattr(STDIN_FILENO, TCSANOW, &originalSettings);
            return false;
        }

        if (character == '\r' || character == '\n') {
            std::cout << '\r' << "\033[K"
                      << prompt << line << '\n' << std::flush;
            break;
        }

        if (character == 4 && line.empty()) {
            // Ctrl+D on an empty line ends the shell input loop.
            tcsetattr(STDIN_FILENO, TCSANOW, &originalSettings);
            return false;
        }

        if (character == 127 || character == '\b') {
            if (!line.empty()) {
                line.pop_back();
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

            if (direction == 'A') {
                // Up: refresh shared history when starting navigation.
                if (historyPosition == entries.size()) {
                    draft = line;

                    // Fetch the latest entries, including commands from other tabs.
                    entries = history.getEntries();
                    historyPosition = entries.size();
                }

                if (historyPosition > 0) {
                    --historyPosition;
                    line = entries[historyPosition];
                    redrawLine();
                }
            } else if (direction == 'B') {
                // Down: move to a newer entry or restore the draft.
                if (historyPosition < entries.size()) {
                    ++historyPosition;

                    if (historyPosition == entries.size()) {
                        line = draft;
                    } else {
                        line = entries[historyPosition];
                    }

                    redrawLine();
                }
            }

            continue;
        }

        if (character >= 32 && character <= 126) {
            line += character;
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
            lastExitStatus = executor.executePipeline(pipelineCommands);

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