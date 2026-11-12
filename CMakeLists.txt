cmake_minimum_required(VERSION 3.21)

# ---- Perfiles de compilación ----
# Puedes cambiar Release/Debug aquí o usar cmake --build build --config <perfil>
# set(CMAKE_CONFIGURATION_TYPES "Release;Debug" CACHE STRING "" FORCE)

project(KI_MCP_Gefaehrte VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT BUILD_TESTS)
    set(BUILD_TESTS OFF)
endif()

# ---- SKYRIM_MODS_FOLDER: define a qué carpeta copiar el .dll tras compilar ----
if(DEFINED ENV{SKYRIM_MODS_FOLDER})
    set(OUTPUT_FOLDER "$ENV{SKYRIM_MODS_FOLDER}/${PROJECT_NAME}")
else()
    set(OUTPUT_FOLDER "${CMAKE_BINARY_DIR}/${PROJECT_NAME}")
endif()
message(STATUS "Carpeta de salida (para el .dll y CONFIG): ${OUTPUT_FOLDER}")

# ---- Dependencias ----
# SDK CommonLibSSE-NG vivo (submódulo, tag v13.1.0). Se compila por fuente:
# el bundle precompilado se enlaza contra otra versión de fmt y produce
# LNK2001. add_subdirectory no expone add_commonlibsse_plugin, por eso el
# include explícito del ayudante después.
add_subdirectory(lib/commonlibsse-ng)
include(lib/commonlibsse-ng/cmake/CommonLibSSE.cmake)

# ---- Plugin ----
add_commonlibsse_plugin(${PROJECT_NAME} SOURCES plugin.cpp) # <--- specifies plugin.cpp
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23) # <--- use C++23 standard
# windows.h define macros min/max que rompen std::min/std::max (error C2589).
target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_precompile_headers(${PROJECT_NAME} PRIVATE PCH.h) # <--- PCH.h is required!
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_FILENAME "KI_MCP_Gefaehrte") # <--- the plugin name

# ---- Compilación ----
# El .dll aparece en build/<project>/ ó en SKYRIM_MODS_FOLDER si está definido.
message(STATUS "Compilando ${PROJECT_NAME}... Usa: cmake --build build --config Release")

# ---- Instalación local (opcional) ----
if(DEFINED DLL_FOLDER)
    add_custom_command(
        TARGET "${PROJECT_NAME}"
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
    )
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        add_custom_command(
            TARGET "${PROJECT_NAME}"
            POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_PDB_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_PDB_FILE_NAME:${PROJECT_NAME}>"
        )
    endif()
endif()
