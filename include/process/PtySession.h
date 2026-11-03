#ifndef PTYSESSION_H
#define PTYSESSION_H

#include "terminal/TabSession.h"

class PtySession : public TabSession {
private:
    int masterFd;
    pid_t processId;

public:
    PtySession();
    ~PtySession() override;

    PtySession(const PtySession&) = delete;
    PtySession& operator=(const PtySession&) = delete;

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