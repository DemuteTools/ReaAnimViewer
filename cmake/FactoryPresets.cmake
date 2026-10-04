# SPDX-License-Identifier: MIT
#
# Story 10-2 -- the factory presets are compiled into the DLL: every
# presets/factory/*.ravpreset becomes one {stem, text} entry of a generated header, so
# they update with the extension by every route and are read-only by construction.
# Used by the main build and by tests/ (same sources, same header).
function(rav_generate_factory_presets preset_dir template out_file)
    file(GLOB _rav_presets CONFIGURE_DEPENDS "${preset_dir}/*.ravpreset")
    list(SORT _rav_presets)
    set(_entries "")
    foreach(_f IN LISTS _rav_presets)
        get_filename_component(_stem "${_f}" NAME_WE)
        # Editing a preset re-runs the configure step, which regenerates the header (the
        # GLOB's CONFIGURE_DEPENDS only notices files added or removed).
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_f}")
        file(READ "${_f}" _text)
        string(REPLACE "\r\n" "\n" _text "${_text}")
        string(FIND "${_text}" ")RAVPRESET\"" _bad)
        if(NOT _bad EQUAL -1)
            message(FATAL_ERROR "${_f} contains the raw-string delimiter )RAVPRESET\"")
        endif()
        string(LENGTH "${_text}" _len)
        if(_len GREATER 16000)
            message(FATAL_ERROR "${_f} is over 16000 bytes (MSVC string literal limit): split the literal")
        endif()
        string(APPEND _entries "    {\"${_stem}\", R\"RAVPRESET(${_text})RAVPRESET\"},\n")
    endforeach()
    set(RAV_FACTORY_PRESET_ENTRIES "${_entries}")
    configure_file("${template}" "${out_file}" @ONLY)
endfunction()
