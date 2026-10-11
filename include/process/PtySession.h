#ifndef PTYSESSION_H
#define PTYSESSION_H

#include <string>
#include <sys/types.h>

class PtySession {
private:
    int masterFd;
    pid_t processId;

public:
    PtySession();
    ~PtySession();

    PtySession(const PtySession&) = delete;
    PtySession& operator=(const PtySession&) = delete;

    bool start();
    bool writeInput(const std::string& input);
    std::string readOutput();

    int getFileDescriptor() const;
    pid_t getPid() const;
};

#endif