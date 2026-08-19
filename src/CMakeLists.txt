set(CORE_SOURCES
    utopia/AST/AST.cpp
    utopia/AST/ASTCloner.cpp
    utopia/CodeGen/CodeGen.cpp
    utopia/CodeGen/BackendContext.cpp
    utopia/CodeGen/DebugInfoEmitter.cpp
    utopia/CodeGen/TBAAManager.cpp
    utopia/CodeGen/Mangler.cpp
    utopia/CodeGen/Intrinsics.cpp
    utopia/CodeGen/SimdIntrinsics.cpp
    utopia/Common/Logger.cpp
    utopia/Driver/Backend.cpp
    utopia/Driver/CompilerDriver.cpp
    utopia/Driver/ModuleLoader.cpp
    utopia/Driver/BuildCache.cpp
    utopia/Driver/Compiler.cpp
    utopia/Driver/Linker.cpp
    utopia/Format/PieceFactory.cpp
    utopia/Format/CodeWriter.cpp
    utopia/Format/Solution.cpp
    utopia/Lexer/Lexer.cpp
    utopia/Lexer/Preprocessor.cpp
    utopia/Parser/Parser.cpp
    utopia/Sema/Sema.cpp
    utopia/Sema/DeclCollector.cpp
    utopia/Sema/TypeChecker.cpp
    utopia/Sema/TypeConversion.cpp
    utopia/Sema/TemplateDeduction.cpp
    utopia/Sema/OverloadResolution.cpp
    utopia/Sema/EffectAnalyzer.cpp
    utopia/Sema/ControlFlowAnalyzer.cpp
)

add_library(utopia_core STATIC ${CORE_SOURCES})

target_link_libraries(utopia_core PUBLIC ${llvm_libs} yaml-cpp)

target_compile_definitions(utopia_core PUBLIC 
    $<$<CONFIG:Debug>:UTOPIA_DEBUG_BUILD>
    #UTOPIA_DEBUG_LOGS
    $<$<CONFIG:Release>:UTOPIA_RELEASE_BUILD>
    UTOPIA_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    UTOPIA_INTERNAL_LIB_PATH="${CMAKE_INSTALL_PREFIX}/lib/utopia"
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(utopia_core PUBLIC UTOPIA_DEBUG_BUILD)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_definitions(utopia_core PUBLIC UTOPIA_RELEASE_BUILD)
endif()

target_include_directories(utopia_core PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)