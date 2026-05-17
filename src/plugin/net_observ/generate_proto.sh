#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NCCL_SRC_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
PROTO_FILE="${SCRIPT_DIR}/ruijie-json.proto"
SRC_DIR="${SCRIPT_DIR}"
INCLUDE_DIR="${NCCL_SRC_DIR}/include/net_observ"

echo "=== Proto Generation Script ==="
echo "SCRIPT_DIR: $SCRIPT_DIR"
echo "NCCL_SRC_DIR: $NCCL_SRC_DIR"
echo "PROTO_FILE: $PROTO_FILE"
echo "SRC_DIR: $SRC_DIR"
echo "INCLUDE_DIR: $INCLUDE_DIR"
echo ""

if [ ! -f "$PROTO_FILE" ]; then
    echo "Error: proto file not found: $PROTO_FILE"
    exit 1
fi

if ! command -v protoc &> /dev/null; then
    echo "Error: protoc not found. Please install protobuf-compiler"
    echo "  Ubuntu/Debian: sudo apt-get install protobuf-compiler libgrpc++-dev protobuf-compiler-grpc"
    exit 1
fi

echo "Found protoc: $(which protoc)"
protoc --version

mkdir -p "$INCLUDE_DIR"

echo ""
echo "Generating protobuf C++ files to $SRC_DIR..."
protoc \
    --cpp_out="$SRC_DIR" \
    --proto_path="$(dirname "$PROTO_FILE")" \
    "$PROTO_FILE"

echo "Generated files in $SRC_DIR:"
ls -la "$SRC_DIR"/ruijie-json.pb.* 2>/dev/null || echo "  No .pb files found"

if command -v grpc_cpp_plugin &> /dev/null; then
    echo ""
    echo "Found grpc_cpp_plugin: $(which grpc_cpp_plugin)"
    echo "Generating gRPC C++ files to $SRC_DIR..."
    protoc \
        --grpc_out="$SRC_DIR" \
        --proto_path="$(dirname "$PROTO_FILE")" \
        --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) \
        "$PROTO_FILE"
    echo "Generated gRPC files in $SRC_DIR:"
    ls -la "$SRC_DIR"/ruijie-json.grpc.pb.* 2>/dev/null || echo "  No .grpc.pb files found"
else
    echo "Warning: grpc_cpp_plugin not found, skipping gRPC generation"
fi

echo ""
echo "Moving header files to $INCLUDE_DIR..."

if [ -f "$SRC_DIR/ruijie-json.pb.h" ]; then
    mv "$SRC_DIR/ruijie-json.pb.h" "$INCLUDE_DIR/"
    echo "  Moved ruijie-json.pb.h"
else
    echo "  Warning: ruijie-json.pb.h not found in $SRC_DIR"
fi

if [ -f "$SRC_DIR/ruijie-json.grpc.pb.h" ]; then
    mv "$SRC_DIR/ruijie-json.grpc.pb.h" "$INCLUDE_DIR/"
    echo "  Moved ruijie-json.grpc.pb.h"
else
    echo "  Warning: ruijie-json.grpc.pb.h not found in $SRC_DIR"
fi

echo ""
echo "=== Final Result ==="
echo "Source files (src/plugin/net_observ/):"
ls -la "$SRC_DIR"/*.pb.cc "$SRC_DIR"/*.grpc.pb.cc 2>/dev/null || echo "  No source files"
echo ""
echo "Header files (src/include/net_observ/):"
ls -la "$INCLUDE_DIR"/*.pb.h "$INCLUDE_DIR"/*.grpc.pb.h 2>/dev/null || echo "  No header files"
