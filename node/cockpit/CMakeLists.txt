add_library(edge_cockpit_core STATIC context.cpp ingress.cpp metrics.cpp)
target_include_directories(edge_cockpit_core PUBLIC . ../../utils ${Boost_INCLUDE_DIRS})
target_link_libraries(edge_cockpit_core PUBLIC network Threads::Threads)
target_compile_options(edge_cockpit_core PRIVATE -Wall -Wextra -Wpedantic)
add_executable(edge_cockpit main.cpp cockpit_task.cpp cockpit_control.cpp cockpit_services.cpp cockpit_results.cpp
    model_client.cpp vehicle_simulator.cpp)
target_link_libraries(edge_cockpit PRIVATE edge_cockpit_core stackflow)
target_compile_options(edge_cockpit PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS edge_cockpit RUNTIME DESTINATION bin)
