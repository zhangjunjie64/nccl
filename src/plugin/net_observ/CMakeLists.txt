# Net observ plugin sources (no protobuf/gRPC dependency)

set(PLUGIN_NET_OBSERV_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/net_observ.cc
)

# Add net observ plugin sources to parent scope
set(PLUGIN_NET_OBSERV_SOURCES ${PLUGIN_NET_OBSERV_SOURCES} PARENT_SCOPE)