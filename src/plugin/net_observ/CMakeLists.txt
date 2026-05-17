cmake_minimum_required(VERSION 3.25)

set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(PROTO_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/src/include/net_observ)
set(PROTO_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(PROTO_SRC ${PROTO_SRC_DIR}/ruijie-json.pb.cc)
set(PROTO_HDR ${PROTO_INCLUDE_DIR}/ruijie-json.pb.h)
set(GRPC_SRC ${PROTO_SRC_DIR}/ruijie-json.grpc.pb.cc)
set(GRPC_HDR ${PROTO_INCLUDE_DIR}/ruijie-json.grpc.pb.h)

set(PROTO_LINK_LIBS "")

if(EXISTS ${PROTO_SRC} AND EXISTS ${GRPC_SRC} AND EXISTS ${PROTO_HDR} AND EXISTS ${GRPC_HDR})
    message(STATUS "Using pre-generated proto files")
    add_custom_target(net_observ_proto)
    set(NCCL_HAS_PROTO TRUE)
else()
    find_program(PROTOC_EXE protoc)
    find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin)
    
    if(PROTOC_EXE AND GRPC_CPP_PLUGIN)
        message(STATUS "Found protoc: ${PROTOC_EXE}")
        message(STATUS "Found grpc_cpp_plugin: ${GRPC_CPP_PLUGIN}")
        
        file(MAKE_DIRECTORY ${PROTO_INCLUDE_DIR})
        
        add_custom_command(
            OUTPUT ${PROTO_SRC} ${PROTO_HDR} ${GRPC_SRC} ${GRPC_HDR}
            COMMAND ${CMAKE_COMMAND} -E env
                SCRIPT_DIR=${PROTO_DIR}
                ${PROTO_DIR}/generate_proto.sh
            DEPENDS ${PROTO_DIR}/ruijie-json.proto ${PROTO_DIR}/generate_proto.sh
            COMMENT "Generating proto files via generate_proto.sh"
            VERBATIM
        )
        
        add_custom_target(net_observ_proto DEPENDS ${PROTO_SRC} ${PROTO_HDR} ${GRPC_SRC} ${GRPC_HDR})
        set(NCCL_HAS_PROTO TRUE)
    else()
        message(STATUS "protoc or grpc_cpp_plugin not found")
        message(STATUS "Run manually: cd ${PROTO_DIR} && ./generate_proto.sh")
        add_custom_target(net_observ_proto)
        set(NCCL_HAS_PROTO FALSE)
    endif()
endif()

set(PLUGIN_NET_OBSERV_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/net_observ.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/net_observ_proxy.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/net_observ_int.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/net_observ_collab.cc
)

if(NCCL_HAS_PROTO OR EXISTS ${PROTO_SRC})
    list(APPEND PLUGIN_NET_OBSERV_SOURCES ${PROTO_SRC} ${GRPC_SRC})
    
    find_library(PROTOBUF_LIB NAMES protobuf)
    find_library(GRPC_LIB NAMES grpc)
    find_library(GRPCPP_LIB NAMES grpc++)
    
    if(PROTOBUF_LIB AND GRPCPP_LIB)
        message(STATUS "Found protobuf library: ${PROTOBUF_LIB}")
        message(STATUS "Found grpc++ library: ${GRPCPP_LIB}")
        set(PROTO_LINK_LIBS ${PROTOBUF_LIB} ${GRPCPP_LIB})
        if(GRPC_LIB)
            list(APPEND PROTO_LINK_LIBS ${GRPC_LIB})
        endif()
    else()
        message(STATUS "protobuf or grpc++ library not found, linking may fail")
    endif()
endif()

set(PLUGIN_NET_OBSERV_INCLUDE_DIRS
    ${PROTO_INCLUDE_DIR}
    PARENT_SCOPE
)

set(PLUGIN_NET_OBSERV_SOURCES ${PLUGIN_NET_OBSERV_SOURCES} PARENT_SCOPE)
set(PLUGIN_NET_OBSERV_LINK_LIBS ${PROTO_LINK_LIBS} PARENT_SCOPE)
set(NCCL_HAS_PROTO ${NCCL_HAS_PROTO} PARENT_SCOPE)
