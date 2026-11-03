#ifndef SHELL_IO_H
#define SHELL_IO_H

#include <atomic>
#include <cstddef>
#include <ostream>
#include <streambuf>
#include <sys/types.h>

class FileDescriptorBuffer : public std::streambuf {
private:
    int fileDescriptor;

protected:
    std::streamsize xsputn(
        const char* data,
        std::streamsize count
    ) override;

    int_type overflow(int_type character) override;
    int sync() override;

public:
    explicit FileDescriptorBuffer(int fd);
};

class ShellIO {
private:
    int inputFd;
    int outputFd;
    int errorFd;

    std::atomic<pid_t>* foregroundProcessGroup;
    std::atomic<pid_t>* foregroundHelper;

    FileDescriptorBuffer outputBuffer;
    FileDescriptorBuffer errorBuffer;

    std::ostream outputStream;
    std::ostream errorStream;

public:
    ShellIO(
        int inputFd,
        int outputFd,
        int errorFd,
        std::atomic<pid_t>* foregroundProcessGroup = nullptr,
        std::atomic<pid_t>* foregroundHelper = nullptr
    );

    ShellIO(const ShellIO&) = delete;
    ShellIO& operator=(const ShellIO&) = delete;

    int getInputFd() const;
    int getOutputFd() const;
    int getErrorFd() const;

    std::ostream& out();
    std::ostream& err();

    void setForegroundHelper(pid_t helperPid) const;
    void clearForegroundHelper() const;
    pid_t getForegroundHelper() const;

    void setForegroundProcessGroup(pid_t processGroupId) const;
    void clearForegroundProcessGroup() const;
    pid_t getForegroundProcessGroup() const;
};

#endif