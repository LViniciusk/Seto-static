cmake_minimum_required(VERSION 3.16)
project(SetoStatic C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_program(SETO_CCACHE ccache)
if(SETO_CCACHE AND NOT CMAKE_C_COMPILER_LAUNCHER)
    set(CMAKE_C_COMPILER_LAUNCHER "${SETO_CCACHE}")
    message(STATUS "Using ccache: ${SETO_CCACHE}")
endif()

# Recompiled MIPS-to-C corpora see little FPS benefit from -O3 vectorization
# while compile times and object sizes explode; keep -O1 on corpora and -O3 on src/ & psprecomp.
function(seto_corpus_target_opts tgt)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${tgt} PRIVATE -O1)
        if(WIN32)
            get_target_property(_srcs ${tgt} SOURCES)
            foreach(_f IN LISTS _srcs)
                if(_f MATCHES "_register\\.c$")
                    set_source_files_properties(${_f} PROPERTIES COMPILE_OPTIONS "-O1;-Wa,-mbig-obj")
                endif()
            endforeach()
        endif()
    endif()
endfunction()

# psprecomp at 5ca18fac, with the local patches applied (see README).
# The game's real threads need the opt-in scheduler.
set(PSPRECOMP_REAL_THREADS ON)
add_subdirectory(build/deps/psprecomp psprecomp)

