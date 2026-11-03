# libFuzzer harness for gen_graph yaml parser (M8.2).
#
# Build (Clang required):
#   cmake -S . -B build -DTHYME_ENABLE_FUZZ=ON
#   cmake --build build --target fuzz_gen_graph_yaml
#
# Run seeded corpus (no mutation):
#   ./build/tools/gen/gen_graph/fuzz_gen_graph_yaml -runs=0 tools/gen/gen_graph/testdata
#
# GEN-07 structured driver (no Clang needed; spawns the gen_graph binary on
# YAML-aware mutations of the fixture graphs; quick/full ctest passes):
#   ./build/tools/gen/gen_graph/fuzz_gen_graph 2000
#   ctest --test-dir build -R fuzz_gen_graph
#
# Nightly timed fuzz (M8.3):
#   bash tools/gen/gen_graph/fuzz/run_nightly.sh build fuzz-artifacts
#
# CTest (when THYME_ENABLE_FUZZ=ON):
#   ctest -R test_fuzz_yaml_corpus
