#ifndef PIPELINE_EXECUTOR_H
#define PIPELINE_EXECUTOR_H

#include "shell/Redirection.h"

#include <string>
#include <vector>

class ShellIO;

class PipelineExecutor {
public:
    int executePipeline(
        const std::vector<ParsedCommand>& commands,
        ShellIO* io = nullptr
    ) const;
};

#endif