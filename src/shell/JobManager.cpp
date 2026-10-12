#include "shell/JobManager.h"

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

    const pid_t shellProcessGroup = getpgrp();
    bool terminalHandedOver = false;

    if (isatty(STDIN_FILENO)) {
        if (tcsetpgrp(STDIN_FILENO, job->processGroupId) == -1) {
            return false;
        }

        terminalHandedOver = true;
    }

    // A stopped job must be resumed before waiting for it.
    if (job->state == JobState::Stopped) {
        if (kill(-job->processGroupId, SIGCONT) == -1) {
            if (terminalHandedOver) {
                tcsetpgrp(STDIN_FILENO, shellProcessGroup);
            }

            update();
            return false;
        }

        job->state = JobState::Running;
    }

    int status = 0;
    pid_t result;

    // WUNTRACED makes waitpid return if Ctrl+Z stops this process.
    do {
        result = waitpid(job->processId, &status, WUNTRACED);
    } while (result == -1 && errno == EINTR);

    if (terminalHandedOver) {
        if (tcsetpgrp(STDIN_FILENO, shellProcessGroup) == -1) {
            return false;
        }
    }

    if (result == -1) {
        return false;
    }

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

    if (job->state == JobState::Stopped) {
        if (kill(-job->processGroupId, SIGCONT) == -1) {
            return false;
        }

        job->state = JobState::Running;
    }

    return true;
}