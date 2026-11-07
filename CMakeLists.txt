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
# see README. The namespace keeps its symbols separate from the EBOOT.
file(GLOB FONT_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/s5-font-reloc/*.c)
add_library(fontprx OBJECT ${FONT_SOURCES})
target_compile_definitions(fontprx PRIVATE PSPRECOMP_NAMESPACE=font_ PSPRECOMP_TRACE)
target_include_directories(fontprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/s5-font-reloc)
target_link_libraries(fontprx PRIVATE psprecomp)

add_executable(Seto-static host.c umd.c fpl.c font_prx.c $<TARGET_OBJECTS:fontprx>)
target_link_libraries(Seto-static PRIVATE tagforce)
