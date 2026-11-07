cmake_minimum_required(VERSION 3.16)
project(SetoStatic C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# psprecomp at 5ca18fac, with the local patches applied (see README).
# The game's real threads need the opt-in scheduler.
set(PSPRECOMP_REAL_THREADS ON)
add_subdirectory(build/psprecomp psprecomp)

# C emitted from the real EBOOT by `allegrexrecomp emit` (see README).
file(GLOB TAGFORCE_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/tagforce/*.c)
add_library(tagforce STATIC ${TAGFORCE_SOURCES})
target_include_directories(tagforce PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/build/tagforce)
# Function-entry tracing is what proves translated code ran.
target_compile_definitions(tagforce PUBLIC PSPRECOMP_TRACE)
target_link_libraries(tagforce PUBLIC psprecomp)

# S5: the offline-relocated sceFont_Library corpus. Generated, not committed;
# see README. Without it the EBOOT-only host still builds.
file(GLOB FONT_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/s5-font-reloc/*.c)

if(FONT_SOURCES)
    add_library(fontprx OBJECT ${FONT_SOURCES})
    target_compile_definitions(fontprx PRIVATE PSPRECOMP_NAMESPACE=font_ PSPRECOMP_TRACE)
    target_include_directories(fontprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/s5-font-reloc)
    target_link_libraries(fontprx PRIVATE psprecomp)
endif()

# S6: the relocated modtitle corpus. Its imports are not firmware NIDs but
# EBOOT exports, so every stub is routed through title_import_forward().
file(GLOB TITLE_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/s6-title-reloc/*.c)
if(TITLE_SOURCES)
    add_library(titleprx OBJECT ${TITLE_SOURCES})
    target_compile_definitions(titleprx PRIVATE
        PSPRECOMP_NAMESPACE=title_ PSPRECOMP_TRACE
        psp_hle_call=title_import_forward)
    target_include_directories(titleprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/s6-title-reloc)
    target_link_libraries(titleprx PRIVATE psprecomp)
endif()

set(SETO_SOURCES host.c umd.c fpl.c font_prx.c)
if(TITLE_SOURCES)
    list(APPEND SETO_SOURCES title_prx.c)
endif()
add_executable(Seto-static ${SETO_SOURCES})
if(FONT_SOURCES)
    target_sources(Seto-static PRIVATE $<TARGET_OBJECTS:fontprx>)
    target_compile_definitions(Seto-static PRIVATE HAVE_FONT_CORPUS)
else()
    message(STATUS "S5 font corpus not found under build/s5-font-reloc; building the EBOOT-only host")
endif()
if(TITLE_SOURCES)
    target_sources(Seto-static PRIVATE $<TARGET_OBJECTS:titleprx>)
    target_compile_definitions(Seto-static PRIVATE HAVE_TITLE_CORPUS)
else()
    message(STATUS "S6 title corpus not found under build/s6-title-reloc; building without modtitle")
endif()
target_link_libraries(Seto-static PRIVATE tagforce)
