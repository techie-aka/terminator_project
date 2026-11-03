#include "shell/SignalControl.h"

#include <unistd.h>

// These variables are used by the process-backed shell.
// sig_atomic_t allows them to be accessed safely by the signal handler.

volatile sig_atomic_t SignalControl::foregroundProcessGroup = 0;
volatile sig_atomic_t SignalControl::foregroundIsHelper = 0;
volatile sig_atomic_t SignalControl::shellProcessGroup = 0;
volatile sig_atomic_t SignalControl::shutdownRequested = 0;


void SignalControl::handleSignal(int signal) {
    const pid_t targetGroup = foregroundProcessGroup;

    int targetSignal = 0;

    if (signal == SIGUSR1) {
        // Ctrl+C notification.
        //
        // If the foreground target is a helper shell, forward SIGUSR1
        // so that the helper can interrupt its own foreground command.
        //
        // Otherwise, forward the actual SIGINT.
        targetSignal = foregroundIsHelper
            ? SIGUSR1
            : SIGINT;

    } else if (signal == SIGUSR2) {
        // Ctrl+Z notification.
        targetSignal = foregroundIsHelper
            ? SIGUSR2
            : SIGTSTP;

    } else if (signal == SIGTERM || signal == SIGHUP) {
        // The shell must leave its input loop.
        shutdownRequested = 1;
        targetSignal = signal;

    } else {
        return;
    }

    if (targetGroup > 0) {
        // Send the signal to the complete foreground process group.
        (void)kill(-targetGroup, targetSignal);

    } else if (targetSignal == SIGINT && shellProcessGroup > 0) {
        // At an idle prompt, there may be no foreground job.
        // Forward SIGINT to the shell process group so MultiWatch
        // can receive Ctrl+C through its own signal handler.
        (void)kill(-shellProcessGroup, SIGINT);
    }
}


bool SignalControl::install() {
    shellProcessGroup = getpgrp();

    foregroundProcessGroup = 0;
    foregroundIsHelper = 0;
    shutdownRequested = 0;

    struct sigaction action {};

    action.sa_handler = handleSignal;
    sigemptyset(&action.sa_mask);

    // Blocking reads and waits should be interrupted.
    action.sa_flags = 0;

    const int handledSignals[] = {
        SIGUSR1,
        SIGUSR2,
        SIGTERM,
        SIGHUP
    };

    for (int signal : handledSignals) {
        if (sigaction(signal, &action, nullptr) == -1) {
            return false;
        }
    }

    return true;
}


void SignalControl::setForegroundProcessGroup(
    pid_t processGroupId
) {
    foregroundProcessGroup = processGroupId > 0
        ? static_cast<sig_atomic_t>(processGroupId)
        : 0;

    foregroundIsHelper = 0;
}


void SignalControl::clearForegroundProcessGroup() {
    foregroundProcessGroup = 0;
    foregroundIsHelper = 0;
}


void SignalControl::setForegroundHelperProcessGroup(
    pid_t processGroupId
) {
    foregroundProcessGroup = processGroupId > 0
        ? static_cast<sig_atomic_t>(processGroupId)
        : 0;

    foregroundIsHelper = 1;
}


void SignalControl::clearForegroundHelperProcessGroup() {
    foregroundProcessGroup = 0;
    foregroundIsHelper = 0;
}


void SignalControl::requestShutdown() {
    shutdownRequested = 1;
}


bool SignalControl::isShutdownRequested() {
    return shutdownRequested != 0;
}