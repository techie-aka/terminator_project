#include "thread/ThreadSession.h"

#include "shell/ShellEngine.h"
#include "shell/ShellIO.h"

#include <cerrno>
#include <cstring>
#include <exception>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <chrono>

namespace {

// Prevent writing to a closed input pipe from terminating the GUI process.
ssize_t writeWithoutSigpipe(
    int fileDescriptor,
    const char* data,
    std::size_t size
) {
    sigset_t signalSet;
    sigset_t originalMask;
    sigset_t pendingBeforeWrite;

    sigemptyset(&signalSet);
    sigaddset(&signalSet, SIGPIPE);

    const int maskError = pthread_sigmask(
        SIG_BLOCK,
        &signalSet,
        &originalMask
    );

    if (maskError != 0) {
        errno = maskError;
        return -1;
    }

    bool sigpipeWasAlreadyPending = false;

    if (sigpending(&pendingBeforeWrite) == 0) {
        sigpipeWasAlreadyPending =
            sigismember(&pendingBeforeWrite, SIGPIPE) == 1;
    }

    const ssize_t result = ::write(
        fileDescriptor,
        data,
        size
    );

    const int writeError = errno;

    if (result == -1 &&
        writeError == EPIPE &&
        !sigpipeWasAlreadyPending) {

        const timespec noWait{0, 0};

        while (sigtimedwait(&signalSet, nullptr, &noWait) == -1 &&
               errno == EINTR) {
        }
    }

    (void)pthread_sigmask(
        SIG_SETMASK,
        &originalMask,
        nullptr
    );

    if (result == -1) {
        errno = writeError;
    }

    return result;
}

bool setNonBlocking(int fileDescriptor) {
    const int flags = fcntl(fileDescriptor, F_GETFL, 0);

    return flags != -1 &&
        fcntl(
            fileDescriptor,
            F_SETFL,
            flags | O_NONBLOCK
        ) != -1;
}

void closeIfOpen(int& fileDescriptor) {
    if (fileDescriptor != -1) {
        close(fileDescriptor);
        fileDescriptor = -1;
    }
}

} // namespace

ThreadSession::ThreadSession()
    : inputWriteFd(-1),
      outputReadFd(-1),
      foregroundProcessGroup(0),
      foregroundHelper(0),
      shellFinished(false),
      started(false) {
}

ThreadSession::~ThreadSession() {
    stop();
}

bool ThreadSession::start() {
    if (started || shellThread.joinable()) {
        return false;
    }

    int inputPipe[2] = {-1, -1};
    int outputPipe[2] = {-1, -1};

    // GUI -> Shell
    if (pipe2(inputPipe, O_CLOEXEC) == -1) {
        return false;
    }

    // Shell -> GUI
    if (pipe2(outputPipe, O_CLOEXEC) == -1) {
        const int savedError = errno;

        close(inputPipe[0]);
        close(inputPipe[1]);

        errno = savedError;
        return false;
    }

    // GUI reads must not block the X11 event loop.
    if (!setNonBlocking(outputPipe[0])) {
        const int savedError = errno;

        close(inputPipe[0]);
        close(inputPipe[1]);
        close(outputPipe[0]);
        close(outputPipe[1]);

        errno = savedError;
        return false;
    }

    shellFinished.store(false, std::memory_order_relaxed);

    try {
        shellThread = std::thread(
            &ThreadSession::runShell,
            this,
            inputPipe[0],
            outputPipe[1]
        );
    } catch (const std::system_error&) {
        close(inputPipe[0]);
        close(inputPipe[1]);
        close(outputPipe[0]);
        close(outputPipe[1]);

        return false;
    }

    // Important: threads share the process file-descriptor table.
    // Do not close the worker-side descriptors here.
    // The shell thread closes them when it exits.
    inputWriteFd = inputPipe[1];
    outputReadFd = outputPipe[0];

    started = true;
    return true;
}

void ThreadSession::runShell(
    int inputReadFd,
    int outputWriteFd
) {
    try {
        // stdout and stderr go to the same GUI output pipe.
        ShellIO io(
            inputReadFd,
            outputWriteFd,
            outputWriteFd,
            &foregroundProcessGroup,
            &foregroundHelper
        );

        ShellEngine shell;
        shell.run(io);

        io.out().flush();
        io.err().flush();

    } catch (const std::exception& exception) {
        const std::string message =
            std::string("terminator: shell thread failed: ") +
            exception.what() + "\n";

        (void)writeWithoutSigpipe(
            outputWriteFd,
            message.data(),
            message.size()
        );

    } catch (...) {
        const std::string message =
            "terminator: shell thread failed with an unknown exception\n";

        (void)writeWithoutSigpipe(
            outputWriteFd,
            message.data(),
            message.size()
        );
    }

    close(inputReadFd);
    close(outputWriteFd);

    shellFinished.store(true, std::memory_order_release);
}

