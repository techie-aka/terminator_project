
#include "shell/JobManager.h"
#include "shell/SignalControl.h"

#include <cerrno>
#include <csignal>
#include <ostream>

#include <sys/wait.h>
#include <unistd.h>

JobManager::JobManager()
    : nextJobId(1) {
}

Job* JobManager::findJob(int id) {
    for (Job& job : jobs) {
        if (job.id == id) {
            return &job;
        }
    }

    return nullptr;
}

const Job* JobManager::findJob(int id) const {
    for (const Job& job : jobs) {
        if (job.id == id) {
            return &job;
        }
    }

    return nullptr;
}

int JobManager::addJob(
    pid_t processId,
    pid_t processGroupId,
    const std::string& command,
    JobState initialState
) {
    const int id = nextJobId++;

    jobs.push_back({
        id,
        processId,
        processGroupId,
        command,
        initialState,
        0
    });

    return id;
}

int JobManager::latestJobId() const {
    for (auto it = jobs.rbegin(); it != jobs.rend(); ++it) {
        if (it->state != JobState::Done) {
            return it->id;
        }
    }

    return 0;
}

void JobManager::update() {
    int status = 0;

    while (true) {
        const pid_t result = waitpid(
            -1,
            &status,
            WNOHANG | WUNTRACED | WCONTINUED
        );

        if (result > 0) {
            for (Job& job : jobs) {
                if (job.processId != result) {
                    continue;
                }

                if (WIFEXITED(status)) {
                    job.state = JobState::Done;
                    job.exitStatus = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    job.state = JobState::Done;
                    job.exitStatus = 128 + WTERMSIG(status);
                } else if (WIFSTOPPED(status)) {
                    job.state = JobState::Stopped;
                } else if (WIFCONTINUED(status)) {
                    job.state = JobState::Running;
                }

                break;
            }

            continue;
        }

        if (result == -1 && errno == EINTR) {
            continue;
        }

        break;
    }
}

void JobManager::print(std::ostream& output) const {
    for (const Job& job : jobs) {
        output << '[' << job.id << "] ";

        switch (job.state) {
            case JobState::Running:
                output << "Running";
                break;

            case JobState::Stopped:
                output << "Stopped";
                break;

            case JobState::Done:
                output << "Done (" << job.exitStatus << ')';
                break;
        }

        output << '\t' << job.command << '\n';
    }
}

bool JobManager::foreground(int id, int& exitStatus) {
    update();

    // ID 0 means use the most recent non-completed job.
    if (id == 0) {
        id = latestJobId();
    }

    Job* job = findJob(id);

    if (job == nullptr || job->state == JobState::Done) {
        return false;
    }

    /*
     * The selected job becomes the foreground signal target.
     *
     * This is required because Ctrl+C and Ctrl+Z are delivered through
     * the shell's SIGUSR1/SIGUSR2 notification handlers.
     */
    SignalControl::setForegroundProcessGroup(
        job->processGroupId
    );

    const pid_t shellProcessGroup = getpgrp();
    bool terminalHandedOver = false;

    // Give the job terminal ownership before resuming or waiting for it.
    if (isatty(STDIN_FILENO)) {
        if (tcsetpgrp(
                STDIN_FILENO,
                job->processGroupId
            ) == -1) {

            SignalControl::clearForegroundProcessGroup();
            return false;
        }

        terminalHandedOver = true;
    }

    // A stopped job must be resumed before waiting for it.
    if (job->state == JobState::Stopped) {
        if (kill(-job->processGroupId, SIGCONT) == -1) {

            if (terminalHandedOver) {
                (void)tcsetpgrp(
                    STDIN_FILENO,
                    shellProcessGroup
                );
            }

            SignalControl::clearForegroundProcessGroup();

            update();
            return false;
        }

        job->state = JobState::Running;
    }

    int status = 0;
    pid_t result;

    /*
     * Wait for the job's representative process.
     *
     * WUNTRACED is important: it allows Ctrl+Z to return control
     * to the shell when the foreground job is suspended.
     */
    do {
        result = waitpid(
            job->processId,
            &status,
            WUNTRACED
        );
    } while (result == -1 && errno == EINTR);

    // Restore terminal ownership to the shell.
    bool terminalRestoreFailed = false;

    if (terminalHandedOver) {
        if (tcsetpgrp(
                STDIN_FILENO,
                shellProcessGroup
            ) == -1) {

            terminalRestoreFailed = true;
        }
    }

    // The foreground job has returned control to the shell.
    SignalControl::clearForegroundProcessGroup();

    if (result == -1) {
        return false;
    }

    // Update the tracked job according to the final wait status.
    if (WIFEXITED(status)) {
        job->state = JobState::Done;
        job->exitStatus = WEXITSTATUS(status);
        exitStatus = job->exitStatus;
    } else if (WIFSIGNALED(status)) {
        job->state = JobState::Done;
        job->exitStatus = 128 + WTERMSIG(status);
        exitStatus = job->exitStatus;
    } else if (WIFSTOPPED(status)) {
        job->state = JobState::Stopped;
        exitStatus = 128 + WSTOPSIG(status);
    }

    if (terminalRestoreFailed) {
        return false;
    }

    return true;
}

bool JobManager::background(int id) {
    update();

    if (id == 0) {
        id = latestJobId();
    }

    Job* job = findJob(id);

    if (job == nullptr || job->state == JobState::Done) {
        return false;
    }

    // Resume a suspended process group without taking terminal ownership.
    if (job->state == JobState::Stopped) {
        if (kill(-job->processGroupId, SIGCONT) == -1) {
            return false;
        }

        job->state = JobState::Running;
    }

    return true;
}
