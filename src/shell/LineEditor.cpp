
#include "shell/LineEditor.h"
#include "shell/History.h"
#include "shell/SignalControl.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>

#include <termios.h>
#include <unistd.h>

#include "shell/ShellIO.h"
#include <ostream>

#include <dirent.h>
#include <sys/stat.h>

namespace {

enum class InputStatus {
    Complete,
    Incomplete,
    BackslashContinuation
};

enum class ContextKind {
    Normal,
    CommandSubstitution,
    Backtick
};

struct ScanContext {
    ContextKind kind;
    char quote;
    int parenthesisDepth;
};

// Inspect input without executing it.
// Separate contexts allow quotes inside command substitutions to be
// tracked independently from the surrounding command.
InputStatus analyzeInput(const std::string& input) {
    std::vector<ScanContext> contexts;
    contexts.push_back({ContextKind::Normal, '\0', 0});

    bool escaped = false;

    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        ScanContext& context = contexts.back();

        if (escaped) {
            escaped = false;
            continue;
        }

        // Single-quoted content is literal.
        if (context.quote == '\'') {
            if (c == '\'') {
                context.quote = '\0';
            }
            continue;
        }

        if (c == '\\') {
            if (i + 1 == input.size()) {
                return InputStatus::BackslashContinuation;
            }

            const char next = input[i + 1];

            if (context.quote == '"') {
                if (next == '$' || next == '`' || next == '"' ||
                    next == '\\' || next == '\n') {
                    escaped = true;
                    continue;
                }
            } else {
                escaped = true;
                continue;
            }
        }

        if (context.kind == ContextKind::Backtick && c == '`') {
            contexts.pop_back();
            continue;
        }

        if (context.quote == '"') {
            if (c == '"') {
                context.quote = '\0';
                continue;
            }

            if (c == '$' && i + 1 < input.size() &&
                input[i + 1] == '(') {
                contexts.push_back({
                    ContextKind::CommandSubstitution, '\0', 1
                });
                ++i;
                continue;
            }

            if (c == '`') {
                contexts.push_back({
                    ContextKind::Backtick, '\0', 0
                });
                continue;
            }

            continue;
        }

        if (c == '\'') {
            context.quote = '\'';
            continue;
        }

        if (c == '"') {
            context.quote = '"';
            continue;
        }

        if (c == '$' && i + 1 < input.size() &&
            input[i + 1] == '(') {
            contexts.push_back({
                ContextKind::CommandSubstitution, '\0', 1
            });
            ++i;
            continue;
        }

        if (c == '`') {
            contexts.push_back({
                ContextKind::Backtick, '\0', 0
            });
            continue;
        }

        if (context.kind == ContextKind::CommandSubstitution) {
            if (c == '(') {
                ++context.parenthesisDepth;
            } else if (c == ')') {
                --context.parenthesisDepth;

                if (context.parenthesisDepth == 0) {
                    contexts.pop_back();
                }
            }
        }
    }

    for (const ScanContext& context : contexts) {
        if (context.quote != '\0') {
            return InputStatus::Incomplete;
        }
    }

    if (contexts.size() > 1) {
        return InputStatus::Incomplete;
    }

    return InputStatus::Complete;
}

// Read one byte, retrying if the system call is interrupted.
bool readByte(int inputFd, char& byte) {
    while (true) {
        const ssize_t count = ::read(inputFd, &byte, 1);

        if (count == 1) {
            return true;
        }

        if (count == -1 && errno == EINTR) {

            // A process-backed shell may receive SIGHUP or SIGTERM
            // while waiting for user input.
            //
            // In that case, stop reading instead of retrying forever.
            if (SignalControl::isShutdownRequested()) {
                return false;
            }

            // Convert an idle-prompt interrupt notification into
            // the Ctrl+C byte expected by the line editor.
            if (SignalControl::consumeInputInterruptRequest()) {
                byte = '\x03';
                return true;
            }

            continue;
        }

        return false;
    }
}

