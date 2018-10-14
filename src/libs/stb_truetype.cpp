#include <bx/math.h>
#include <bx/string.h>

#define STBTT_ifloor(x)    (int)bx::floor(x)
#define STBTT_iceil(x)     (int)bx::ceil(x)
#define STBTT_sqrt(x)      bx::sqrt(x)
#define STBTT_pow(x, y)    bx::pow(x, y)
#define STBTT_fmod(x, y)   bx::mod(x, y)
#define STBTT_cos(x)       bx::cos(x)
#define STBTT_acos(x)      bx::acos(x)
#define STBTT_fabs(x)      bx::abs(x)

#define STBTT_strlen(x)    bx::strLen(x)

#define STBTT_memcpy       bx::memCopy
#define STBTT_memset       bx::memSet

BX_PRAGMA_DIAGNOSTIC_IGNORED_MSVC(4189) // local variable is initialized but not referenced
BX_PRAGMA_DIAGNOSTIC_IGNORED_MSVC(4244) // 'argument': conversion from 'XXXX' to 'YYYY', possible loss of data
BX_PRAGMA_DIAGNOSTIC_IGNORED_CLANG_GCC("-Wunused-variable")

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
