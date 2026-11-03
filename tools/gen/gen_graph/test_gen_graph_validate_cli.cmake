# GEN-02 binary-level negatives: the semantic validation pass runs in the
# gen_graph binary after load, before any emit — each invalid graph exits 2
# with a message naming the violation and leaves no partial output behind.
# (Rule-by-rule coverage lives in the fast parser-level test_gen_graph_validate.)
if(NOT GEN OR NOT THYME)
  message(FATAL_ERROR "GEN and THYME required")
endif()
set(ROOT "${CMAKE_BINARY_DIR}/validate_cli")
file(REMOVE_RECURSE "${ROOT}")
file(MAKE_DIRECTORY "${ROOT}")

set(YAML_HEAD
  "version: 1\npool_n: 4\nring_cap: 8\nnodes:\n  - id: prod\n    type: producer\n    hz: 10\n    process: p1\n  - id: cons\n    type: consumer\n    hz: 10\n    process: p2\n")
set(YAML_TAIL
  "processes:\n  - id: p1\n    role: main\n  - id: p2\n    role: aux\nsupervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 2\n")

# expect_reject <name> <needle> <yaml-body-between-head-and-tail>
function(expect_reject name needle body)
  set(yaml "${ROOT}/${name}.yaml")
  file(WRITE "${yaml}" "${YAML_HEAD}${body}${YAML_TAIL}")
  execute_process(COMMAND "${GEN}" --in "${yaml}" --out "${ROOT}/${name}_app"
    RESULT_VARIABLE rc OUTPUT_VARIABLE cout ERROR_VARIABLE cerr)
  if(rc EQUAL 0)
    message(FATAL_ERROR "${name}: gen_graph accepted an invalid graph")
  endif()
  if(NOT rc EQUAL 2)
    message(FATAL_ERROR "${name}: exit code ${rc}, expected 2:\n${cout}${cerr}")
  endif()
  string(FIND "${cout}${cerr}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "${name}: error does not mention '${needle}':\n${cout}${cerr}")
  endif()
  if(EXISTS "${ROOT}/${name}_app")
    message(FATAL_ERROR "${name}: gen_graph left partial output behind")
  endif()
endfunction()

expect_reject(dup_node "duplicate node id 'cons'"
  "  - id: cons\n    type: consumer\n    hz: 10\n    process: p2\nedges: []\n")

# A dangling endpoint resolves as same-process at load, so this graph carries
# no shm (load would reject same-process latest + shm before validation runs).
expect_reject(dangling "to 'ghost' is not a declared node"
  "edges:\n  - from: prod\n    to: ghost\n    channel: ch\n    cpp_type: Msg\n    pool: MsgPool\n    transport: spsc\n")

expect_reject(bad_channel "channel name 'my-channel' is not a valid C identifier"
  "edges:\n  - from: prod\n    to: cons\n    channel: my-channel\n    cpp_type: Msg\n    pool: MsgPool\n    transport: spsc\n    shm: _ch\n")

# Cross-process latest + shm with the built-in scaffold (no shm_kinds.txt):
# the missing kind is a hard error, not a silently dropped channel.
expect_reject(missing_kind "cpp_type 'Msg' has no shm kind"
  "edges:\n  - from: prod\n    to: cons\n    channel: ch\n    cpp_type: Msg\n    pool: MsgPool\n    transport: spsc\n    shm: _ch\n")

message(STATUS "gen_graph validate CLI negatives OK")
