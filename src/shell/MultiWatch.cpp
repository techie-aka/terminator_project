#include "shell/MultiWatch.h"

#include "shell/CommandParser.h"
#include "shell/CommandExecutor.h"
#include "shell/PipelineExecutor.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <utility>

#include <fcntl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {

constexpr std::size_t MAX_FRAME_DATA = 4096;
constexpr int RESTART_DELAY_SECONDS = 2;
constexpr int TERMINATION_GRACE_MS = 300;
constexpr const char* SEPARATOR =
    "----------------------------------------------------";

enum class FrameType : std::uint8_t {
    RunStart = 1,
    Output = 2,
    RunEnd = 3
};

#pragma pack(push, 1)
struct FrameHeader {
    std::uint8_t type;
    std::int64_t timestamp;
    std::uint32_t length;
};
#pragma pack(pop)

struct MonitorState {
    std::string command;
    pid_t pid = -1;
    pid_t processGroup = -1;
    int readFd = -1;
    bool active = false;
    bool inRun = false;
    std::int64_t timestamp = 0;
    std::vector<char> pending;
    std::string output;
};

volatile sig_atomic_t interruptRequested = 0;

extern "C" void handleMultiWatchInterrupt(int) {
    interruptRequested = 1;
}

bool writeAll(int fd, const void* data, std::size_t length) {
    const char* bytes = static_cast<const char*>(data);
    std::size_t written = 0;

    while (written < length) {
        const ssize_t result =
            ::write(fd, bytes + written, length - written);

        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }

        if (result == -1 && errno == EINTR) {
            continue;
        }

        return false;
    }

    return true;
}

bool writeFrame(
    int fd,
    FrameType type,
    std::int64_t timestamp,
    const char* data = nullptr,
    std::size_t length = 0
) {
    FrameHeader header{};
    header.type = static_cast<std::uint8_t>(type);
    header.timestamp = timestamp;
    header.length = static_cast<std::uint32_t>(length);

    return writeAll(fd, &header, sizeof(header)) &&
           (length == 0 || writeAll(fd, data, length));
}

void setDefaultCommandSignals() {
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGQUIT, SIG_DFL);
    std::signal(SIGTSTP, SIG_DFL);
    std::signal(SIGTTIN, SIG_DFL);
    std::signal(SIGTTOU, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
}

int runOneCommand(
    const std::string& commandLine,
    int lastExitStatus
) {
    CommandParser parser;
    const std::vector<std::string> stages =
        parser.splitPipeline(commandLine);

    std::vector<ParsedCommand> commands;

    for (const std::string& stage : stages) {
        ParsedCommand parsed;
        std::string error;

        if (!parser.parseCommand(
                stage,
                lastExitStatus,
                parsed,
                error)) {
            std::cerr << "multiWatch: " << error << '\n';
            return 2;
        }

        if (parsed.arguments.empty()) {
            std::cerr << "multiWatch: empty command\n";
            return 2;
        }

        const std::string& name = parsed.arguments.front();

        if (name == "cd" ||
            name == "exit" ||
            name == "history" ||
            name == "source" ||
            name == "jobs" ||
            name == "fg" ||
            name == "bg" ||
            name == "newtab" ||
            name == "multiWatch") {
            std::cerr << "multiWatch: unsupported shell built-in: "
                      << name << '\n';
            return 2;
        }

        commands.push_back(std::move(parsed));
    }

    if (commands.size() == 1) {
        CommandExecutor executor;

        return executor.execute(
            commands[0].arguments,
            false,
            nullptr,
            nullptr,
            nullptr,
            &commands[0].redirection,
            true
        );
    }

    PipelineExecutor pipelineExecutor;
    return pipelineExecutor.executePipeline(commands);
}

bool validateMonitoredCommand(
    const std::string& commandLine,
    int lastExitStatus,
    std::string& errorMessage
) {
    CommandParser parser;

    const std::vector<std::string> stages =
        parser.splitPipeline(commandLine);

    for (const std::string& stage : stages) {
        ParsedCommand parsed;

        if (!parser.parseCommand(
                stage,
                lastExitStatus,
                parsed,
                errorMessage)) {
            return false;
        }

        if (parsed.arguments.empty()) {
            errorMessage = "empty command in pipeline";
            return false;
        }

        const std::string& name = parsed.arguments.front();

        if (name == "cd" ||
            name == "exit" ||
            name == "history" ||
            name == "source" ||
            name == "jobs" ||
            name == "fg" ||
            name == "bg" ||
            name == "newtab" ||
            name == "multiWatch") {
            errorMessage =
                "shell built-ins cannot be monitored: " + name;
            return false;
        }
    }

    return true;
}

