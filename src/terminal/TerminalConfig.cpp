#include "terminal/TerminalConfig.h"
using namespace std;

const string PROMPT = "user@terminator:";

const int PROMPT_X = 0;
const int START_Y = 42;
const int LINE_HEIGHT = 20;
const int PROMPT_INPUT_GAP = 5;

const int WINDOW_WIDTH = 600;
const int WINDOW_HEIGHT = 300;

const int CURSOR_BLINK_INTERVAL_MS = 500;

vector<string> history;