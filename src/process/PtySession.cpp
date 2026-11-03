#include "process/PtySession.h"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

PtySession::PtySession()
    : masterFd(-1), processId(-1) {
}

bool PtySession::start() {
    if (processId > 0 || masterFd != -1) {
        return false;
    }

    struct winsize terminalSize {};
    terminalSize.ws_row = 24;
    terminalSize.ws_col = 80;

    processId = forkpty(&masterFd, nullptr, nullptr, &terminalSize);

    if (processId == -1) {
        processId = -1;

        if (masterFd != -1) {
            close(masterFd);
            masterFd = -1;
        }

        return false;
    }

    if (processId == 0) {
        // Child process: launch our custom shell instead of Bash.
        setenv("TERM", "xterm-256color", 1);

        execl(
            "./build/terminator-shell",
            "terminator-shell",
            static_cast<char*>(nullptr)
        );

        // Reached only if exec fails.
        _exit(127);
    }

    // Parent process: non-blocking reads keep the X11 loop responsive.
    const int flags = fcntl(masterFd, F_GETFL, 0);

    if (flags == -1 || fcntl(masterFd, F_SETFL, flags | O_NONBLOCK) == -1) {
        kill(-processId, SIGHUP);
        waitpid(processId, nullptr, 0);
        close(masterFd);
        masterFd = -1;
        processId = -1;
        return false;
    }

    return true;
}

bool PtySession::writeInput(const std::string& input) {
    if (masterFd == -1) {
        return false;
    }

    std::size_t written = 0;

    while (written < input.size()) {
        const ssize_t count = write(
            masterFd,
            input.data() + written,
            input.size() - written
        );

        if (count > 0) {
            written += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            // A non-blocking PTY may temporarily be unable to accept input.
            return false;
        }
    }

    return true;
}

std::string PtySession::readOutput() {
    if (masterFd == -1) {
        return "";
    }

    std::string output;
    char buffer[4096];

    while (true) {
        const ssize_t count = read(masterFd, buffer, sizeof(buffer));

        if (count > 0) {
            output.append(buffer, static_cast<std::size_t>(count));
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count == 0 || (count < 0 && errno == EIO)) {
            // Linux PTYs commonly report EIO after the slave side closes.
            close(masterFd);
            masterFd = -1;
            break;
        } else {
            // EAGAIN/EWOULDBLOCK means there is no more output right now.
            break;
        }
    }

    return output;
}

int PtySession::getFileDescriptor() const {
    return masterFd;
}

pid_t PtySession::getPid() const {
    return processId;
}

PtySession::~PtySession() {
    stop();
}

void PtySession::stop() {

    if (processId > 0) {

        // First ask the shell to shut down gracefully.
        //
        // SignalControl forwards SIGHUP to its active foreground job.
        // The shell then exits and JobManager cleans up background jobs.
        (void)kill(-processId, SIGHUP);


        int status = 0;
        bool exited = false;


        // Give the shell a short amount of time to finish.
        const auto waitBriefly = [&](int attempts) {

            for (int attempt = 0; attempt < attempts; ++attempt) {

                const pid_t result = waitpid(
                    processId,
                    &status,
                    WNOHANG
                );

                if (result == processId ||
                    (result == -1 && errno == ECHILD)) {

                    return true;
                }

                if (result == -1 && errno != EINTR) {
                    return true;
                }

                usleep(10000);
            }

            return false;
        };


        // First wait for graceful shutdown.
        exited = waitBriefly(100);


        // If necessary, escalate to SIGTERM.
        if (!exited) {
            (void)kill(-processId, SIGTERM);

            exited = waitBriefly(30);
        }


        // Last resort: terminate the shell process group.
        if (!exited) {
            (void)kill(-processId, SIGKILL);
        }


        // Reap the shell process.
        while (waitpid(processId, &status, 0) == -1 &&
               errno == EINTR) {
        }

        processId = -1;
    }


    // Close the PTY master descriptor.
    if (masterFd != -1) {
        close(masterFd);
        masterFd = -1;
    }
}

void PtySession::sendInterrupt() {
    if (processId > 0) {
        (void)kill(processId, SIGUSR1);
    }
}

void PtySession::sendSuspend() {
    if (processId > 0) {
        (void)kill(processId, SIGUSR2);
    }
}