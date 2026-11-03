#include "shell/ShellIO.h"

#include <cerrno>
#include <unistd.h>

FileDescriptorBuffer::FileDescriptorBuffer(int fd)
    : fileDescriptor(fd) {
}

std::streamsize FileDescriptorBuffer::xsputn(
    const char* data,
    std::streamsize count
) {
    std::streamsize written = 0;

    while (written < count) {
        const ssize_t result = ::write(
            fileDescriptor,
            data + written,
            static_cast<std::size_t>(count - written)
        );

        if (result > 0) {
            written += result;
        } else if (result == -1 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }

    return written;
}

FileDescriptorBuffer::int_type FileDescriptorBuffer::overflow(
    int_type character
) {
    if (traits_type::eq_int_type(
            character,
            traits_type::eof())) {
        return traits_type::not_eof(character);
    }

    const char byte = traits_type::to_char_type(character);

    return xsputn(&byte, 1) == 1
        ? character
        : traits_type::eof();
}

int FileDescriptorBuffer::sync() {
    return 0;
}

ShellIO::ShellIO(
    int inputFd,
    int outputFd,
    int errorFd,
    std::atomic<pid_t>* foregroundProcessGroup,
    std::atomic<pid_t>* foregroundHelper
)
    : inputFd(inputFd),
      outputFd(outputFd),
      errorFd(errorFd),
      foregroundProcessGroup(foregroundProcessGroup),
      foregroundHelper(foregroundHelper),
      outputBuffer(outputFd),
      errorBuffer(errorFd),
      outputStream(&outputBuffer),
      errorStream(&errorBuffer) {
}

int ShellIO::getInputFd() const {
    return inputFd;
}

int ShellIO::getOutputFd() const {
    return outputFd;
}

int ShellIO::getErrorFd() const {
    return errorFd;
}

std::ostream& ShellIO::out() {
    return outputStream;
}

std::ostream& ShellIO::err() {
    return errorStream;
}

void ShellIO::setForegroundProcessGroup(
    pid_t processGroupId
) const {
    if (foregroundProcessGroup != nullptr) {
        foregroundProcessGroup->store(
            processGroupId,
            std::memory_order_relaxed
        );
    }
}

void ShellIO::clearForegroundProcessGroup() const {
    setForegroundProcessGroup(0);
}

pid_t ShellIO::getForegroundProcessGroup() const {
    if (foregroundProcessGroup == nullptr) {
        return 0;
    }

    return foregroundProcessGroup->load(
        std::memory_order_relaxed
    );
}

void ShellIO::setForegroundHelper(pid_t helperPid) const {
    if (foregroundHelper != nullptr) {
        foregroundHelper->store(
            helperPid,
            std::memory_order_relaxed
        );
    }
}

void ShellIO::clearForegroundHelper() const {
    setForegroundHelper(0);
}

pid_t ShellIO::getForegroundHelper() const {
    if (foregroundHelper == nullptr) {
        return 0;
    }

    return foregroundHelper->load(
        std::memory_order_relaxed
    );
}