#include "terminal/Terminal.h"
#include "process/PtySession.h"

#include <algorithm>
#include <utility>

Terminal::Terminal()
    : Terminal(std::make_unique<PtySession>()) {
}

Terminal::Terminal(std::unique_ptr<TabSession> session)
    : session(std::move(session)),
      firstVisibleLine(0) {
}

bool Terminal::start() {
    return session && session->start();
}

bool Terminal::sendInput(const std::string& input) {
    return session && session->writeInput(input);
}

bool Terminal::receiveOutput() {
    if (!session) {
        return false;
    }

    const std::string output = session->readOutput();

    if (output.empty()) {
        return false;
    }

    return buffer.appendOutput(output);
}

TabSession& Terminal::getSession() {
    return *session;
}

const TerminalBuffer& Terminal::getBuffer() const {
    return buffer;
}

TerminalBuffer& Terminal::getBuffer() {
    return buffer;
}

int Terminal::getFirstVisibleLine() const {
    return firstVisibleLine;
}

void Terminal::setFirstVisibleLine(int line) {
    const int totalLines =
        static_cast<int>(buffer.getLines().size());

    firstVisibleLine = std::clamp(
        line,
        0,
        std::max(0, totalLines - 1)
    );
}