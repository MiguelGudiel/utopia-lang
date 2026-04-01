# File: src/CMakeLists.txt

file(GLOB_RECURSE SOURCE_FILES "*.cpp")

add_library(utopia_core STATIC ${SOURCE_FILES})

# Enlazamos con LLVM usando la variable heredada del CMakeLists.txt raíz
target_link_libraries(utopia_core PUBLIC ${llvm_libs})

# La forma profesional de exponer los includes de una librería
target_include_directories(utopia_core PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)