#define STB_TRUETYPE_IMPLEMENTATION

#include "../memory/allocator.h"
#define STBTT_malloc(x, u) M_ALLOC(x)
#define STBTT_free(x, u) M_FREE(x)

#include <stb/stb_truetype.h>