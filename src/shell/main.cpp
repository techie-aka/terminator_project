#include "shell/Builtins.h"
#include "shell/CommandExecutor.h"
#include "shell/CommandParser.h"
#include "shell/History.h"
#include "shell/JobManager.h"
#include "shell/PipelineExecutor.h"
#include "shell/CommandSubstitution.h"
#include "shell/Redirection.h"
#include "shell/LineEditor.h"
#include "shell/MultiWatch.h"

#include <csignal>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <algorithm>
#include <termios.h>
#include <unistd.h>

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
    CommandSubstitution commandSubstitution;
    LineEditor lineEditor;
    MultiWatch multiWatch;

    if (!history.isReady()) {
        std::cerr << "terminator: shared history is unavailable\n";
    }

    std::string line;
    int lastExitStatus = 0;
    bool shouldExit = false;

    // A reusable execution path for interactive and sourced commands.
    std::function<int(const std::string&)> executeLine;

    executeLine = [&](const std::string& commandLine) -> int {

        // Handle multiWatch before command substitution and normal parsing.
        // Its quoted command-list syntax must remain intact.
        if (MultiWatch::isInvocation(commandLine)) {
            std::vector<std::string> monitoredCommands;
            std::string multiWatchError;

            if (!MultiWatch::parseInvocation(
                    commandLine,
                    monitoredCommands,
                    multiWatchError)) {
                std::cerr << "terminator: multiWatch: "
                        << multiWatchError << '\n';

                lastExitStatus = 2;
                return lastExitStatus;
            }

            lastExitStatus = multiWatch.execute(
                monitoredCommands,
                lastExitStatus
            );

            return lastExitStatus;
        }


        // First expand command substitutions. A pipe produced inside a
        // substitution must not accidentally become an outer pipeline.
        std::string commandToParse;
        std::string substitutionError;

        if (!commandSubstitution.expand(
                commandLine,
                executeLine,
                commandToParse,
                substitutionError)) {
            std::cerr << "terminator: " << substitutionError << '\n';
            lastExitStatus = 2;
            return lastExitStatus;
        }

        // Preserve the existing trailing '&' background-command behavior.
        const std::size_t lastCharacter =
            commandToParse.find_last_not_of(" \t\r\n");

        bool background = false;

        if (lastCharacter != std::string::npos &&
            commandToParse[lastCharacter] == '&') {
            background = true;
            commandToParse.erase(lastCharacter, 1);
        }

        // Split the command into pipeline stages before parsing the
        // redirection belonging to each individual stage.
        const std::vector<std::string> pipelineStages =
            parser.splitPipeline(commandToParse);

        if (pipelineStages.size() > 1) {
            if (background) {
                std::cerr
                    << "terminator: background pipelines are not supported yet\n";

                lastExitStatus = 2;
                return lastExitStatus;
            }

            std::vector<ParsedCommand> pipelineCommands;

            for (const std::string& stage : pipelineStages) {
                ParsedCommand parsedStage;
                std::string parseError;

                if (!parser.parseCommand(
                        stage,
                        lastExitStatus,
                        parsedStage,
                        parseError)) {
                    std::cerr << "terminator: " << parseError << '\n';
                    lastExitStatus = 2;
                    return lastExitStatus;
                }

                if (parsedStage.arguments.empty()) {
                    std::cerr
                        << "terminator: invalid empty pipeline stage\n";

                    lastExitStatus = 2;
                    return lastExitStatus;
                }

                pipelineCommands.push_back(parsedStage);
            }

            // Each stage carries its own input/output redirection metadata.
            lastExitStatus =
                pipelineExecutor.executePipeline(pipelineCommands);

            return lastExitStatus;
        }

        // Parse an ordinary command and extract optional redirections.
        ParsedCommand parsedCommand;
        std::string parseError;

        if (!parser.parseCommand(
                commandToParse,
                lastExitStatus,
                parsedCommand,
                parseError)) {
            std::cerr << "terminator: " << parseError << '\n';
            lastExitStatus = 2;
            return lastExitStatus;
        }

        const std::vector<std::string>& arguments =
            parsedCommand.arguments;

        // Empty input must not change the previous exit status.
        if (arguments.empty()) {
            return lastExitStatus;
        }

        jobs.update();

        const bool hasRedirection =
            parsedCommand.redirection.hasInput ||
            parsedCommand.redirection.hasOutput;

        // Built-ins execute in the shell process. This iteration supports
        // redirection only for external commands and pipeline stages.
        const std::string& commandName = arguments[0];

        const bool isShellBuiltin =
            commandName == "cd" ||
            commandName == "exit" ||
            commandName == "history" ||
            commandName == "source" ||
            commandName == "jobs" ||
            commandName == "fg" ||
            commandName == "bg" ||
            commandName == "multiWatch" ||
            commandName == "newtab";

        if (hasRedirection && isShellBuiltin) {
            std::cerr
                << "terminator: redirection for shell built-ins "
                << "is not supported yet\n";

            lastExitStatus = 2;
            return lastExitStatus;
        }

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

        // Pass the parsed redirection information to the external-command
        // executor. The executor applies it in the child before execvp().
        lastExitStatus = executor.execute(
            arguments,
            background,
            &startedProcess,
            &startedProcessGroup,
            &processStopped,
            &parsedCommand.redirection
        );

        // Preserve existing background job and stopped-job registration.
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
        if (!lineEditor.readCommandLine(history, line)) {
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