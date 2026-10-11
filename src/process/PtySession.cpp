
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

bool PtySession::createShellConfig() {
    char path[] = "/tmp/terminator-bashrc-XXXXXX";

    const int fd = mkstemp(path);

    if (fd == -1) {
        return false;
    }

    rcFilePath = path;

    // Preserve the user's Bash configuration and define our newtab command.
    const std::string config =
        "if [ -f \"$HOME/.bashrc\" ]; then source \"$HOME/.bashrc\"; fi\n"
        "function newtab() { printf '\\033]777;newtab\\007'; }\n"
        "export TERM=xterm-256color\n";

    std::size_t written = 0;

    while (written < config.size()) {
        const ssize_t count = write(
            fd, config.data() + written, config.size() - written
        );

        if (count > 0) {
            written += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            close(fd);
            unlink(rcFilePath.c_str());
            rcFilePath.clear();
            return false;
        }
    }

    close(fd);
    return true;
}

bool PtySession::start() {
    if (processId > 0 || masterFd != -1) {
        return false;
    }

    if (!createShellConfig()) {
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

        unlink(rcFilePath.c_str());
        rcFilePath.clear();
        return false;
    }

    if (processId == 0) {
        // Child process: replace the child with interactive Bash.
        setenv("TERM", "xterm-256color", 1);

        execl(
            "/bin/bash", "bash",
            "--noprofile", "--rcfile", rcFilePath.c_str(), "-i",
            static_cast<char*>(nullptr)
        );

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
        unlink(rcFilePath.c_str());
        rcFilePath.clear();
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
            masterFd, input.data() + written, input.size() - written
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
    if (processId > 0) {
        // Ask the shell and its process group to terminate first.
        kill(-processId, SIGHUP);

        int status = 0;
        bool exited = false;

        // Give the shell a short opportunity to exit cleanly.
        for (int attempt = 0; attempt < 20; ++attempt) {
            const pid_t result = waitpid(processId, &status, WNOHANG);

            if (result == processId || (result == -1 && errno == ECHILD)) {
                exited = true;
                break;
            }

            usleep(10000);
        }

        if (!exited) {
            kill(-processId, SIGTERM);
            usleep(50000);

            const pid_t result = waitpid(processId, &status, WNOHANG);

            if (result == 0) {
                kill(-processId, SIGKILL);
            }

            while (waitpid(processId, &status, 0) == -1 && errno == EINTR) {
            }
        }

        processId = -1;
    }

    if (masterFd != -1) {
        close(masterFd);
        masterFd = -1;
    }

    if (!rcFilePath.empty()) {
        unlink(rcFilePath.c_str());
    }
}
