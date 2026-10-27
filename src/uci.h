#pragma once

#include <string>

namespace UCI {

// Reads commands from stdin until "quit" or EOF. With command-line arguments, runs them
// as a single command instead (e.g. "./engine test full"). Returns the exit code.
int loop(int argc, char* argv[]);

// Prints one line (or several, newline-separated) atomically. The search thread and the
// UCI thread both write to stdout, so all protocol output goes through here.
void send(const std::string& text);

}  // namespace UCI