bool ThreadSession::writeInput(const std::string& input) {
    if (inputWriteFd == -1) {
        return false;
    }

    std::size_t written = 0;

    while (written < input.size()) {
        const ssize_t count = writeWithoutSigpipe(
            inputWriteFd,
            input.data() + written,
            input.size() - written
        );

        if (count > 0) {
            written += static_cast<std::size_t>(count);
        } else if (count == -1 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }

    return true;
}

std::string ThreadSession::readOutput() {
    if (outputReadFd == -1) {
        return "";
    }

    std::string output;
    char buffer[4096];

    while (true) {
        const ssize_t count = ::read(
            outputReadFd,
            buffer,
            sizeof(buffer)
        );

        if (count > 0) {
            output.append(
                buffer,
                static_cast<std::size_t>(count)
            );

        } else if (count == 0) {
            closeIfOpen(outputReadFd);
            break;

        } else if (errno == EINTR) {
            continue;

        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;

        } else {
            closeIfOpen(outputReadFd);
            break;
        }
    }

    return output;
}

int ThreadSession::getFileDescriptor() const {
    return outputReadFd;
}

pid_t ThreadSession::getPid() const {
    // Threads do not have individual PIDs.
    // All thread-backed tabs belong to the same GUI process.
    return started ? getpid() : -1;
}

void ThreadSession::stop() {

    // Closing this pipe tells an idle shell that no more
    // commands will arrive.
    closeIfOpen(inputWriteFd);

    if (shellThread.joinable() &&
        shellThread.get_id() != std::this_thread::get_id()) {

        pid_t lastHelperSignalled = 0;
        pid_t lastGroupSignalled = 0;


        // Send a signal to any newly published foreground target.
        //
        // We track the last PID signalled because the shell may
        // publish a new helper while shutdown is taking place.
        const auto sendToNewTargets = [&](int signal) {

            const pid_t helper = foregroundHelper.load(
                std::memory_order_relaxed
            );

            if (helper > 0 &&
                helper != lastHelperSignalled) {

                // Helpers are created as process-group leaders.
                (void)kill(-helper, signal);

                lastHelperSignalled = helper;
            }


            const pid_t group = foregroundProcessGroup.load(
                std::memory_order_relaxed
            );

            if (group > 0 &&
                group != lastGroupSignalled) {

                (void)kill(-group, signal);

                lastGroupSignalled = group;
            }
        };


        // First request graceful termination.
        //
        // A helper forwards this signal to its own foreground
        // command. MultiWatch handles it by terminating workers.
        sendToNewTargets(SIGTERM);


        const auto graceDeadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(500);


        while (!shellFinished.load(std::memory_order_acquire)) {

            // A command/helper might have been published after
            // shutdown started. Signal it as soon as it appears.
            sendToNewTargets(
                std::chrono::steady_clock::now() < graceDeadline
                    ? SIGTERM
                    : SIGKILL
            );


            // Always drain output while waiting.
            //
            // Otherwise the shell thread might block writing to
            // a full output pipe, making join() wait forever.
            (void)readOutput();


            // Escalate if graceful termination did not work.
            if (std::chrono::steady_clock::now() >= graceDeadline) {

                if (lastHelperSignalled > 0) {
                    (void)kill(-lastHelperSignalled, SIGKILL);
                }

                if (lastGroupSignalled > 0) {
                    (void)kill(-lastGroupSignalled, SIGKILL);
                }
            }


            std::this_thread::sleep_for(
                std::chrono::milliseconds(5)
            );
        }


        // The shell thread has completed.
        shellThread.join();
    }


    // Collect any remaining output.
    (void)readOutput();

    closeIfOpen(outputReadFd);


    // Reset all session state.
    foregroundProcessGroup.store(
        0,
        std::memory_order_relaxed
    );

    foregroundHelper.store(
        0,
        std::memory_order_relaxed
    );

    started = false;
}

void ThreadSession::sendInterrupt() {

    // If a helper shell is active, send a notification to
    // its process group. The helper will forward SIGINT.
    const pid_t helper = foregroundHelper.load();

    if (helper > 0) {
        (void)kill(-helper, SIGUSR1);
        return;
    }

    // Otherwise interrupt the active foreground command.
    const pid_t group = foregroundProcessGroup.load();

    if (group > 0) {
        (void)kill(-group, SIGINT);
        return;
    }

    // At the interactive prompt there is no foreground job.
    // Send Ctrl+C directly to the shell's input pipe.
    (void)writeInput(std::string(1, '\x03'));
}

void ThreadSession::sendSuspend() {

    // Forward Ctrl+Z through an active helper shell.
    const pid_t helper = foregroundHelper.load();

    if (helper > 0) {
        (void)kill(-helper, SIGUSR2);
        return;
    }


    // Otherwise suspend the foreground command group.
    const pid_t group = foregroundProcessGroup.load();

    if (group > 0) {
        (void)kill(-group, SIGTSTP);
    }
}