// Read complete CSI/SS3 sequences:
// ESC [ D, ESC [ 3 ~, ESC O H, etc.
bool readEscapeSequence(int inputFd, std::string& sequence) {
    sequence.clear();

    char introducer = '\0';

    if (!readByte(inputFd, introducer)) {
        return false;
    }

    if (introducer == '[') {
        while (sequence.size() < 16) {
            char c = '\0';

            if (!readByte(inputFd, c)) {
                return false;
            }

            sequence += c;

            // CSI final bytes are in the range 0x40-0x7e.
            if (c >= '@' && c <= '~') {
                return true;
            }
        }

        return false;
    }

    if (introducer == 'O') {
        char finalByte = '\0';

        if (!readByte(inputFd, finalByte)) {
            return false;
        }

        sequence = "O";
        sequence += finalByte;
        return true;
    }

    return false;
}

struct CompletionMatch {
    std::string insertion;
    std::string display;
    std::string name;
};

// Find the beginning of the word immediately before the cursor.
std::size_t findWordStart(
    const std::string& text,
    std::size_t cursor
) {
    char quote = '\0';
    bool escaped = false;
    std::size_t wordStart = 0;

    cursor = std::min(cursor, text.size());

    for (std::size_t i = 0; i < cursor; ++i) {
        const char character = text[i];

        if (escaped) {
            escaped = false;
            continue;
        }

        if (character == '\\' && quote != '\'') {
            escaped = true;
            continue;
        }

        if (quote != '\0') {
            if (character == quote) {
                quote = '\0';
            }
            continue;
        }

        if (character == '\'' || character == '"') {
            quote = character;
        }
        else if (std::isspace(
                     static_cast<unsigned char>(character))) {
            wordStart = i + 1;
        }
    }

    return wordStart;
}

// Remove shell-style backslash escapes when looking up a path.
std::string unescapeCompletionPath(const std::string& value) {
    std::string result;
    bool escaped = false;

    for (char character : value) {
        if (escaped) {
            result += character;
            escaped = false;
        }
        else if (character == '\\') {
            escaped = true;
        }
        else {
            result += character;
        }
    }

    if (escaped) {
        result += '\\';
    }

    return result;
}

// Escape special characters when inserting a filename into the command.
std::string escapeCompletionName(
    const std::string& name,
    char quote
) {
    std::string result;

    for (char character : name) {

        if (quote == '\'') {
            if (character == '\'') {
                result += "'\\''";
            } else {
                result += character;
            }
        }
        else if (quote == '"') {
            if (character == '"' || character == '\\' ||
                character == '$' || character == '`') {
                result += '\\';
            }

            result += character;
        }
        else {
            if (std::isspace(static_cast<unsigned char>(character)) ||
                character == '\\' || character == '\'' ||
                character == '"' || character == '$' ||
                character == '`' || character == '&' ||
                character == ';' || character == '|' ||
                character == '(' || character == ')' ||
                character == '<' || character == '>' ||
                character == '*' || character == '?' ||
                character == '[' || character == ']' ||
                character == '{' || character == '}' ||
                character == '!') {
                result += '\\';
            }

            result += character;
        }
    }

    return result;
}

// Expand ~/ to the user's home directory.
std::string expandCompletionDirectory(std::string directory) {

    if (directory.empty()) {
        return ".";
    }

    if (directory == "~" ||
        directory.rfind("~/", 0) == 0) {

        const char* home = std::getenv("HOME");

        if (home != nullptr) {
            directory.replace(0, 1, home);
        }
    }

    return unescapeCompletionPath(directory);
}

