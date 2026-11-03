#include "generated/record_table.h"
#include "thyme/recorder/recorder_cli.h"

int main(int argc, char** argv) {
  return thyme::recorder_main(argc, argv, GetThymeRecordTable());
}
