#ifndef PIPELINE_EXECUTOR_H
#define PIPELINE_EXECUTOR_H

#include "shell/Redirection.h"

#include <string>
#include <vector>

// Executes foreground pipelines made up of external commands.
// Each inner vector contains one command followed by its arguments.
class PipelineExecutor {
public:
    int executePipeline(
        const std::vector<ParsedCommand>& commands
    ) const;
};

#endif // PIPELINE_EXECUTOR_H