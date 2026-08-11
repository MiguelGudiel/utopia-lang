set(CLI_SOURCES
    src/main.cpp
    src/ProjectManager.cpp
    src/BuildScriptRunner.cpp
    src/Core/ProjectBuilder.cpp
    src/CLI/OptionsParser.cpp
    src/Commands/Build/BuildCommand.cpp
    src/Commands/Run/RunCommand.cpp
    src/Commands/Fmt/FmtCommand.cpp
)

add_executable(utopia ${CLI_SOURCES})

target_link_libraries(utopia PRIVATE utopia_core)

if(LLVM_ENABLE_THREADS)
    find_package(Threads REQUIRED)
    target_link_libraries(utopia PRIVATE Threads::Threads)
endif()

target_include_directories(utopia PRIVATE 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

install(TARGETS utopia DESTINATION bin)