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