void workerLoop(
    const std::string& command,
    int outputFd,
    int lastExitStatus
) {
    // This worker is the process-group leader for its entire command tree.
    (void)setpgid(0, 0);

    setDefaultCommandSignals();
    std::signal(SIGPIPE, SIG_DFL);

    while (true) {
        const std::int64_t timestamp =
            static_cast<std::int64_t>(time(nullptr));

        if (!writeFrame(
                outputFd,
                FrameType::RunStart,
                timestamp)) {
            _exit(0);
        }

        int capturePipe[2];

        if (pipe(capturePipe) == -1) {
            const char* message =
                "multiWatch: unable to create output pipe\n";

            (void)writeFrame(
                outputFd,
                FrameType::Output,
                timestamp,
                message,
                std::strlen(message)
            );

            (void)writeFrame(
                outputFd,
                FrameType::RunEnd,
                timestamp
            );

            _exit(1);
        }

        const pid_t commandPid = fork();

        if (commandPid == -1) {
            const char* message =
                "multiWatch: unable to fork command\n";

            close(capturePipe[0]);
            close(capturePipe[1]);

            (void)writeFrame(
                outputFd,
                FrameType::Output,
                timestamp,
                message,
                std::strlen(message)
            );

            (void)writeFrame(
                outputFd,
                FrameType::RunEnd,
                timestamp
            );

            _exit(1);
        }

        if (commandPid == 0) {
            close(capturePipe[0]);

            // The command tree must not keep the frame pipe open.
            close(outputFd);

            const int nullFd = open("/dev/null", O_RDONLY);

            if (nullFd >= 0) {
                (void)dup2(nullFd, STDIN_FILENO);

                if (nullFd != STDIN_FILENO) {
                    close(nullFd);
                }
            }

            if (dup2(capturePipe[1], STDOUT_FILENO) == -1 ||
                dup2(capturePipe[1], STDERR_FILENO) == -1) {
                _exit(126);
            }

            close(capturePipe[1]);

            setDefaultCommandSignals();

            lastExitStatus = runOneCommand(command, lastExitStatus);
            _exit(0);
        }

        close(capturePipe[1]);

        char buffer[MAX_FRAME_DATA];

        while (true) {
            const ssize_t bytesRead =
                read(capturePipe[0], buffer, sizeof(buffer));

            if (bytesRead > 0) {
                if (!writeFrame(
                        outputFd,
                        FrameType::Output,
                        timestamp,
                        buffer,
                        static_cast<std::size_t>(bytesRead))) {
                    close(capturePipe[0]);
                    _exit(0);
                }

                continue;
            }

            if (bytesRead == -1 && errno == EINTR) {
                continue;
            }

            break;
        }

        close(capturePipe[0]);

        int commandStatus = 0;

        while (waitpid(commandPid, &commandStatus, 0) == -1 &&
               errno == EINTR) {}

        if (!writeFrame(
                outputFd,
                FrameType::RunEnd,
                timestamp)) {
            _exit(0);
        }

        // Start the next invocation two seconds after completion.
        timespec delay{RESTART_DELAY_SECONDS, 0};

        while (nanosleep(&delay, &delay) == -1 &&
               errno == EINTR) {}
    }
}

void printCompletedRun(const MonitorState& monitor) {
    std::cout << '"' << monitor.command << "\", "
              << monitor.timestamp << ": "
              << SEPARATOR << '\n';

    std::cout.write(
        monitor.output.data(),
        static_cast<std::streamsize>(monitor.output.size())
    );

    if (monitor.output.empty() ||
        monitor.output.back() != '\n') {
        std::cout << '\n';
    }

    std::cout << SEPARATOR << '\n' << std::flush;
}

void consumeFrames(MonitorState& monitor) {
    std::size_t consumed = 0;

    while (monitor.pending.size() - consumed >=
           sizeof(FrameHeader)) {
        FrameHeader header{};

        std::memcpy(
            &header,
            monitor.pending.data() + consumed,
            sizeof(FrameHeader)
        );

        if (header.length > MAX_FRAME_DATA) {
            // A corrupt frame should not leave this monitor stuck.
            monitor.pending.clear();
            monitor.inRun = false;
            monitor.output.clear();
            return;
        }

        const std::size_t frameSize =
            sizeof(FrameHeader) + header.length;

        if (monitor.pending.size() - consumed < frameSize) {
            break;
        }

        const char* payload =
            monitor.pending.data() + consumed + sizeof(FrameHeader);

        const FrameType type =
            static_cast<FrameType>(header.type);

        if (type == FrameType::RunStart) {
            monitor.inRun = true;
            monitor.timestamp = header.timestamp;
            monitor.output.clear();
        } else if (type == FrameType::Output &&
                   monitor.inRun) {
            monitor.output.append(payload, header.length);
        } else if (type == FrameType::RunEnd &&
                   monitor.inRun) {
            printCompletedRun(monitor);
            monitor.inRun = false;
            monitor.output.clear();
        }

        consumed += frameSize;
    }

    if (consumed > 0) {
        monitor.pending.erase(
            monitor.pending.begin(),
            monitor.pending.begin() +
                static_cast<std::ptrdiff_t>(consumed)
        );
    }
}

