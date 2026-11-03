# thyme_probe: read-only SHM flow view (ECO-09/ECO-11). Thin main over
# thyme_probe_core so the CLI logic stays testable.
add_executable(thyme_probe main.cpp)
target_link_libraries(thyme_probe PRIVATE thyme_probe_core)
