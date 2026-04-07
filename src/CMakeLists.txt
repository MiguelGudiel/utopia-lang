# File: src/CMakeLists.txt

set(CORE_SOURCES
    utopia/AST/AST.cpp
    utopia/CodeGen/CodeGen.cpp
    utopia/Driver/CompilerDriver.cpp
    utopia/Driver/ModuleLoader.cpp
    utopia/Driver/BuildCache.cpp
    utopia/Lexer/Lexer.cpp
    utopia/Parser/Parser.cpp
    utopia/Sema/Sema.cpp
)

add_library(utopia_core STATIC ${CORE_SOURCES})

# Enlazamos con LLVM usando la variable heredada del CMakeLists.txt raíz
target_link_libraries(utopia_core PUBLIC ${llvm_libs} yaml-cpp)

target_compile_definitions(utopia_core PUBLIC 
    $<$<CONFIG:Debug>:UTOPIA_DEBUG_BUILD>
    $<$<CONFIG:Release>:UTOPIA_RELEASE_BUILD>
    UTOPIA_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

# La forma profesional de exponer los includes de una librería
target_include_directories(utopia_core PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)