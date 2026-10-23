#include "shell/PipelineExecutor.h"

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
    const std::vector<std::vector<std::string>>& commands
) const {
    const std::size_t commandCount = commands.size();

    // A pipeline requires at least two commands.
    if (commandCount < 2) {
        std::fprintf(
            stderr,
            "terminator: a pipeline requires at least two commands\n"
        );
        return 2;
    }

    // Reject empty stages before creating any OS resources.
    // This handles leading, trailing and consecutive pipes.
    for (const auto& command : commands) {
        if (command.empty() || command[0].empty()) {
            std::fprintf(
                stderr,
                "terminator: invalid empty pipeline stage\n"
            );
            return 2;
        }
    }

    // N commands need N - 1 pipes. Each pipe contains its read and write ends.
    std::vector<std::array<int, 2>> pipes;
    pipes.reserve(commandCount - 1);

    // Close every pipe descriptor currently held by this process.
    auto closeAllPipes = [&pipes]() {
        for (const auto& pipeFd : pipes) {
            close(pipeFd[0]);
            close(pipeFd[1]);
        }
    };

    // Create all the pipes before launching the commands.
    for (std::size_t i = 0; i + 1 < commandCount; ++i) {
        int pipeFd[2];

        if (pipe(pipeFd) == -1) {
            std::perror("pipe");
            closeAllPipes();
            return 1;
        }

        pipes.push_back({pipeFd[0], pipeFd[1]});
    }

    // Retain each child PID so we can wait for the exact processes
    // created for this pipeline.
    std::vector<pid_t> childPids;
    childPids.reserve(commandCount);

    // Launch every command before waiting, so the stages can run concurrently.
    for (std::size_t i = 0; i < commandCount; ++i) {
        const pid_t pid = fork();

        if (pid == -1) {
            const int forkError = errno;
            std::perror("fork");

            // Release the parent's copies of the pipe descriptors.
            closeAllPipes();

            // Stop and reap every child already started.
            for (pid_t childPid : childPids) {
                if (kill(childPid, SIGTERM) == -1 && errno != ESRCH) {
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
            // Restore ordinary command signal behavior. Full pipeline process
            // groups and terminal handoff belong to the later job-control step.
            std::signal(SIGINT, SIG_DFL);
            std::signal(SIGQUIT, SIG_DFL);
            std::signal(SIGTSTP, SIG_DFL);
            std::signal(SIGTTIN, SIG_DFL);
            std::signal(SIGTTOU, SIG_DFL);

            // Every stage except the first reads from the previous pipe.
            if (i > 0) {
                if (dup2(pipes[i - 1][0], STDIN_FILENO) == -1) {
                    std::perror("dup2 stdin");
                    _exit(126);
                }
            }

            // Every stage except the last writes to the next pipe.
            if (i + 1 < commandCount) {
                if (dup2(pipes[i][1], STDOUT_FILENO) == -1) {
                    std::perror("dup2 stdout");
                    _exit(126);
                }
            }

            // The child now uses stdin/stdout, so close all original pipe FDs.
            for (const auto& pipeFd : pipes) {
                close(pipeFd[0]);
                close(pipeFd[1]);
            }

            // execvp() expects argv to end with a null pointer.
            std::vector<char*> argv;
            argv.reserve(commands[i].size() + 1);

            for (const std::string& arg : commands[i]) {
                argv.push_back(const_cast<char*>(arg.c_str()));
            }
            argv.push_back(nullptr);

            execvp(argv[0], argv.data());

            // execvp() returns only when launching the command fails.
            const int executionError = errno;
            std::perror(commands[i][0].c_str());

            _exit(
                (executionError == ENOENT || executionError == ENOTDIR)
                    ? 127
                    : 126
            );
        }

        // Parent records the child and continues launching stages.
        childPids.push_back(pid);
    }

    // Critical: the parent must close all its own pipe descriptors.
    // Otherwise, a reader may never observe EOF.
    closeAllPipes();

    // Wait for each child, retrying if a wait is interrupted by a signal.
    // Pipeline status is determined by the last command's status.
    std::vector<int> statuses(commandCount, 0);
    bool waitFailed = false;

    for (std::size_t i = 0; i < commandCount; ++i) {
        while (true) {
            const pid_t waited = waitpid(childPids[i], &statuses[i], 0);

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

    const int lastStatus = statuses.back();

    if (WIFEXITED(lastStatus)) {
        return WEXITSTATUS(lastStatus);
    }

    if (WIFSIGNALED(lastStatus)) {
        return 128 + WTERMSIG(lastStatus);
    }

    return 1;
}
