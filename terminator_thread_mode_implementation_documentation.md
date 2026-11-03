# Terminator: Thread-Mode and Process-Mode Implementation Documentation

**Project:** Terminator — X11 terminal emulator with a custom C++ shell  
**Documentation date:** 1 November 2026  
**Scope:** Execution-mode selection, tab/session architecture, thread-mode I/O, helper processes, command substitution, MultiWatch, signals/job control, build procedure, and regression testing.

---

## 1. Executive summary

### 1.1 Confirmed default mode

**Yes. The default execution mode is thread mode.** The uploaded project source contains the following mode-selection logic in `src/main.cpp`:

```cpp
// Thread mode is the default.
bool useProcess = false;

if (argc == 2 &&
    std::string(argv[1]) == "useProcess=True") {
    useProcess = true;
} else if (argc != 1) {
    std::cerr << "Usage: " << argv[0]
              << " [useProcess=True]\n";
    return 2;
}
```

When a tab is created, the session factory selects the implementation:

```cpp
std::unique_ptr<TabSession> session;

if (useProcess) {
    session = std::make_unique<PtySession>();
} else {
    session = std::make_unique<ThreadSession>();
}
```

The build script forwards its command-line arguments to the GUI executable:

```bash
exec ./build/terminator "$@"
```

Therefore, the launch commands are:

| Command | Selected mode |
|---|---|
| `bash build.sh` | **Thread mode (default)** |
| `bash build.sh useProcess=True` | Process mode |
| `./build/terminator` | Thread mode |
| `./build/terminator useProcess=True` | Process mode |

The process-mode flag is case-sensitive and the accepted value is exactly `useProcess=True`.

**Decision:** the project can proceed with thread mode as its default and retain process mode as an explicit compatibility/testing option. The supplied source proves the default selector. A fresh ZIP of the user's final working tree was not supplied after the last manual edits, so this confirmation is based on the uploaded project and the subsequent change instructions; none of those later instructions changes the mode selector.

### 1.2 What the two modes mean

- **Thread mode:** one Terminator GUI process. The GUI/event loop runs on the main thread. Each tab owns a `ThreadSession`, which runs that tab's `ShellEngine` on a separate C++ thread. Pipes carry input and output between the GUI and the shell thread.
- **Process mode:** the GUI creates a `PtySession` for each tab. Each session starts a separate `terminator-shell` process attached to a pseudo-terminal (PTY).
- In **both** modes, ordinary external commands such as `ls`, `grep`, and `sleep` are operating-system processes. “Thread mode” means the shell interpreter for a tab is a thread, not that every command runs as a thread.
- `MultiWatch` remains process-based. In thread mode it is run inside a separate helper shell process, and its monitored workers remain processes.

### 1.3 Current completion status

The following integration work has been completed according to the supplied code and the successful tests reported after applying it:

- Thread mode is the default; process mode remains selectable.
- Thread-backed tabs communicate with the GUI using pipes and `ShellIO`.
- Command substitution runs its inner command in a separate helper shell and captures the helper's stdout.
- Thread-mode `multiWatch` runs in a separate helper shell, so its output is routed back into the correct tab rather than the launching VS Code terminal.
- MultiWatch worker processes are stopped when the monitoring command is interrupted through the supported path.
- Both `terminator-shell` and the GUI build successfully with the project's configured compiler flags.

This documentation includes a regression matrix so the same functionality can be rechecked after future changes.

---

## 2. Architecture overview

### 2.1 Two execution modes share one session interface

The GUI should not need to know whether a tab is implemented by a thread or a process. It communicates through the abstract `TabSession` interface.

```text
                         TERMINATOR GUI PROCESS
┌─────────────────────────────────────────────────────────────────┐
│ src/main.cpp                                                     │
│                                                                 │
│  X11 event loop                                                  │
│  - keyboard and mouse input                                      │
│  - drawing and tab switching                                     │
│  - polling session output descriptors                            │
│                          │                                      │
│                          ▼                                      │
│                    Terminal (per tab)                            │
│             TerminalBuffer + one TabSession                     │
│                          │                                      │
│               ┌──────────┴──────────┐                            │
│               │                     │                            │
│               ▼                     ▼                            │
│         ThreadSession           PtySession                        │
│         (default)             (optional)                          │
│               │                     │                            │
│       input/output pipes          PTY                            │
│               │                     │                            │
│          ShellEngine             exec()                           │
│          on C++ thread             │                             │
└───────────────┼─────────────────────┼─────────────────────────────┘
                │                     │
                ▼                     ▼
        ShellIO for tab          terminator-shell
                                     process
```

`Terminal` owns the session through a `std::unique_ptr<TabSession>`. The interface allows the GUI to write input, read output, obtain the descriptor to poll, request interruption/suspension, and stop the session without depending on the concrete session type.

### 2.2 Thread-mode data flow

Each `ThreadSession` creates two one-way pipes:

```text
GUI input writer  ──────>  shell thread input reader
GUI output reader <──────  shell thread output writer
```

The shell thread constructs `ShellIO` using these descriptors. The tab's standard output and standard error are both routed to its GUI output pipe. The GUI reads the pipe without blocking its X11 event loop and appends received bytes to the corresponding tab's terminal buffer.

Important implementation points:

