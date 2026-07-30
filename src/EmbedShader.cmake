file(READ "${INPUT}" SHADER_HEX HEX)
string(REGEX REPLACE "(..)" "0x\\1," SHADER_BYTES "${SHADER_HEX}")
file(WRITE "${OUTPUT}" "#pragma once\n#include <cstdint>\nalignas(uint32_t) inline constexpr unsigned char ${VARIABLE}[] = {${SHADER_BYTES}};\n")
