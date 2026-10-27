#include "shell/PipelineExecutor.h"
#include "shell/Redirection.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <string>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int PipelineExecutor::executePipeline(
    const std::vector<ParsedCommand>& commands
) const {
    const std::size_t commandCount = commands.size();

    if (commandCount < 2) {
        std::fprintf(
            stderr,
            "terminator: a pipeline requires at least two commands\n"
        );
        return 2;
    }

    // Reject empty commands before acquiring any file descriptors.
    for (const auto& command : commands) {
        if (command.arguments.empty() ||
            command.arguments[0].empty()) {
            std::fprintf(
                stderr,
                "terminator: invalid empty pipeline stage\n"
            );
            return 2;
        }
    }

    // N commands require N - 1 pipes.
    std::vector<std::array<int, 2>> pipes;
    pipes.reserve(commandCount - 1);

    auto closeAllPipes = [&pipes]() {
        for (const auto& pipeFd : pipes) {
            close(pipeFd[0]);
            close(pipeFd[1]);
        }
    };

    // Create every pipe before launching the stages.
    for (std::size_t i = 0; i + 1 < commandCount; ++i) {
        int pipeFd[2];

        if (pipe(pipeFd) == -1) {
            std::perror("pipe");
            closeAllPipes();
            return 1;
        }

        pipes.push_back({pipeFd[0], pipeFd[1]});
    }

    std::vector<pid_t> childPids;
    childPids.reserve(commandCount);

    // Start all stages before waiting for any individual child.
    for (std::size_t i = 0; i < commandCount; ++i) {
        const pid_t pid = fork();

        if (pid == -1) {
            const int forkError = errno;
            std::perror("fork");

            closeAllPipes();

            // Terminate children already launched.
            for (pid_t childPid : childPids) {
                if (kill(childPid, SIGTERM) == -1 &&
                    errno != ESRCH) {
                    std::perror("kill");
                }
            }

            for (pid_t childPid : childPids) {
                int status = 0;

                while (waitpid(childPid, &status, 0) == -1) {
                    if (errno == EINTR) {
                        continue;
                    }

                    if (errno != ECHILD) {
                        std::perror("waitpid");
                    }

                    break;
                }
            }

            errno = forkError;
            return 1;
        }

        if (pid == 0) {
            // Restore normal signal behavior in external commands.
            std::signal(SIGINT, SIG_DFL);
            std::signal(SIGQUIT, SIG_DFL);
            std::signal(SIGTSTP, SIG_DFL);
            std::signal(SIGTTIN, SIG_DFL);
            std::signal(SIGTTOU, SIG_DFL);

            // Connect this stage to the previous pipeline stage.
            if (i > 0) {
                if (dup2(pipes[i - 1][0], STDIN_FILENO) == -1) {
                    std::perror("dup2 stdin");
                    _exit(126);
                }
            }

            // Connect this stage to the next pipeline stage.
            if (i + 1 < commandCount) {
                if (dup2(pipes[i][1], STDOUT_FILENO) == -1) {
                    std::perror("dup2 stdout");
                    _exit(126);
                }
            }

            // Close inherited pipe descriptors after duplicating the
            // endpoints needed for this stage.
            for (const auto& pipeFd : pipes) {
                close(pipeFd[0]);
                close(pipeFd[1]);
            }

            // Explicit file redirection takes precedence over the
            // corresponding pipeline connection.
            RedirectionFailure failure;

            if (!Redirection::applyInChild(
                    commands[i].redirection, failure)) {
                Redirection::reportFailure(failure);
                _exit(1);
            }

            const std::vector<std::string>& arguments =
                commands[i].arguments;

            std::vector<char*> argv;
            argv.reserve(arguments.size() + 1);

            for (const std::string& argument : arguments) {
                argv.push_back(
                    const_cast<char*>(argument.c_str())
                );
            }

            argv.push_back(nullptr);

            execvp(argv[0], argv.data());

            const int executionError = errno;
            std::perror(arguments[0].c_str());

            _exit(
                executionError == ENOENT || executionError == ENOTDIR
                    ? 127
                    : 126
            );
        }

        childPids.push_back(pid);
    }

    // The parent must release its own copies of every pipe descriptor.
    closeAllPipes();

    std::vector<int> statuses(commandCount, 0);
    bool waitFailed = false;

    for (std::size_t i = 0; i < commandCount; ++i) {
        while (true) {
            const pid_t waited =
                waitpid(childPids[i], &statuses[i], 0);

            if (waited == childPids[i]) {
                break;
            }

            if (waited == -1 && errno == EINTR) {
                continue;
            }

            std::perror("waitpid");
            waitFailed = true;
            break;
        }
    }

    if (waitFailed) {
        return 1;
    }

    // The status of a pipeline is the status of its final stage.
    const int lastStatus = statuses.back();

    if (WIFEXITED(lastStatus)) {
        return WEXITSTATUS(lastStatus);
    }

    if (WIFSIGNALED(lastStatus)) {
        return 128 + WTERMSIG(lastStatus);
    }

    return 1;
}