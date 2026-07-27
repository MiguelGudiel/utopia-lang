cmake_minimum_required(VERSION 3.20)
project(UtopiaCompiler VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Opciones del proyecto
option(BUILD_TESTS "Build unit tests" OFF)
option(ENABLE_SANITIZERS "Enable Address and Undefined Behavior Sanitizers" ON)

if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=undefined)
endif()

# Encontrar LLVM
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

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

# LLVM requiere algunas definiciones globales y directorios de inclusión
include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# Directorio global de cabeceras de nuestro proyecto
include_directories(include)

# Añadir subdirectorios
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/yaml-cpp)
add_subdirectory(src)
add_subdirectory(tools/utopia)
#add_subdirectory(tools/lsp)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()