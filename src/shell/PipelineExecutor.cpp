#include "shell/PipelineExecutor.h"
#include "shell/Redirection.h"

#include "shell/ShellIO.h"

#include <spawn.h>
#include <fcntl.h>
#include <cstring>
#include <iostream>
#include <signal.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <string>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

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

int executeThreadPipeline(
    const std::vector<ParsedCommand>& commands,
    ShellIO& io
) {
    const std::size_t commandCount = commands.size();

    std::vector<std::array<int, 2>> pipes;
    pipes.reserve(commandCount - 1);

    auto closeAllPipes = [&]() {
        for (const auto& pipeFd : pipes) {
            close(pipeFd[0]);
            close(pipeFd[1]);
        }
    };

    // Create N - 1 pipes for N commands.
    for (std::size_t i = 0; i + 1 < commandCount; ++i) {
        int pipeFd[2];

        // CLOEXEC is applied atomically. Setting it later with fcntl()
        // leaves a window where another tab's concurrently spawned child
        // could inherit these descriptors.
        if (pipe2(pipeFd, O_CLOEXEC) == -1) {
            io.err() << "terminator: pipeline pipe: "
                     << std::strerror(errno) << '\n';
            closeAllPipes();
            return 1;
        }

        pipes.push_back({pipeFd[0], pipeFd[1]});
    }

    std::vector<pid_t> childPids;
    childPids.reserve(commandCount);

    pid_t processGroupId = -1;
    bool foregroundGroupPublished = false;

    auto terminateChildren = [&]() {
        closeAllPipes();

        if (processGroupId > 0) {
            (void)kill(-processGroupId, SIGTERM);
            (void)kill(-processGroupId, SIGCONT);
        }

        for (pid_t pid : childPids) {
            int status = 0;

            while (waitpid(pid, &status, 0) == -1 &&
                   errno == EINTR) {
            }
        }

        if (foregroundGroupPublished) {
            io.clearForegroundProcessGroup();
            foregroundGroupPublished = false;
        }
    };

    // Start every pipeline stage before waiting.
    for (std::size_t i = 0; i < commandCount; ++i) {
        posix_spawn_file_actions_t actions;

        int result = posix_spawn_file_actions_init(&actions);

        if (result != 0) {
            io.err() << "terminator: pipeline setup: "
                     << std::strerror(result) << '\n';

            terminateChildren();
            return 1;
        }

        int setupError = 0;

        auto addDup = [&](int source, int destination) {
            if (setupError == 0 && source != destination) {
                setupError = posix_spawn_file_actions_adddup2(
                    &actions,
                    source,
                    destination
                );
            }
        };

        // Connect this stage to the appropriate input.
        const int inputFd = i == 0
            ? io.getInputFd()
            : pipes[i - 1][0];

        // Connect this stage to the appropriate output.
        const int outputFd = i + 1 == commandCount
            ? io.getOutputFd()
            : pipes[i][1];

        addDup(inputFd, STDIN_FILENO);
        addDup(outputFd, STDOUT_FILENO);
        addDup(io.getErrorFd(), STDERR_FILENO);

        // Explicit redirection overrides the pipeline connection.
        const RedirectionSpec& redirection =
            commands[i].redirection;

        // Open redirections in this shell thread so failures can be
        // attributed to the correct filename instead of the command.
        ScopedFd inputRedirect;
        ScopedFd outputRedirect;

        if (redirection.hasInput) {
            const int fd = open(
                redirection.inputFile.c_str(),
                O_RDONLY | O_CLOEXEC
            );
            if (fd == -1) {
                io.err() << "terminator: input redirection: "
                         << redirection.inputFile << ": "
                         << std::strerror(errno) << '\n';
                posix_spawn_file_actions_destroy(&actions);
                terminateChildren();
                return 1;
            }
            inputRedirect.reset(fd);
        }

        if (redirection.hasOutput) {
            const int fd = open(
                redirection.outputFile.c_str(),
                O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                0644
            );
            if (fd == -1) {
                io.err() << "terminator: output redirection: "
                         << redirection.outputFile << ": "
                         << std::strerror(errno) << '\n';
                posix_spawn_file_actions_destroy(&actions);
                terminateChildren();
                return 1;
            }
            outputRedirect.reset(fd);
        }

        if (setupError == 0 && inputRedirect.get() >= 0) {
            setupError = posix_spawn_file_actions_adddup2(
                &actions, inputRedirect.get(), STDIN_FILENO
            );
        }
        if (setupError == 0 && outputRedirect.get() >= 0) {
            setupError = posix_spawn_file_actions_adddup2(
                &actions, outputRedirect.get(), STDOUT_FILENO
            );
        }

        // Close all original pipe descriptors in the child.
        for (const auto& pipeFd : pipes) {
            if (setupError != 0) {
                break;
            }

            setupError = posix_spawn_file_actions_addclose(
                &actions, pipeFd[0]
            );

            if (setupError == 0) {
                setupError = posix_spawn_file_actions_addclose(
                    &actions, pipeFd[1]
                );
            }
        }

        // Close the original ShellIO descriptors after duplication.
        const int ioDescriptors[] = {
            io.getInputFd(),
            io.getOutputFd(),
            io.getErrorFd()
        };

        for (std::size_t d = 0;
             d < 3 && setupError == 0;
             ++d) {

            const int fd = ioDescriptors[d];

            if (fd <= STDERR_FILENO) {
                continue;
            }

            bool duplicate = false;
            for (std::size_t previous = 0; previous < d; ++previous) {
                if (ioDescriptors[previous] == fd) {
                    duplicate = true;
                    break;
                }
            }

            if (!duplicate) {
                setupError = posix_spawn_file_actions_addclose(
                    &actions, fd
                );
            }
        }

        if (setupError == 0 && inputRedirect.get() > STDERR_FILENO) {
            setupError = posix_spawn_file_actions_addclose(
                &actions, inputRedirect.get()
            );
        }
        if (setupError == 0 && outputRedirect.get() > STDERR_FILENO) {
            setupError = posix_spawn_file_actions_addclose(
                &actions, outputRedirect.get()
            );
        }

        if (setupError != 0) {
            posix_spawn_file_actions_destroy(&actions);

            io.err() << "terminator: pipeline file actions: "
                     << std::strerror(setupError) << '\n';

            terminateChildren();
            return 1;
        }

        posix_spawnattr_t attributes;

        result = posix_spawnattr_init(&attributes);

        if (result != 0) {
            posix_spawn_file_actions_destroy(&actions);

            io.err() << "terminator: pipeline attributes: "
                     << std::strerror(result) << '\n';

            terminateChildren();
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

        result = posix_spawnattr_setsigdefault(
            &attributes,
            &defaultSignals
        );

        if (result == 0) {
            result = posix_spawnattr_setsigmask(
                &attributes,
                &emptyMask
            );
        }

        const pid_t targetGroup =
            processGroupId == -1 ? 0 : processGroupId;

        if (result == 0) {
            result = posix_spawnattr_setpgroup(
                &attributes,
                targetGroup
            );
        }

        if (result == 0) {
            result = posix_spawnattr_setflags(
                &attributes,
                POSIX_SPAWN_SETPGROUP |
                POSIX_SPAWN_SETSIGDEF |
                POSIX_SPAWN_SETSIGMASK
            );
        }

        if (result != 0) {
            posix_spawnattr_destroy(&attributes);
            posix_spawn_file_actions_destroy(&actions);

            io.err() << "terminator: pipeline attributes: "
                     << std::strerror(result) << '\n';

            terminateChildren();
            return 1;
        }

        const auto& arguments = commands[i].arguments;

        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);

        for (const std::string& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }

        argv.push_back(nullptr);

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
            io.err() << "terminator: "
                     << arguments[0] << ": ";

            if (result == ENOENT || result == ENOTDIR) {
                io.err() << "command not found\n";
            } else {
                io.err() << std::strerror(result) << '\n';
            }

            terminateChildren();

            return result == ENOENT || result == ENOTDIR
                ? 127
                : 126;
        }

        if (processGroupId == -1) {
            processGroupId = childPid;
            // Publish immediately, reducing the window in which a Ctrl+C
            // or Ctrl+Z typed during pipeline startup has no target.
            io.setForegroundProcessGroup(processGroupId);
            foregroundGroupPublished = true;
        }

        childPids.push_back(childPid);
    }

    // The parent must close its copies before waiting.
    closeAllPipes();

    std::vector<int> statuses(commandCount, 0);
    bool waitFailed = false;

    for (std::size_t i = 0; i < commandCount; ++i) {
        while (true) {
            const pid_t waited = waitpid(
                childPids[i],
                &statuses[i],
                0
            );

            if (waited == childPids[i]) {
                break;
            }

            if (waited == -1 && errno == EINTR) {
                continue;
            }

            io.err() << "terminator: waitpid: "
                     << std::strerror(errno) << '\n';

            waitFailed = true;
            break;
        }
    }

    if (foregroundGroupPublished) {
        io.clearForegroundProcessGroup();
        foregroundGroupPublished = false;
    }

    if (waitFailed) {
        return 1;
    }

    // The pipeline exit status comes from the final stage.
    const int lastStatus = statuses.back();

    if (WIFEXITED(lastStatus)) {
        return WEXITSTATUS(lastStatus);
    }

    if (WIFSIGNALED(lastStatus)) {
        return 128 + WTERMSIG(lastStatus);
    }

    return 1;
}

} // namespace


int PipelineExecutor::executePipeline(
    const std::vector<ParsedCommand>& commands,
    ShellIO* io
) const {
    std::ostream& error = io != nullptr ? io->err() : std::cerr;
    const std::size_t commandCount = commands.size();

    if (commandCount < 2) {
        error << "terminator: a pipeline requires at least two commands\n";
        return 2;
    }

    // Reject empty commands before acquiring any file descriptors.
    for (const auto& command : commands) {
        if (command.arguments.empty() ||
            command.arguments[0].empty()) {
            error << "terminator: invalid empty pipeline stage\n";
            return 2;
        }
    }

    if (io != nullptr) {
        return executeThreadPipeline(commands, *io);
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