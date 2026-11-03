#ifndef JOBMANAGER_H
#define JOBMANAGER_H

#include <sys/types.h>

#include <iosfwd>
#include <string>
#include <vector>

class ShellIO;

enum class JobState {
    Running,
    Stopped,
    Done
};

struct Job {
    int id;
    pid_t processId;
    pid_t processGroupId;
    std::string command;
    JobState state;
    int exitStatus;
};

class JobManager {
private:
    std::vector<Job> jobs;
    int nextJobId;

    Job* findJob(int id);
    const Job* findJob(int id) const;

    // Terminates and reaps all jobs when the shell exits.
    void terminateRemainingJobs();

public:
    JobManager();
    ~JobManager();

    // A JobManager owns child-process bookkeeping and must not be copied.
    JobManager(const JobManager&) = delete;
    JobManager& operator=(const JobManager&) = delete;

    int addJob(
        pid_t processId,
        pid_t processGroupId,
        const std::string& command,
        JobState initialState = JobState::Running
    );

    int latestJobId() const;

    void update();
    void print(std::ostream& output) const;

    // Thread mode supplies ShellIO to publish the foreground group
    // belonging to the current tab.
    //
    // Process mode leaves io as nullptr and uses SignalControl.
    bool foreground(
        int id,
        int& exitStatus,
        ShellIO* io = nullptr
    );

    bool background(int id);
};

#endif // JOBMANAGER_H