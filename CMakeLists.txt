cmake_minimum_required(VERSION 3.20)
project(UtopiaCompiler VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTS "Build unit tests" OFF)
option(ENABLE_SANITIZERS "Enable Address and Undefined Behavior Sanitizers" ON)

if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=undefined)
    #add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    #add_link_options(-fsanitize=address,undefined)
endif()

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

find_package(CURL REQUIRED)
message(STATUS "Found CURL ${CURL_VERSION_STRING}")

llvm_map_components_to_libnames(llvm_libs 
    core 
    support 
    native 
    target 
    mc
    targetparser
    passes
    OrcJit
    OrcShared
    ${LLVM_TARGETS_TO_BUILD}
)

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

include_directories(include)

set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/yaml-cpp)
add_subdirectory(runtime/utopia_async)
add_subdirectory(runtime/utopia_runtime)
add_subdirectory(src)
add_subdirectory(tools/utopia)
add_subdirectory(tools/lsp)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(DIRECTORY libs/ DESTINATION lib/utopia)

set(CPACK_PACKAGE_NAME "Utopia")
set(CPACK_PACKAGE_VENDOR "UtopiaLang")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Utopia Programming Language Compiler")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})

set(CPACK_NSIS_MODIFY_PATH ON)

set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/installers")

include(CPack)