1. The output read descriptor is made non-blocking so polling it cannot freeze the GUI.
2. The shell thread owns and closes its worker-side descriptors when it exits.
3. The GUI closes its input writer to signal EOF when a tab is stopped.
4. `ThreadSession::stop()` continues draining output while waiting for the shell thread. This prevents the shell thread from getting stuck because the output pipe is full while the GUI is waiting in `join()`.
5. `foregroundProcessGroup`, `foregroundHelper`, and `shellFinished` are atomic values because they are shared between the GUI thread and the shell thread.
6. Only the GUI thread should call Xlib. The shell thread must communicate through bytes and session state, never draw directly.

### 2.3 Process-mode data flow

Process mode uses a PTY rather than ordinary pipes:

```text
GUI  <────────────── PTY master/slave ──────────────> terminator-shell
```

The `PtySession` starts a separate shell process, attaches the child to the PTY, and provides the GUI with the PTY master descriptor. A PTY behaves more like a real terminal and supports programs that inspect terminal state or use terminal control operations.

Process mode is retained for compatibility and as a comparison path while thread mode is the default.

### 2.4 Shell command execution flow

The common shell execution path is conceptually:

```text
Input line
   │
   ▼
LineEditor
   │
   ▼
ShellEngine
   ├── recognize special multiWatch syntax
   ├── expand command substitution ($(...) / backticks)
   ├── detect background marker (&)
   ├── split pipeline stages (|)
   ├── parse arguments and redirections
   ├── dispatch shell built-ins
   ├── execute external command(s)
   └── update exit status and job state
```

Roles:

- `LineEditor`: reads an interactive command line and manages input editing/history interaction.
- `CommandSubstitution`: finds and expands `$(...)` and supported backtick substitutions.
- `CommandParser`: converts shell text into arguments and redirection metadata; it does not itself execute external programs.
- `Builtins`: implements shell commands that must run in the shell context, such as `cd`, `jobs`, `fg`, and `bg`.
- `CommandExecutor`: launches one external command.
- `PipelineExecutor`: launches multiple commands connected by pipes.
- `JobManager`: tracks jobs and their process groups.
- `MultiWatch`: parses and runs the repeated monitoring feature.
- `ShellIO`: describes the input, output, and error channels for a particular tab.
- `ShellHelper`: starts a separate `terminator-shell` process for functionality that must not execute complex C++ shell code in a forked child of the multithreaded GUI process.

---

## 3. Mode selector and build behavior

### 3.1 Source files responsible for mode selection

| File | Responsibility |
|---|---|
| `src/main.cpp` | Parses the optional `useProcess=True` argument and chooses the session class when creating tabs. |
| `build.sh` | Builds the shell executable and GUI executable, then forwards all user-supplied arguments to the GUI. |
| `include/terminal/TabSession.h` | Defines the common interface implemented by both session types. |
| `include/terminal/ThreadSession.h` / `src/terminal/ThreadSession.cpp` | Implements the thread-backed tab session. |
| `include/process/PtySession.h` / `src/process/PtySession.cpp` | Implements the process-backed PTY session. |
| `include/terminal/Terminal.h` / `src/terminal/Terminal.cpp` | Owns a `TabSession` and exposes it to the GUI. |

### 3.2 Build script behavior

The build script compiles two binaries:

1. `build/terminator-shell` from all `.cpp` files under `src/shell`.
2. `build/terminator` from all project `.cpp` files except `src/shell/main.cpp`, linking X11, util, pthread, and rt.

The relevant structure is:

```bash
#!/bin/bash
set -e

cd "$(dirname "$0")"
mkdir -p build

# Build the standalone shell/helper executable.
g++ -std=c++17 -Wall -Wextra \
    -Iinclude \
    $(find src/shell -name '*.cpp' -print) \
    -o build/terminator-shell \
    -pthread -lrt

# Build the GUI and the shell components used by thread mode.
g++ -std=c++17 -Wall -Wextra \
    -Iinclude \
    $(find src -name '*.cpp' ! -path 'src/shell/main.cpp' -print) \
    -o build/terminator \
    -lX11 -lutil -pthread -lrt

exec ./build/terminator "$@"
```

The helper process resolves `terminator-shell` relative to the running executable, so keep `build/terminator` and `build/terminator-shell` together in the same `build/` directory.

### 3.3 Recommended commands

From the project root:

```bash
# Default: thread mode
bash build.sh

# Optional: process mode
bash build.sh useProcess=True
```

For a compile-only check (do not launch the X11 window):

```bash
g++ -std=c++17 -Wall -Wextra -Iinclude \
    $(find src/shell -name '*.cpp' -print) \
    -o build/terminator-shell -pthread -lrt

g++ -std=c++17 -Wall -Wextra -Iinclude \
    $(find src -name '*.cpp' ! -path 'src/shell/main.cpp' -print) \
    -o build/terminator -lX11 -lutil -pthread -lrt
```

If the script is not executable, either keep using `bash build.sh` or set its permission once:

```bash
chmod +x build.sh
./build.sh
```

---

## 4. Thread-mode implementation details

### 4.1 `TabSession` interface

The session abstraction provides a common interface for the GUI:

