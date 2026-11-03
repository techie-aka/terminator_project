#ifndef SIGNAL_CONTROL_H
#define SIGNAL_CONTROL_H

#include <csignal>
#include <sys/types.h>

class SignalControl {
private:
    static volatile sig_atomic_t foregroundProcessGroup;
    static volatile sig_atomic_t foregroundIsHelper;
    static volatile sig_atomic_t shellProcessGroup;
    static volatile sig_atomic_t shutdownRequested;

    static void handleSignal(int signal);

public:
    static bool install();

    static void setForegroundProcessGroup(pid_t processGroupId);
    static void clearForegroundProcessGroup();

    // Used when the shell is waiting for a helper shell.
    // The helper must receive a notification and forward the
    // corresponding signal to its own foreground command.
    static void setForegroundHelperProcessGroup(pid_t processGroupId);
    static void clearForegroundHelperProcessGroup();

    // Used to stop the shell's input loop during shutdown.
    static void requestShutdown();
    static bool isShutdownRequested();
};

#endif // SIGNAL_CONTROL_H