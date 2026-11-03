#ifndef TERMINAL_H
#define TERMINAL_H

#include "terminal/TabSession.h"
#include "terminal/TerminalBuffer.h"

#include <memory>

class Terminal {
private:
    std::unique_ptr<TabSession> session;
    TerminalBuffer buffer;
    int firstVisibleLine;

public:
    Terminal();

    explicit Terminal(std::unique_ptr<TabSession> session);

    bool start();
    bool sendInput(const std::string& input);

    bool receiveOutput();

    TabSession& getSession();

    const TerminalBuffer& getBuffer() const;
    TerminalBuffer& getBuffer();

    int getFirstVisibleLine() const;
    void setFirstVisibleLine(int line);
};

#endif