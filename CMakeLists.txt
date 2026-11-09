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
     ${CMAKE_CURRENT_SOURCE_DIR}/build/s93-deck-reloc/*.c)
if(DECK_SOURCES)
    add_library(deckprx OBJECT ${DECK_SOURCES})
    target_compile_definitions(deckprx PRIVATE
        PSPRECOMP_NAMESPACE=deck_ PSPRECOMP_TRACE
        psp_hle_call=deck_import_forward)
    target_include_directories(deckprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/s93-deck-reloc)
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

# S9.9: the duel subsystem requested by the first scripted duel. modduel_mgr
# imports libduel_eng/libduel_draw/libdeck_swap/libtutorial, so the six images
# are loaded together into the low window.
file(GLOB DUELMGR_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/s9duel-mgr-reloc/*.c)
file(GLOB DUELENG_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/s9duel-eng-reloc/*.c)
file(GLOB DUELDRAW_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/s9duel-draw-reloc/*.c)
file(GLOB DUELSWAP_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/s9duel-swap-reloc/*.c)
file(GLOB DUELTUT_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/s9duel-tut-reloc/*.c)
file(GLOB DUELREC_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/s9duel-rec-reloc/*.c)
if(DUELMGR_SOURCES)
    add_library(duelmgrprx OBJECT ${DUELMGR_SOURCES})
    target_compile_definitions(duelmgrprx PRIVATE
        PSPRECOMP_NAMESPACE=duelmgr_ PSPRECOMP_TRACE
        psp_hle_call=duel_import_forward)
    target_include_directories(duelmgrprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/s9duel-mgr-reloc)
    target_link_libraries(duelmgrprx PRIVATE psprecomp)
endif()
if(DUELENG_SOURCES)
    add_library(duelengprx OBJECT ${DUELENG_SOURCES})
    target_compile_definitions(duelengprx PRIVATE
        PSPRECOMP_NAMESPACE=dueleng_ PSPRECOMP_TRACE
        psp_hle_call=duel_import_forward)
    target_include_directories(duelengprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/s9duel-eng-reloc)
    target_link_libraries(duelengprx PRIVATE psprecomp)
endif()
if(DUELDRAW_SOURCES)
    add_library(dueldrawprx OBJECT ${DUELDRAW_SOURCES})
    target_compile_definitions(dueldrawprx PRIVATE
        PSPRECOMP_NAMESPACE=dueldraw_ PSPRECOMP_TRACE
        psp_hle_call=duel_import_forward)
    target_include_directories(dueldrawprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/s9duel-draw-reloc)
    target_link_libraries(dueldrawprx PRIVATE psprecomp)
endif()
if(DUELSWAP_SOURCES)
    add_library(duelswapprx OBJECT ${DUELSWAP_SOURCES})
    target_compile_definitions(duelswapprx PRIVATE
        PSPRECOMP_NAMESPACE=duelswap_ PSPRECOMP_TRACE
        psp_hle_call=duel_import_forward)
    target_include_directories(duelswapprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/s9duel-swap-reloc)
    target_link_libraries(duelswapprx PRIVATE psprecomp)
endif()
if(DUELTUT_SOURCES)
    add_library(dueltutprx OBJECT ${DUELTUT_SOURCES})
    target_compile_definitions(dueltutprx PRIVATE
        PSPRECOMP_NAMESPACE=dueltut_ PSPRECOMP_TRACE
        psp_hle_call=duel_import_forward)
    target_include_directories(dueltutprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/s9duel-tut-reloc)
    target_link_libraries(dueltutprx PRIVATE psprecomp)
endif()
if(DUELREC_SOURCES)
    add_library(duelrecprx OBJECT ${DUELREC_SOURCES})
    target_compile_definitions(duelrecprx PRIVATE
        PSPRECOMP_NAMESPACE=duelrec_ PSPRECOMP_TRACE
        psp_hle_call=duel_import_forward)
    target_include_directories(duelrecprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/s9duel-rec-reloc)
    target_link_libraries(duelrecprx PRIVATE psprecomp)
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
if(DUELMGR_SOURCES AND DUELENG_SOURCES AND DUELDRAW_SOURCES AND
   DUELSWAP_SOURCES AND DUELTUT_SOURCES AND DUELREC_SOURCES)
    list(APPEND SETO_SOURCES duel_prx.c)
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
    message(STATUS "S9.5 deck corpus not found under build/s93-deck-reloc; building without moddeck")
endif()
if(DECKTUT_SOURCES)
    target_sources(Seto-static PRIVATE $<TARGET_OBJECTS:decktutprx>)
    target_compile_definitions(Seto-static PRIVATE HAVE_DECKTUT_CORPUS)
else()
    message(STATUS "S9.7 deck tutorial corpus not found under build/s93-decktut-reloc; building without moddeck_tutorial")
endif()
if(DUELMGR_SOURCES AND DUELENG_SOURCES AND DUELDRAW_SOURCES AND
   DUELSWAP_SOURCES AND DUELTUT_SOURCES AND DUELREC_SOURCES)
    target_sources(Seto-static PRIVATE
        $<TARGET_OBJECTS:duelmgrprx> $<TARGET_OBJECTS:duelengprx>
        $<TARGET_OBJECTS:dueldrawprx> $<TARGET_OBJECTS:duelswapprx>
        $<TARGET_OBJECTS:dueltutprx> $<TARGET_OBJECTS:duelrecprx>)
    target_compile_definitions(Seto-static PRIVATE HAVE_DUEL_CORPUS)
else()
    message(STATUS "S9.9 duel corpora not found under build/s9duel-*-reloc; building without the duel subsystem")
endif()
target_link_libraries(Seto-static PRIVATE tagforce gdi32)

add_executable(test-lbn-seek EXCLUDE_FROM_ALL tests/lbn_seek.c)
target_link_libraries(test-lbn-seek PRIVATE psprecomp)