- `start()` — initialize the session.
- `writeInput()` — forward user input to the shell.
- `readOutput()` — retrieve available shell output.
- `getFileDescriptor()` — expose the descriptor monitored by the GUI event loop.
- `getPid()` — return the backing shell/application PID where meaningful.
- `sendInterrupt()` — request interruption of the current foreground operation.
- `sendSuspend()` — request suspension of the current foreground operation.
- `stop()` — shut down the session and release resources.

A thread does not have its own process ID. Therefore, a thread-backed session cannot return a unique shell PID. All thread-backed tabs belong to the same GUI process. The GUI should not interpret that PID as a unique process identifier for each tab.

### 4.2 `ThreadSession` state

The implementation stores:

| Member | Meaning |
|---|---|
| `inputWriteFd` | GUI-side writer for the GUI-to-shell input pipe. |
| `outputReadFd` | GUI-side reader for the shell-to-GUI output pipe. |
| `shellThread` | C++ thread running `ShellEngine::run(ShellIO&)`. |
| `foregroundProcessGroup` | Current foreground external process group for that tab. |
| `foregroundHelper` | PID of a helper shell currently handling a special operation. |
| `shellFinished` | Atomic indication that the shell thread has finished. |
| `started` | Tracks whether the session successfully started. |

The atomics are important because the GUI thread can send a signal or stop a tab while the shell thread is executing a command.

### 4.3 Why ordinary commands are still processes

In thread mode, the shell parser/executor code runs in the tab's C++ thread. When the user enters an external command, `CommandExecutor` or `PipelineExecutor` launches the external executable as a child process. The shell thread waits for or tracks those child processes.

For example:

```text
GUI process
  ├── GUI/X11 thread
  ├── tab 1 shell thread ─── ls process
  ├── tab 2 shell thread ─── grep process
  └── tab 3 shell thread ─── sleep process
```

This is still thread-based shell execution even though external programs are separate processes.

### 4.4 `ShellIO` is the tab's I/O boundary

`ShellIO` contains the descriptors/streams used by a shell instance. In thread mode, writing to global `std::cout` or `std::cerr` is not equivalent to writing to the active tab's `ShellIO` output. Such output can go to the terminal that launched Terminator, such as the VS Code integrated terminal.

For any code that runs as part of a tab shell, use the active `ShellIO` streams or descriptors. Only standalone process-mode shell code should use the process's standard streams directly.

### 4.5 Resource shutdown

Closing a thread-backed tab must:

1. Close the GUI-side input writer so the shell's input read can observe EOF.
2. Stop a foreground process group if one is still active.
3. Stop a foreground helper if one owns the current operation.
4. Continue draining output while the shell thread exits.
5. Join the shell thread.
6. Close remaining GUI-side descriptors and clear session state.

Do not remove the output-draining behavior casually. A full pipe can block the shell thread; joining it without draining the pipe can deadlock tab shutdown.

---

## 5. Command substitution implementation

### 5.1 Problem in the previous implementation

The original capture implementation created a pipe, called `fork()`, redirected the child's `STDOUT_FILENO`, and then called the shell's C++ callback in the child.

There were two separate problems in thread mode:

1. The GUI process is multithreaded. Forking and then running complex C++ shell code in the child of a multithreaded process is unsafe because locks held by other threads may remain locked in the child.
2. `ShellIO` writes to its own descriptors. Redirecting only `STDOUT_FILENO` does not redirect output written through `ShellIO::out()`.

That is why changing only `dup2()` was not enough.

### 5.2 New helper-process design

The revised path is:

```text
Tab shell thread
   │
   ├── receives: echo result=$(printf captured)
   │
   ├── CommandSubstitution identifies $(...)
   │
   ├── ShellHelper creates a capture pipe
   │
   ├── posix_spawn starts build/terminator-shell -c "printf captured"
   │        ├── stdin  -> /dev/null
   │        ├── stdout -> capture pipe
   │        └── stderr -> this tab's error descriptor
   │
   ├── parent reads capture pipe until EOF
   │
   ├── trailing newline characters are removed
   │
   └── captured text is escaped/protected and inserted into the command
```

`posix_spawn()` starts a new helper process and applies file actions before the helper executes. The multithreaded GUI does not fork and run the C++ shell callback itself.

### 5.3 Files involved

| File | Implementation responsibility |
|---|---|
| `include/shell/CommandSubstitution.h` | Public interface for expanding substitution syntax. |
| `src/shell/CommandSubstitution.cpp` | Scans for matching `$()` and backtick delimiters, handles quotes/escapes/nesting, captures output, strips trailing newlines, and protects substituted special characters. |
| `include/shell/ShellHelper.h` | Declares helper launch and output-capture operations. |
| `src/shell/ShellHelper.cpp` | Locates the sibling shell executable, configures `posix_spawn` file actions and signal attributes, launches the helper, reads captured output, and waits for it. |
| `include/shell/ShellEngine.h` / `src/shell/ShellEngine.cpp` | Calls the substitution layer during command execution and provides a one-command execution entry point for helpers. |
| `src/shell/main.cpp` | Implements the internal `-c command` helper mode. |

### 5.4 Important implementation details

