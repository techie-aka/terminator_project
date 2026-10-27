#include "shell/CommandExecutor.h"
#include "shell/Redirection.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr int EXEC_FAILURE = 1;
constexpr int REDIRECTION_FAILURE = 2;

struct ChildSetupFailure {
    int errorNumber;
    int kind;
    bool input;
};

// Report a child setup failure through the close-on-exec status pipe.
void reportChildFailure(
    int fd,
    int errorNumber,
    int kind,
    bool input = false
) {
    const ChildSetupFailure failure{errorNumber, kind, input};
    const char* data = reinterpret_cast<const char*>(&failure);
    std::size_t remaining = sizeof(failure);

    while (remaining > 0) {
        const ssize_t written = write(fd, data, remaining);

        if (written > 0) {
            data += written;
            remaining -= static_cast<std::size_t>(written);
        } else if (written == -1 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

// Wait for one specific child, retrying interrupted waits.
bool waitForChild(pid_t pid, int& status) {
    while (true) {
        const pid_t result = waitpid(pid, &status, 0);

        if (result == pid) {
            return true;
        }

        if (result == -1 && errno == EINTR) {
            continue;
        }

        return false;
    }
}

} // namespace

int CommandExecutor::execute(
    const std::vector<std::string>& arguments,
    bool background,
    pid_t* startedProcess,
    pid_t* startedProcessGroup,
    bool* processStopped,
    const RedirectionSpec* redirection
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

    // execvp() expects a null-terminated argv array.
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);

    for (const std::string& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }

    argv.push_back(nullptr);

    // The status pipe distinguishes successful exec from setup failure.
    // FD_CLOEXEC closes the write end automatically after successful exec.
    int execStatusPipe[2];

    if (pipe(execStatusPipe) == -1) {
        std::cerr << "terminator: pipe: "
                  << std::strerror(errno) << '\n';
        return 1;
    }

    const int flags = fcntl(execStatusPipe[1], F_GETFD);

    if (flags == -1 ||
        fcntl(
            execStatusPipe[1],
            F_SETFD,
            flags | FD_CLOEXEC
        ) == -1) {
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

        // Give the external command its own process group.
        (void)setpgid(0, 0);

        // External programs must have ordinary signal behavior.
        std::signal(SIGINT, SIG_DFL);
        std::signal(SIGQUIT, SIG_DFL);
        std::signal(SIGTSTP, SIG_DFL);
        std::signal(SIGTTIN, SIG_DFL);
        std::signal(SIGTTOU, SIG_DFL);

        // Redirection must succeed before the command is executed.
        if (redirection != nullptr) {
            RedirectionFailure failure;

            if (!Redirection::applyInChild(*redirection, failure)) {
                reportChildFailure(
                    execStatusPipe[1],
                    failure.errorNumber,
                    REDIRECTION_FAILURE,
                    failure.input
                );

                _exit(1);
            }
        }

        execvp(argv[0], argv.data());

        // execvp() returns only if execution failed.
        reportChildFailure(
            execStatusPipe[1],
            errno,
            EXEC_FAILURE
        );

        _exit(
            errno == ENOENT || errno == ENOTDIR ? 127 : 126
        );
    }

    close(execStatusPipe[1]);

    // Establish the process group from the parent side too.
    (void)setpgid(childPid, childPid);

    // Successful exec produces EOF on this pipe.
    // Failures send a complete ChildSetupFailure structure.
    ChildSetupFailure childFailure{};
    char* destination = reinterpret_cast<char*>(&childFailure);
    std::size_t bytesReceived = 0;
    bool statusReadFailed = false;

    while (bytesReceived < sizeof(childFailure)) {
        const ssize_t bytesRead = read(
            execStatusPipe[0],
            destination + bytesReceived,
            sizeof(childFailure) - bytesReceived
        );

        if (bytesRead > 0) {
            bytesReceived += static_cast<std::size_t>(bytesRead);
        } else if (bytesRead == 0) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            statusReadFailed = true;
            break;
        }
    }

    close(execStatusPipe[0]);

    if (statusReadFailed) {
        const int savedError = errno;

        (void)kill(childPid, SIGTERM);

        int ignoredStatus = 0;
        (void)waitForChild(childPid, ignoredStatus);

        std::cerr << "terminator: cannot read child setup status: "
                  << std::strerror(savedError) << '\n';
        return 1;
    }

    if (bytesReceived != 0) {
        int childStatus = 0;

        if (!waitForChild(childPid, childStatus)) {
            std::cerr << "terminator: waitpid: "
                      << std::strerror(errno) << '\n';
            return 1;
        }

        if (bytesReceived != sizeof(childFailure)) {
            std::cerr << "terminator: incomplete child setup status\n";
            return 1;
        }

        if (childFailure.kind == REDIRECTION_FAILURE) {
            RedirectionFailure failure;
            failure.errorNumber = childFailure.errorNumber;
            failure.input = childFailure.input;

            if (redirection != nullptr) {
                failure.filename = failure.input
                    ? redirection->inputFile
                    : redirection->outputFile;
            }

            Redirection::reportFailure(failure);
            return 1;
        }

        if (childFailure.errorNumber == ENOENT ||
            childFailure.errorNumber == ENOTDIR) {
            std::cerr << "terminator: " << arguments[0]
                      << ": command not found\n";
            return 127;
        }

        std::cerr << "terminator: " << arguments[0]
                  << ": " << std::strerror(childFailure.errorNumber)
                  << '\n';
        return 126;
    }

    // Only successfully executed commands can be registered as jobs.
    if (startedProcess != nullptr) {
        *startedProcess = childPid;
    }

    if (startedProcessGroup != nullptr) {
        *startedProcessGroup = childPid;
    }

    if (background) {
        return 0;
    }

    // Give a foreground command access to the terminal.
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

    // Restore the shell's terminal ownership.
    if (terminalHandedOver &&
        tcsetpgrp(STDIN_FILENO, shellProcessGroup) == -1) {
        std::cerr << "terminator: tcsetpgrp: "
                  << std::strerror(errno) << '\n';
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