# C emitted from the real EBOOT by `allegrexrecomp emit` (see README).
file(GLOB TAGFORCE_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/tagforce/*.c)
include(cmake/CardFrontOverlay.cmake)
add_library(tagforce STATIC ${TAGFORCE_SOURCES})
seto_corpus_target_opts(tagforce)
target_include_directories(tagforce PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/tagforce)
# Function-entry tracing is what proves translated code ran.
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/tagforce/tagforce_funcs_000.c ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/tagforce/tagforce_funcs_040.c PROPERTIES COMPILE_DEFINITIONS PSPRECOMP_TRACE)
target_link_libraries(tagforce PUBLIC psprecomp)

# Font PRX corpus (sceFont_Library). Without it the EBOOT-only host still builds.
file(GLOB FONT_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/font/*.c)

if(FONT_SOURCES)
    add_library(fontprx OBJECT ${FONT_SOURCES})
    seto_corpus_target_opts(fontprx)
    target_compile_definitions(fontprx PRIVATE PSPRECOMP_NAMESPACE=font_)
    target_include_directories(fontprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/font)
    target_link_libraries(fontprx PRIVATE psprecomp)
endif()

# Relocated modtitle corpus. Its imports are not firmware NIDs but
# EBOOT exports, so every stub is routed through title_import_forward().
file(GLOB TITLE_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/title/*.c)
if(TITLE_SOURCES)
    add_library(titleprx OBJECT ${TITLE_SOURCES})
    seto_corpus_target_opts(titleprx)
    target_compile_definitions(titleprx PRIVATE
        PSPRECOMP_NAMESPACE=title_
        psp_hle_call=title_import_forward)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/title/title_funcs_000.c ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/title/title_funcs_005.c PROPERTIES COMPILE_DEFINITIONS PSPRECOMP_TRACE)
    target_include_directories(titleprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/title)
    target_link_libraries(titleprx PRIVATE psprecomp)
endif()

# Relocated modstory corpus, loaded when the game asks for libstory.prx.
file(GLOB STORY_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/story/*.c)
if(STORY_SOURCES)
    add_library(storyprx OBJECT ${STORY_SOURCES})
    seto_corpus_target_opts(storyprx)
    target_compile_definitions(storyprx PRIVATE
        PSPRECOMP_NAMESPACE=story_
        psp_hle_call=story_import_forward)
    target_include_directories(storyprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/story)
    target_link_libraries(storyprx PRIVATE psprecomp)
endif()

# Relocated modfield corpus, loaded when modstory asks for the field module.
file(GLOB FIELD_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/field/*.c)
if(FIELD_SOURCES)
    add_library(fieldprx OBJECT ${FIELD_SOURCES})
    seto_corpus_target_opts(fieldprx)
    target_compile_definitions(fieldprx PRIVATE
        PSPRECOMP_NAMESPACE=field_
        psp_hle_call=field_import_forward)
    target_include_directories(fieldprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/field)
    target_link_libraries(fieldprx PRIVATE psprecomp)
endif()

# Relocated moddeck_tutorial corpus, requested after the office exit.
file(GLOB DECKTUT_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/decktut/*.c)
if(DECKTUT_SOURCES)
    add_library(decktutprx OBJECT ${DECKTUT_SOURCES})
    seto_corpus_target_opts(decktutprx)
    target_compile_definitions(decktutprx PRIVATE
        PSPRECOMP_NAMESPACE=decktut_
        psp_hle_call=decktut_import_forward)
    target_include_directories(decktutprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/decktut)
    target_link_libraries(decktutprx PRIVATE psprecomp)
endif()

# Duel subsystem PRXs (modduel_mgr imports libduel_eng/libduel_draw/libdeck_swap/libtutorial).
file(GLOB DUELMGR_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/duel-mgr/*.c)
file(GLOB DUELENG_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/duel-eng/*.c)
file(GLOB TF3ENG_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/duel-tf3eng/*.c)
file(GLOB DUELDRAW_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/duel-draw/*.c)
file(GLOB DUELSWAP_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/duel-swap/*.c)
file(GLOB DUELTUT_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/duel-tut/*.c)
file(GLOB DUELREC_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/duel-rec/*.c)

# Relocated moddeck corpus, loaded when Free Duel asks for it.
file(GLOB DECK_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/deck/*.c)
if(DECK_SOURCES)
    if(TF3ENG_SOURCES AND DUELMGR_SOURCES AND DUELDRAW_SOURCES AND
       DUELSWAP_SOURCES AND DUELTUT_SOURCES AND DUELREC_SOURCES)
        include(cmake/Tf3ExtraDeck.cmake)
    endif()
    add_library(deckprx OBJECT ${DECK_SOURCES})
    seto_corpus_target_opts(deckprx)
    target_compile_definitions(deckprx PRIVATE
        PSPRECOMP_NAMESPACE=deck_
        psp_hle_call=deck_import_forward)
    target_include_directories(deckprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/deck)
    target_link_libraries(deckprx PRIVATE psprecomp)
endif()

if(DUELMGR_SOURCES)
    add_library(duelmgrprx OBJECT ${DUELMGR_SOURCES})
    seto_corpus_target_opts(duelmgrprx)
    target_compile_definitions(duelmgrprx PRIVATE
        PSPRECOMP_NAMESPACE=duelmgr_
        psp_hle_call=duel_import_forward)
    target_include_directories(duelmgrprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/duel-mgr)
    target_link_libraries(duelmgrprx PRIVATE psprecomp)
endif()
if(TF3ENG_SOURCES)
    add_library(tf3engprx OBJECT ${TF3ENG_SOURCES})
    seto_corpus_target_opts(tf3engprx)
    target_compile_definitions(tf3engprx PRIVATE
        PSPRECOMP_NAMESPACE=tf3eng_
        psp_hle_call=tf3eng_import_forward)
    target_include_directories(tf3engprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/duel-tf3eng)
    target_link_libraries(tf3engprx PRIVATE psprecomp)
endif()
if(DUELENG_SOURCES)
    add_library(duelengprx OBJECT ${DUELENG_SOURCES})
    seto_corpus_target_opts(duelengprx)
    target_compile_definitions(duelengprx PRIVATE
        PSPRECOMP_NAMESPACE=dueleng_
        psp_hle_call=duel_import_forward)
    target_include_directories(duelengprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/duel-eng)
    target_link_libraries(duelengprx PRIVATE psprecomp)
endif()
if(DUELDRAW_SOURCES)
    add_library(dueldrawprx OBJECT ${DUELDRAW_SOURCES})
    seto_corpus_target_opts(dueldrawprx)
    target_compile_definitions(dueldrawprx PRIVATE
        PSPRECOMP_NAMESPACE=dueldraw_
        psp_hle_call=duel_import_forward)
    target_include_directories(dueldrawprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/duel-draw)
    target_link_libraries(dueldrawprx PRIVATE psprecomp)
endif()
if(DUELSWAP_SOURCES)
    add_library(duelswapprx OBJECT ${DUELSWAP_SOURCES})
    seto_corpus_target_opts(duelswapprx)
    target_compile_definitions(duelswapprx PRIVATE
        PSPRECOMP_NAMESPACE=duelswap_
        psp_hle_call=duel_import_forward)
    target_include_directories(duelswapprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/duel-swap)
    target_link_libraries(duelswapprx PRIVATE psprecomp)
endif()
if(DUELTUT_SOURCES)
    add_library(dueltutprx OBJECT ${DUELTUT_SOURCES})
    seto_corpus_target_opts(dueltutprx)
    target_compile_definitions(dueltutprx PRIVATE
        PSPRECOMP_NAMESPACE=dueltut_
        psp_hle_call=duel_import_forward)
    target_include_directories(dueltutprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/duel-tut)
    target_link_libraries(dueltutprx PRIVATE psprecomp)
endif()
if(DUELREC_SOURCES)
    add_library(duelrecprx OBJECT ${DUELREC_SOURCES})
    seto_corpus_target_opts(duelrecprx)
    target_compile_definitions(duelrecprx PRIVATE
        PSPRECOMP_NAMESPACE=duelrec_
        psp_hle_call=duel_import_forward)
    target_include_directories(duelrecprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/duel-rec)
    target_link_libraries(duelrecprx PRIVATE psprecomp)
endif()

# Store & character-list overlays loaded when talking to the card shopkeeper.
file(GLOB CHARALIST_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/charalist/*.c)
file(GLOB SHOP_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/shop/*.c)
if(CHARALIST_SOURCES)
    add_library(charalistprx OBJECT ${CHARALIST_SOURCES})
    seto_corpus_target_opts(charalistprx)
    target_compile_definitions(charalistprx PRIVATE
        PSPRECOMP_NAMESPACE=charalist_
        psp_hle_call=store_import_forward)
    target_include_directories(charalistprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/charalist)
    target_link_libraries(charalistprx PRIVATE psprecomp)
endif()
if(SHOP_SOURCES)
    set(SETO_TF3_SHOP_ISO "${CMAKE_CURRENT_SOURCE_DIR}/TF3.iso" CACHE FILEPATH
        "ULES-01183 ISO used to import the Pure GX shop catalogue")
    if(TF3ENG_SOURCES AND EXISTS "${SETO_TF3_SHOP_ISO}")
        include(cmake/Tf3ShopOverlay.cmake)
    endif()
    add_library(shopprx OBJECT ${SHOP_SOURCES})
    seto_corpus_target_opts(shopprx)
    target_compile_definitions(shopprx PRIVATE
        PSPRECOMP_NAMESPACE=shop_
        psp_hle_call=store_import_forward)
    target_include_directories(shopprx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/shop)
    target_link_libraries(shopprx PRIVATE psprecomp)
endif()

set(EXTRA_TARGETS)
set(EXTRA_DEFS)
macro(add_extra_corpus slug prefix macro_name)
    file(GLOB _SRCS "${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/${slug}/${prefix}_*.c")
    if(_SRCS)
        add_library(${prefix}prx OBJECT ${_SRCS})
        seto_corpus_target_opts(${prefix}prx)
        target_compile_definitions(${prefix}prx PRIVATE
            PSPRECOMP_NAMESPACE=${prefix}_
            psp_hle_call=extra_import_forward)
        target_include_directories(${prefix}prx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/${slug})
        target_link_libraries(${prefix}prx PRIVATE psprecomp)
        list(APPEND EXTRA_TARGETS ${prefix}prx)
        list(APPEND EXTRA_DEFS HAVE_${macro_name}_CORPUS)
    endif()
endmacro()
add_extra_corpus(cardalbum    cardalbum    CARDALBUM)
add_extra_corpus(limitlist    limitlist    LIMITLIST)
add_extra_corpus(recipeviewer recipeviewer RECIPEVIEWER)
add_extra_corpus(tutoriallist tutoriallist TUTORIALLIST)
add_extra_corpus(selectcard   selectcard   SELECTCARD)
add_extra_corpus(labo         labo         LABO)
add_extra_corpus(conv         convmachine  CONVMACHINE)
add_extra_corpus(password     password     PASSWORD)
add_extra_corpus(htmlview     htmlview     HTMLVIEW)

set(SETO_SOURCES
    src/host.c src/gl_present.c src/ge_queue.c src/ge_backend.c
    src/umd.c src/fpl.c src/font_prx.c src/multimedia.c src/seto_net.c)
if(WIN32)
    list(APPEND SETO_SOURCES src/host_window.c)
else()
    list(APPEND SETO_SOURCES src/host_window_linux.c)
endif()
if(TITLE_SOURCES)
    list(APPEND SETO_SOURCES src/title_prx.c)
endif()
if(STORY_SOURCES)
    list(APPEND SETO_SOURCES src/story_prx.c)
endif()
if(FIELD_SOURCES)
    list(APPEND SETO_SOURCES src/field_prx.c)
endif()
if(DECK_SOURCES)
    list(APPEND SETO_SOURCES src/deck_prx.c)
endif()
if(DECKTUT_SOURCES)
    list(APPEND SETO_SOURCES src/decktut_prx.c)
endif()
if(DUELMGR_SOURCES AND (DUELENG_SOURCES OR TF3ENG_SOURCES) AND DUELDRAW_SOURCES AND
   DUELSWAP_SOURCES AND DUELTUT_SOURCES AND DUELREC_SOURCES)
    list(APPEND SETO_SOURCES src/duel_prx.c)
    if(TF3ENG_SOURCES)
        list(APPEND SETO_SOURCES src/tf3_bridge.c)
    endif()
endif()
if(CHARALIST_SOURCES OR SHOP_SOURCES)
    list(APPEND SETO_SOURCES src/store_prx.c)
endif()
if(EXTRA_TARGETS)
    list(APPEND SETO_SOURCES src/extra_prx.c)
endif()
add_executable(Seto-static ${SETO_SOURCES})
target_include_directories(Seto-static PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(FONT_SOURCES)
    target_sources(Seto-static PRIVATE $<TARGET_OBJECTS:fontprx>)
    target_compile_definitions(Seto-static PRIVATE HAVE_FONT_CORPUS)
else()
    message(STATUS "font corpus not found under build/reloc/font; building the EBOOT-only host")
endif()
if(TITLE_SOURCES)
    target_sources(Seto-static PRIVATE $<TARGET_OBJECTS:titleprx>)
    target_compile_definitions(Seto-static PRIVATE HAVE_TITLE_CORPUS)
else()
    message(STATUS "title corpus not found under build/reloc/title; building without modtitle")
endif()
if(STORY_SOURCES)
    target_sources(Seto-static PRIVATE $<TARGET_OBJECTS:storyprx>)
    target_compile_definitions(Seto-static PRIVATE HAVE_STORY_CORPUS)
else()
    message(STATUS "story corpus not found under build/reloc/story; building without modstory")
endif()
if(FIELD_SOURCES)
    target_sources(Seto-static PRIVATE $<TARGET_OBJECTS:fieldprx>)
    target_compile_definitions(Seto-static PRIVATE HAVE_FIELD_CORPUS)
else()
    message(STATUS "field corpus not found under build/reloc/field; building without modfield")
endif()
if(DECK_SOURCES)
    target_sources(Seto-static PRIVATE $<TARGET_OBJECTS:deckprx>)
    target_compile_definitions(Seto-static PRIVATE HAVE_DECK_CORPUS)
else()
    message(STATUS "deck corpus not found under build/reloc/deck; building without moddeck")
endif()
if(DECKTUT_SOURCES)
    target_sources(Seto-static PRIVATE $<TARGET_OBJECTS:decktutprx>)
    target_compile_definitions(Seto-static PRIVATE HAVE_DECKTUT_CORPUS)
else()
    message(STATUS "deck tutorial corpus not found under build/reloc/decktut; building without moddeck_tutorial")
endif()
if(DUELMGR_SOURCES AND (DUELENG_SOURCES OR TF3ENG_SOURCES) AND DUELDRAW_SOURCES AND
   DUELSWAP_SOURCES AND DUELTUT_SOURCES AND DUELREC_SOURCES)
    target_sources(Seto-static PRIVATE
        $<TARGET_OBJECTS:duelmgrprx>
        $<TARGET_OBJECTS:dueldrawprx> $<TARGET_OBJECTS:duelswapprx>
        $<TARGET_OBJECTS:dueltutprx> $<TARGET_OBJECTS:duelrecprx>)
    if(TF3ENG_SOURCES)
        target_sources(Seto-static PRIVATE $<TARGET_OBJECTS:tf3engprx>)
        target_compile_definitions(Seto-static PRIVATE HAVE_DUEL_CORPUS HAVE_TF3ENG_CORPUS)
        if(DUELENG_SOURCES)
            target_sources(Seto-static PRIVATE $<TARGET_OBJECTS:duelengprx>)
            target_compile_definitions(Seto-static PRIVATE HAVE_DUELENG_CORPUS)
        endif()
    else()
        target_sources(Seto-static PRIVATE $<TARGET_OBJECTS:duelengprx>)
        target_compile_definitions(Seto-static PRIVATE HAVE_DUEL_CORPUS HAVE_DUELENG_CORPUS)
    endif()
else()
    message(STATUS "duel corpora not found under build/reloc/duel-*; building without the duel subsystem")
endif()
if(CHARALIST_SOURCES)
    target_sources(Seto-static PRIVATE $<TARGET_OBJECTS:charalistprx>)
    target_compile_definitions(Seto-static PRIVATE HAVE_CHARALIST_CORPUS)
else()
    message(STATUS "charalist corpus not found under build/reloc/charalist; building without modcharalist")
endif()
if(SHOP_SOURCES)
    target_sources(Seto-static PRIVATE $<TARGET_OBJECTS:shopprx>)
    if(TF3ENG_SOURCES AND EXISTS "${SETO_TF3_SHOP_ISO}")
        include(cmake/Tf3Shop.cmake)
    endif()
    target_compile_definitions(Seto-static PRIVATE HAVE_SHOP_CORPUS)
else()
    message(STATUS "shop corpus not found under build/reloc/shop; building without modshop")
endif()
if(EXTRA_TARGETS)
    foreach(_t IN LISTS EXTRA_TARGETS)
        target_sources(Seto-static PRIVATE $<TARGET_OBJECTS:${_t}>)
    endforeach()
    target_compile_definitions(Seto-static PRIVATE HAVE_EXTRA_CORPUS ${EXTRA_DEFS})
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libswresample libswscale libavutil)
if(WIN32)
    target_link_libraries(Seto-static PRIVATE tagforce gdi32 winmm ws2_32 gdiplus opengl32 PkgConfig::FFMPEG)
else()
    find_package(Threads REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)
    pkg_check_modules(GL REQUIRED IMPORTED_TARGET gl)
    target_compile_definitions(Seto-static PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
    target_link_libraries(Seto-static PRIVATE tagforce PkgConfig::SDL2 PkgConfig::PNG PkgConfig::GL PkgConfig::FFMPEG Threads::Threads m ${CMAKE_DL_LIBS})
endif()
find_program(SETO_LLD NAMES ld.lld lld)
if(SETO_LLD AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(Seto-static PRIVATE -fuse-ld=lld)
    message(STATUS "Using lld linker: ${SETO_LLD}")
endif()

add_executable(test-lbn-seek EXCLUDE_FROM_ALL tests/lbn_seek.c)
target_link_libraries(test-lbn-seek PRIVATE psprecomp)

add_executable(test-ge-queue EXCLUDE_FROM_ALL tests/ge_queue.c src/ge_queue.c)
target_include_directories(test-ge-queue PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}
                           ${CMAKE_CURRENT_SOURCE_DIR}/src
                           ${CMAKE_CURRENT_SOURCE_DIR}/build/deps/psprecomp/include)

add_executable(bench-render EXCLUDE_FROM_ALL tests/render_bench.c)
target_link_libraries(bench-render PRIVATE psprecomp)

add_executable(test-seto-net EXCLUDE_FROM_ALL tests/seto_net_test.c src/seto_net.c)
target_include_directories(test-seto-net PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test-seto-net PRIVATE psprecomp $<$<BOOL:${WIN32}>:ws2_32>)