- `ShellHelper` resolves the helper executable using `/proc/self/exe` and expects `terminator-shell` beside the running GUI/shell executable.
- The helper receives `/dev/null` as stdin. This prevents a command inside `$(...)` from consuming keystrokes intended for the interactive tab shell. Interactive stdin from inside command substitution is therefore not supported.
- For command substitution, helper stdout is captured in a dedicated pipe. Helper stderr remains connected to the current tab's error descriptor.
- The parent reads the output pipe while the helper runs. This is important: if the parent waited first while the helper filled the pipe, both sides could block.
- Trailing newline characters are removed from captured output before insertion.
- The scanner supports nested `$()` and backtick substitution, subject to the shell's existing tokenizer/parser behavior.
- A non-zero status from an inner command does not make output capture itself fail. The command's stdout can still be substituted.
- In thread mode, the helper PID is published as the foreground helper so GUI signal handling can interrupt the correct operation.

### 5.5 Manual smoke tests

```bash
# Simple substitution
echo result=$(printf captured)

# Nested substitution
echo nested=$(printf '%s' "$(printf inner)")

# Pipeline inside substitution
echo upper=$(printf hello | tr a-z A-Z)

# Whitespace in quoted result
echo "value=$(printf 'two words')"

# Backtick form (if retained in the user's current version)
echo value=`printf legacy`
```

Expected visible results include `result=captured`, `nested=inner`, `upper=HELLO`, and `value=two words`.

---

## 6. MultiWatch implementation

### 6.1 What MultiWatch does

`MultiWatch` accepts a list of quoted commands, for example:

```bash
multiWatch ["date", "uptime", "ps -ef | head"]
```

The implementation starts one worker process for each monitored command. Each worker repeatedly runs its command, captures the command's output, emits a framed run-start/output/run-end stream, waits for the configured restart delay, and starts the next run.

The constants in the implementation include:

- `MAX_FRAME_DATA = 4096`
- `RESTART_DELAY_SECONDS = 2`
- `TERMINATION_GRACE_MS = 300`

A completed run is displayed with its command, timestamp, and output between separator lines.

### 6.2 Why MultiWatch stays process-based

MultiWatch uses `fork()`, process groups, pipes, `select()`, signal handling, and `waitpid()` as part of its design. It does not need to be converted into threads to work with the thread-based shell.

The correct boundary is:

```text
Thread-mode tab shell
       │
       └── ShellHelper::runCommand(...)
                 │
                 └── separate terminator-shell -c "multiWatch [...]"
                            │
                            ├── MultiWatch worker process group A
                            ├── MultiWatch worker process group B
                            └── MultiWatch worker process group C
```

The helper is a separate process. Its stdout and stderr are connected to the active tab's output/error descriptors. Inside that helper, MultiWatch can use its existing process-oriented implementation without running its fork-based worker logic inside the multithreaded GUI process.

**`include/shell/MultiWatch.h` and `src/shell/MultiWatch.cpp` are retained.** The fix is in the dispatch/routing layer (`ShellEngine` and `ShellHelper`), not a conversion of MultiWatch's workers.

### 6.3 Output routing

Before the helper routing fix, MultiWatch printed completed output through global `std::cout` and errors through global `std::cerr`. In thread mode those streams belong to the GUI process and can point at the terminal used to launch the GUI.

The helper launch sets:

- helper stdin to `/dev/null`;
- helper stdout to the active tab's output descriptor;
- helper stderr to the active tab's error descriptor.

Thus the MultiWatch output appears inside the correct Terminator tab.

### 6.4 Interruption and cleanup

The helper PID is recorded as the tab's foreground helper. When the GUI requests Ctrl+C, the thread session sends the helper the control signal expected by the helper shell. The helper's signal-control layer forwards the request to the MultiWatch operation, whose own signal handler sets its interruption flag.

MultiWatch then:

1. Stops accepting another monitoring iteration.
2. Sends termination signals to each worker process group.
3. Waits briefly for graceful termination.
4. Sends `SIGKILL` to any remaining worker group.
5. Reaps worker processes with `waitpid()`.
6. Restores the prior signal handler and returns to the helper shell.

The tab shell can then continue accepting commands.

### 6.5 MultiWatch regression commands

```bash
multiWatch ["printf first", "printf second"]
```

Check that both outputs appear in the active tab and repeat at the expected interval. Then press Ctrl+C and check that the prompt returns.

Also test a pipeline command:

```bash
multiWatch ["printf hello | tr a-z A-Z"]
```

Test closing a tab while MultiWatch is running. No worker should remain running after the tab has closed.

---

## 7. Job control, foreground signals, and tab cleanup

### 7.1 Foreground command tracking

Thread mode cannot use the same terminal foreground-process-group mechanism as a traditional PTY shell. There is no dedicated PTY slave for each thread-backed tab. Instead, the shell records the active process group in session state and the GUI sends signals to the appropriate group.

The thread session keeps:

- `foregroundProcessGroup`: the active external command process group.
- `foregroundHelper`: a helper shell PID while command substitution or MultiWatch is active.

These are atomic because the shell and GUI execute on different threads.

### 7.2 `fg` and job tracking

`JobManager` tracks running/stopped job process IDs and process groups. The built-in dispatch path must pass the current tab's `ShellIO`/session context where necessary so a foreground job can update the correct tab's foreground process-group state rather than using only process-global state.

`Builtins.cpp` is part of this integration because it dispatches `fg`/`bg` operations. `JobManager.h` and `JobManager.cpp` contain the job state and waiting/cleanup behavior.

