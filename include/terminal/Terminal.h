
#ifndef TERMINAL_H
#define TERMINAL_H

#include "process/PtySession.h"
#include "terminal/TerminalBuffer.h"

class Terminal {
private:
    PtySession session;
    TerminalBuffer buffer;
    int firstVisibleLine;

public:
    Terminal();

    bool start();
    bool sendInput(const std::string& input);

    // Returns true when shell output requests a new tab.
    bool receiveOutput();

    PtySession& getSession();
    const TerminalBuffer& getBuffer() const;
    TerminalBuffer& getBuffer();

    int getFirstVisibleLine() const;
    void setFirstVisibleLine(int line);
};

#endif
