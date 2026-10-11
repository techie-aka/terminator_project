#ifndef COMMANDPARSER_H
#define COMMANDPARSER_H

#include <string>
#include <vector>

class CommandParser {
public:
    std::vector<std::string> parse(
        const std::string& input,
        int lastExitStatus
    ) const;
};

#endif