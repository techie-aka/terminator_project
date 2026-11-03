#include "shell/ShellHelper.h"

#include "shell/ShellIO.h"
#include "shell/SignalControl.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

std::string findHelperExecutable() {
    char executablePath[PATH_MAX + 1];
    const ssize_t length = readlink(
        "/proc/self/exe",
        executablePath,
        PATH_MAX
    );

    if (length <= 0) {
        return "";
    }

    executablePath[length] = '\0';
    std::string path(executablePath);

    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return "";
    }

    // Both binaries are built into the same build/ directory.
    return path.substr(0, slash + 1) + "terminator-shell";
}

bool spawnShellCommand(
    const std::string& command,
    int inputFd,
    int outputFd,
    int errorFd,
    pid_t& childPid,
    std::string& errorMessage
) {
    const std::string executable = findHelperExecutable();
    if (executable.empty() || access(executable.c_str(), X_OK) == -1) {
        errorMessage = "cannot find executable build/terminator-shell";
        return false;
    }

    posix_spawn_file_actions_t actions;
    int result = posix_spawn_file_actions_init(&actions);
    if (result != 0) {
        errorMessage = std::string("spawn file actions: ") +
                       std::strerror(result);
        return false;
    }

    // Connect the helper's standard streams to the requested descriptors.
    if (inputFd != STDIN_FILENO) {
        result = posix_spawn_file_actions_adddup2(
            &actions, inputFd, STDIN_FILENO
        );
    }

    if (result == 0 && outputFd != STDOUT_FILENO) {
        result = posix_spawn_file_actions_adddup2(
            &actions, outputFd, STDOUT_FILENO
        );
    }

    if (result == 0 && errorFd != STDERR_FILENO) {
        result = posix_spawn_file_actions_adddup2(
            &actions, errorFd, STDERR_FILENO
        );
    }

    if (result != 0) {
        posix_spawn_file_actions_destroy(&actions);
        errorMessage = std::string("spawn stream setup: ") +
                       std::strerror(result);
        return false;
    }

    posix_spawnattr_t attributes;
    result = posix_spawnattr_init(&attributes);
    if (result != 0) {
        posix_spawn_file_actions_destroy(&actions);
        errorMessage = std::string("spawn attributes: ") +
                       std::strerror(result);
        return false;
    }

    // The helper gets its own process group. This lets the GUI send signals
    // to this helper without affecting another tab or the GUI process.
    sigset_t defaultSignals;
    sigset_t emptyMask;

    sigemptyset(&defaultSignals);
    sigaddset(&defaultSignals, SIGINT);
    sigaddset(&defaultSignals, SIGQUIT);
    sigaddset(&defaultSignals, SIGTSTP);
    sigaddset(&defaultSignals, SIGTTIN);
    sigaddset(&defaultSignals, SIGTTOU);
    sigaddset(&defaultSignals, SIGPIPE);
    sigemptyset(&emptyMask);

    result = posix_spawnattr_setsigdefault(
        &attributes, &defaultSignals
    );
    if (result == 0) {
        result = posix_spawnattr_setsigmask(&attributes, &emptyMask);
    }
    if (result == 0) {
        result = posix_spawnattr_setpgroup(&attributes, 0);
    }
    if (result == 0) {
        result = posix_spawnattr_setflags(
            &attributes,
            POSIX_SPAWN_SETSIGDEF |
            POSIX_SPAWN_SETSIGMASK |
            POSIX_SPAWN_SETPGROUP
        );
    }

    if (result != 0) {
        posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        errorMessage = std::string("spawn signal setup: ") +
                       std::strerror(result);
        return false;
    }

    char* arguments[] = {
        const_cast<char*>(executable.c_str()),
        const_cast<char*>("-c"),
        const_cast<char*>(command.c_str()),
        nullptr
    };

    result = posix_spawn(
        &childPid,
        executable.c_str(),
        &actions,
        &attributes,
        arguments,
        environ
    );

    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);

    if (result != 0) {
        errorMessage = std::string("starting helper shell: ") +
                       std::strerror(result);
        return false;
    }

    return true;
}

int statusToExitCode(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    if (WIFSTOPPED(status)) {
        return 128 + WSTOPSIG(status);
    }

    return 1;
}

