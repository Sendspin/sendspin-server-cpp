# Embeds a file in a C++ header as a byte array, so a binary serves it with no file beside it.
#
# As a script (cmake -P), with INPUT, OUTPUT and NAME set: writes OUTPUT holding
# `inline constexpr unsigned char NAME[]` with INPUT's bytes and `NAME_SIZE`, their count.
#
# Included, it defines sendspin_server_cpp_embed_asset(TARGET INPUT NAME HEADER), which regenerates
# HEADER (a file name) under TARGET's binary directory whenever INPUT changes, and adds that
# directory to TARGET's include path.

if(CMAKE_SCRIPT_MODE_FILE)
    file(READ "${INPUT}" hex HEX)
    string(LENGTH "${hex}" hex_length)
    math(EXPR size "${hex_length} / 2")
    # One 0x.. per byte, 16 to a line
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){16})" "\\1\n    " bytes "${bytes}")
    file(WRITE "${OUTPUT}.tmp"
        "// Generated from ${INPUT} by cmake/embed_asset.cmake; do not edit\n"
        "#pragma once\n"
        "#include <cstddef>\n"
        "inline constexpr unsigned char ${NAME}[] = {\n    ${bytes}\n};\n"
        "inline constexpr std::size_t ${NAME}_SIZE = ${size};\n")
    file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
    file(REMOVE "${OUTPUT}.tmp")
    return()
endif()

set(SENDSPIN_SERVER_EMBED_ASSET_SCRIPT ${CMAKE_CURRENT_LIST_FILE})

function(sendspin_server_cpp_embed_asset TARGET INPUT NAME HEADER)
    set(dir ${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_assets)
    add_custom_command(
        OUTPUT ${dir}/${HEADER}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${INPUT} -DOUTPUT=${dir}/${HEADER} -DNAME=${NAME}
                -P ${SENDSPIN_SERVER_EMBED_ASSET_SCRIPT}
        DEPENDS ${INPUT} ${SENDSPIN_SERVER_EMBED_ASSET_SCRIPT}
        COMMENT "Embedding ${INPUT}"
        VERBATIM)
    target_sources(${TARGET} PRIVATE ${dir}/${HEADER})
    target_include_directories(${TARGET} PRIVATE ${dir})
endfunction()
