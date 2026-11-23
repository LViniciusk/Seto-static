# Direct calls in the translated PRX bypass dispatch, so inject at the entries.
function(tf3_shop_entries source_name)
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/shop/${source_name}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
    file(READ "${source}" code)
    math(EXPR last "${ARGC} - 1")
    foreach(index RANGE 1 ${last} 2)
        math(EXPR body_index "${index} + 1")
        set(pc "${ARGV${index}}")
        set(entry "L_${pc}: PSP_MARK(0x${pc}u);")
        string(FIND "${code}" "${entry}" at)
        if(at EQUAL -1)
            message(FATAL_ERROR "Unexpected modshop translation at ${pc} in ${source}")
        endif()
        string(REPLACE "${entry}" "${entry}\n${ARGV${body_index}}\n" code "${code}")
    endforeach()
    set(output "${CMAKE_CURRENT_BINARY_DIR}/tf3-shop/${source_name}")
    set(shop_overlay_code "${code}")
    configure_file("${CMAKE_CURRENT_LIST_DIR}/Tf3ShopOverlay.c.in" "${output}" @ONLY)
    list(REMOVE_ITEM SHOP_SOURCES "${source}")
    list(APPEND SHOP_SOURCES "${output}")
    set(SHOP_SOURCES "${SHOP_SOURCES}" PARENT_SCOPE)
endfunction()

set(shop_text_entry [=[
    {
        extern uint32_t gx_shop_text(unsigned, unsigned);
        uint32_t text = gx_shop_text(r_a0 & 0xFFFFu, SHOP_TEXT_KIND);
        if (text) {
            r_v0 = text;
            if (psp_ra_is_jump(r_ra)) psp_dispatch(r_ra);
            return;
        }
    }
]=])
string(REPLACE "SHOP_TEXT_KIND" "0" shop_name_entry "${shop_text_entry}")
string(REPLACE "SHOP_TEXT_KIND" "1u + (r_a1 == 1u)" shop_desc_entry "${shop_text_entry}")
tf3_shop_entries(shop_funcs_002.c 03606A3C "${shop_name_entry}" 03606A88 "${shop_desc_entry}")
tf3_shop_entries(shop_funcs_000.c 03600848 [=[
    {
        extern uint32_t gx_shop_grid(unsigned);
        uint32_t grid = gx_shop_grid(r_a0);
        if (grid) {
            r_v0 = grid;
            if (psp_ra_is_jump(r_ra)) psp_dispatch(r_ra);
            return;
        }
    }
]=] 03600C50 [=[
    {
        extern uint32_t gx_shop_cover(uint32_t, unsigned);
        uint32_t cover = gx_shop_cover(r_a0, r_a1);
        if (cover) {
            r_v0 = cover;
            if (psp_ra_is_jump(r_ra)) psp_dispatch(r_ra);
            return;
        }
    }
]=])
tf3_shop_entries(shop_funcs_001.c 03602524 [=[
    {
        extern int gx_shop_refresh_cursor(void);
        if (gx_shop_refresh_cursor()) r_s0 = UINT16_MAX;
    }
]=])
tf3_shop_entries(shop_funcs_003.c 03606D08 [=[
    {
        extern int gx_shop_draw(unsigned, uint32_t);
        if (gx_shop_draw(r_a0 & 0xFFFFu, r_a1)) {
            if (psp_ra_is_jump(r_ra)) psp_dispatch(r_ra);
            return;
        }
    }
]=])
