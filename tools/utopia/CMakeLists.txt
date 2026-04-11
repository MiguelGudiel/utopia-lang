set(CLI_SOURCES
    src/main.cpp
    src/Linker.cpp
    src/ProjectManager.cpp
)

add_executable(utopia ${CLI_SOURCES})

# Enlazamos el ejecutable con el núcleo del compilador que creamos en src/
target_link_libraries(utopia PRIVATE utopia_core)

# LLVM y CMake a veces requieren enlazar librerías del sistema adicionales (como pthread o dl)
# Esto maneja esas dependencias de forma cruzada
if(LLVM_ENABLE_THREADS)
    find_package(Threads REQUIRED)
    target_link_libraries(utopia PRIVATE Threads::Threads)
endif()

target_include_directories(utopia_core PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)