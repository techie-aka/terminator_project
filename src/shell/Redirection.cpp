#include "shell/Redirection.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

bool Redirection::applyInChild(
    const RedirectionSpec& specification,
    RedirectionFailure& failure
) {
    failure = RedirectionFailure{};

    int inputFd = -1;
    int outputFd = -1;

    // Open the input first. If it does not exist, do not create or
    // truncate the output file and do not execute the requested command.
    if (specification.hasInput) {
        inputFd = open(specification.inputFile.c_str(), O_RDONLY);

        if (inputFd == -1) {
            failure.errorNumber = errno;
            failure.filename = specification.inputFile;
            failure.input = true;
            return false;
        }
    }

    // Create an output file if necessary; otherwise truncate it.
    if (specification.hasOutput) {
        outputFd = open(
            specification.outputFile.c_str(),
            O_WRONLY | O_CREAT | O_TRUNC,
            0644
        );

        if (outputFd == -1) {
            failure.errorNumber = errno;
            failure.filename = specification.outputFile;
            failure.input = false;

            if (inputFd != -1) {
                close(inputFd);
            }

            return false;
        }
    }

    // Replace standard input with the input file.
    if (inputFd != -1) {
        if (dup2(inputFd, STDIN_FILENO) == -1) {
            failure.errorNumber = errno;
            failure.filename = specification.inputFile;
            failure.input = true;

            close(inputFd);

            if (outputFd != -1) {
                close(outputFd);
            }

            return false;
        }

        close(inputFd);
    }

    // Replace standard output with the output file.
    if (outputFd != -1) {
        if (dup2(outputFd, STDOUT_FILENO) == -1) {
            failure.errorNumber = errno;
            failure.filename = specification.outputFile;
            failure.input = false;

            close(outputFd);
            return false;
        }

        close(outputFd);
    }

    return true;
}

void Redirection::reportFailure(const RedirectionFailure& failure) {
    const char* operation = failure.input
        ? "input redirection"
        : "output redirection";

    std::fprintf(
        stderr,
        "terminator: %s: %s: %s\n",
        operation,
        failure.filename.c_str(),
        std::strerror(failure.errorNumber)
    );
}