### 7.3 Shutdown expectations

At minimum, test these situations:

- Close a tab while a foreground `sleep` is running.
- Close a tab while `multiWatch` is running.
- Close a tab while a background job is present.
- Switch between tabs while one tab has a running command.
- Interrupt a helper and then issue a new command.

The intended result is that work belonging to a closed tab is stopped/reaped and that commands in other tabs continue unaffected.

### 7.4 Limitations to keep visible

This integration does not claim complete POSIX job-control parity with an interactive Bash session. In particular, process-group behavior across multi-stage pipelines, terminal foreground control, and every Ctrl+Z/`fg` edge case should be covered by the regression matrix before claiming full job-control support.

---

## 8. File inventory: what each file is responsible for

This table combines the files that implement the two execution modes with the files involved in the helper integration. The `Status` column distinguishes newly added helper files, feature-fix files, and supporting architecture files already present in the uploaded project.

| File | Status in the work described here | Responsibility |
|---|---|---|
| `build.sh` | Supporting build file; inspect if build setup changes | Builds `terminator-shell` and `terminator`, forwards mode argument. |
| `src/main.cpp` | Mode-selection integration and tab/session GUI integration | Defaults to thread mode; selects `ThreadSession` or `PtySession`; GUI event loop and tab lifecycle. |
| `include/terminal/TabSession.h` | Supporting architecture | Abstract interface shared by both session types. |
| `include/terminal/ThreadSession.h` | Supporting thread-mode architecture | Thread session state and method declarations. |
| `src/terminal/ThreadSession.cpp` | Thread-mode implementation; also touched by helper/job cleanup integration | Creates pipes and shell thread; implements input, output, signaling, and shutdown. |
| `include/process/PtySession.h` | Supporting process-mode architecture | PTY session declaration. |
| `src/process/PtySession.cpp` | Supporting process-mode implementation | Starts and controls the independent shell process in PTY mode. |
| `include/terminal/Terminal.h` | Supporting architecture | Owns the `TabSession` and terminal buffer. |
| `src/terminal/Terminal.cpp` | Supporting architecture | Connects terminal lifecycle to its session. |
| `include/shell/ShellIO.h` | Supporting shell I/O architecture | Exposes the per-tab input/output/error descriptors and streams. |
| `src/shell/ShellIO.cpp` | Supporting shell I/O implementation | Implements stream buffering and writes to the session descriptors. |
| `include/shell/ShellEngine.h` | Modified for helper execution entry point | Declares interactive, ShellIO-backed, and one-shot command execution. |
| `src/shell/ShellEngine.cpp` | Modified for substitution and MultiWatch routing | Coordinates parsing, substitution, built-ins, pipelines, external commands, jobs, and MultiWatch. |
| `include/shell/ShellHelper.h` | **New file** | Declares helper launch and output-capture operations. |
| `src/shell/ShellHelper.cpp` | **New file** | Uses `posix_spawn`, configures standard descriptors, captures output, and waits for helper processes. |
| `include/shell/CommandSubstitution.h` | Replaced/updated | Declares substitution expansion using the helper-based interface. |
| `src/shell/CommandSubstitution.cpp` | Replaced/updated | Scans substitution syntax and invokes helper-based capture. |
| `src/shell/main.cpp` | Replaced/updated | Adds internal `-c command` helper mode without interactive prompt. |
| `include/shell/MultiWatch.h` | Retained | Declares MultiWatch parsing and execution. |
| `src/shell/MultiWatch.cpp` | Retained | Implements worker processes, framed output, rerun timing, interruption, and cleanup. |
| `include/shell/JobManager.h` | Modified in the thread/job-control integration | Declares job records and foreground group management. |
| `src/shell/JobManager.cpp` | Modified in the thread/job-control integration | Implements job polling, waiting, foreground handling, and cleanup. |
| `src/shell/Builtins.cpp` | Modified in the thread/job-control integration | Passes session context to built-ins such as `fg` and handles shell built-ins. |

### 8.1 Consolidated list of files modified/added by the integration fixes

Across the thread-mode job-control integration and the later helper-process corrections, the identified modified/added paths are:

```text
include/shell/CommandSubstitution.h
include/shell/JobManager.h
include/shell/ShellEngine.h
include/shell/ShellHelper.h          (new)
src/main.cpp
src/shell/Builtins.cpp
src/shell/CommandSubstitution.cpp
src/shell/JobManager.cpp
src/shell/ShellEngine.cpp
src/shell/ShellHelper.cpp            (new)
src/shell/main.cpp
src/terminal/ThreadSession.cpp
```

The thread/process execution foundation was already present in the supplied ZIP and is described separately in sections 2–4. `build.sh` already discovers all shell source files and forwards arguments, so the helper integration did not require a build-script edit. `MultiWatch.h` and `MultiWatch.cpp` were deliberately retained.

Other shell files remain involved in normal command execution even when they were not specifically changed for the helper fix:

- `CommandParser.cpp` — tokenization, pipeline splitting, and redirection parsing.
- `CommandExecutor.cpp` — external command launch and wait behavior.
- `PipelineExecutor.cpp` — pipe creation, stage launch, descriptor closure, and pipeline status.
- `Redirection.cpp` — input/output redirection support.
- `LineEditor.cpp` — interactive input and history editing.
- `History.cpp` — command history.
- `SignalControl.cpp` — process-mode/helper signal handling.

