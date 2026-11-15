# cmake -DIN=<binary> -DOUT=<header> -DNAME=<symbol> -P BinToHeader.cmake
file(READ "${IN}" _hex HEX)
string(LENGTH "${_hex}" _len)
math(EXPR _bytes "${_len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _arr "${_hex}")
string(REGEX REPLACE "((0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,))" "\\1\n" _arr "${_arr}")
file(WRITE "${OUT}" "// generated from ${IN} - do not edit\n#pragma once\ninline constexpr unsigned char ${NAME}[${_bytes}] = {\n${_arr}\n};\n")
