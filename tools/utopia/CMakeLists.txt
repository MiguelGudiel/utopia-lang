set(CLI_SOURCES
    src/main.cpp
    src/ProjectManager.cpp
    src/BuildScriptRunner.cpp
    src/Core/ProjectBuilder.cpp
    src/CLI/OptionsParser.cpp
    src/Commands/Build/BuildCommand.cpp
    src/Commands/Run/RunCommand.cpp
    src/Commands/Fmt/FmtCommand.cpp
    src/Commands/Yip/YipCommand.cpp
    src/Network/HttpClient.cpp
    src/Core/EnvLoader.cpp
)

add_executable(utopia ${CLI_SOURCES})

# The async runtime is only referenced from JIT-compiled user code, so the
# static archive would be dropped by the linker; pull it in whole and export
# its symbols so the JIT can resolve them via
# DynamicLibrarySearchGenerator::GetForCurrentProcess.
target_link_libraries(utopia PRIVATE utopia_core CURL::libcurl)

if(UNIX AND NOT APPLE)
    target_link_libraries(utopia PRIVATE
        "-Wl,--whole-archive" utopia_async "-Wl,--no-whole-archive")
    target_link_options(utopia PRIVATE -rdynamic)
else()
    target_link_libraries(utopia PRIVATE utopia_async)
endif()

if(WIN32)
    target_link_libraries(utopia PRIVATE ws2_32)
endif()

if(DEFINED YIP_BASE_URL)
    target_compile_definitions(utopia PRIVATE UTOPIA_YIP_BASE_URL=\"${YIP_BASE_URL}\")
endif()

if(LLVM_ENABLE_THREADS)
    find_package(Threads REQUIRED)
    target_link_libraries(utopia PRIVATE Threads::Threads)
endif()

target_include_directories(utopia PRIVATE 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

install(TARGETS utopia DESTINATION bin)