**Change-control note:** the table describes the implementation files and the files identified in the two integration passes. Because no post-change project ZIP was uploaded after the latest manual code was applied, it is not a byte-for-byte Git diff of the user's current working tree. Use `git status` / `git diff` in the project repository if an exact final changed-file report is required.

---

## 9. Regression test plan

Run the tests first in default thread mode and then in process mode. For every test, verify both the command result and the destination of the output. In thread mode, output should appear in the correct Terminator tab—not in the VS Code terminal used to launch the application.

### 9.1 Build and mode selection

| ID | Mode | Action | Expected result |
|---|---|---|---|
| B01 | Thread | Run `bash build.sh` | Both binaries build; GUI starts in thread mode. |
| B02 | Process | Run `bash build.sh useProcess=True` | Both binaries build; GUI starts in process mode. |
| B03 | Thread | Run `./build/terminator` | Thread mode is selected. |
| B04 | Process | Run `./build/terminator useProcess=True` | Process mode is selected. |
| B05 | Argument validation | Run `./build/terminator true` | Usage text is printed and the GUI does not start. |
| B06 | Compile | Run both compile-only commands with `-Wall -Wextra` | Both targets compile without errors. |

### 9.2 Basic input/output and tab isolation

| ID | Mode | Action | Expected result |
|---|---|---|---|
| IO01 | Both | Run `echo hello` | `hello` appears in the active tab. |
| IO02 | Both | Run `pwd` | Current working directory is printed. |
| IO03 | Thread | Run `echo from-tab` | Output does not leak into the launching VS Code terminal. |
| IO04 | Thread | Open two tabs and run a different `echo` command in each | Output is displayed only in the originating tab. |
| IO05 | Thread | Run a long-output command such as `seq 1 10000` | GUI remains responsive and all output is eventually displayed. |
| IO06 | Both | Press Enter on an empty line | No crash; previous exit status is not unintentionally reset. |

### 9.3 Command substitution

| ID | Mode | Command/action | Expected result |
|---|---|---|---|
| CS01 | Both | `echo result=$(printf captured)` | Displays `result=captured`; helper output is not printed a second time. |
| CS02 | Both | `echo nested=$(printf '%s' "$(printf inner)")` | Displays `nested=inner`. |
| CS03 | Both | `echo upper=$(printf hello \| tr a-z A-Z)` | Displays `upper=HELLO`. |
| CS04 | Both | `echo "value=$(printf 'two words')"` | Displays both words inside one quoted value. |
| CS05 | Both | `echo value=$(printf 'line\n\n')` | Trailing newline characters are removed from substitution output. |
| CS06 | Both | `echo value=$(sh -c 'printf out; printf err >&2')` | `out` is captured into the command; `err` is shown on the tab's error stream. |
| CS07 | Both | `echo value=$(false; printf still-output)` | Captured stdout is still substituted even though an inner command returned non-zero. |
| CS08 | Both | `echo value=$(printf 'a|b&c')` | Substituted shell metacharacters are treated as data, not as a new outer pipeline/background command. |
| CS09 | Both | `echo value=$(printf test` | A clear unterminated-substitution error is shown; shell remains usable. |
| CS10 | Thread | Run substitution while another tab is active | Output remains associated with the correct tab. |
| CS11 | Thread | Press Ctrl+C during a long-running substitution | Helper/inner command stops through the supported signal path; shell can accept another command. |
| CS12 | Both | Run `echo value=`printf legacy`` | Backtick substitution works if the current scanner version retains this supported form. |

### 9.4 MultiWatch

| ID | Mode | Command/action | Expected result |
|---|---|---|---|
| MW01 | Thread | `multiWatch ["printf first"]` | Output appears inside the Terminator tab, not in the launching terminal. |
| MW02 | Thread | `multiWatch ["printf first", "printf second"]` | Both monitored commands produce separate output sections. |
| MW03 | Thread | `multiWatch ["printf hello \| tr a-z A-Z"]` | Pipeline output is captured and displayed in the tab. |
| MW04 | Thread | Press Ctrl+C while MultiWatch is running | Monitoring stops, worker groups are cleaned up, and the prompt returns. |
| MW05 | Thread | Close the tab while MultiWatch is running | Helper and worker processes do not remain running. |
| MW06 | Thread | Open two tabs and run MultiWatch in one while using the other | MultiWatch output stays in its original tab; the other tab remains usable. |
| MW07 | Process | Run the same MultiWatch commands | Existing process-mode execution continues to work. |
| MW08 | Both | Try an unsupported shell built-in inside MultiWatch | A clear validation error is shown; no unsupported built-in is executed as an external command. |
| MW09 | Both | Supply malformed list syntax | A parser error is shown and the shell remains usable. |
| MW10 | Both | Monitor a command that exits non-zero | The worker reports output/status behavior without crashing the monitor. |

### 9.5 External commands, pipelines, and redirection

