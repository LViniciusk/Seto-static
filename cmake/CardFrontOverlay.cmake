# The composer has direct translated callers, so replace its entry before compiling.
set(card_front_source "${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/tagforce/tagforce_funcs_059.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${card_front_source}")
file(READ "${card_front_source}" card_front_code)
set(card_front_entry "L_08051710: PSP_MARK(0x08051710u);")
string(FIND "${card_front_code}" "${card_front_entry}" card_front_at)
if(card_front_at EQUAL -1)
    message(FATAL_ERROR "Unexpected translation of the large card composer")
endif()
set(card_front_hook [=[
    {
        extern int32_t seto_ge_card_front_path(uint32_t texture_addr);
        if (r_a3 && psp_read32(r_a3) == 0x2E47494Du &&
            seto_ge_card_front_path(r_a3 + 0x80u) >= 0) {
            const uint32_t x = r_a1, y = r_a2, caller = r_ra;
            /* Bind the same 128x64 GIM as the art pass. The GE backend
               recognizes the tagged UV and samples the complete front. */
            r_a1 = r_a3;
            psp_func_08031704();
            r_a0 = r_v0;
            r_a1 = 128;
            r_a2 = 64;
            r_a3 = 108;
            r_t0 = 154;
            psp_func_08036F84();
            r_a0 = r_v0;
            r_a1 = x;
            r_a2 = y;
            r_a3 = 0;
            r_t0 = 0x4000;
            r_t1 = r_t2 = r_t3 = 0;
            psp_func_08036D8C();
            r_ra = caller;
            if (psp_ra_is_jump(r_ra)) psp_dispatch(r_ra);
            PSP_SP_CHECK(0x08051880u);
            return;
        }
    }
]=])
string(REPLACE "${card_front_entry}" "${card_front_entry}\n${card_front_hook}"
       card_front_code "${card_front_code}")
set(card_front_output "${CMAKE_CURRENT_BINARY_DIR}/card-front/tagforce_funcs_059.c")
if(EXISTS "${card_front_output}")
    file(READ "${card_front_output}" previous_card_front_code)
endif()
if(NOT card_front_code STREQUAL previous_card_front_code)
    file(WRITE "${card_front_output}" "${card_front_code}")
endif()
list(REMOVE_ITEM TAGFORCE_SOURCES "${card_front_source}")
list(APPEND TAGFORCE_SOURCES "${card_front_output}")
