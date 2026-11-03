# Trigger center host + manual trigger CLI (P3).
add_executable(trigger_center trigger_center_main.cpp)
# OPS-11: systemd Type=notify/WatchdogSec readiness (shared sd_notify shim).
target_link_libraries(trigger_center PRIVATE thyme_recorder thyme_app
  Threads::Threads)
add_executable(triggerctl triggerctl.cpp)
target_link_libraries(triggerctl PRIVATE thyme_recorder)
add_executable(demo_producer demo_producer.cpp)
target_link_libraries(demo_producer PRIVATE thyme_recorder Threads::Threads)
