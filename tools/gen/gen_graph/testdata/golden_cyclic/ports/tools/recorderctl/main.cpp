#include "thyme/record/record_control.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

void PrintUsage() {
  std::fprintf(stderr,
              "usage: thyme-recorderctl --socket PATH (trigger | arm | disarm | status)\n");
}

}  // namespace

int main(int argc, char** argv) {
  const char* socket_path = "/tmp/thyme_recorder.sock";
  const char* command = nullptr;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
      socket_path = argv[++i];
    } else if (command == nullptr) {
      command = argv[i];
    } else {
      PrintUsage();
      return 2;
    }
  }

  if (command == nullptr) {
    PrintUsage();
    return 2;
  }

  const char* cmd = nullptr;
  if (std::strcmp(command, "trigger") == 0) {
    cmd = "trigger";
  } else if (std::strcmp(command, "arm") == 0) {
    cmd = "arm";
  } else if (std::strcmp(command, "disarm") == 0) {
    cmd = "disarm";
  } else if (std::strcmp(command, "status") == 0) {
    cmd = "status";
  } else {
    PrintUsage();
    return 2;
  }

  if (!thyme::record_control::SendClientCommand(socket_path, cmd)) {
    std::fprintf(stderr, "thyme-recorderctl: send to %s failed\n", socket_path);
    return 1;
  }
  return 0;
}
