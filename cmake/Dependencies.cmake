# Dependencies are target-scoped; no installed stackflow or simdjson archive is required.
find_path(EDGE_ZMQ_INCLUDE zmq.h HINTS "${ZMQ_ROOT}/include" REQUIRED)
find_library(EDGE_ZMQ_LIBRARY NAMES zmq HINTS "${ZMQ_ROOT}/lib" REQUIRED)
add_library(edge_zmq UNKNOWN IMPORTED)
set_target_properties(edge_zmq PROPERTIES IMPORTED_LOCATION "${EDGE_ZMQ_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${EDGE_ZMQ_INCLUDE}")
