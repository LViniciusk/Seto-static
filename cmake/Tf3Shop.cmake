find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(tf3_pack_extractor "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_tf3_shop_pack.py")
set(tf3_shop_pack "${CMAKE_CURRENT_BINARY_DIR}/tf3-shop-catalog.h")
set(tf3_shop_gx_data "${CMAKE_CURRENT_SOURCE_DIR}/build/gx_data")
add_custom_command(OUTPUT "${tf3_shop_pack}"
    COMMAND "${Python3_EXECUTABLE}" "${tf3_pack_extractor}" "${SETO_TF3_SHOP_ISO}" "${tf3_shop_pack}"
            --gx-data "${tf3_shop_gx_data}"
    DEPENDS "${tf3_pack_extractor}" "${SETO_TF3_SHOP_ISO}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_gx_carddb.py"
            "${tf3_shop_gx_data}/gx_id_map.h"
    VERBATIM)
target_sources(Seto-static PRIVATE "${tf3_shop_pack}")
target_include_directories(Seto-static PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(Seto-static PRIVATE HAVE_TF3_SHOP_PACK)

# The stock trunk rejects internal type 9. TF3 assigns that type to Gemini
# monsters, so only the existing TF1 lookup should run the stock type check.
set(trunk_src "${CMAKE_CURRENT_SOURCE_DIR}/build/reloc/tagforce/tagforce_funcs_045.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${trunk_src}")
file(READ "${trunk_src}" shop_overlay_code)
set(trunk_entry "    /* 0803B36C  beql       $s0, $zero, 0x0803B398 */")
string(FIND "${shop_overlay_code}" "${trunk_entry}" trunk_match)
string(FIND "${shop_overlay_code}" "0x06600000u + (r_s0 - 2480u)" gx_lookup)
if(trunk_match EQUAL -1 OR gx_lookup EQUAL -1)
    message(FATAL_ERROR "Expected the Phase C trunk lookup in ${trunk_src}")
endif()
string(REPLACE "${trunk_entry}"
    "    if (r_s0 >= 2480u && r_s0 < 3480u) goto L_0803B390;\n${trunk_entry}"
    shop_overlay_code "${shop_overlay_code}")
set(newgame_clear "L_0803A6B0: PSP_MARK(0x0803A6B0u);")
string(FIND "${shop_overlay_code}" "${newgame_clear}" newgame_match)
if(newgame_match EQUAL -1)
    message(FATAL_ERROR "Expected the New Game clear block in ${trunk_src}")
endif()
string(REPLACE "${newgame_clear}"
    "${newgame_clear}\n    for (uint32_t _gx_i = 0u; _gx_i < 4000u; _gx_i += 4u) psp_write32(0x06600000u + _gx_i, 0u);"
    shop_overlay_code "${shop_overlay_code}")
set(trunk_output "${CMAKE_CURRENT_BINARY_DIR}/tf3-shop/tagforce_funcs_045.c")
configure_file("${CMAKE_CURRENT_LIST_DIR}/Tf3ShopOverlay.c.in" "${trunk_output}" @ONLY)
get_target_property(eboot_sources tagforce SOURCES)
list(REMOVE_ITEM eboot_sources "${trunk_src}")
list(APPEND eboot_sources "${trunk_output}")
set_property(TARGET tagforce PROPERTY SOURCES "${eboot_sources}")

# Build a patched copy so regenerating the external runtime cannot drop save support.
set(savedata_src "${CMAKE_CURRENT_SOURCE_DIR}/build/deps/psprecomp/src/hle/savedata.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${savedata_src}")
file(READ "${savedata_src}" savedata_code)
function(tf3_savedata_replace old new)
    string(FIND "${savedata_code}" "${old}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "Unexpected psprecomp savedata source while adding the GX trunk")
    endif()
    string(REPLACE "${old}" "${new}" savedata_code "${savedata_code}")
    set(savedata_code "${savedata_code}" PARENT_SCOPE)
endfunction()
tf3_savedata_replace(
    "static uint32_t do_save(uint32_t param"
    "#include \"${CMAKE_CURRENT_SOURCE_DIR}/src/gx_savedata.h\"\n\nstatic uint32_t do_save(uint32_t param")
tf3_savedata_replace(
    [=[        FILE *f = fopen(path, "wb");
        if (!f) { free(tmp); return ERR_SAVE_SAVE_MS_NOSPACE; }]=]
    [=[        const uint8_t *trunk = data_size ? gx_save_trunk(param, file) : NULL;
        if (trunk) {
            int ok = gx_save_write(path, tmp, data_size, trunk);
            free(tmp);
            return ok ? 0 : ERR_SAVE_SAVE_MS_NOSPACE;
        }
        FILE *f = fopen(path, "wb");
        if (!f) { free(tmp); return ERR_SAVE_SAVE_MS_NOSPACE; }]=])
tf3_savedata_replace(
    [=[    uint32_t fsize = (uint32_t)sz;
    if (data_buf) {]=]
    [=[    uint8_t gx_trunk[GX_EXT_TRUNK_SIZE];
    uint8_t *trunk = data_buf ? gx_save_trunk(param, file) : NULL;
    int has_gx_trunk = trunk ? gx_save_read(f, &sz, gx_trunk) : 0;
    if (has_gx_trunk < 0) { fclose(f); return broken; }
    uint32_t fsize = (uint32_t)sz;
    if (data_buf) {]=])
tf3_savedata_replace(
    [=[        psp_write32(param + OFF_DATASIZE, fsize);
    }]=]
    [=[        psp_write32(param + OFF_DATASIZE, fsize);
        if (trunk) {
            if (has_gx_trunk) memcpy(trunk, gx_trunk, GX_EXT_TRUNK_SIZE);
            else gx_trunk_defaults(trunk);
        }
    }]=])
set(patched_savedata "${CMAKE_CURRENT_BINARY_DIR}/tf3_savedata.c")
configure_file("${CMAKE_CURRENT_LIST_DIR}/Tf3Savedata.c.in" "${patched_savedata}" @ONLY)
get_target_property(runtime_sources psprecomp SOURCES)
list(FILTER runtime_sources EXCLUDE REGEX "(^|/)src/hle/savedata\\.c$")
set_property(TARGET psprecomp PROPERTY SOURCES "${runtime_sources};${patched_savedata}")
