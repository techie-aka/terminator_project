#ifndef JOBMANAGER_H
#define JOBMANAGER_H

#include <sys/types.h>

#include <iosfwd>
#include <string>
#include <vector>

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

public:
    JobManager();

    int addJob(
        pid_t processId,
        pid_t processGroupId,
        const std::string& command,
        JobState initialState = JobState::Running
    );

    int latestJobId() const;

    void update();
    void print(std::ostream& output) const;

    bool foreground(int id, int& exitStatus);
    bool background(int id);
};

#endif