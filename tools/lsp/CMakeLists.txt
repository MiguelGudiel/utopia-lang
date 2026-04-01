file(GLOB_RECURSE SOURCE_FILES "*.cpp")

add_executable(utopia_lsp ${SOURCE_FILES})

# Le decimos a este ejecutable dónde buscar los archivos .hpp
# CMAKE_SOURCE_DIR apunta a la raíz de tu proyecto (donde está la carpeta 'include')
target_include_directories(utopia_lsp PRIVATE ${CMAKE_SOURCE_DIR}/include)

# Enlazamos el ejecutable con el núcleo del compilador que creamos en src/
target_link_libraries(utopia_lsp PRIVATE utopia_core)

# LLVM y CMake a veces requieren enlazar librerías del sistema adicionales (como pthread o dl)
if(LLVM_ENABLE_THREADS)
    find_package(Threads REQUIRED)
    target_link_libraries(utopia_lsp PRIVATE Threads::Threads)
endif()