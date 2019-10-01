cmake_minimum_required( VERSION 3.12 )

find_package(LLVM REQUIRED CONFIG)

project( External.llvm
         VERSION ${LLVM_PACKAGE_VERSION}
         DESCRIPTION "LLVM external library." )

message( STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}" )
message( STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}" )

add_library( LLVM INTERFACE IMPORTED GLOBAL )

target_include_directories( LLVM SYSTEM INTERFACE ${LLVM_INCLUDE_DIRS} )
target_compile_definitions( LLVM INTERFACE ${LLVM_DEFINITIONS} )
target_compile_features( LLVM INTERFACE cxx_std_11 )
