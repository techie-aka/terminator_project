#include "shell/CommandExecutor.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

int CommandExecutor::execute(
    const std::vector<std::string>& arguments
) const {
    if (arguments.empty()) {
        return 0;
    }

    // execvp() requires a null-terminated array of C-style strings.
    std::vector<char*> argv;

    for (const std::string& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }

    argv.push_back(nullptr);

    const pid_t childPid = fork();

    if (childPid == -1) {
        std::cerr << "terminator: fork: "
                  << std::strerror(errno) << '\n';
        return 1;
    }

    if (childPid == 0) {
        // Child process: give this command its own process group.
        (void)setpgid(0, 0);

        // Restore normal signal behavior for the external program.
        std::signal(SIGINT, SIG_DFL);
        std::signal(SIGQUIT, SIG_DFL);
        std::signal(SIGTSTP, SIG_DFL);
        std::signal(SIGTTIN, SIG_DFL);
        std::signal(SIGTTOU, SIG_DFL);

        execvp(argv[0], argv.data());

        // execvp() returns only if execution failed.
        const int executionError = errno;

        if (executionError == ENOENT || executionError == ENOTDIR) {
            dprintf(
                STDERR_FILENO,
                "terminator: %s: command not found\n",
                arguments[0].c_str()
            );
            _exit(127);
        }

        dprintf(
            STDERR_FILENO,
            "terminator: %s: %s\n",
            arguments[0].c_str(),
            std::strerror(executionError)
        );

        _exit(126);
    }

    // Parent process: establish the child's process group.
    (void)setpgid(childPid, childPid);

    // Give the external command the terminal while it runs.
    const pid_t shellProcessGroup = getpgrp();
    bool terminalHandedOver = false;

    if (isatty(STDIN_FILENO) &&
        tcsetpgrp(STDIN_FILENO, childPid) == 0) {
        terminalHandedOver = true;
    }

    // Wait for the foreground command to complete.
    int status = 0;
    pid_t waitResult;

    do {
        waitResult = waitpid(childPid, &status, 0);
    } while (waitResult == -1 && errno == EINTR);

    // Return terminal control to our shell.
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

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    return 1;
}