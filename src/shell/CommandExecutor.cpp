
#include "shell/CommandExecutor.h"
#include "shell/Redirection.h"
#include "shell/SignalControl.h"

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

#include "shell/ShellIO.h"

#include <spawn.h>
#include <signal.h>

extern char** environ;

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

class ScopedFd {
public:
    ScopedFd() = default;
    explicit ScopedFd(int fd) : fd_(fd) {}
    ~ScopedFd() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    int get() const { return fd_; }
    void reset(int fd) {
        if (fd_ >= 0) {
            close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

int executeWithShellIO(
    const std::vector<std::string>& arguments,
    bool background,
    pid_t* startedProcess,
    pid_t* startedProcessGroup,
    bool* processStopped,
    const RedirectionSpec* redirection,
    bool inheritProcessGroup,
    ShellIO& io
) {
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);

    for (const std::string& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);

    // Open redirections in the shell thread. This gives us an exact error
    // message and avoids confusing a missing redirection file with ENOENT
    // from posix_spawnp(command).
    ScopedFd inputRedirect;
    ScopedFd outputRedirect;

    if (redirection != nullptr && redirection->hasInput) {
        const int fd = open(
            redirection->inputFile.c_str(),
            O_RDONLY | O_CLOEXEC
        );
        if (fd == -1) {
            io.err() << "terminator: input redirection: "
                     << redirection->inputFile << ": "
                     << std::strerror(errno) << '\n';
            return 1;
        }
        inputRedirect.reset(fd);
    }

    if (redirection != nullptr && redirection->hasOutput) {
        const int fd = open(
            redirection->outputFile.c_str(),
            O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
            0644
        );
        if (fd == -1) {
            io.err() << "terminator: output redirection: "
                     << redirection->outputFile << ": "
                     << std::strerror(errno) << '\n';
            return 1;
        }
        outputRedirect.reset(fd);
    }

    posix_spawn_file_actions_t actions;
    int result = posix_spawn_file_actions_init(&actions);
    if (result != 0) {
        io.err() << "terminator: spawn setup: "
                 << std::strerror(result) << '\n';
        return 1;
    }

    int setupError = 0;

    auto addDup = [&](int source, int destination) {
        if (setupError == 0 && source != destination) {
            setupError = posix_spawn_file_actions_adddup2(
                &actions, source, destination
            );
        }
    };

    auto addClose = [&](int fd) {
        if (setupError == 0 && fd > STDERR_FILENO) {
            setupError = posix_spawn_file_actions_addclose(&actions, fd);
        }
    };

    // First connect the child to this tab's streams.
    addDup(io.getInputFd(), STDIN_FILENO);
    addDup(io.getOutputFd(), STDOUT_FILENO);
    addDup(io.getErrorFd(), STDERR_FILENO);

    // Then apply explicit redirection, which takes precedence over ShellIO.
    if (inputRedirect.get() >= 0) {
        addDup(inputRedirect.get(), STDIN_FILENO);
    }
    if (outputRedirect.get() >= 0) {
        addDup(outputRedirect.get(), STDOUT_FILENO);
    }

    // The child only needs the duplicated standard descriptors.
    const int ioDescriptors[] = {
        io.getInputFd(), io.getOutputFd(), io.getErrorFd()
    };
    for (std::size_t i = 0; i < 3 && setupError == 0; ++i) {
        const int fd = ioDescriptors[i];
        if (fd <= STDERR_FILENO) {
            continue;
        }
        bool duplicate = false;
        for (std::size_t j = 0; j < i; ++j) {
            if (ioDescriptors[j] == fd) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            addClose(fd);
        }
    }
    addClose(inputRedirect.get());
    addClose(outputRedirect.get());

    if (setupError != 0) {
        posix_spawn_file_actions_destroy(&actions);
        io.err() << "terminator: spawn file actions: "
                 << std::strerror(setupError) << '\n';
        return 1;
    }

    posix_spawnattr_t attributes;
    result = posix_spawnattr_init(&attributes);
    if (result != 0) {
        posix_spawn_file_actions_destroy(&actions);
        io.err() << "terminator: spawn attributes: "
                 << std::strerror(result) << '\n';
        return 1;
    }

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

    result = posix_spawnattr_setsigdefault(&attributes, &defaultSignals);
    if (result == 0) {
        result = posix_spawnattr_setsigmask(&attributes, &emptyMask);
    }

    short spawnFlags = POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
    if (result == 0 && !inheritProcessGroup) {
        result = posix_spawnattr_setpgroup(&attributes, 0);
        spawnFlags = static_cast<short>(spawnFlags | POSIX_SPAWN_SETPGROUP);
    }
    if (result == 0) {
        result = posix_spawnattr_setflags(&attributes, spawnFlags);
    }

    if (result != 0) {
        posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        io.err() << "terminator: spawn attributes: "
                 << std::strerror(result) << '\n';
        return 1;
    }

    pid_t childPid = -1;
    result = posix_spawnp(
        &childPid,
        argv[0],
        &actions,
        &attributes,
        argv.data(),
        environ
    );

    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);

    if (result != 0) {
        if (result == ENOENT || result == ENOTDIR) {
            io.err() << "terminator: " << arguments[0]
                     << ": command not found\n";
            return 127;
        }
        io.err() << "terminator: " << arguments[0]
                 << ": " << std::strerror(result) << '\n';
        return 126;
    }

    const pid_t childProcessGroup = inheritProcessGroup
        ? getpgrp()
        : childPid;

    if (startedProcess != nullptr) {
        *startedProcess = childPid;
    }
    if (startedProcessGroup != nullptr) {
        *startedProcessGroup = childProcessGroup;
    }

    if (background) {
        return 0;
    }

    // Do not publish the application's own process group as a tab's
    // foreground target. Inherited groups are used by internal helpers;
    // ordinary foreground commands always receive their own group.
    const bool publishedForegroundGroup = !inheritProcessGroup;
    if (publishedForegroundGroup) {
        io.setForegroundProcessGroup(childProcessGroup);
    }

    int status = 0;
    pid_t waitResult;
    do {
        waitResult = waitpid(childPid, &status, WUNTRACED);
    } while (waitResult == -1 && errno == EINTR);

    if (publishedForegroundGroup) {
        io.clearForegroundProcessGroup();
    }

    if (waitResult == -1) {
        io.err() << "terminator: waitpid: "
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

} // namespace

int CommandExecutor::execute(
    const std::vector<std::string>& arguments,
    bool background,
    pid_t* startedProcess,
    pid_t* startedProcessGroup,
    bool* processStopped,
    const RedirectionSpec* redirection,
    bool inheritProcessGroup,
    ShellIO* io
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

    // Thread mode uses posix_spawn so no C++ child-side work is
    // performed after fork() in the multithreaded GUI process.
    if (io != nullptr) {
        return executeWithShellIO(
            arguments,
            background,
            startedProcess,
            startedProcessGroup,
            processStopped,
            redirection,
            inheritProcessGroup,
            *io
        );
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

    /*
     * Block GUI notification signals during foreground process creation.
     *
     * Otherwise Ctrl+Z could arrive after fork() but before the foreground
     * process group is registered. Blocking makes the notification pending
     * until the correct process group is available.
     *
     * MultiWatch and background commands do not need this tracking.
     */
    const bool trackForeground =
        !background && !inheritProcessGroup;

    sigset_t originalSignalMask{};
    bool signalRequestsBlocked = false;

    auto restoreSignalRequests = [&]() -> bool {
        if (!signalRequestsBlocked) {
            return true;
        }

        if (sigprocmask(
                SIG_SETMASK,
                &originalSignalMask,
                nullptr
            ) == -1) {

            std::cerr << "terminator: sigprocmask: "
                      << std::strerror(errno) << '\n';
            return false;
        }

        signalRequestsBlocked = false;
        return true;
    };

    if (trackForeground) {
        sigset_t notificationSignals{};

        sigemptyset(&notificationSignals);
        sigaddset(&notificationSignals, SIGUSR1);
        sigaddset(&notificationSignals, SIGUSR2);

        if (sigprocmask(
                SIG_BLOCK,
                &notificationSignals,
                &originalSignalMask
            ) == -1) {

            const int savedError = errno;

            close(execStatusPipe[0]);
            close(execStatusPipe[1]);

            std::cerr << "terminator: sigprocmask: "
                      << std::strerror(savedError) << '\n';
            return 1;
        }

        signalRequestsBlocked = true;
    }

    const pid_t childPid = fork();

    if (childPid == -1) {
        const int savedError = errno;

        close(execStatusPipe[0]);
        close(execStatusPipe[1]);

        (void)restoreSignalRequests();

        std::cerr << "terminator: fork: "
                  << std::strerror(savedError) << '\n';
        return 1;
    }

    if (childPid == 0) {
        close(execStatusPipe[0]);

        // The child must not inherit the shell's temporarily blocked
        // GUI notification signals.
        if (trackForeground &&
            sigprocmask(
                SIG_SETMASK,
                &originalSignalMask,
                nullptr
            ) == -1) {

            reportChildFailure(
                execStatusPipe[1],
                errno,
                EXEC_FAILURE
            );

            _exit(126);
        }

        // Give the external command its own process group.
        if (!inheritProcessGroup) {
            setpgid(0, 0);
        }

        // External programs must have ordinary signal behavior.
        std::signal(SIGINT, SIG_DFL);
        std::signal(SIGQUIT, SIG_DFL);
        std::signal(SIGTSTP, SIG_DFL);
        std::signal(SIGTTIN, SIG_DFL);
        std::signal(SIGTTOU, SIG_DFL);

        // Redirection must succeed before the command is executed.
        if (redirection != nullptr) {
            RedirectionFailure failure;

            if (!Redirection::applyInChild(
                    *redirection,
                    failure
                )) {

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
        const int executionError = errno;

        reportChildFailure(
            execStatusPipe[1],
            executionError,
            EXEC_FAILURE
        );

        _exit(
            executionError == ENOENT || executionError == ENOTDIR
                ? 127
                : 126
        );
    }

    close(execStatusPipe[1]);

    // Establish the process group from the parent side too.
    if (!inheritProcessGroup) {
        setpgid(childPid, childPid);
    }

    /*
     * Read the child setup status.
     *
     * Successful exec closes the status pipe because of FD_CLOEXEC.
     * Failed setup writes a ChildSetupFailure structure.
     */
    ChildSetupFailure childFailure{};

    char* destination =
        reinterpret_cast<char*>(&childFailure);

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

    // Handle an error while reading the child setup status.
    if (statusReadFailed) {
        const int savedError = errno;

        if (trackForeground) {
            SignalControl::clearForegroundProcessGroup();
        }

        (void)kill(childPid, SIGTERM);

        int ignoredStatus = 0;
        (void)waitForChild(childPid, ignoredStatus);

        (void)restoreSignalRequests();

        std::cerr << "terminator: cannot read child setup status: "
                  << std::strerror(savedError) << '\n';

        return 1;
    }

    // The child reported an execution or redirection failure.
    if (bytesReceived != 0) {
        int childStatus = 0;

        if (!waitForChild(childPid, childStatus)) {
            const int savedError = errno;

            (void)restoreSignalRequests();

            std::cerr << "terminator: waitpid: "
                      << std::strerror(savedError) << '\n';
            return 1;
        }

        if (trackForeground) {
            SignalControl::clearForegroundProcessGroup();
        }

        (void)restoreSignalRequests();

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
                  << ": "
                  << std::strerror(childFailure.errorNumber)
                  << '\n';

        return 126;
    }

    // The child successfully executed.
    if (startedProcess != nullptr) {
        *startedProcess = childPid;
    }

    if (startedProcessGroup != nullptr) {
        *startedProcessGroup =
            inheritProcessGroup ? getpgrp() : childPid;
    }

    // Background commands do not become the foreground signal target.
    if (background) {
        return 0;
    }

    /*
     * Publish the foreground process group before unblocking notifications.
     *
     * Any Ctrl+C or Ctrl+Z received during process creation is now pending
     * and will be forwarded to this process group when unblocked.
     */
    if (trackForeground) {
        SignalControl::setForegroundProcessGroup(childPid);

        if (!restoreSignalRequests()) {
            SignalControl::clearForegroundProcessGroup();

            (void)kill(-childPid, SIGTERM);

            int ignoredStatus = 0;
            (void)waitForChild(childPid, ignoredStatus);

            return 1;
        }
    }

    // Give a foreground command access to the terminal.
    const pid_t shellProcessGroup = getpgrp();
    bool terminalHandedOver = false;

    if (!inheritProcessGroup &&
        isatty(STDIN_FILENO) &&
        tcsetpgrp(STDIN_FILENO, childPid) == 0) {

        terminalHandedOver = true;
    }

    int status = 0;
    pid_t waitResult;

    // WUNTRACED makes waitpid return when Ctrl+Z stops the command.
    do {
        waitResult = waitpid(
            childPid,
            &status,
            WUNTRACED
        );
    } while (waitResult == -1 && errno == EINTR);

    // Restore the shell's terminal ownership.
    if (terminalHandedOver &&
        tcsetpgrp(STDIN_FILENO, shellProcessGroup) == -1) {

        std::cerr << "terminator: tcsetpgrp: "
                  << std::strerror(errno) << '\n';
    }

    // The foreground command has returned control to the shell.
    if (trackForeground) {
        SignalControl::clearForegroundProcessGroup();
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
