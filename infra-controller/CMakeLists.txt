add_library(stackflow STATIC src/StackFlow.cpp src/StackFlowUtil.cpp src/channel.cpp
  ../hybrid-comm/src/pzmq_data.cpp)
target_include_directories(stackflow PUBLIC include ../hybrid-comm/include ../utils)
target_link_libraries(stackflow PUBLIC edge_zmq Threads::Threads)
install(TARGETS stackflow ARCHIVE DESTINATION lib)