void requestWorkerTermination(
    const std::vector<MonitorState>& monitors
) {
    for (const MonitorState& monitor : monitors) {
        if (monitor.processGroup > 0) {
            (void)kill(-monitor.processGroup, SIGTERM);
        }
    }

    const int attempts = TERMINATION_GRACE_MS / 10;

    for (int i = 0; i < attempts; ++i) {
        bool anyGroupAlive = false;

        for (const MonitorState& monitor : monitors) {
            if (monitor.processGroup <= 0) {
                continue;
            }

            if (kill(-monitor.processGroup, 0) == 0 ||
                errno != ESRCH) {
                anyGroupAlive = true;
            }
        }

        if (!anyGroupAlive) {
            break;
        }

        timespec pause{0, 10 * 1000 * 1000};
        (void)nanosleep(&pause, nullptr);
    }

    // Kill descendants that ignored SIGTERM, even if the worker leader
    // has already exited and been reaped.
    for (const MonitorState& monitor : monitors) {
        if (monitor.processGroup > 0) {
            (void)kill(-monitor.processGroup, SIGKILL);
        }
    }

    for (const MonitorState& monitor : monitors) {
        if (monitor.pid <= 0) {
            continue;
        }

        int status = 0;

        while (waitpid(monitor.pid, &status, 0) == -1 &&
               errno == EINTR) {}
    }
}

} // namespace

bool MultiWatch::isInvocation(const std::string& line) {
    std::size_t position =
        line.find_first_not_of(" \t\r\n");

    if (position == std::string::npos) {
        return false;
    }

    const std::size_t start = position;

    while (position < line.size() &&
           line[position] != ' ' &&
           line[position] != '\t' &&
           line[position] != '\r' &&
           line[position] != '\n') {
        ++position;
    }

    return line.substr(start, position - start) == "multiWatch";
}

bool MultiWatch::parseInvocation(
    const std::string& line,
    std::vector<std::string>& commands,
    std::string& errorMessage
) {
    commands.clear();
    errorMessage.clear();

    std::size_t position =
        line.find_first_not_of(" \t\r\n");

    if (position == std::string::npos ||
        line.compare(position, 10, "multiWatch") != 0) {
        errorMessage =
            "expected multiWatch [\"command 1\", \"command 2\"]";
        return false;
    }

    position += 10;

    auto skipSpaces = [&]() {
        while (position < line.size() &&
               (line[position] == ' ' ||
                line[position] == '\t' ||
                line[position] == '\r' ||
                line[position] == '\n')) {
            ++position;
        }
    };

    skipSpaces();

    if (position >= line.size() ||
        line[position] != '[') {
        errorMessage = "expected '[' after multiWatch";
        return false;
    }

    ++position;
    skipSpaces();

    if (position < line.size() &&
        line[position] == ']') {
        errorMessage = "provide at least one command";
        return false;
    }

    while (position < line.size()) {
        skipSpaces();

        if (position >= line.size() ||
            (line[position] != '\'' &&
             line[position] != '"')) {
            errorMessage =
                "each monitored command must be quoted";
            return false;
        }

        const char quote = line[position++];
        std::string command;
        bool closed = false;

        while (position < line.size()) {
            const char c = line[position++];

            if (c == quote) {
                closed = true;
                break;
            }

            if (c == '\\' && position < line.size()) {
                const char next = line[position];

                if (next == quote || next == '\\') {
                    command += next;
                    ++position;
                    continue;
                }
            }

            command += c;
        }

        if (!closed) {
            errorMessage = "unterminated quoted command";
            return false;
        }

        if (command.find_first_not_of(" \t\r\n") ==
            std::string::npos) {
            errorMessage =
                "monitored commands cannot be empty";
            return false;
        }

        commands.push_back(std::move(command));

        skipSpaces();

        if (position < line.size() &&
            line[position] == ',') {
            ++position;
            skipSpaces();
            continue;
        }

        if (position < line.size() &&
            line[position] == ']') {
            ++position;
            skipSpaces();

            if (position != line.size()) {
                errorMessage =
                    "unexpected text after the command list";
                return false;
            }

            return true;
        }

        errorMessage =
            "expected ',' or ']' after a monitored command";
        return false;
    }

    errorMessage = "missing closing ']'";
    return false;
}

