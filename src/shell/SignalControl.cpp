#include "shell/SignalControl.h"

#include <unistd.h>

volatile sig_atomic_t SignalControl::foregroundProcessGroup = 0;
volatile sig_atomic_t SignalControl::shellProcessGroup = 0;

void SignalControl::handleSignal(int signal) {

    int targetSignal;

    if (signal == SIGUSR1) {
        targetSignal = SIGINT;
    } else if (signal == SIGUSR2) {
        targetSignal = SIGTSTP;
    } else {
        return;
    }

    pid_t foregroundGroup = foregroundProcessGroup;

    if (foregroundGroup > 0) {
        kill(-foregroundGroup, targetSignal);
    } else if (targetSignal == SIGINT && shellProcessGroup > 0) {
        kill(-shellProcessGroup, SIGINT);
    }
}

bool SignalControl::install() {

    shellProcessGroup = getpgrp();

    struct sigaction action {};

    action.sa_handler = handleSignal;

    sigemptyset(&action.sa_mask);

    action.sa_flags = 0;

    if (sigaction(SIGUSR1, &action, nullptr) == -1) {
        return false;
    }

    if (sigaction(SIGUSR2, &action, nullptr) == -1) {
        return false;
    }

    return true;
}

void SignalControl::setForegroundProcessGroup(pid_t processGroupId) {

    foregroundProcessGroup = processGroupId > 0
        ? processGroupId
        : 0;
}

void SignalControl::clearForegroundProcessGroup() {
    foregroundProcessGroup = 0;
}