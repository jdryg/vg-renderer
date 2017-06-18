#define STB_TRUETYPE_IMPLEMENTATION

#if NVG_CUSTOM_MEMORY_ALLOCATOR
#include "../memory/allocator.h"
#define STBTT_malloc(x, u) M_ALLOC(x)
#define STBTT_free(x, u) M_FREE(x)
#else
#include <malloc.h>
#define STBTT_malloc(x, u) malloc(x)
#define STBTT_free(x, u) free(x)
#endif

#include <stb/stb_truetype.h>