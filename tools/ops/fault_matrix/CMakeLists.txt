# OPS-08: fault-matrix role helper driven by fault_matrix.py.
add_executable(fault_role fault_role.cpp)
target_link_libraries(fault_role PRIVATE thyme_recorder Threads::Threads)