| ID | Mode | Command/action | Expected result |
|---|---|---|---|
| EX01 | Both | `ls` | Lists directory entries. In thread mode, bare `ls` may be one entry per line because stdout is a pipe. |
| EX02 | Both | `ls -C` | Explicitly requests column output. |
| EX03 | Both | `ls -1` | One entry per line. |
| EX04 | Both | `ls -l` | Long listing format remains intact. |
| EX05 | Both | `ls \| cat` | Pipeline works; no forced formatting should be applied to pipeline output. |
| EX06 | Both | `printf hello \| tr a-z A-Z` | Displays `HELLO`. |
| EX07 | Both | `echo file-test > /tmp/terminator-test.txt` | File is created with expected content; output is redirected, not displayed as normal stdout. |
| EX08 | Both | `cat < /tmp/terminator-test.txt` | Input redirection reads the expected content. |
| EX09 | Both | Run `false`, then on the next prompt run `echo status=$?` | The shell exposes the preceding command's non-zero status to the next command. |
| EX10 | Both | Run a missing executable | An error is displayed in the correct tab and the shell remains usable. |

### 9.6 Built-ins, history, and working directory

| ID | Mode | Action | Expected result |
|---|---|---|---|
| BI01 | Both | `pwd` then `cd /tmp` then `pwd` | The current directory changes as supported by the mode. |
| BI02 | Thread | `cd /tmp` in tab A, then `pwd` in tab B | **Known shared-process behavior:** tab B may also observe `/tmp`. This is a thread-mode limitation, not an independent per-tab cwd. |
| BI03 | Process | `cd /tmp` in tab A, then `pwd` in tab B | Tab B retains its own process working directory. |
| BI04 | Both | Run `history` after a few commands | History entries are displayed according to the shared-history design. |
| BI05 | Both | Run `jobs` with a background job | Job state is displayed. |
| BI06 | Both | Run `exit` in one tab | Only that tab's shell/session should exit; GUI behavior should remain controlled. |

### 9.7 Signals, jobs, and shutdown

| ID | Mode | Action | Expected result |
|---|---|---|---|
| SG01 | Thread | Run `sleep 30`, press Ctrl+C | Foreground command is interrupted; prompt returns. |
| SG02 | Process | Run `sleep 30`, press Ctrl+C | PTY/process-mode interrupt works. |
| SG03 | Thread | Start `sleep 30 &`, run `jobs` | Background job is listed. |
| SG04 | Both | Start a background job, close its tab | Session cleanup terminates/reaps the job according to the implemented cleanup path. |
| SG05 | Thread | Run `multiWatch [...]`, press Ctrl+C | Signal reaches the helper, MultiWatch workers stop, shell resumes. |
| SG06 | Thread | Run command substitution around a long command, press Ctrl+C | Helper is interrupted; no stuck shell thread. |
| SG07 | Both | Start a long foreground command and close the tab | No orphan foreground command remains. |
| SG08 | Both | Run stopped-job workflow using Ctrl+Z, `jobs`, and `fg` | Validate supported stop/resume behavior. Treat pipeline job-control as a focused edge case. |

### 9.8 GUI regression

| ID | Action | Expected result |
|---|---|---|
| GUI01 | Open a new tab | New tab receives its own session and buffer. |
| GUI02 | Switch between tabs while commands are running | Output is routed to each correct buffer. |
| GUI03 | Close an inactive tab | Active tab remains selected and usable. |
| GUI04 | Close the active tab | Another tab is selected, or the application exits cleanly if no tabs remain. |
| GUI05 | Scroll output and use scrollbar | Viewport remains synchronized with buffer contents. |
| GUI06 | Type, delete, move cursor, and submit a command | Input editing continues to work. |
| GUI07 | Resize or expose the window | Drawing remains on the GUI thread and the window redraws correctly. |

### 9.9 Minimum acceptance gate before moving to the next project phase

At minimum, require all of the following to pass in your environment:

- [ ] `bash build.sh` launches thread mode by default.
- [ ] `bash build.sh useProcess=True` launches process mode.
- [ ] Both binaries compile with `-Wall -Wextra`.
- [ ] Basic command output appears inside the correct tab.
- [ ] Two tabs do not mix their output.
- [ ] Simple, nested, quoted, and pipeline command substitutions work.
- [ ] MultiWatch output appears in the correct tab.
- [ ] Ctrl+C stops MultiWatch and the shell prompt returns.
- [ ] Closing a tab while MultiWatch is running does not leave its worker processes behind.
- [ ] External commands, pipelines, and basic redirection work in both modes.
- [ ] Existing process mode remains usable as a comparison path.

Do not claim full job-control parity until the Ctrl+Z, `fg`, and pipeline-specific cases are separately verified.

---

## 10. Known behavior and limitations

### 10.1 `ls` output is line-oriented in thread mode

This is expected when GNU `ls` sees stdout connected to a pipe. The thread-mode session uses a pipe to deliver output to the GUI, so `isatty(STDOUT_FILENO)` is false. GNU `ls` therefore normally uses one entry per line.

Use:

```bash
ls -C
```

to request columns explicitly. The current helper integration does not itself guarantee automatic column formatting for bare `ls`. A future compatibility improvement could inject a column format only when `ls` is a direct command, not when output is piped or redirected. Do not add `--format=vertical` globally because it can change script/pipeline output.

### 10.2 Working directory is process-wide in thread mode

