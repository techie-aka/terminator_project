
#include "shell/JobManager.h"

#include "shell/ShellIO.h"
#include "shell/SignalControl.h"

#include <cerrno>
#include <csignal>
#include <chrono>
#include <ostream>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

namespace {

bool processGroupExists(pid_t processGroupId) {
    if (processGroupId <= 0) {
        return false;
    }

    if (kill(-processGroupId, 0) == 0) {
        return true;
    }

    return errno == EPERM;
}


void sendToProcessGroup(pid_t processGroupId, int signal) {
    if (processGroupId > 0) {
        (void)kill(-processGroupId, signal);
    }
}

} // namespace

JobManager::JobManager()
    : nextJobId(1) {
}

JobManager::~JobManager() {
    terminateRemainingJobs();
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
    for (Job& job : jobs) {
        if (job.state == JobState::Done || job.processId <= 0) {
            continue;
        }

        while (true) {
            int status = 0;

            const pid_t result = waitpid(
                job.processId,
                &status,
                WNOHANG | WUNTRACED | WCONTINUED
            );

            if (result == job.processId) {
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

                if (job.state == JobState::Done) {
                    break;
                }

                // Check for any additional status changes.
                continue;
            }

            if (result == -1 && errno == EINTR) {
                continue;
            }

            if (result == -1 && errno == ECHILD) {
                // The process is no longer a waitable child.
                // Its actual exit status is unavailable.
                job.state = JobState::Done;
                job.exitStatus = -1;
            }

            break;
        }
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
                if (job.exitStatus == -1) {
                    output << "Done (status unavailable)";
                } else {
                    output << "Done (" << job.exitStatus << ')';
                }
                break;
        }

        output << '\t' << job.command << '\n';
    }
}

bool JobManager::printStoppedJob(
    int id,
    std::ostream& output
) const {
    const Job* job = findJob(id);

    if (job == nullptr || job->state != JobState::Stopped) {
        return false;
    }

    output << '[' << job->id << "] Stopped "
           << job->command << '\n' << std::flush;

    return true;
}

bool JobManager::foreground(
    int id,
    int& exitStatus,
    ShellIO* io
) {
    update();

    // ID 0 means the latest available job.
    if (id == 0) {
        id = latestJobId();
    }

    Job* job = findJob(id);

    if (job == nullptr || job->state == JobState::Done) {
        return false;
    }

    // Publish the foreground job to the correct signal handler.
    //
    // Thread mode:
    //     The current tab owns the ShellIO object.
    //
    // Process mode:
    //     The shell uses process-global SignalControl.
    if (io != nullptr) {
        io->setForegroundProcessGroup(job->processGroupId);
    } else {
        SignalControl::setForegroundProcessGroup(
            job->processGroupId
        );
    }

    const pid_t shellProcessGroup = getpgrp();

    bool terminalHandedOver = false;

    // Only process mode has a real terminal attached to stdin.
    if (io == nullptr && isatty(STDIN_FILENO)) {
        if (tcsetpgrp(
                STDIN_FILENO,
                job->processGroupId
            ) == -1) {

            SignalControl::clearForegroundProcessGroup();
            return false;
        }

        terminalHandedOver = true;
    }

    // Resume the job if it was previously stopped by Ctrl+Z.
    if (job->state == JobState::Stopped) {
        if (kill(-job->processGroupId, SIGCONT) == -1) {

            if (terminalHandedOver) {
                (void)tcsetpgrp(
                    STDIN_FILENO,
                    shellProcessGroup
                );
            }

            if (io != nullptr) {
                io->clearForegroundProcessGroup();
            } else {
                SignalControl::clearForegroundProcessGroup();
            }

            update();
            return false;
        }

        job->state = JobState::Running;
    }

    int status = 0;
    pid_t result;

    // Wait until the process exits or gets stopped again.
    do {
        result = waitpid(
            job->processId,
            &status,
            WUNTRACED
        );
    } while (result == -1 && errno == EINTR);

    // Return terminal ownership to the shell in process mode.
    if (terminalHandedOver) {
        (void)tcsetpgrp(
            STDIN_FILENO,
            shellProcessGroup
        );
    }

    // Clear the foreground target.
    if (io != nullptr) {
        io->clearForegroundProcessGroup();
    } else {
        SignalControl::clearForegroundProcessGroup();
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

    // Resume a suspended process group without taking terminal ownership.
    if (job->state == JobState::Stopped) {
        if (kill(-job->processGroupId, SIGCONT) == -1) {
            return false;
        }

        job->state = JobState::Running;
    }

    return true;
}

void JobManager::terminateRemainingJobs() {
    update();

    std::vector<pid_t> processGroups;
    processGroups.reserve(jobs.size());

    // Collect all process groups that still have running processes.
    for (const Job& job : jobs) {
        if (job.processGroupId > 0 &&
            processGroupExists(job.processGroupId)) {

            processGroups.push_back(job.processGroupId);
        }
    }

    if (processGroups.empty()) {
        return;
    }

    // First request graceful termination.
    //
    // SIGHUP tells jobs that the shell is exiting.
    // SIGCONT allows stopped jobs to receive that signal.
    for (pid_t group : processGroups) {
        sendToProcessGroup(group, SIGHUP);
        sendToProcessGroup(group, SIGCONT);
    }

    // Wait briefly for jobs to exit.
    const auto waitForGroups = [&](int milliseconds) {
        const int attempts = milliseconds / 10;

        for (int attempt = 0; attempt < attempts; ++attempt) {
            bool anyAlive = false;

            for (pid_t group : processGroups) {
                if (processGroupExists(group)) {
                    anyAlive = true;
                    break;
                }
            }

            if (!anyAlive) {
                return;
            }

            std::this_thread::sleep_for(
                std::chrono::milliseconds(10)
            );
        }
    };

    waitForGroups(200);

    // Escalate to SIGTERM for processes that ignored SIGHUP.
    for (pid_t group : processGroups) {
        sendToProcessGroup(group, SIGTERM);
    }

    waitForGroups(150);

    // Final escalation.
    for (pid_t group : processGroups) {
        sendToProcessGroup(group, SIGKILL);
    }

    // Reap direct child processes so they do not become zombies.
    for (Job& job : jobs) {
        if (job.processId <= 0) {
            continue;
        }

        int status = 0;
        pid_t result;

        do {
            result = waitpid(
                job.processId,
                &status,
                0
            );
        } while (result == -1 && errno == EINTR);

        if (result == job.processId) {

            if (WIFEXITED(status)) {
                job.exitStatus = WEXITSTATUS(status);

            } else if (WIFSIGNALED(status)) {
                job.exitStatus = 128 + WTERMSIG(status);
            }

            job.state = JobState::Done;

        } else if (result == -1 && errno == ECHILD) {
            job.state = JobState::Done;
        }
    }
}