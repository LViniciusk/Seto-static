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

# S9.3: the relocated modstory corpus, loaded when the game asks for
# libstory.prx. Its imports are EBOOT libehsys/libfield exports.
file(GLOB STORY_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/s93-story-reloc/*.c)
if(STORY_SOURCES)
    add_library(storyprx OBJECT ${STORY_SOURCES})
    target_compile_definitions(storyprx PRIVATE
        PSPRECOMP_NAMESPACE=story_ PSPRECOMP_TRACE
        psp_hle_call=story_import_forward)
    target_include_directories(storyprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/s93-story-reloc)
    target_link_libraries(storyprx PRIVATE psprecomp)
endif()

# S9.3: the relocated modfield corpus, loaded when modstory asks for the
# field module. Its imports are libehsys EBOOT exports.
file(GLOB FIELD_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/s93-field-reloc/*.c)
if(FIELD_SOURCES)
    add_library(fieldprx OBJECT ${FIELD_SOURCES})
    target_compile_definitions(fieldprx PRIVATE
        PSPRECOMP_NAMESPACE=field_ PSPRECOMP_TRACE
        psp_hle_call=field_import_forward)
    target_include_directories(fieldprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/s93-field-reloc)
    target_link_libraries(fieldprx PRIVATE psprecomp)
endif()

# S9.5: the relocated moddeck corpus, loaded when Free Duel asks for it.
file(GLOB DECK_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/s93-deck2-reloc/*.c)
if(DECK_SOURCES)
    add_library(deckprx OBJECT ${DECK_SOURCES})
    target_compile_definitions(deckprx PRIVATE
        PSPRECOMP_NAMESPACE=deck_ PSPRECOMP_TRACE
        psp_hle_call=deck_import_forward)
    target_include_directories(deckprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/s93-deck2-reloc)
    target_link_libraries(deckprx PRIVATE psprecomp)
endif()

# S9.7: the relocated moddeck_tutorial corpus, requested after the office exit.
file(GLOB DECKTUT_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/s93-decktut-reloc/*.c)
if(DECKTUT_SOURCES)
    add_library(decktutprx OBJECT ${DECKTUT_SOURCES})
    target_compile_definitions(decktutprx PRIVATE
        PSPRECOMP_NAMESPACE=decktut_ PSPRECOMP_TRACE
        psp_hle_call=decktut_import_forward)
    target_include_directories(decktutprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/s93-decktut-reloc)
    target_link_libraries(decktutprx PRIVATE psprecomp)
endif()

set(SETO_SOURCES host.c host_window.c umd.c fpl.c font_prx.c)
if(TITLE_SOURCES)
    list(APPEND SETO_SOURCES title_prx.c)
endif()
if(STORY_SOURCES)
    list(APPEND SETO_SOURCES story_prx.c)
endif()
if(FIELD_SOURCES)
    list(APPEND SETO_SOURCES field_prx.c)
endif()
if(DECK_SOURCES)
    list(APPEND SETO_SOURCES deck_prx.c)
endif()
if(DECKTUT_SOURCES)
    list(APPEND SETO_SOURCES decktut_prx.c)
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
if(STORY_SOURCES)
    target_sources(Seto-static PRIVATE $<TARGET_OBJECTS:storyprx>)
    target_compile_definitions(Seto-static PRIVATE HAVE_STORY_CORPUS)
else()
    message(STATUS "S9.3 story corpus not found under build/s93-story-reloc; building without modstory")
endif()
if(FIELD_SOURCES)
    target_sources(Seto-static PRIVATE $<TARGET_OBJECTS:fieldprx>)
    target_compile_definitions(Seto-static PRIVATE HAVE_FIELD_CORPUS)
else()
    message(STATUS "S9.3 field corpus not found under build/s93-field-reloc; building without modfield")
endif()
if(DECK_SOURCES)
    target_sources(Seto-static PRIVATE $<TARGET_OBJECTS:deckprx>)
    target_compile_definitions(Seto-static PRIVATE HAVE_DECK_CORPUS)
else()
    message(STATUS "S9.5 deck corpus not found under build/s93-deck2-reloc; building without moddeck")
endif()
if(DECKTUT_SOURCES)
    target_sources(Seto-static PRIVATE $<TARGET_OBJECTS:decktutprx>)
    target_compile_definitions(Seto-static PRIVATE HAVE_DECKTUT_CORPUS)
else()
    message(STATUS "S9.7 deck tutorial corpus not found under build/s93-decktut-reloc; building without moddeck_tutorial")
endif()
target_link_libraries(Seto-static PRIVATE tagforce gdi32)

add_executable(test-lbn-seek EXCLUDE_FROM_ALL tests/lbn_seek.c)
target_link_libraries(test-lbn-seek PRIVATE psprecomp)
