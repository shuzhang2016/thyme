# .thyme.bin -> MCAP export CLI (REC-06).
add_executable(thyme_bin_to_mcap main.cpp)
target_link_libraries(thyme_bin_to_mcap PRIVATE thyme_runtime)