bool waitForChild(pid_t childPid, int& status) {
    while (true) {
        const pid_t result = waitpid(childPid, &status, 0);

        if (result == childPid) {
            return true;
        }

        if (result == -1 && errno == EINTR) {
            continue;
        }

        return false;
    }
}

} // namespace

int ShellHelper::runCommand(
    const std::string& command,
    int outputFd,
    int errorFd,
    ShellIO* owner
) {
    const int nullInput = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (nullInput == -1) {
        if (errorFd >= 0) {
            dprintf(errorFd, "terminator: cannot open /dev/null: %s\n",
                    std::strerror(errno));
        }
        return 1;
    }

    pid_t childPid = -1;
    std::string errorMessage;
    const bool started = spawnShellCommand(
        command,
        nullInput,
        outputFd,
        errorFd,
        childPid,
        errorMessage
    );
    close(nullInput);

    if (!started) {
        if (errorFd >= 0) {
            dprintf(errorFd, "terminator: %s\n", errorMessage.c_str());
        }
        return 1;
    }

    if (owner != nullptr) {
        // Thread mode: store the helper PID in this tab's ShellIO.
        owner->setForegroundHelper(childPid);
    } else {
        // Process mode: mark this PID as a helper process group.
        SignalControl::setForegroundHelperProcessGroup(childPid);
    }

    int status = 0;
    const bool waited = waitForChild(childPid, status);

    if (owner != nullptr) {
        owner->clearForegroundHelper();
    } else {
        SignalControl::clearForegroundHelperProcessGroup();
    }

    if (!waited) {
        if (errorFd >= 0) {
            dprintf(errorFd, "terminator: waitpid: %s\n",
                    std::strerror(errno));
        }
        return 1;
    }

    return statusToExitCode(status);
}

bool ShellHelper::captureOutput(
    const std::string& command,
    int errorFd,
    std::string& output,
    std::string& errorMessage,
    ShellIO* owner
) {
    output.clear();
    errorMessage.clear();

    int outputPipe[2] = {-1, -1};
    if (pipe2(outputPipe, O_CLOEXEC) == -1) {
        errorMessage = std::string("command substitution pipe: ") +
                       std::strerror(errno);
        return false;
    }

    const int nullInput = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (nullInput == -1) {
        const int savedError = errno;
        close(outputPipe[0]);
        close(outputPipe[1]);
        errno = savedError;
        errorMessage = std::string("command substitution input: ") +
                       std::strerror(errno);
        return false;
    }

    pid_t childPid = -1;
    const bool started = spawnShellCommand(
        command,
        nullInput,
        outputPipe[1],
        errorFd,
        childPid,
        errorMessage
    );

    close(nullInput);
    close(outputPipe[1]);

    if (!started) {
        close(outputPipe[0]);
        return false;
    }

    // In thread mode, the GUI uses this PID for Ctrl+C forwarding.
    // In process mode, SignalControl forwards signals to this helper group.
    if (owner != nullptr) {
        // Thread mode: store the helper PID in this tab's ShellIO.
        owner->setForegroundHelper(childPid);
    } else {
        // Process mode: mark this PID as a helper process group.
        SignalControl::setForegroundHelperProcessGroup(childPid);
    }

    char buffer[4096];
    bool readSucceeded = true;

    while (true) {
        const ssize_t bytesRead = read(
            outputPipe[0], buffer, sizeof(buffer)
        );

        if (bytesRead > 0) {
            output.append(buffer, static_cast<std::size_t>(bytesRead));
        } else if (bytesRead == 0) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            errorMessage = std::string("command substitution read: ") +
                           std::strerror(errno);
            readSucceeded = false;
            break;
        }
    }

    close(outputPipe[0]);

    int status = 0;
    const bool waited = waitForChild(childPid, status);

    if (owner != nullptr) {
        owner->clearForegroundHelper();
    } else {
        SignalControl::clearForegroundHelperProcessGroup();
    }

    if (!waited) {
        errorMessage = std::string("command substitution waitpid: ") +
                       std::strerror(errno);
        return false;
    }

    // A non-zero inner command is valid: its stdout is still substituted.
    return readSucceeded;
}