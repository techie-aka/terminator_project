#include "shell/ShellEngine.h"
#include "shell/SignalControl.h"

#include <csignal>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    const bool oneShotCommand =
        argc == 3 && std::string(argv[1]) == "-c";

    // The interactive shell ignores terminal-generated signals and receives
    // GUI notifications through SIGUSR1/SIGUSR2. A one-shot helper keeps
    // SIGINT's default action when no foreground job is active, so an
    // interrupted command substitution can terminate cleanly. MultiWatch
    // installs its own SIGINT handler while it is running.
    if (!oneShotCommand) {
        std::signal(SIGINT, SIG_IGN);
    }

    std::signal(SIGQUIT, SIG_IGN);
    std::signal(SIGTSTP, SIG_IGN);
    std::signal(SIGTTIN, SIG_IGN);
    std::signal(SIGTTOU, SIG_IGN);

    if (!SignalControl::install()) {
        std::cerr
            << "terminator: unable to install signal handlers\n";
        return 1;
    }

    ShellEngine shell;

    // Internal helper mode: execute one command and exit without a prompt.
    if (argc == 3 && std::string(argv[1]) == "-c") {
        return shell.runCommand(argv[2]);
    }

    if (argc != 1) {
        std::cerr << "usage: terminator-shell [-c command]\n";
        return 2;
    }

    return shell.run();
}