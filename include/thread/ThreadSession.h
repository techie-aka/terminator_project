#ifndef THREAD_SESSION_H
#define THREAD_SESSION_H

#include "terminal/TabSession.h"

#include <atomic>
#include <thread>

class ThreadSession final : public TabSession {
private:
    int inputWriteFd;
    int outputReadFd;

    std::thread shellThread;
    std::atomic<pid_t> foregroundProcessGroup;
    std::atomic<pid_t> foregroundHelper;
    std::atomic<bool> shellFinished;
    bool started;

    void runShell(int inputReadFd, int outputWriteFd);

public:
    ThreadSession();
    ~ThreadSession() override;

    ThreadSession(const ThreadSession&) = delete;
    ThreadSession& operator=(const ThreadSession&) = delete;

    bool start() override;
    bool writeInput(const std::string& input) override;
    std::string readOutput() override;

    int getFileDescriptor() const override;
    pid_t getPid() const override;

    void stop() override;

    void sendInterrupt() override;
    void sendSuspend() override;
};

#endif