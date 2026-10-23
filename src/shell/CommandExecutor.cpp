#include "shell/CommandExecutor.h"

#include <cerrno>
#include <csignal>
#include <cstdio>

#include <cstring>
#include <vector>
#include <array>

#include <fcntl.h>
#include <iostream>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int CommandExecutor::execute(
    const std::vector<std::string>& arguments,
    bool background,
    pid_t* startedProcess,
    pid_t* startedProcessGroup,
    bool* processStopped
) const {
    if (startedProcess != nullptr) {
        *startedProcess = -1;
    }

    if (startedProcessGroup != nullptr) {
        *startedProcessGroup = -1;
    }

    if (processStopped != nullptr) {
        *processStopped = false;
    }

    if (arguments.empty()) {
        return 0;
    }

    // execvp() requires a null-terminated array of C-style strings.
    std::vector<char*> argv;

    for (const std::string& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }

    argv.push_back(nullptr);

    // This pipe tells the parent whether execvp() succeeded.
    int execStatusPipe[2];

    if (pipe(execStatusPipe) == -1) {
        std::cerr << "terminator: pipe: "
                  << std::strerror(errno) << '\n';
        return 1;
    }

    // Successful execvp() closes the write end automatically.
    const int flags = fcntl(execStatusPipe[1], F_GETFD);

    if (flags == -1 ||
        fcntl(execStatusPipe[1], F_SETFD, flags | FD_CLOEXEC) == -1) {
        const int savedError = errno;
        close(execStatusPipe[0]);
        close(execStatusPipe[1]);

        std::cerr << "terminator: fcntl: "
                  << std::strerror(savedError) << '\n';
        return 1;
    }

    const pid_t childPid = fork();

    if (childPid == -1) {
        const int savedError = errno;
        close(execStatusPipe[0]);
        close(execStatusPipe[1]);

        std::cerr << "terminator: fork: "
                  << std::strerror(savedError) << '\n';
        return 1;
    }

    if (childPid == 0) {
        close(execStatusPipe[0]);

        // Give the child its own process group.
        (void)setpgid(0, 0);

        // External programs must have normal signal behavior.
        std::signal(SIGINT, SIG_DFL);
        std::signal(SIGQUIT, SIG_DFL);
        std::signal(SIGTSTP, SIG_DFL);
        std::signal(SIGTTIN, SIG_DFL);
        std::signal(SIGTTOU, SIG_DFL);

        execvp(argv[0], argv.data());

        // Reached only if execvp() failed.
        const int executionError = errno;

        // Send the error to the parent before terminating.
        (void)write(
            execStatusPipe[1],
            &executionError,
            sizeof(executionError)
        );

        _exit(
            executionError == ENOENT || executionError == ENOTDIR
                ? 127
                : 126
        );
    }

    close(execStatusPipe[1]);

    // Establish the child's process group from the parent side too.
    (void)setpgid(childPid, childPid);

    // A successful exec closes the pipe; a failed exec sends errno.
    int executionError = 0;
    ssize_t statusBytes;

    do {
        statusBytes = read(
            execStatusPipe[0],
            &executionError,
            sizeof(executionError)
        );
    } while (statusBytes == -1 && errno == EINTR);

    close(execStatusPipe[0]);

    if (statusBytes != 0) {
        // Do not register an unsuccessful exec as a running job.
        int childStatus = 0;

        while (waitpid(childPid, &childStatus, 0) == -1 &&
               errno == EINTR) {
        }

        if (statusBytes > 0) {
            if (executionError == ENOENT ||
                executionError == ENOTDIR) {
                std::cerr << "terminator: " << arguments[0]
                          << ": command not found\n";
                return 127;
            }

            std::cerr << "terminator: " << arguments[0]
                      << ": " << std::strerror(executionError) << '\n';
            return 126;
        }

        std::cerr << "terminator: cannot confirm command execution: "
                  << std::strerror(errno) << '\n';
        return 1;
    }

    // Register the process only after execvp() succeeds.
    if (startedProcess != nullptr) {
        *startedProcess = childPid;
    }

    if (startedProcessGroup != nullptr) {
        *startedProcessGroup = childPid;
    }

    // A successfully launched background command returns immediately.
    if (background) {
        return 0;
    }

    // Give the foreground command control of the terminal.
    const pid_t shellProcessGroup = getpgrp();
    bool terminalHandedOver = false;

    if (isatty(STDIN_FILENO) &&
        tcsetpgrp(STDIN_FILENO, childPid) == 0) {
        terminalHandedOver = true;
    }

    int status = 0;
    pid_t waitResult;

    do {
        waitResult = waitpid(childPid, &status, WUNTRACED);
    } while (waitResult == -1 && errno == EINTR);

    // Restore terminal ownership to the shell.
    if (terminalHandedOver) {
        if (tcsetpgrp(STDIN_FILENO, shellProcessGroup) == -1) {
            std::cerr << "terminator: tcsetpgrp: "
                      << std::strerror(errno) << '\n';
        }
    }

    if (waitResult == -1) {
        std::cerr << "terminator: waitpid: "
                  << std::strerror(errno) << '\n';
        return 1;
    }

    if (WIFSTOPPED(status)) {
        if (processStopped != nullptr) {
            *processStopped = true;
        }

        return 128 + WSTOPSIG(status);
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    return 1;
}


int CommandExecutor::executePipeline(
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

    // N commands need N - 1 pipes.
    // Each pipe stores {read end, write end}.
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

    // Retain each child PID so that we can wait for the exact
    // processes created for this pipeline.
    std::vector<pid_t> childPids;
    childPids.reserve(commandCount);

    // Launch every command. The children can run concurrently;
    // the parent does not wait until all stages have been started.
    for (std::size_t i = 0; i < commandCount; ++i) {
        pid_t pid = fork();

        if (pid == -1) {
            std::perror("fork");

            // Release the parent's pipe descriptors first.
            closeAllPipes();

            // Stop and reap every child already started.
            for (pid_t childPid : childPids) {
                if (kill(childPid, SIGTERM) == -1 &&
                    errno != ESRCH) {
                    std::perror("kill");
                }
            }

            for (pid_t childPid : childPids) {
                int status;

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

            return 1;
        }

        if (pid == 0) {
            // Restore ordinary command signal behavior.
            // Full foreground process-group integration belongs
            // to the later job-control checkpoint.
            std::signal(SIGINT, SIG_DFL);
            std::signal(SIGQUIT, SIG_DFL);
            std::signal(SIGTSTP, SIG_DFL);
            std::signal(SIGTTIN, SIG_DFL);
            std::signal(SIGTTOU, SIG_DFL);

            // If this is not the first stage, read from the
            // previous pipe.
            if (i > 0) {
                if (dup2(pipes[i - 1][0], STDIN_FILENO) == -1) {
                    std::perror("dup2 stdin");
                    _exit(126);
                }
            }

            // If this is not the last stage, write into the
            // next pipe.
            if (i + 1 < commandCount) {
                if (dup2(pipes[i][1], STDOUT_FILENO) == -1) {
                    std::perror("dup2 stdout");
                    _exit(126);
                }
            }

            // Each child inherits all the pipes created by the
            // parent. After dup2(), it only needs standard input
            // and standard output, so close all original pipe FDs.
            for (const auto& pipeFd : pipes) {
                close(pipeFd[0]);
                close(pipeFd[1]);
            }

            // Build argv: command name, arguments, then nullptr.
            // execvp() uses this array to launch the external command.
            std::vector<char*> argv;
            argv.reserve(commands[i].size() + 1);

            for (const std::string& arg : commands[i]) {
                argv.push_back(const_cast<char*>(arg.c_str()));
            }

            argv.push_back(nullptr);

            execvp(argv[0], argv.data());

            // execvp() only returns when execution fails.
            int savedErrno = errno;
            std::perror(commands[i][0].c_str());

            _exit(
                (savedErrno == ENOENT || savedErrno == ENOTDIR)
                    ? 127
                    : 126
            );
        }

        // Parent: record the new child and continue launching stages.
        childPids.push_back(pid);
    }

    // The parent must close its copies of every pipe end.
    // Otherwise, a reader may never receive EOF.
    closeAllPipes();

    // Wait for every child we launched. Retry interrupted waits.
    // Remember the status of the final pipeline stage.
    std::vector<int> statuses(commandCount, 0);
    bool waitFailed = false;

    for (std::size_t i = 0; i < commandCount; ++i) {
        while (true) {
            pid_t waited = waitpid(childPids[i], &statuses[i], 0);

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

    // Pipeline status is the status of the LAST command,
    // not the first failed command.
    const int lastStatus = statuses.back();

    if (WIFEXITED(lastStatus)) {
        return WEXITSTATUS(lastStatus);
    }

    if (WIFSIGNALED(lastStatus)) {
        return 128 + WTERMSIG(lastStatus);
    }

    return 1;
}