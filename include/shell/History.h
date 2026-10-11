#ifndef HISTORY_H
#define HISTORY_H

#include <semaphore.h>

#include <iosfwd>
#include <string>
#include <vector>

class History {
private:
    struct SharedHistory;

    int sharedMemoryFd;
    SharedHistory* sharedHistory;
    sem_t* mutex;
    bool ready;

    bool lockShared() const;
    bool unlockShared() const;

public:
    History();
    ~History();

    History(const History&) = delete;
    History& operator=(const History&) = delete;

    bool isReady() const;
    bool add(const std::string& command);
    bool print(std::ostream& output) const;

    std::vector<std::string> getEntries() const;
};

#endif