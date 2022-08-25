#ifndef VG_CONFIG_H
#define VG_CONFIG_H

#ifndef VG_CONFIG_DEBUG
#	define VG_CONFIG_DEBUG 0
#endif

#ifndef VG_CONFIG_ENABLE_SHAPE_CACHING
#	define VG_CONFIG_ENABLE_SHAPE_CACHING 1
#endif

#ifndef VG_CONFIG_ENABLE_SIMD
#	define VG_CONFIG_ENABLE_SIMD 1
#endif

#ifndef VG_CONFIG_FORCE_AA_OFF
#	define VG_CONFIG_FORCE_AA_OFF 0
#endif

#ifndef VG_CONFIG_LIBTESS2_SCRATCH_BUFFER
#	define VG_CONFIG_LIBTESS2_SCRATCH_BUFFER (4 * 1024 * 1024) // Set to 0 to let libtess2 use malloc/free
#endif

#ifndef VG_CONFIG_UV_INT16
#	define VG_CONFIG_UV_INT16 1
#endif

// If set to 1, submitCommandList() calls pustState()/popState() and resetClip() before and after
// executing the commands. Otherwise, the state produced by the command list will affect the global
// state after the execution of the commands.
#ifndef VG_CONFIG_COMMAND_LIST_PRESERVE_STATE
#	define VG_CONFIG_COMMAND_LIST_PRESERVE_STATE 0
#endif

// NOTE: beginCommandList()/endCommandList() blocks require an indirect jump for each function/path command,
// because they change the Context' vtable. If this is set to 0, all functions call their implementation 
// directly (i.e. there will probably still be a jump there but it'll be unconditional/direct).
// If you care about perf so much that an indirect unconditional jump is a problem for you, or if you aren't
// planning on using command lists at all, set this to 0 and use only clXXX functions to build command lists. 
#ifndef VG_CONFIG_COMMAND_LIST_BEGIN_END_API
#	define VG_CONFIG_COMMAND_LIST_BEGIN_END_API 1
#endif

#define VG_EPSILON 1e-5f

#define VG_COLOR_RED_Pos     0
#define VG_COLOR_RED_Msk     (0xFFu << VG_COLOR_RED_Pos)
#define VG_COLOR_GREEN_Pos   8
#define VG_COLOR_GREEN_Msk   (0xFFu << VG_COLOR_GREEN_Pos)
#define VG_COLOR_BLUE_Pos    16
#define VG_COLOR_BLUE_Msk    (0xFFu << VG_COLOR_BLUE_Pos)
#define VG_COLOR_ALPHA_Pos   24
#define VG_COLOR_ALPHA_Msk   (0xFFu << VG_COLOR_ALPHA_Pos)
#define VG_COLOR_RGB_Msk     (VG_COLOR_RED_Msk | VG_COLOR_GREEN_Msk | VG_COLOR_BLUE_Msk)

#define VG_COLOR32(r, g, b, a) (0 \
	| (((uint32_t)(r) << VG_COLOR_RED_Pos) & VG_COLOR_RED_Msk) \
	| (((uint32_t)(g) << VG_COLOR_GREEN_Pos) & VG_COLOR_GREEN_Msk) \
	| (((uint32_t)(b) << VG_COLOR_BLUE_Pos) & VG_COLOR_BLUE_Msk) \
	| (((uint32_t)(a) << VG_COLOR_ALPHA_Pos) & VG_COLOR_ALPHA_Msk) \
)

#define VG_TEXT_ALIGN_VER_Pos 0
#define VG_TEXT_ALIGN_VER_Msk (0x03u << VG_TEXT_ALIGN_VER_Pos)
#define VG_TEXT_ALIGN_HOR_Pos 2
#define VG_TEXT_ALIGN_HOR_Msk (0x03u << VG_TEXT_ALIGN_HOR_Pos)
#define VG_TEXT_ALIGN(hor, ver)  (0 \
	| (((uint32_t)(hor) << VG_TEXT_ALIGN_HOR_Pos) & VG_TEXT_ALIGN_HOR_Msk) \
	| (((uint32_t)(ver) << VG_TEXT_ALIGN_VER_Pos) & VG_TEXT_ALIGN_VER_Msk) \
)

#define VG_STROKE_FLAGS_LINE_JOIN_Pos   0
#define VG_STROKE_FLAGS_LINE_JOIN_Msk   (0x03u << VG_STROKE_FLAGS_LINE_JOIN_Pos)
#define VG_STROKE_FLAGS_LINE_CAP_Pos    2
#define VG_STROKE_FLAGS_LINE_CAP_Msk    (0x03u << VG_STROKE_FLAGS_LINE_CAP_Pos)
#define VG_STROKE_FLAGS_AA_Pos          4
#define VG_STROKE_FLAGS_AA_Msk          (0x01u << VG_STROKE_FLAGS_AA_Pos)
#define VG_STROKE_FLAGS_FIXED_WIDTH_Pos 5
#define VG_STROKE_FLAGS_FIXED_WIDTH_Msk (0x01u << VG_STROKE_FLAGS_FIXED_WIDTH_Pos)
#define VG_STROKE_FLAGS(cap, join, aa) (0 \
	| (((uint32_t)(join) << VG_STROKE_FLAGS_LINE_JOIN_Pos) & VG_STROKE_FLAGS_LINE_JOIN_Msk) \
	| (((uint32_t)(cap) << VG_STROKE_FLAGS_LINE_CAP_Pos) & VG_STROKE_FLAGS_LINE_CAP_Msk) \
	| (((uint32_t)(aa) << VG_STROKE_FLAGS_AA_Pos) & VG_STROKE_FLAGS_AA_Msk) \
)

#define VG_FILL_FLAGS_PATH_TYPE_Pos 0
#define VG_FILL_FLAGS_PATH_TYPE_Msk (0x01u << VG_FILL_FLAGS_PATH_TYPE_Pos)
#define VG_FILL_FLAGS_FILL_RULE_Pos 1
#define VG_FILL_FLAGS_FILL_RULE_Msk (0x01u << VG_FILL_FLAGS_FILL_RULE_Pos)
#define VG_FILL_FLAGS_AA_Pos        2
#define VG_FILL_FLAGS_AA_Msk        (0x01u << VG_FILL_FLAGS_AA_Pos)
#define VG_FILL_FLAGS(type, rule, aa) (0 \
	| (((uint32_t)(type) << VG_FILL_FLAGS_PATH_TYPE_Pos) & VG_FILL_FLAGS_PATH_TYPE_Msk) \
	| (((uint32_t)(rule) << VG_FILL_FLAGS_FILL_RULE_Pos) & VG_FILL_FLAGS_FILL_RULE_Msk) \
	| (((uint32_t)(aa) << VG_FILL_FLAGS_AA_Pos) & VG_FILL_FLAGS_AA_Msk) \
)

#endif // VG_CONFIG_H