`chdir()` changes the working directory of the entire process. Since all thread-backed tabs share one process, `cd` in one tab can change the working directory observed by other tabs. Process mode has a separate process per tab and therefore separate working directories.

### 10.3 Programs that require a real terminal

Thread mode uses pipes rather than a PTY. Programs that need terminal ioctls, terminal modes, full-screen display, or interactive terminal input may not work correctly (examples include `vim`, `less`, `top`, and `stty`). Use process mode for testing such programs unless a PTY-backed design is added to thread mode later.

### 10.4 Command substitution stdin

The helper shell receives `/dev/null` as stdin. This ensures it cannot consume interactive keystrokes intended for the main tab shell. A command inside `$(...)` that expects interactive stdin is not supported by this design.

### 10.5 Helper executable location

`ShellHelper` expects `terminator-shell` beside the running executable, normally both under `build/`. If one binary is moved without the other, helper-based features can fail to launch.

### 10.6 MultiWatch remains process-based

Do not reinterpret the assignment as requiring MultiWatch workers to become threads. The shell session default is thread-based; MultiWatch retains its independent worker processes and is launched through a separate helper shell in thread mode.

### 10.7 Full job-control behavior

A pipe-backed shell thread is not identical to a traditional terminal-attached shell process. Signal forwarding, suspended process groups, foreground ownership, and pipelines require targeted tests. Treat these as explicit acceptance criteria rather than assuming that successful simple commands prove full job-control correctness.

---

## 11. Safe code-cleaning plan

The objective is to make the project understandable without changing behavior that has just been stabilized. Do not rename classes, move files, and alter process logic in one large refactor.

### Pass 1 — Stabilize and document the current state

- Keep the current file and class boundaries.
- Add short comments for ownership, descriptor direction, and signal flow.
- Avoid comments that merely restate the line of code.
- Run the acceptance tests in section 9 after any edit.

### Pass 2 — Simplify `ShellEngine.cpp`

`ShellEngine` currently orchestrates many responsibilities. Refactor one logical block at a time into small private functions, for example:

- `handleMultiWatch(...)`
- `expandCommand(...)`
- `parsePipeline(...)`
- `executeBuiltin(...)`
- `executeExternalCommand(...)`
- `updateJobState(...)`

These are design targets, not copy/paste declarations. Use the state already owned by `runImpl()` and pass dependencies explicitly. After moving one block, compile and run command-substitution, MultiWatch, pipeline, and builtin tests.

### Pass 3 — Make process launching easier to read

In `CommandExecutor.cpp`, make the high-level flow obvious:

1. Validate arguments.
2. Prepare child descriptors/redirection.
3. Launch the child.
4. Publish foreground process-group information.
5. Wait or register background state.
6. Return status.

Keep descriptor duplication and redirection setup together in a named helper. Do not duplicate subtly different `fork`/`exec` implementations across command types without clear need.

### Pass 4 — Make pipelines easier to inspect

Keep these pipeline phases visibly separate:

1. Create all required pipes.
2. Launch each stage with the correct stdin/stdout.
3. Close parent-side descriptors that are no longer needed.
4. Wait for children.
5. Determine the pipeline's exit status.
6. Clear foreground process-group state.

A descriptor leak can prevent EOF from reaching downstream commands, so pipe closure deserves explicit comments and tests.

### Pass 5 — Keep MultiWatch isolated

Do not rewrite MultiWatch's process model while simplifying it. Separate parsing, worker launch, frame collection, output rendering, and shutdown cleanup into clearly named helpers only after the integration is stable. Keep the helper boundary intact.

### Pass 6 — Clean the GUI last

Only after shell behavior is stable should `src/main.cpp` be broken into smaller event-handling functions. Every Xlib operation must remain on the GUI thread. Do not move drawing into `ThreadSession` or `ShellEngine`.

---

## 12. Practical workflow for future changes

For every future feature or cleanup:

1. Create a Git commit or backup before editing.
2. Identify the one responsibility being changed.
3. Change the smallest number of files possible.
4. Build both modes.
5. Run the targeted tests in thread mode.
6. Run the equivalent tests in process mode to catch regressions.
7. Run the broader regression checklist before merging the change.
8. Record any known limitation instead of silently treating it as completed.

Useful Git commands:

```bash
git status --short
git diff --stat
git diff -- src/main.cpp src/shell/ShellEngine.cpp
git diff --check
```

A useful commit sequence would be:

- `thread-session: make thread mode the default`
- `shell-helper: isolate substitution and MultiWatch execution`
- `job-control: route foreground signals per tab`
- `docs: record architecture and regression plan`

Use commit names that reflect the actual changes you have made, not necessarily these exact examples.

---

## 13. Final decision

The code selects **thread mode by default** and preserves **process mode behind `useProcess=True`**. The two modes have different terminal semantics: pipes in thread mode and a PTY in process mode. Command substitution and MultiWatch use a separate helper shell in thread mode so their internal process behavior and output routing do not leak into or execute unsafely inside the multithreaded GUI process.

The next project phase can proceed once the minimum acceptance gate in section 9.9 has passed on the user's machine. Retain process mode as a regression/reference mode, especially for terminal-dependent commands. Keep `ls` column formatting and full job-control parity documented as separate limitations rather than treating them as blockers to the confirmed default-mode decision.