int MultiWatch::execute(
    const std::vector<std::string>& commands,
    int lastExitStatus
) const {
    if (commands.empty()) {
        std::cerr
            << "multiWatch: provide at least one command\n";
        return 2;
    }

    for (const std::string& command : commands) {
        std::string validationError;

        if (!validateMonitoredCommand(
                command,
                lastExitStatus,
                validationError)) {
            std::cerr << "multiWatch: "
                      << validationError << '\n';
            return 2;
        }
    }

    struct sigaction action{};
    struct sigaction previousAction{};

    action.sa_handler = handleMultiWatchInterrupt;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0; // Let select() return EINTR on Ctrl+C.

    interruptRequested = 0;

    if (sigaction(
            SIGINT,
            &action,
            &previousAction) == -1) {
        std::perror("multiWatch sigaction");
        return 1;
    }

    std::vector<MonitorState> monitors;
    monitors.reserve(commands.size());

    bool launchFailed = false;

    for (const std::string& command : commands) {
        int outputPipe[2];

        if (pipe(outputPipe) == -1) {
            std::perror("multiWatch pipe");
            launchFailed = true;
            break;
        }

        if (outputPipe[0] >= FD_SETSIZE) {
            std::cerr
                << "multiWatch: too many monitored commands "
                << "for select()\n";

            close(outputPipe[0]);
            close(outputPipe[1]);

            launchFailed = true;
            break;
        }

        const pid_t pid = fork();

        if (pid == -1) {
            std::perror("multiWatch fork");

            close(outputPipe[0]);
            close(outputPipe[1]);

            launchFailed = true;
            break;
        }

        if (pid == 0) {
            close(outputPipe[0]);
            (void)setpgid(0, 0);

            workerLoop(
                command,
                outputPipe[1],
                lastExitStatus
            );

            _exit(0);
        }

        close(outputPipe[1]);
        (void)setpgid(pid, pid);

        MonitorState monitor;
        monitor.command = command;
        monitor.pid = pid;
        monitor.processGroup = pid;
        monitor.readFd = outputPipe[0];
        monitor.active = true;

        monitors.push_back(std::move(monitor));
    }

    if (launchFailed || monitors.empty()) {
        requestWorkerTermination(monitors);

        for (MonitorState& monitor : monitors) {
            if (monitor.readFd >= 0) {
                close(monitor.readFd);
            }
        }

        (void)sigaction(
            SIGINT,
            &previousAction,
            nullptr
        );

        return 1;
    }

    bool loopFailed = false;

    while (!interruptRequested) {
        fd_set readSet;
        FD_ZERO(&readSet);

        int maxFd = -1;

        for (const MonitorState& monitor : monitors) {
            if (monitor.active && monitor.readFd >= 0) {
                FD_SET(monitor.readFd, &readSet);
                maxFd = std::max(maxFd, monitor.readFd);
            }
        }

        if (maxFd == -1) {
            break;
        }

        const int ready = select(
            maxFd + 1,
            &readSet,
            nullptr,
            nullptr,
            nullptr
        );

        if (ready == -1) {
            if (errno == EINTR) {
                continue;
            }

            std::perror("multiWatch select");
            loopFailed = true;
            break;
        }

        for (MonitorState& monitor : monitors) {
            if (!monitor.active ||
                !FD_ISSET(monitor.readFd, &readSet)) {
                continue;
            }

            char buffer[8192];

            const ssize_t bytesRead =
                read(monitor.readFd, buffer, sizeof(buffer));

            if (bytesRead > 0) {
                monitor.pending.insert(
                    monitor.pending.end(),
                    buffer,
                    buffer + bytesRead
                );

                consumeFrames(monitor);
            } else if (bytesRead == 0) {
                monitor.active = false;

                close(monitor.readFd);
                monitor.readFd = -1;

                if (monitor.inRun) {
                    printCompletedRun(monitor);
                    monitor.inRun = false;
                }

                int status = 0;

                while (waitpid(
                           monitor.pid,
                           &status,
                           0) == -1 &&
                       errno == EINTR) {}

                monitor.pid = -1;
                monitor.processGroup = -1;
            } else if (errno != EINTR &&
                       errno != EAGAIN) {
                monitor.active = false;

                close(monitor.readFd);
                monitor.readFd = -1;
            }
        }
    }

    // Preserve output collected before Ctrl+C, then stop every worker
    // process group, including its command and pipeline children.
    for (const MonitorState& monitor : monitors) {
        if (monitor.inRun) {
            printCompletedRun(monitor);
        }
    }

    requestWorkerTermination(monitors);

    for (MonitorState& monitor : monitors) {
        if (monitor.readFd >= 0) {
            close(monitor.readFd);
        }
    }

    const bool wasInterrupted =
        interruptRequested != 0;

    (void)sigaction(
        SIGINT,
        &previousAction,
        nullptr
    );

    interruptRequested = 0;

    return wasInterrupted ? 130 : (loopFailed ? 1 : 0);
}