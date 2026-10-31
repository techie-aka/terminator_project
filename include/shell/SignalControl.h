#ifndef SIGNAL_CONTROL_H
#define SIGNAL_CONTROL_H

#include <csignal>
#include <sys/types.h>

class SignalControl {
private:
    static volatile sig_atomic_t foregroundProcessGroup;
    static volatile sig_atomic_t shellProcessGroup;

    static void handleSignal(int signal);

public:
    static bool install();

    static void setForegroundProcessGroup(pid_t processGroupId);

    static void clearForegroundProcessGroup();
};

#endif // SIGNAL_CONTROL_H