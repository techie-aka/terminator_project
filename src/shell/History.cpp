#include "shell/History.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr std::uint32_t HISTORY_MAGIC = 0x54485259;
constexpr std::size_t HISTORY_CAPACITY = 10000;
constexpr std::size_t MAX_COMMAND_LENGTH = 1024;
}

struct History::SharedHistory {
    std::uint32_t magic;
    std::size_t count;
    std::size_t nextIndex;
    char entries[HISTORY_CAPACITY][MAX_COMMAND_LENGTH];
};

History::History()
    : sharedMemoryFd(-1),
      sharedHistory(nullptr),
      mutex(SEM_FAILED),
      ready(false) {
    char sharedMemoryName[64];
    char semaphoreName[64];

    const unsigned int userId = static_cast<unsigned int>(getuid());

    std::snprintf(
        sharedMemoryName,
        sizeof(sharedMemoryName),
        "/terminator_history_%u",
        userId
    );

    std::snprintf(
        semaphoreName,
        sizeof(semaphoreName),
        "/terminator_history_lock_%u",
        userId
    );

    // Open one shared-memory region for all Terminator shell processes.
    sharedMemoryFd = shm_open(
        sharedMemoryName,
        O_CREAT | O_RDWR,
        0600
    );

    if (sharedMemoryFd == -1) {
        return;
    }

    if (ftruncate(
            sharedMemoryFd,
            static_cast<off_t>(sizeof(SharedHistory))
        ) == -1) {
        return;
    }

    void* memory = mmap(
        nullptr,
        sizeof(SharedHistory),
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        sharedMemoryFd,
        0
    );

    if (memory == MAP_FAILED) {
        return;
    }

    sharedHistory = static_cast<SharedHistory*>(memory);

    // A named semaphore synchronizes access across processes.
    mutex = sem_open(
        semaphoreName,
        O_CREAT,
        0600,
        1
    );

    if (mutex == SEM_FAILED) {
        return;
    }

    if (!lockShared()) {
        return;
    }

    // Initialize the region once; do not erase history when another
    // tab or a new application session attaches to it.
    if (sharedHistory->magic != HISTORY_MAGIC) {
        std::memset(sharedHistory, 0, sizeof(SharedHistory));
        sharedHistory->magic = HISTORY_MAGIC;
    }

    ready = true;
    unlockShared();
}

History::~History() {
    // Do not unlink shared memory here: other tabs may still be using it.
    if (mutex != SEM_FAILED) {
        sem_close(mutex);
    }

    if (sharedHistory != nullptr) {
        munmap(sharedHistory, sizeof(SharedHistory));
    }

    if (sharedMemoryFd != -1) {
        close(sharedMemoryFd);
    }
}

bool History::lockShared() const {
    if (sharedHistory == nullptr || mutex == SEM_FAILED) {
        return false;
    }

    while (sem_wait(mutex) == -1) {
        if (errno != EINTR) {
            return false;
        }
    }

    return true;
}

bool History::unlockShared() const {
    return mutex != SEM_FAILED && sem_post(mutex) == 0;
}

bool History::isReady() const {
    return ready;
}

bool History::add(const std::string& command) {
    if (!ready || !lockShared()) {
        return false;
    }

    // Keep a multiline command as one history entry.
    std::string storedCommand;

    for (char character : command) {
        if (character == '\n') {
            storedCommand += "\\n";
        } else if (character != '\r') {
            storedCommand += character;
        }
    }

    char* entry = sharedHistory->entries[sharedHistory->nextIndex];

    std::strncpy(
        entry,
        storedCommand.c_str(),
        MAX_COMMAND_LENGTH - 1
    );

    entry[MAX_COMMAND_LENGTH - 1] = '\0';

    sharedHistory->nextIndex =
        (sharedHistory->nextIndex + 1) % HISTORY_CAPACITY;

    if (sharedHistory->count < HISTORY_CAPACITY) {
        ++sharedHistory->count;
    }

    return unlockShared();
}

bool History::print(std::ostream& output) const {
    if (!ready || !lockShared()) {
        return false;
    }

    const std::size_t firstIndex =
        (sharedHistory->nextIndex + HISTORY_CAPACITY -
         sharedHistory->count) % HISTORY_CAPACITY;

    for (std::size_t i = 0; i < sharedHistory->count; ++i) {
        const std::size_t index =
            (firstIndex + i) % HISTORY_CAPACITY;

        output << (i + 1) << "  "
               << sharedHistory->entries[index] << '\n';
    }

    return unlockShared();
}

std::vector<std::string> History::getEntries() const {
    std::vector<std::string> entries;

    if (!ready || !lockShared()) {
        return entries;
    }

    entries.reserve(sharedHistory->count);

    const std::size_t firstIndex =
        (sharedHistory->nextIndex + HISTORY_CAPACITY -
         sharedHistory->count) % HISTORY_CAPACITY;

    for (std::size_t i = 0; i < sharedHistory->count; ++i) {
        const std::size_t index =
            (firstIndex + i) % HISTORY_CAPACITY;

        entries.emplace_back(sharedHistory->entries[index]);
    }

    unlockShared();
    return entries;
}