# The TF1 moddeck PRX is translated to C, so changing its mapped MIPS bytes
# would leave the executed checks unchanged. Patch the generated C instead.
# Apply all edits before writing the generated copies, so reconfigure is stable.
function(tf3_limit_deck_source source_name)
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/deck/${source_name}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
    file(READ "${source}" code)
    math(EXPR last "${ARGC} - 1")
    foreach(index RANGE 1 ${last} 3)
        math(EXPR old_index "${index} + 1")
        math(EXPR new_index "${index} + 2")
        set(pc "${ARGV${index}}")
        set(old "${ARGV${old_index}}")
        set(new "${ARGV${new_index}}")
        string(REGEX MATCH "/\\* ${pc} [^\n]*\\*/[ \t\r\n]+" prefix "${code}")
        string(FIND "${code}" "${prefix}${old}" position)
        if(NOT prefix OR position EQUAL -1)
            message(FATAL_ERROR "Unexpected moddeck translation at ${pc} in ${source}")
        endif()
        string(REPLACE "${prefix}${old}" "${prefix}${new}" code "${code}")
    endforeach()

    set(output "${CMAKE_CURRENT_BINARY_DIR}/tf3-deck/${source_name}")
    if(EXISTS "${output}")
        file(READ "${output}" existing)
    endif()
    if(NOT code STREQUAL existing)
        file(WRITE "${output}" "${code}")
    endif()
    list(REMOVE_ITEM DECK_SOURCES "${source}")
    list(APPEND DECK_SOURCES "${output}")
    set(DECK_SOURCES "${DECK_SOURCES}" PARENT_SCOPE)
endfunction()

tf3_limit_deck_source(deck_funcs_000.c
    01400198 "r_a0 = r_s0 & 0xFFFFu" [[
    extern int psp_deck_card_is_token(uint32_t kid);
    if (psp_deck_card_is_token(r_s0)) {
        r_s0 += 1;
        goto L_01400274;
    }
    r_a0 = r_s0 & 0xFFFFu]])
tf3_limit_deck_source(deck_funcs_001.c
    014025AC "r_v0 = psp_sltu(r_v0, (uint32_t)35)" "r_v0 = psp_sltu(r_v0, (uint32_t)15)"
    014033B4 "r_v0 = psp_sltu(r_v0, (uint32_t)35)" "r_v0 = psp_sltu(r_v0, (uint32_t)15)")
tf3_limit_deck_source(deck_funcs_002.c
    0140672C "r_at = psp_slt(r_a1, (uint32_t)31)" "r_at = psp_slt(r_a1, (uint32_t)16)"
    # Input is enabled and no native dialog is active at this point.
    014053D4 "r_v0 = psp_read16(r_v0 + -7120)" [[
    extern int psp_deck_warn_extra_on_open(void);
    if (psp_deck_warn_extra_on_open()) {
        r_v0 = 0;
        r_ra = psp_read32(r_sp + 12);
        goto L_01405494;
    }
    r_v0 = psp_read16(r_v0 + -7120)]])
tf3_limit_deck_source(deck_funcs_003.c
    01409840 "r_a1 = r_v0 | 0xE000u" "r_a1 = 0x00010000u"
    014099C8 "r_s0 = r_v0 + r_zero" [[
    extern uint32_t psp_deck_all_ehp_size(uint32_t iso_size);
    r_s0 = psp_deck_all_ehp_size(r_v0)]])
tf3_limit_deck_source(deck_funcs_005.c
    01410084 "r_at = psp_sltu(r_v0, (uint32_t)31)" "r_at = psp_sltu(r_v0, (uint32_t)16)")
