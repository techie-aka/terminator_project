#include "shell/Builtins.h"
#include "shell/CommandExecutor.h"
#include "shell/CommandParser.h"
#include "shell/History.h"

#include <csignal>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <algorithm>
#include <termios.h>
#include <unistd.h>

bool readCommandLine(
    const History& history,
    std::string& line
) {
    termios originalSettings {};

    if (tcgetattr(STDIN_FILENO, &originalSettings) == -1) {
        return false;
    }

    termios settings = originalSettings;
    settings.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    settings.c_cc[VMIN] = 1;
    settings.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &settings) == -1) {
        return false;
    }

    std::vector<std::string> entries = history.getEntries();
    std::size_t historyPosition = entries.size();
    std::string draft;
    line.clear();

    const std::string prompt = "user@terminator> ";

    auto redrawLine = [&]() {
        std::cout << '\r' << "\033[K" << prompt << line << std::flush;
    };

    while (true) {
        char character = '\0';

        if (::read(STDIN_FILENO, &character, 1) != 1) {
            tcsetattr(STDIN_FILENO, TCSANOW, &originalSettings);
            return false;
        }

        if (character == '\r' || character == '\n') {
            std::cout << '\r' << "\033[K"
                      << prompt << line << '\n' << std::flush;
            break;
        }

        if (character == 4 && line.empty()) {
            // Ctrl+D on an empty line ends the shell input loop.
            tcsetattr(STDIN_FILENO, TCSANOW, &originalSettings);
            return false;
        }

        if (character == 127 || character == '\b') {
            if (!line.empty()) {
                line.pop_back();
                redrawLine();
            }
            continue;
        }

        if (character == '\x1b') {
            char bracket = '\0';
            char direction = '\0';

            if (::read(STDIN_FILENO, &bracket, 1) != 1 ||
                bracket != '[' ||
                ::read(STDIN_FILENO, &direction, 1) != 1) {
                continue;
            }

            if (direction == 'A') {
                // Up: refresh shared history when starting navigation.
                if (historyPosition == entries.size()) {
                    draft = line;

                    // Fetch the latest entries, including commands from other tabs.
                    entries = history.getEntries();
                    historyPosition = entries.size();
                }

                if (historyPosition > 0) {
                    --historyPosition;
                    line = entries[historyPosition];
                    redrawLine();
                }
            } else if (direction == 'B') {
                // Down: move to a newer entry or restore the draft.
                if (historyPosition < entries.size()) {
                    ++historyPosition;

                    if (historyPosition == entries.size()) {
                        line = draft;
                    } else {
                        line = entries[historyPosition];
                    }

                    redrawLine();
                }
            }

            continue;
        }

        if (character >= 32 && character <= 126) {
            line += character;
            std::cout << character << std::flush;
        }
    }

    if (tcsetattr(STDIN_FILENO, TCSANOW, &originalSettings) == -1) {
        return false;
    }

    return true;
}

int main() {
    // Allow the shell to restore foreground terminal ownership.
    std::signal(SIGTTOU, SIG_IGN);

    CommandParser parser;
    CommandExecutor executor;
    Builtins builtins;
    History history;

    if (!history.isReady()) {
        std::cerr << "terminator: shared history is unavailable\n";
    }

    std::string line;
    int lastExitStatus = 0;
    bool shouldExit = false;

    // A reusable execution path for interactive and sourced commands.
    std::function<int(const std::string&)> executeLine;

    executeLine = [&](const std::string& commandLine) -> int {
        const std::vector<std::string> arguments =
            parser.parse(commandLine, lastExitStatus);

        // Do not change $? for an empty line.
        if (arguments.empty()) {
            return lastExitStatus;
        }

        // Preserve the Task 1 new-tab feature.
        if (arguments[0] == "newtab" && arguments.size() == 1) {
            std::cout << "\033]777;newtab\a" << std::flush;
            lastExitStatus = 0;
            return lastExitStatus;
        }

        if (builtins.execute(
                arguments,
                lastExitStatus,
                shouldExit,
                executeLine,
                history)) {
            return lastExitStatus;
        }

        lastExitStatus = executor.execute(arguments);
        return lastExitStatus;
    };

    while (true) {
        std::cout << "user@terminator> " << std::flush;

        if (!readCommandLine(history, line)) {
            break;
        }

        // Ignore whitespace-only input.
        if (line.find_first_not_of(" \t\r") == std::string::npos) {
            continue;
        }

        // Record interactive input before executing it, so "history"
        // also appears in the displayed list.
        history.add(line);

        executeLine(line);

        if (shouldExit) {
            break;
        }
    }

    return lastExitStatus;
}