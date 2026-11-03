#include "terminal/TerminalConfig.h"

using namespace std;

const string PROMPT = "user@terminator:";

const int PROMPT_X = 16;
const int START_Y = 72;
const int LINE_HEIGHT = 28;
const int PROMPT_INPUT_GAP = 8;

// Start with a comfortably sized window. X11Window also supports resizing.
const int WINDOW_WIDTH = 1100;
const int WINDOW_HEIGHT = 720;

const int CURSOR_BLINK_INTERVAL_MS = 500;

vector<string> history;
