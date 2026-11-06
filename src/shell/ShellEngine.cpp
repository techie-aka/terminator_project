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
#include "shell/SignalControl.h"
#include "shell/ShellEngine.h"
#include "shell/ShellIO.h"
#include "shell/ShellHelper.h"

#include <csignal>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <algorithm>
#include <termios.h>
#include <unistd.h>

int ShellEngine::runImpl(ShellIO* io, const std::string* singleCommand) {

    std::ostream& output = io ? io->out() : std::cout;
    std::ostream& error = io ? io->err() : std::cerr;
    
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
        error << "terminator: shared history is unavailable\n";
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
            int intervalSeconds = 0;
            std::string multiWatchError;

            if (!MultiWatch::parseInvocation(
                    commandLine,
                    monitoredCommands,
                    intervalSeconds,
                    multiWatchError)) {
                error << "terminator: multiWatch: "
                        << multiWatchError << '\n';

                lastExitStatus = 2;
                return lastExitStatus;
            }

            // In thread mode, execute MultiWatch in a separate shell process.
            // Its workers use fork(), so they must not be created from the
            // multithreaded GUI process. The helper inherits this tab's output.
            if (io != nullptr) {
                lastExitStatus = ShellHelper::runCommand(
                    commandLine,
                    io->getOutputFd(),
                    io->getErrorFd(),
                    io
                );
            } else {
                // Process mode already has one shell process per tab.
                lastExitStatus = multiWatch.execute(
                    monitoredCommands,
                    lastExitStatus,
                    intervalSeconds
                );
            }

            return lastExitStatus;
        }


        // First expand command substitutions. A pipe produced inside a
        // substitution must not accidentally become an outer pipeline.
        std::string commandToParse;
        std::string substitutionError;

        if (!commandSubstitution.expand(
                commandLine,
                commandToParse,
                substitutionError,
                io != nullptr ? io->getErrorFd() : STDERR_FILENO,
                io)) {
            error << "terminator: " << substitutionError << '\n';
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
                error
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
                    error << "terminator: " << parseError << '\n';
                    lastExitStatus = 2;
                    return lastExitStatus;
                }

                if (parsedStage.arguments.empty()) {
                    error
                        << "terminator: invalid empty pipeline stage\n";

                    lastExitStatus = 2;
                    return lastExitStatus;
                }

                pipelineCommands.push_back(parsedStage);
            }

            // Each stage carries its own input/output redirection metadata.
            lastExitStatus =
                pipelineExecutor.executePipeline(pipelineCommands, io);

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
            error << "terminator: " << parseError << '\n';
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
            error
                << "terminator: redirection for shell built-ins "
                << "is not supported yet\n";

            lastExitStatus = 2;
            return lastExitStatus;
        }

        // Preserve the existing new-tab feature.
        if (arguments[0] == "newtab" && arguments.size() == 1) {
            output << "\033]777;newtab\a" << std::flush;
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
                jobs,
                io)) {
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
            &parsedCommand.redirection,
            false,
            io
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
                output << '[' << jobId << "] "
                        << startedProcess << '\n' << std::flush;

                lastExitStatus = 0;
            } else {
                output << '[' << jobId << "] Stopped "
                        << commandLine << '\n' << std::flush;
            }
        }

        return lastExitStatus;
    };

    // Helper shells use this path to execute exactly one command without
    // displaying an interactive prompt or waiting for more input.
    if (singleCommand != nullptr) {
        return executeLine(*singleCommand);
    }

    while (!SignalControl::isShutdownRequested()) {
        const bool inputAvailable = io
            ? lineEditor.readCommandLine(history, line, *io)
            : lineEditor.readCommandLine(history, line);

        if (!inputAvailable) {
            break;
        }

        // Ignore whitespace-only input.
        if (line.find_first_not_of(" \t\r") == std::string::npos) {
            continue;
        }

        // Record the command before executing it.
        history.add(line);

        executeLine(line);

        // Exit if the user requested exit or the shell received
        // a shutdown signal.
        if (shouldExit || SignalControl::isShutdownRequested()) {
            break;
        }
    }

    return lastExitStatus;
}

int ShellEngine::run() {
    return runImpl(nullptr);
}

int ShellEngine::run(ShellIO& io) {
    return runImpl(&io);
}

int ShellEngine::runCommand(const std::string& command) {
    return runImpl(nullptr, &command);
}