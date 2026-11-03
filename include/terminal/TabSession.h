#ifndef TAB_SESSION_H
#define TAB_SESSION_H

#include <string>
#include <sys/types.h>

class TabSession {
public:
    virtual ~TabSession() = default;

    virtual bool start() = 0;
    virtual bool writeInput(const std::string& input) = 0;
    virtual std::string readOutput() = 0;

    virtual int getFileDescriptor() const = 0;
    virtual pid_t getPid() const = 0;

    virtual void stop() = 0;

    virtual void sendInterrupt() = 0;
    virtual void sendSuspend() = 0;
};

#endif