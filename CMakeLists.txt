cmake_minimum_required(VERSION 3.20)
project(llvm-passes)

set(PROTO_LIB "" CACHE FILEPATH "Path to protobuf generated static library")
set(PROTO_DIR "" CACHE PATH "Directory containing generated protobuf files")

add_subdirectory(InstructionCount)
add_subdirectory(examples)