// Find files and directories matching the currently typed word.
std::vector<CompletionMatch> findFileMatches(
    const std::string& typedWord
) {
    std::vector<CompletionMatch> matches;

    if (typedWord.empty()) {
        return matches;
    }

    char quote = '\0';
    std::size_t quoteLength = 0;

    if (typedWord.front() == '\'' ||
        typedWord.front() == '"') {

        quote = typedWord.front();
        quoteLength = 1;
    }

    const std::string pathWord =
        typedWord.substr(quoteLength);

    const std::size_t slash =
        pathWord.find_last_of('/');

    const std::string directoryPrefix =
        slash == std::string::npos
            ? ""
            : pathWord.substr(0, slash + 1);

    const std::string typedName =
        slash == std::string::npos
            ? pathWord
            : pathWord.substr(slash + 1);

    const std::string namePrefix =
        unescapeCompletionPath(typedName);

    std::string directoryToOpen =
        slash == std::string::npos
            ? "."
            : pathWord.substr(0, slash);

    if (directoryToOpen.empty() &&
        slash != std::string::npos) {
        directoryToOpen = "/";
    }

    directoryToOpen =
        expandCompletionDirectory(directoryToOpen);

    DIR* directory = opendir(directoryToOpen.c_str());

    if (directory == nullptr) {
        return matches;
    }

    const std::string insertionPrefix =
        typedWord.substr(0, quoteLength) + directoryPrefix;

    struct dirent* entry = nullptr;

    while ((entry = readdir(directory)) != nullptr) {

        const std::string name(entry->d_name);

        // Hidden files require the user to type a leading dot.
        if (name.empty() ||
            (name.front() == '.' &&
             (namePrefix.empty() || namePrefix.front() != '.'))) {
            continue;
        }

        if (name.compare(0, namePrefix.size(), namePrefix) != 0) {
            continue;
        }

        std::string fullPath = directoryToOpen;

        if (!fullPath.empty() && fullPath.back() != '/') {
            fullPath += '/';
        }

        fullPath += name;

        struct stat fileInfo{};

        const bool isDirectory =
            stat(fullPath.c_str(), &fileInfo) == 0 &&
            S_ISDIR(fileInfo.st_mode);

        std::string insertion =
            insertionPrefix + escapeCompletionName(name, quote);

        if (isDirectory) {
            insertion += '/';
        }

        matches.push_back({
            insertion,
            name + (isDirectory ? "/" : ""),
            name
        });
    }

    closedir(directory);

    std::sort(
        matches.begin(),
        matches.end(),
        [](const CompletionMatch& first,
           const CompletionMatch& second) {
            return first.name < second.name;
        }
    );

    return matches;
}

// Calculate the longest common prefix of all matching filenames.
std::string longestCommonPrefix(
    const std::vector<CompletionMatch>& matches
) {
    if (matches.empty()) {
        return "";
    }

    std::string common = matches.front().name;

    for (std::size_t i = 1; i < matches.size(); ++i) {

        std::size_t length = 0;

        while (length < common.size() &&
               length < matches[i].name.size() &&
               common[length] == matches[i].name[length]) {

            ++length;
        }

        common.resize(length);
    }

    return common;
}

// Read clipboard contents until the closing bracketed-paste marker.
bool readBracketedPaste(
    int inputFd,
    std::string& pastedText
) {
    const std::string endMarker = "\x1b[201~";

    pastedText.clear();

    while (true) {
        char character = '\0';

        if (!readByte(inputFd, character)) {
            return false;
        }

        pastedText += character;

        if (pastedText.size() >= endMarker.size() &&
            pastedText.compare(
                pastedText.size() - endMarker.size(),
                endMarker.size(),
                endMarker
            ) == 0) {

            pastedText.erase(
                pastedText.size() - endMarker.size()
            );

            return true;
        }
    }
}

// Convert Windows and classic Mac line endings to '\n'.
std::string normalizePastedText(
    const std::string& pastedText
) {
    std::string normalized;
    normalized.reserve(pastedText.size());

    for (std::size_t i = 0; i < pastedText.size(); ++i) {

        if (pastedText[i] == '\r') {

            if (i + 1 < pastedText.size() &&
                pastedText[i + 1] == '\n') {
                ++i;
            }

            normalized += '\n';
        }
        else {
            normalized += pastedText[i];
        }
    }

    // Preserve backslashes. The existing multiline scanner
    // determines their meaning according to the quote context.
    return normalized;
}

