
#include "terminal/Terminal.h"

#include <algorithm>

Terminal::Terminal()
    : firstVisibleLine(0) {
}

bool Terminal::start() {
    return session.start();
}

bool Terminal::sendInput(const std::string& input) {
    return session.writeInput(input);
}

bool Terminal::receiveOutput() {
    const std::string output = session.readOutput();

    if (output.empty()) {
        return false;
    }

    return buffer.appendOutput(output);
}

PtySession& Terminal::getSession() {
    return session;
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
    const int totalLines = static_cast<int>(buffer.getLines().size());

    firstVisibleLine = std::clamp(
        line, 0, std::max(0, totalLines - 1)
    );
}
