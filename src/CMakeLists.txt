set(CORE_SOURCES
    utopia/AST/AST.cpp
    utopia/CodeGen/CodeGen.cpp
    utopia/CodeGen/BackendContext.cpp
    utopia/CodeGen/Mangler.cpp
    utopia/Driver/Backend.cpp
    utopia/Driver/CompilerDriver.cpp
    utopia/Driver/ModuleLoader.cpp
    utopia/Driver/BuildCache.cpp
    utopia/Driver/Compiler.cpp
    utopia/Driver/Linker.cpp
    utopia/Lexer/Lexer.cpp
    utopia/Parser/Parser.cpp
    utopia/Sema/Sema.cpp
    utopia/Sema/EffectAnalyzer.cpp
)

add_library(utopia_core STATIC ${CORE_SOURCES})

target_link_libraries(utopia_core PUBLIC ${llvm_libs} yaml-cpp)

target_compile_definitions(utopia_core PUBLIC 
    $<$<CONFIG:Debug>:UTOPIA_DEBUG_BUILD>
    $<$<CONFIG:Release>:UTOPIA_RELEASE_BUILD>
    UTOPIA_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    UTOPIA_INTERNAL_LIB_PATH="${CMAKE_INSTALL_PREFIX}/lib/utopia"
)

target_include_directories(utopia_core PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)