// Construct the primary prompt using the current directory.
std::string buildPrimaryPrompt() {

    char currentDirectory[4096];

    if (getcwd(
            currentDirectory,
            sizeof(currentDirectory)
        ) == nullptr) {

        return "user@terminator:? > ";
    }

    return "user@terminator:" +
        std::string(currentDirectory) +
        "> ";
}

} // namespace

namespace {

bool readCommandLineFrom(
    const History& history,
    std::string& line,
    int inputFd,
    std::ostream& output
) {

    // Discard any stale interrupt left by an earlier built-in,
    // such as MultiWatch.
    SignalControl::clearInputInterruptRequest();

    const bool hasTerminal = isatty(inputFd) != 0;

    termios originalSettings{};

    if (hasTerminal &&
        tcgetattr(inputFd, &originalSettings) == -1) {
        return false;
    }

    if (hasTerminal) {
        termios settings = originalSettings;

        settings.c_lflag &=
            static_cast<tcflag_t>(~(ICANON | ECHO));

        settings.c_cc[VMIN] = 1;
        settings.c_cc[VTIME] = 0;

        if (tcsetattr(inputFd, TCSANOW, &settings) == -1) {
            return false;
        }
    }

    const auto restoreTerminal = [&]() {
        return !hasTerminal ||
            tcsetattr(inputFd, TCSANOW, &originalSettings) != -1;
    };

    // History navigation state.
    std::vector<std::string> entries = history.getEntries();
    std::size_t historyPosition = entries.size();
    std::string draft;

    // physicalLine is the current line being edited.
    // logicalCommand accumulates all lines of a multiline command.
    std::string physicalLine;
    std::string logicalCommand;

    // Position where the next character will be inserted.
    std::size_t cursorPosition = 0;

    bool continuing = false;
    bool joinNextLine = false;
    bool historyRecall = false;

    // State for numbered filename-completion choices.
    bool awaitingCompletionChoice = false;

    std::vector<CompletionMatch> completionChoices;

    std::string completionChoiceInput;

    std::size_t completionWordStart = 0;
    std::size_t completionWordLength = 0;

    line.clear();

    const std::string primaryPrompt = buildPrimaryPrompt();
    const std::string continuationPrompt = "> ";

    std::string currentPrompt = primaryPrompt;

    // Redraw the entire editable line and return the visual cursor
    // to its logical position using backspace characters.
    const auto redrawLine = [&]() {
        output << '\r' << "\033[K"
                  << currentPrompt << physicalLine;

        for (std::size_t i = cursorPosition;
             i < physicalLine.size(); ++i) {
            output << '\b';
        }

        output << std::flush;
    };

    std::size_t renderedHistoryRows = 1;
    bool historyBlockDisplayed = false;

    // Redraw the recalled history entry without appending duplicates.
    const auto redrawHistoryCommand = [&]() {

        // Move to the first row of the previous recalled command.
        if (historyBlockDisplayed && renderedHistoryRows > 1) {

            output << "\033["
                << (renderedHistoryRows - 1)
                << "A";
        }

        // Erase the previous command block and rows below it.
        output << '\r' << "\033[0J";

        std::string completeCommand = logicalCommand;

        if (!completeCommand.empty()) {
            completeCommand += '\n';
        }

        completeCommand += physicalLine;

        // Calculate how many logical lines this command occupies.
        renderedHistoryRows = 1 +
            static_cast<std::size_t>(
                std::count(
                    completeCommand.begin(),
                    completeCommand.end(),
                    '\n'
                )
            );

        historyBlockDisplayed = true;

        std::size_t lineStart = 0;
        bool firstLine = true;

        while (true) {

            const std::size_t lineEnd =
                completeCommand.find('\n', lineStart);

            const std::string prompt = firstLine
                ? primaryPrompt
                : continuationPrompt;

            output << prompt;

            if (lineEnd == std::string::npos) {

                output << completeCommand.substr(lineStart);
                break;
            }

            output << completeCommand.substr(
                lineStart,
                lineEnd - lineStart
            ) << '\n';

            lineStart = lineEnd + 1;
            firstLine = false;
        }

        output << std::flush;
    };

    // Load one history entry into the multiline editor.
    const auto loadHistoryEntry = [&](const std::string& entry) {

        const std::size_t lastNewline =
            entry.rfind('\n');

        if (lastNewline == std::string::npos) {

            logicalCommand.clear();
            physicalLine = entry;

            currentPrompt = primaryPrompt;
            continuing = false;
        }
        else {

            logicalCommand = entry.substr(0, lastNewline);
            physicalLine = entry.substr(lastNewline + 1);

            currentPrompt = continuationPrompt;
            continuing = true;
        }

        cursorPosition = physicalLine.size();

        historyRecall = true;
        joinNextLine = false;

        redrawHistoryCommand();
    };

    // Insert clipboard contents as one logical command.
    // Pasted newlines must never behave like individual Enter presses.
    const auto insertPastedText = [&](const std::string& rawText) {

        const std::string pastedText =
            normalizePastedText(rawText);

        if (pastedText.empty()) {
            return;
        }

        const std::string beforeCursor =
            physicalLine.substr(0, cursorPosition);

        const std::string afterCursor =
            physicalLine.substr(cursorPosition);

        const std::size_t firstNewline =
            pastedText.find('\n');

        // Ordinary single-line paste.
        if (firstNewline == std::string::npos) {

            physicalLine =
                beforeCursor + pastedText + afterCursor;

            cursorPosition =
                beforeCursor.size() + pastedText.size();

            historyRecall = false;

            redrawLine();
            return;
        }

        // Complete the first currently edited physical line.
        const std::string firstLine =
            beforeCursor + pastedText.substr(0, firstNewline);

        output << '\r' << "\033[K"
            << currentPrompt << firstLine << '\n';

        if (!logicalCommand.empty() && !joinNextLine) {
            logicalCommand += '\n';
        }

        logicalCommand += firstLine;
        joinNextLine = false;

        std::size_t segmentStart = firstNewline + 1;

        // Process all remaining pasted lines.
        while (true) {

            const std::size_t nextNewline =
                pastedText.find('\n', segmentStart);

            if (nextNewline == std::string::npos) {

                const std::string finalSegment =
                    pastedText.substr(segmentStart);

                physicalLine = finalSegment + afterCursor;
                cursorPosition = finalSegment.size();

                break;
            }

            const std::string completeSegment =
                pastedText.substr(
                    segmentStart,
                    nextNewline - segmentStart
                );

            logicalCommand += '\n';
            logicalCommand += completeSegment;

            output << continuationPrompt
                << completeSegment << '\n';

            segmentStart = nextNewline + 1;
        }

        continuing = true;
        historyRecall = false;
        currentPrompt = continuationPrompt;

        // Display the final editable line and restore the cursor.
        output << currentPrompt << physicalLine;

        for (std::size_t i = cursorPosition;
            i < physicalLine.size();
            ++i) {
            output << '\b';
        }

        output << std::flush;
    };

    // Apply a selected filename and preserve the remaining command text.
    const auto applyCompletion = [&](const std::string& replacement) {

        physicalLine.replace(
            completionWordStart,
            completionWordLength,
            replacement
        );

        cursorPosition =
            completionWordStart + replacement.size();

        historyRecall = false;

        redrawLine();
    };


    // Complete the word immediately before the cursor.
    const auto completeFileName = [&]() {

        const std::size_t wordStart =
            findWordStart(physicalLine, cursorPosition);

        const std::string typedWord = physicalLine.substr(
            wordStart,
            cursorPosition - wordStart
        );

        std::vector<CompletionMatch> matches =
            findFileMatches(typedWord);

        // No matching files: leave everything unchanged.
        if (matches.empty()) {
            return;
        }

        completionWordStart = wordStart;
        completionWordLength = cursorPosition - wordStart;

        // One match: insert it immediately.
        if (matches.size() == 1) {

            applyCompletion(matches.front().insertion);
            return;
        }

        char quote = '\0';
        std::size_t quoteLength = 0;

        if (!typedWord.empty() &&
            (typedWord.front() == '\'' ||
            typedWord.front() == '"')) {

            quote = typedWord.front();
            quoteLength = 1;
        }

        const std::string pathWord =
            typedWord.substr(quoteLength);

        const std::size_t slash =
            pathWord.find_last_of('/');

        const std::string typedName =
            slash == std::string::npos
                ? pathWord
                : pathWord.substr(slash + 1);

        const std::string namePrefix =
            unescapeCompletionPath(typedName);

        const std::string directoryPrefix =
            slash == std::string::npos
                ? ""
                : pathWord.substr(0, slash + 1);

        const std::string commonName =
            longestCommonPrefix(matches);

        // First extend the input to the longest common prefix.
        if (commonName.size() > namePrefix.size()) {

            const std::string commonReplacement =
                typedWord.substr(0, quoteLength) +
                directoryPrefix +
                escapeCompletionName(commonName, quote);

            physicalLine.replace(
                wordStart,
                cursorPosition - wordStart,
                commonReplacement
            );

            cursorPosition =
                wordStart + commonReplacement.size();

            completionWordLength =
                cursorPosition - wordStart;

            historyRecall = false;

            redrawLine();
        }

        // Several choices remain. Display the numbered list.
        completionChoices = matches;
        completionChoiceInput.clear();

        awaitingCompletionChoice = true;

        output << '\n';

        for (std::size_t i = 0;
            i < completionChoices.size();
            ++i) {

            output << (i + 1) << ". "
                << completionChoices[i].display;

            if (i + 1 < completionChoices.size()) {
                output << "    ";
            }
        }

        output << "\nChoose a number";

        if (completionChoices.size() <= 9) {
            output << " (press 1-"
                << completionChoices.size() << ")";
        }
        else {
            output << " and press Enter";
        }

        output << ": " << std::flush;
    };

    output << currentPrompt << std::flush;

    while (true) {
        char character = '\0';

        if (!readByte(inputFd, character)) {
            restoreTerminal();
            return false;
        }

        // Handle numbered filename-completion choices.
        if (awaitingCompletionChoice) {

            const auto dismissChoices = [&]() {
                awaitingCompletionChoice = false;
                completionChoices.clear();
                completionChoiceInput.clear();
            };

            // Ctrl+C: cancel the current command entirely.
            if (character == '\x03') {

                dismissChoices();

                physicalLine.clear();
                logicalCommand.clear();

                cursorPosition = 0;

                continuing = false;
                joinNextLine = false;
                historyRecall = false;

                draft.clear();
                historyPosition = entries.size();

                currentPrompt = primaryPrompt;

                output << "^C\n"
                    << currentPrompt
                    << std::flush;

                continue;
            }

            // Numeric selection.
            if (character >= '0' && character <= '9') {

                // Nine or fewer choices: one digit selects immediately.
                if (completionChoices.size() <= 9) {

                    const std::size_t choice =
                        static_cast<std::size_t>(character - '1');

                    if (character != '0' &&
                        choice < completionChoices.size()) {

                        const std::string replacement =
                            completionChoices[choice].insertion;

                        dismissChoices();

                        output << '\n';

                        applyCompletion(replacement);

                    } else {

                        // Invalid number: dismiss the choices but retain
                        // the command currently being edited.
                        dismissChoices();

                        output << '\n';

                        redrawLine();
                    }

                } else {

                    // More than nine choices: collect a multi-digit number.
                    completionChoiceInput += character;

                    output << character << std::flush;
                }

                continue;
            }

            // Enter confirms a multi-digit choice.
            if (character == '\r' || character == '\n') {

                std::size_t choice = 0;

                try {
                    if (!completionChoiceInput.empty()) {
                        choice = static_cast<std::size_t>(
                            std::stoul(completionChoiceInput)
                        );
                    }
                }
                catch (...) {
                    choice = 0;
                }

                const bool validChoice =
                    choice > 0 && choice <= completionChoices.size();

                const std::string replacement = validChoice
                    ? completionChoices[choice - 1].insertion
                    : "";

                dismissChoices();

                output << '\n';

                if (validChoice) {
                    applyCompletion(replacement);
                } else {
                    redrawLine();
                }

                continue;
            }

            // Backspace while entering a multi-digit choice.
            if (character == 127 || character == '\b') {

                if (!completionChoiceInput.empty()) {
                    completionChoiceInput.pop_back();
                    output << "\b \b" << std::flush;
                }

                continue;
            }

            // Escape cancels the choices without deleting the command.
            if (character == '\x1b') {

                dismissChoices();

                output << '\n';

                redrawLine();

                continue;
            }

            // Any unexpected input cancels the selection menu.
            dismissChoices();

            output << '\n';

            redrawLine();

            continue;
        }

        // Ctrl+C at the normal prompt or multiline continuation.
        if (character == '\x03') {

            physicalLine.clear();
            logicalCommand.clear();

            cursorPosition = 0;

            continuing = false;
            joinNextLine = false;
            historyRecall = false;

            draft.clear();
            historyPosition = entries.size();

            currentPrompt = primaryPrompt;

            output << "^C\n"
                << currentPrompt
                << std::flush;

            continue;
        }

        // Tab completes the filename or directory before the cursor.
        if (character == '\t') {
            completeFileName();
            continue;
        }

        // Enter submits the current physical line to the multiline scanner.
        if (character == '\r' || character == '\n') {
            // Display the complete line before submitting it, even when
            // the cursor is currently somewhere in the middle.
            output << '\r' << "\033[K"
                      << currentPrompt << physicalLine
                      << '\n' << std::flush;

            if (!logicalCommand.empty() && !joinNextLine) {
                logicalCommand += '\n';
            }

            logicalCommand += physicalLine;

            joinNextLine = false;
            physicalLine.clear();
            cursorPosition = 0;

            const InputStatus status = analyzeInput(logicalCommand);

            if (status == InputStatus::BackslashContinuation) {
                // Remove the trailing backslash. The next physical line
                // will be joined directly to this one.
                logicalCommand.pop_back();

                joinNextLine = true;
                continuing = true;
                currentPrompt = continuationPrompt;

                output << currentPrompt << std::flush;
                continue;
            }

            if (status == InputStatus::Incomplete) {
                continuing = true;
                currentPrompt = continuationPrompt;

                output << currentPrompt << std::flush;
                continue;
            }

            line = logicalCommand;
            break;
        }

        // Ctrl+D exits the shell when the current physical line is empty.
        if (character == 4 && physicalLine.empty()) {
            restoreTerminal();
            return false;
        }

        // Backspace: delete the character immediately before the cursor.
        if (character == 127 || character == '\b') {
            if (cursorPosition > 0) {
                physicalLine.erase(cursorPosition - 1, 1);
                --cursorPosition;
                redrawLine();
            }

            continue;
        }

        // Ctrl+A: move to the beginning of the line.
        if (character == '\x01') {
            cursorPosition = 0;
            redrawLine();
            continue;
        }

        // Ctrl+E: move to the end of the line.
        if (character == '\x05') {
            cursorPosition = physicalLine.size();
            redrawLine();
            continue;
        }

        // Ctrl+U: delete from the beginning to the cursor.
        if (character == '\x15') {
            physicalLine.erase(0, cursorPosition);
            cursorPosition = 0;
            redrawLine();
            continue;
        }

        // Ctrl+K: delete from the cursor to the end.
        if (character == '\x0b') {
            physicalLine.erase(cursorPosition);
            redrawLine();
            continue;
        }

        // Ctrl+W: delete the previous word and its preceding whitespace.
        if (character == '\x17') {
            std::size_t wordStart = cursorPosition;

            while (wordStart > 0 &&
                   std::isspace(static_cast<unsigned char>(
                       physicalLine[wordStart - 1]))) {
                --wordStart;
            }

            while (wordStart > 0 &&
                   !std::isspace(static_cast<unsigned char>(
                       physicalLine[wordStart - 1]))) {
                --wordStart;
            }

            physicalLine.erase(
                wordStart, cursorPosition - wordStart
            );

            cursorPosition = wordStart;
            redrawLine();
            continue;
        }

        // Arrow, Home, End and Delete keys arrive as escape sequences.
        if (character == '\x1b') {
            std::string sequence;

            if (!readEscapeSequence(inputFd, sequence)) {
                continue;
            }

            // Start of a bracketed clipboard paste.
            if (sequence == "200~") {

                std::string pastedText;

                if (readBracketedPaste(inputFd, pastedText)) {
                    insertPastedText(pastedText);
                }

                continue;
            }

            if (sequence == "D") {
                // Left arrow.
                if (cursorPosition > 0) {
                    --cursorPosition;
                    redrawLine();
                }
            } else if (sequence == "C") {
                // Right arrow.
                if (cursorPosition < physicalLine.size()) {
                    ++cursorPosition;
                    redrawLine();
                }
            } else if (sequence == "H" || sequence == "1~" ||
                       sequence == "7~" || sequence == "OH") {
                // Home.
                cursorPosition = 0;
                redrawLine();
            } else if (sequence == "F" || sequence == "4~" ||
                       sequence == "8~" || sequence == "OF") {
                // End.
                cursorPosition = physicalLine.size();
                redrawLine();
            } else if (sequence == "3~") {
                // Delete: remove the character at the cursor.
                if (cursorPosition < physicalLine.size()) {
                    physicalLine.erase(cursorPosition, 1);
                    redrawLine();
                }
            } else if ((!continuing || historyRecall) && sequence == "A") {
                // Up arrow: browse older history entries.
                if (historyPosition == entries.size()) {
                    draft = physicalLine;

                    // Refresh history so entries from other tabs appear.
                    entries = history.getEntries();
                    historyPosition = entries.size();
                }

                if (historyPosition > 0) {
                    --historyPosition;
                    loadHistoryEntry(entries[historyPosition]);
                }
            } else if ((!continuing || historyRecall) && sequence == "B") {
                // Down arrow: browse newer history entries.
                if (historyPosition < entries.size()) {

                    ++historyPosition;

                    if (historyPosition == entries.size()) {

                        // Restore the command that was being typed before history browsing.
                        logicalCommand.clear();

                        physicalLine = draft;
                        cursorPosition = physicalLine.size();

                        continuing = false;
                        historyRecall = false;
                        joinNextLine = false;

                        currentPrompt = primaryPrompt;

                        redrawHistoryCommand();;
                    }
                    else {
                        loadHistoryEntry(entries[historyPosition]);
                    }
                }
            }

            continue;
        }

        if (character == '\x0c') {
            // Clear the screen and redraw the current editing line.
            output << "\033[2J\033[H" << std::flush;
            redrawLine();
            continue;
        }

        // Insert printable ASCII characters at the current cursor position.
        if (character >= 32 && character <= 126) {
            physicalLine.insert(cursorPosition, 1, character);
            ++cursorPosition;
            redrawLine();
        }
        historyRecall = false;

    }

    if (!restoreTerminal()) {
        return false;
    }

    return true;
}
}


bool LineEditor::readCommandLine(
    const History& history,
    std::string& line
) {
    return readCommandLineFrom(
        history,
        line,
        STDIN_FILENO,
        std::cout
    );
}

bool LineEditor::readCommandLine(
    const History& history,
    std::string& line,
    ShellIO& io
) {
    return readCommandLineFrom(
        history,
        line,
        io.getInputFd(),
        io.out()
    );
}