set(LSP_SOURCES
    src/main.cpp
    src/Server.cpp
    src/Protocol.cpp
    src/DocumentManager.cpp
    src/LocalVarCollector.cpp
    src/AstUtils.cpp
    src/Hover.cpp
    src/Definition.cpp
    src/Completion.cpp
    src/SignatureHelp.cpp
    src/Formatting.cpp
    src/SemanticTokens.cpp
    src/Symbols.cpp
    src/References.cpp
    src/Folding.cpp
    src/Links.cpp
    src/CodeActions.cpp
)

add_executable(utopia_lsp ${LSP_SOURCES})

target_include_directories(utopia_lsp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(utopia_lsp PRIVATE utopia_core)

if(LLVM_ENABLE_THREADS)
    find_package(Threads REQUIRED)
    target_link_libraries(utopia_lsp PRIVATE Threads::Threads)
endif()
