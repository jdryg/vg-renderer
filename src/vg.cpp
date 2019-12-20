// TODO:
// - More than 254 clip regions: Either use another view (extra parameter in createContext)
// or draw a fullscreen quad to reset the stencil buffer to 0.
// - Recycle the memory of cached meshes so resetting a cached mesh is faster.
// - Find a way to move stroker operations into separate functions (i.e. all strokePath 
// functions differ only on the createDrawCommand_XXX() call; strokerXXX calls are the same
// and the code is duplicated).
// - Allow strokes and fills with gradients and image patterns to be used as clip masks (might
// be useful if the same command list is used both inside and outside a beginClip()/endClip() 
// block)
#include <vg/vg.h>
#include <vg/path.h>
#include <vg/stroker.h>
#include "vg_util.h"
#include "libs/fontstash.h"
#include <bx/allocator.h>
#include <bx/mutex.h>
#include <bx/handlealloc.h>
#include <bx/string.h>
#include <bgfx/bgfx.h>
#include <bgfx/embedded_shader.h>

// Shaders
#include "shaders/vs_textured.bin.h"
#include "shaders/fs_textured.bin.h"
#include "shaders/vs_color_gradient.bin.h"
#include "shaders/fs_color_gradient.bin.h"
#include "shaders/vs_image_pattern.bin.h"
#include "shaders/fs_image_pattern.bin.h"
#include "shaders/vs_stencil.bin.h"
#include "shaders/fs_stencil.bin.h"

BX_PRAGMA_DIAGNOSTIC_IGNORED_MSVC(4706) // assignment within conditional expression

#define VG_CONFIG_MIN_FONT_SCALE                 0.1f
#define VG_CONFIG_MAX_FONT_SCALE                 4.0f
#define VG_CONFIG_MAX_FONT_IMAGES                4
#define VG_CONFIG_MIN_FONT_ATLAS_SIZE            512
#define VG_CONFIG_COMMAND_LIST_CACHE_STACK_SIZE  32
#define VG_CONFIG_COMMAND_LIST_ALIGNMENT         16

// Minimum font size (after scaling with the current transformation matrix),
// below which no text will be rendered.
#define VG_CONFIG_MIN_FONT_SIZE              4.0f

namespace vg
{
static const bgfx::EmbeddedShader s_EmbeddedShaders[] =
{
	BGFX_EMBEDDED_SHADER(vs_textured),
	BGFX_EMBEDDED_SHADER(fs_textured),
	BGFX_EMBEDDED_SHADER(vs_color_gradient),
	BGFX_EMBEDDED_SHADER(fs_color_gradient),
	BGFX_EMBEDDED_SHADER(vs_image_pattern),
	BGFX_EMBEDDED_SHADER(fs_image_pattern),
	BGFX_EMBEDDED_SHADER(vs_stencil),
	BGFX_EMBEDDED_SHADER(fs_stencil),

	BGFX_EMBEDDED_SHADER_END()
};

struct State
{
	float m_TransformMtx[6];
	float m_ScissorRect[4];
	float m_GlobalAlpha;
	float m_FontScale;
	float m_AvgScale;
};

struct ClipState
{
	ClipRule::Enum m_Rule;
	uint32_t m_FirstCmdID;
	uint32_t m_NumCmds;
};

struct HandleFlags
{
	enum Enum : uint16_t
	{
		LocalHandle = 0x0001
	};
};

struct Gradient
{
	float m_Matrix[9];
	float m_Params[4]; // {Extent.x, Extent.y, Radius, Feather}
	float m_InnerColor[4];
	float m_OuterColor[4];
};

struct ImagePattern
{
	float m_Matrix[9];
	ImageHandle m_ImageHandle;
};

struct DrawCommand
{
	struct Type
	{
		// NOTE: Originally there were only 3 types of commands, Textured, ColorGradient and Clip. 
		// In order to be able to support int16 UVs *and* repeatable image patterns (which require UVs
		// outside the [0, 1) range), a separate type of command has been added for image patterns.
		// The vertex shader of ImagePattern command calculates UVs the same way the gradient shader
		// calculates the gradient factor. 
		// The idea is that when using multiple image patterns, a new draw call will always be created
		// for each image, so there's little harm in changing shader program as well (?!?). In other words,
		// 2 paths with different image patterns wouldn't have been batched together either way.
		enum Enum : uint32_t
		{
			Textured = 0,
			ColorGradient,
			ImagePattern,
			Clip,

			NumTypes
		};
	};

	Type::Enum m_Type;
	ClipState m_ClipState;
	uint32_t m_VertexBufferID;
	uint32_t m_FirstVertexID;
	uint32_t m_FirstIndexID;
	uint32_t m_NumVertices;
	uint32_t m_NumIndices;
	uint16_t m_ScissorRect[4];
	uint16_t m_HandleID; // Type::Textured => ImageHandle, Type::ColorGradient => GradientHandle, Type::ImagePattern => ImagePatternHandle
};

struct GPUVertexBuffer
{
	bgfx::DynamicVertexBufferHandle m_PosBufferHandle;
	bgfx::DynamicVertexBufferHandle m_UVBufferHandle;
	bgfx::DynamicVertexBufferHandle m_ColorBufferHandle;
};

struct GPUIndexBuffer
{
	bgfx::DynamicIndexBufferHandle m_bgfxHandle;
};

struct VertexBuffer
{
	float* m_Pos;
	uv_t* m_UV;
	uint32_t* m_Color;
	uint32_t m_Count;
};

struct IndexBuffer
{
	uint16_t* m_Indices;
	uint32_t m_Count;
	uint32_t m_Capacity;
};

struct Image
{
	uint16_t m_Width;
	uint16_t m_Height;
	uint32_t m_Flags;
	bgfx::TextureHandle m_bgfxHandle;
};

struct FontData
{
	uint8_t* m_Data;
	int m_FonsHandle;
	bool m_Owned;
};

struct CommandType
{
	enum Enum : uint32_t
	{
		// Path commands
		BeginPath = 0,
		MoveTo,
		LineTo,
		CubicTo,
		QuadraticTo, 
		ArcTo,
		Arc,
		Rect,
		RoundedRect,
		RoundedRectVarying,
		Circle,
		Ellipse,
		Polyline,
		ClosePath,
		FirstPathCommand = BeginPath,
		LastPathCommand = ClosePath,

		// Stroker commands
		FillPathColor,
		FillPathGradient,
		FillPathImagePattern,
		StrokePathColor,
		StrokePathGradient,
		StrokePathImagePattern,
		
		FirstStrokerCommand = FillPathColor,
		LastStrokerCommand = StrokePathImagePattern,

		//
		IndexedTriList,

		// State commands
		BeginClip,
		EndClip,
		ResetClip,
		CreateLinearGradient,
		CreateBoxGradient,
		CreateRadialGradient,
		CreateImagePattern,
		PushState,
		PopState,
		ResetScissor,
		SetScissor,
		IntersectScissor,
		TransformIdentity,
		TransformScale,
		TransformTranslate,
		TransformRotate,
		TransformMult,
		SetViewBox,

		// Text
		Text,
		TextBox,

		// Command lists
		SubmitCommandList,
	};
};

struct CommandHeader
{
	CommandType::Enum m_Type;
	uint32_t m_Size;
};

struct CachedMesh
{
	float* m_Pos;
	uint32_t* m_Colors;
	uint16_t* m_Indices;
	uint32_t m_NumVertices;
	uint32_t m_NumIndices;
};

struct CachedCommand
{
	uint16_t m_FirstMeshID;
	uint16_t m_NumMeshes;
	float m_InvTransformMtx[6];
};

struct CommandListCache
{
	CachedMesh* m_Meshes;
	uint32_t m_NumMeshes;
	CachedCommand* m_Commands;
	uint32_t m_NumCommands;
	float m_AvgScale;
};

struct CommandList
{
	uint8_t* m_CommandBuffer;
	uint32_t m_CommandBufferCapacity;
	uint32_t m_CommandBufferPos;

	char* m_StringBuffer;
	uint32_t m_StringBufferCapacity;
	uint32_t m_StringBufferPos;

	uint32_t m_Flags;
	uint16_t m_NumGradients;
	uint16_t m_NumImagePatterns;

	CommandListCache* m_Cache;
};

#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
struct ContextVTable
{
	void(*beginPath)(Context* ctx);
	void(*moveTo)(Context* ctx, float x, float y);
	void(*lineTo)(Context* ctx, float x, float y);
	void(*cubicTo)(Context* ctx, float c1x, float c1y, float c2x, float c2y, float x, float y);
	void(*quadraticTo)(Context* ctx, float cx, float cy, float x, float y);
	void(*arcTo)(Context* ctx, float x1, float y1, float x2, float y2, float r);
	void(*arc)(Context* ctx, float cx, float cy, float r, float a0, float a1, Winding::Enum dir);
	void(*rect)(Context* ctx, float x, float y, float w, float h);
	void(*roundedRect)(Context* ctx, float x, float y, float w, float h, float r);
	void(*roundedRectVarying)(Context* ctx, float x, float y, float w, float h, float rtl, float rtr, float rbr, float rbl);
	void(*circle)(Context* ctx, float cx, float cy, float radius);
	void(*ellipse)(Context* ctx, float cx, float cy, float rx, float ry);
	void(*polyline)(Context* ctx, const float* coords, uint32_t numPoints);
	void(*closePath)(Context* ctx);
	void(*fillPathColor)(Context* ctx, Color color, uint32_t flags);
	void(*fillPathGradient)(Context* ctx, GradientHandle gradient, uint32_t flags);
	void(*fillPathImagePattern)(Context* ctx, ImagePatternHandle img, Color color, uint32_t flags);
	void(*strokePathColor)(Context* ctx, Color color, float width, uint32_t flags);
	void(*strokePathGradient)(Context* ctx, GradientHandle gradient, float width, uint32_t flags);
	void(*strokePathImagePattern)(Context* ctx, ImagePatternHandle img, Color color, float width, uint32_t flags);
	void(*beginClip)(Context* ctx, ClipRule::Enum rule);
	void(*endClip)(Context* ctx);
	void(*resetClip)(Context* ctx);
	GradientHandle(*createLinearGradient)(Context* ctx, float sx, float sy, float ex, float ey, Color icol, Color ocol);
	GradientHandle(*createBoxGradient)(Context* ctx, float x, float y, float w, float h, float r, float f, Color icol, Color ocol);
	GradientHandle(*createRadialGradient)(Context* ctx, float cx, float cy, float inr, float outr, Color icol, Color ocol);
	ImagePatternHandle(*createImagePattern)(Context* ctx, float cx, float cy, float w, float h, float angle, ImageHandle image);
	void(*pushState)(Context* ctx);
	void(*popState)(Context* ctx);
	void(*resetScissor)(Context* ctx);
	void(*setScissor)(Context* ctx, float x, float y, float w, float h);
	bool(*intersectScissor)(Context* ctx, float x, float y, float w, float h);
	void(*transformIdentity)(Context* ctx);
	void(*transformScale)(Context* ctx, float x, float y);
	void(*transformTranslate)(Context* ctx, float x, float y);
	void(*transformRotate)(Context* ctx, float ang_rad);
	void(*transformMult)(Context* ctx, const float* mtx, TransformOrder::Enum order);
	void(*setViewBox)(Context* ctx, float x, float y, float w, float h);
	void(*text)(Context* ctx, const TextConfig& cfg, float x, float y, const char* str, const char* end);
	void(*textBox)(Context* ctx, const TextConfig& cfg, float x, float y, float breakWidth, const char* text, const char* end, uint32_t textboxFlags);
	void(*indexedTriList)(Context* ctx, const float* pos, const uv_t* uv, uint32_t numVertices, const Color* color, uint32_t numColors, const uint16_t* indices, uint32_t numIndices, ImageHandle img);
	void(*submitCommandList)(Context* ctx, CommandListHandle handle);
};
#endif // VG_CONFIG_COMMAND_LIST_BEGIN_END_API

struct Context
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	const ContextVTable* m_VTable;
#endif

	ContextConfig m_Config;
	Stats m_Stats;
	bx::AllocatorI* m_Allocator;
	uint16_t m_ViewID;
	uint16_t m_CanvasWidth;
	uint16_t m_CanvasHeight;
	float m_DevicePixelRatio;
	float m_TesselationTolerance;
	float m_FringeWidth;

	Stroker* m_Stroker;
	Path* m_Path;
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	CommandListHandle m_ActiveCommandList;
#endif

	VertexBuffer* m_VertexBuffers;
	GPUVertexBuffer* m_GPUVertexBuffers;
	uint32_t m_NumVertexBuffers;
	uint32_t m_VertexBufferCapacity;
	uint32_t m_FirstVertexBufferID;

	IndexBuffer* m_IndexBuffers;
	GPUIndexBuffer* m_GPUIndexBuffers;
	uint32_t m_NumIndexBuffers;
	uint16_t m_ActiveIndexBufferID;

	float** m_Vec2DataPool;
	uint32_t m_Vec2DataPoolCapacity;

	uint32_t** m_Uint32DataPool;
	uint32_t m_Uint32DataPoolCapacity;

#if VG_CONFIG_UV_INT16
	int16_t** m_UVDataPool;
	uint32_t m_UVDataPoolCapacity;
#endif
#if BX_CONFIG_SUPPORTS_THREADING
	bx::Mutex* m_DataPoolMutex;
#endif

	Image* m_Images;
	uint32_t m_ImageCapacity;
	bx::HandleAlloc* m_ImageHandleAlloc;

	CommandList* m_CmdLists;
	bx::HandleAlloc* m_CmdListHandleAlloc;
	uint32_t m_SubmitCmdListRecursionDepth;
#if VG_CONFIG_ENABLE_SHAPE_CACHING
	CommandListCache* m_CmdListCacheStack[VG_CONFIG_COMMAND_LIST_CACHE_STACK_SIZE];
	uint32_t m_CmdListCacheStackTop;
#endif

	float* m_TransformedVertices;
	uint32_t m_TransformedVertexCapacity;
	bool m_PathTransformed;

	DrawCommand* m_DrawCommands;
	uint32_t m_NumDrawCommands;
	uint32_t m_DrawCommandCapacity;

	State* m_StateStack;
	uint32_t m_StateStackTop;

	ClipState m_ClipState;
	DrawCommand* m_ClipCommands;
	uint32_t m_NumClipCommands;
	uint32_t m_ClipCommandCapacity;
	bool m_RecordClipCommands;
	bool m_ForceNewClipCommand;
	bool m_ForceNewDrawCommand;

	Gradient* m_Gradients;
	uint32_t m_NextGradientID;

	ImagePattern* m_ImagePatterns;
	uint32_t m_NextImagePatternID;

	FONScontext* m_FontStashContext;
	ImageHandle m_FontImages[VG_CONFIG_MAX_FONT_IMAGES];
	uint32_t m_FontImageID;
	uv_t m_FontImageWhitePixelUV[2];

	float* m_TextVertices;
	FONSquad* m_TextQuads;
	uint32_t m_TextQuadCapacity;
	FONSstring m_TextString;
	FontData* m_FontData;
	uint32_t m_NextFontID;

	bgfx::VertexLayout m_PosVertexDecl;
	bgfx::VertexLayout m_UVVertexDecl;
	bgfx::VertexLayout m_ColorVertexDecl;
	bgfx::ProgramHandle m_ProgramHandle[DrawCommand::Type::NumTypes];
	bgfx::UniformHandle m_TexUniform;
	bgfx::UniformHandle m_PaintMatUniform;
	bgfx::UniformHandle m_ExtentRadiusFeatherUniform;
	bgfx::UniformHandle m_InnerColorUniform;
	bgfx::UniformHandle m_OuterColorUniform;
};

static State* getState(Context* ctx);
static void updateState(State* state);
static const uv_t* getWhitePixelUV(Context* ctx);
static void updateWhitePixelUV(Context* ctx);

static float* allocTransformedVertices(Context* ctx, uint32_t numVertices);
static const float* transformPath(Context* ctx);

static VertexBuffer* allocVertexBuffer(Context* ctx);
static float* allocVertexBufferData_Vec2(Context* ctx);
static uint32_t* allocVertexBufferData_Uint32(Context* ctx);
static void releaseVertexBufferData_Vec2(Context* ctx, float* data);
static void releaseVertexBufferData_Uint32(Context* ctx, uint32_t* data);
static void releaseVertexBufferDataCallback_Vec2(void* ptr, void* userData);
static void releaseVertexBufferDataCallback_Uint32(void* ptr, void* userData);

static uint16_t allocIndexBuffer(Context* ctx);
static void releaseIndexBuffer(Context* ctx, uint16_t* data);
static void releaseIndexBufferCallback(void* ptr, void* userData);

#if VG_CONFIG_UV_INT16
static int16_t* allocVertexBufferData_UV(Context* ctx);
static void releaseVertexBufferData_UV(Context* ctx, int16_t* data);
static void releaseVertexBufferDataCallback_UV(void* ptr, void* userData);
#endif

static DrawCommand* allocDrawCommand(Context* ctx, uint32_t numVertices, uint32_t numIndices, DrawCommand::Type::Enum type, uint16_t handle);
static DrawCommand* allocClipCommand(Context* ctx, uint32_t numVertices, uint32_t numIndices);
static void createDrawCommand_VertexColor(Context* ctx, const float* vtx, uint32_t numVertices, const uint32_t* colors, uint32_t numColors, const uint16_t* indices, uint32_t numIndices);
static void createDrawCommand_ImagePattern(Context* ctx, ImagePatternHandle handle, const float* vtx, uint32_t numVertices, const uint32_t* colors, uint32_t numColors, const uint16_t* indices, uint32_t numIndices);
static void createDrawCommand_ColorGradient(Context* ctx, GradientHandle handle, const float* vtx, uint32_t numVertices, const uint32_t* colors, uint32_t numColors, const uint16_t* indices, uint32_t numIndices);
static void createDrawCommand_Clip(Context* ctx, const float* vtx, uint32_t numVertices, const uint16_t* indices, uint32_t numIndices);

static ImageHandle allocImage(Context* ctx);
static void resetImage(Image* img);

static void renderTextQuads(Context* ctx, uint32_t numQuads, Color color);
static bool allocTextAtlas(Context* ctx);
static void flushTextAtlas(Context* ctx);

static CommandListHandle allocCommandList(Context* ctx);
static bool isCommandListHandleValid(Context* ctx, CommandListHandle handle);
static uint8_t* clAllocCommand(Context* ctx, CommandList* cl, CommandType::Enum cmdType, uint32_t dataSize);
static uint32_t clStoreString(Context* ctx, CommandList* cl, const char* str, uint32_t len);

#if VG_CONFIG_ENABLE_SHAPE_CACHING
static void clCacheRender(Context* ctx, CommandList* cl);
static void clCacheReset(Context* ctx, CommandListCache* cache);
static CommandListCache* clGetCache(Context* ctx, CommandList* cl);
static CommandListCache* allocCommandListCache(Context* ctx);
static void freeCommandListCache(Context* ctx, CommandListCache* cache);
static void pushCommandListCache(Context* ctx, CommandListCache* cache);
static void popCommandListCache(Context* ctx);
static CommandListCache* getCommandListCacheStackTop(Context* ctx);
static void beginCachedCommand(Context* ctx);
static void endCachedCommand(Context* ctx);
static void addCachedCommand(Context* ctx, const float* pos, uint32_t numVertices, const uint32_t* colors, uint32_t numColors, const uint16_t* indices, uint32_t numIndices);
static void submitCachedMesh(Context* ctx, Color col, const CachedMesh* meshList, uint32_t numMeshes);
static void submitCachedMesh(Context* ctx, GradientHandle gradientHandle, const CachedMesh* meshList, uint32_t numMeshes);
static void submitCachedMesh(Context* ctx, ImagePatternHandle imgPatter, Color color, const CachedMesh* meshList, uint32_t numMeshes);
#endif

static void ctxBeginPath(Context* ctx);
static void ctxMoveTo(Context* ctx, float x, float y);
static void ctxLineTo(Context* ctx, float x, float y);
static void ctxCubicTo(Context* ctx, float c1x, float c1y, float c2x, float c2y, float x, float y);
static void ctxQuadraticTo(Context* ctx, float cx, float cy, float x, float y);
static void ctxArc(Context* ctx, float cx, float cy, float r, float a0, float a1, Winding::Enum dir);
static void ctxArcTo(Context* ctx, float x1, float y1, float x2, float y2, float r);
static void ctxRect(Context* ctx, float x, float y, float w, float h);
static void ctxRoundedRect(Context* ctx, float x, float y, float w, float h, float r);
static void ctxRoundedRectVarying(Context* ctx, float x, float y, float w, float h, float rtl, float rtr, float rbr, float rbl);
static void ctxCircle(Context* ctx, float cx, float cy, float radius);
static void ctxEllipse(Context* ctx, float cx, float cy, float rx, float ry);
static void ctxPolyline(Context* ctx, const float* coords, uint32_t numPoints);
static void ctxClosePath(Context* ctx);
static void ctxFillPathColor(Context* ctx, Color color, uint32_t flags);
static void ctxFillPathGradient(Context* ctx, GradientHandle gradientHandle, uint32_t flags);
static void ctxFillPathImagePattern(Context* ctx, ImagePatternHandle imgPatternHandle, Color color, uint32_t flags);
static void ctxStrokePathColor(Context* ctx, Color color, float width, uint32_t flags);
static void ctxStrokePathGradient(Context* ctx, GradientHandle gradientHandle, float width, uint32_t flags);
static void ctxStrokePathImagePattern(Context* ctx, ImagePatternHandle imgPatternHandle, Color color, float width, uint32_t flags);
static void ctxBeginClip(Context* ctx, ClipRule::Enum rule);
static void ctxEndClip(Context* ctx);
static void ctxResetClip(Context* ctx);
static GradientHandle ctxCreateLinearGradient(Context* ctx, float sx, float sy, float ex, float ey, Color icol, Color ocol);
static GradientHandle ctxCreateBoxGradient(Context* ctx, float x, float y, float w, float h, float r, float f, Color icol, Color ocol);
static GradientHandle ctxCreateRadialGradient(Context* ctx, float cx, float cy, float inr, float outr, Color icol, Color ocol);
static ImagePatternHandle ctxCreateImagePattern(Context* ctx, float cx, float cy, float w, float h, float angle, ImageHandle image);
static void ctxPushState(Context* ctx);
static void ctxPopState(Context* ctx);
static void ctxResetScissor(Context* ctx);
static void ctxSetScissor(Context* ctx, float x, float y, float w, float h);
static bool ctxIntersectScissor(Context* ctx, float x, float y, float w, float h);
static void ctxTransformIdentity(Context* ctx);
static void ctxTransformScale(Context* ctx, float x, float y);
static void ctxTransformTranslate(Context* ctx, float x, float y);
static void ctxTransformRotate(Context* ctx, float ang_rad);
static void ctxTransformMult(Context* ctx, const float* mtx, TransformOrder::Enum order);
static void ctxSetViewBox(Context* ctx, float x, float y, float w, float h);
static void ctxIndexedTriList(Context* ctx, const float* pos, const uv_t* uv, uint32_t numVertices, const Color* colors, uint32_t numColors, const uint16_t* indices, uint32_t numIndices, ImageHandle img);
static void ctxText(Context* ctx, const TextConfig& cfg, float x, float y, const char* str, const char* end);
static void ctxTextBox(Context* ctx, const TextConfig& cfg, float x, float y, float breakWidth, const char* str, const char* end, uint32_t textboxFlags);
static void ctxSubmitCommandList(Context* ctx, CommandListHandle handle);

// Active command list wrappers
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
static void aclBeginPath(Context* ctx);
static void aclMoveTo(Context* ctx, float x, float y);
static void aclLineTo(Context* ctx, float x, float y);
static void aclCubicTo(Context* ctx, float c1x, float c1y, float c2x, float c2y, float x, float y);
static void aclQuadraticTo(Context* ctx, float cx, float cy, float x, float y);
static void aclArc(Context* ctx, float cx, float cy, float r, float a0, float a1, Winding::Enum dir);
static void aclArcTo(Context* ctx, float x1, float y1, float x2, float y2, float r);
static void aclRect(Context* ctx, float x, float y, float w, float h);
static void aclRoundedRect(Context* ctx, float x, float y, float w, float h, float r);
static void aclRoundedRectVarying(Context* ctx, float x, float y, float w, float h, float rtl, float rtr, float rbr, float rbl);
static void aclCircle(Context* ctx, float cx, float cy, float radius);
static void aclEllipse(Context* ctx, float cx, float cy, float rx, float ry);
static void aclPolyline(Context* ctx, const float* coords, uint32_t numPoints);
static void aclClosePath(Context* ctx);
static void aclFillPathColor(Context* ctx, Color color, uint32_t flags);
static void aclFillPathGradient(Context* ctx, GradientHandle gradientHandle, uint32_t flags);
static void aclFillPathImagePattern(Context* ctx, ImagePatternHandle imgPatternHandle, Color color, uint32_t flags);
static void aclStrokePathColor(Context* ctx, Color color, float width, uint32_t flags);
static void aclStrokePathGradient(Context* ctx, GradientHandle gradientHandle, float width, uint32_t flags);
static void aclStrokePathImagePattern(Context* ctx, ImagePatternHandle imgPatternHandle, Color color, float width, uint32_t flags);
static void aclBeginClip(Context* ctx, ClipRule::Enum rule);
static void aclEndClip(Context* ctx);
static void aclResetClip(Context* ctx);
static GradientHandle aclCreateLinearGradient(Context* ctx, float sx, float sy, float ex, float ey, Color icol, Color ocol);
static GradientHandle aclCreateBoxGradient(Context* ctx, float x, float y, float w, float h, float r, float f, Color icol, Color ocol);
static GradientHandle aclCreateRadialGradient(Context* ctx, float cx, float cy, float inr, float outr, Color icol, Color ocol);
static ImagePatternHandle aclCreateImagePattern(Context* ctx, float cx, float cy, float w, float h, float angle, ImageHandle image);
static void aclPushState(Context* ctx);
static void aclPopState(Context* ctx);
static void aclResetScissor(Context* ctx);
static void aclSetScissor(Context* ctx, float x, float y, float w, float h);
static bool aclIntersectScissor(Context* ctx, float x, float y, float w, float h);
static void aclTransformIdentity(Context* ctx);
static void aclTransformScale(Context* ctx, float x, float y);
static void aclTransformTranslate(Context* ctx, float x, float y);
static void aclTransformRotate(Context* ctx, float ang_rad);
static void aclTransformMult(Context* ctx, const float* mtx, TransformOrder::Enum order);
static void aclSetViewBox(Context* ctx, float x, float y, float w, float h);
static void aclIndexedTriList(Context* ctx, const float* pos, const uv_t* uv, uint32_t numVertices, const Color* colors, uint32_t numColors, const uint16_t* indices, uint32_t numIndices, ImageHandle img);
static void aclText(Context* ctx, const TextConfig& cfg, float x, float y, const char* str, const char* end);
static void aclTextBox(Context* ctx, const TextConfig& cfg, float x, float y, float breakWidth, const char* str, const char* end, uint32_t textboxFlags);
static void aclSubmitCommandList(Context* ctx, CommandListHandle handle);

const ContextVTable g_CtxVTable = {
	ctxBeginPath,
	ctxMoveTo,
	ctxLineTo,
	ctxCubicTo,
	ctxQuadraticTo,
	ctxArcTo,
	ctxArc,
	ctxRect,
	ctxRoundedRect,
	ctxRoundedRectVarying,
	ctxCircle,
	ctxEllipse,
	ctxPolyline,
	ctxClosePath,
	ctxFillPathColor,
	ctxFillPathGradient,
	ctxFillPathImagePattern,
	ctxStrokePathColor,
	ctxStrokePathGradient,
	ctxStrokePathImagePattern,
	ctxBeginClip,
	ctxEndClip,
	ctxResetClip,
	ctxCreateLinearGradient,
	ctxCreateBoxGradient,
	ctxCreateRadialGradient,
	ctxCreateImagePattern,
	ctxPushState,
	ctxPopState,
	ctxResetScissor,
	ctxSetScissor,
	ctxIntersectScissor,
	ctxTransformIdentity,
	ctxTransformScale,
	ctxTransformTranslate,
	ctxTransformRotate,
	ctxTransformMult,
	ctxSetViewBox,
	ctxText,
	ctxTextBox,
	ctxIndexedTriList,
	ctxSubmitCommandList
};

const ContextVTable g_ActiveCmdListVTable = {
	aclBeginPath,
	aclMoveTo,
	aclLineTo,
	aclCubicTo,
	aclQuadraticTo,
	aclArcTo,
	aclArc,
	aclRect,
	aclRoundedRect,
	aclRoundedRectVarying,
	aclCircle,
	aclEllipse,
	aclPolyline,
	aclClosePath,
	aclFillPathColor,
	aclFillPathGradient,
	aclFillPathImagePattern,
	aclStrokePathColor,
	aclStrokePathGradient,
	aclStrokePathImagePattern,
	aclBeginClip,
	aclEndClip,
	aclResetClip,
	aclCreateLinearGradient,
	aclCreateBoxGradient,
	aclCreateRadialGradient,
	aclCreateImagePattern,
	aclPushState,
	aclPopState,
	aclResetScissor,
	aclSetScissor,
	aclIntersectScissor,
	aclTransformIdentity,
	aclTransformScale,
	aclTransformTranslate,
	aclTransformRotate,
	aclTransformMult,
	aclSetViewBox,
	aclText,
	aclTextBox,
	aclIndexedTriList,
	aclSubmitCommandList
};
#endif

#define CMD_WRITE(ptr, type, value) *(type*)(ptr) = (value); ptr += sizeof(type)
#define CMD_READ(ptr, type) *(type*)(ptr); ptr += sizeof(type)

inline uint32_t alignSize(uint32_t sz, uint32_t alignment)
{
	VG_CHECK(bx::isPowerOf2<uint32_t>(alignment), "Invalid alignment value");
	const uint32_t mask = alignment - 1;
	return (sz & (~mask)) + ((sz & mask) != 0 ? alignment : 0);
}

inline bool isAligned(uint32_t sz, uint32_t alignment)
{
	VG_CHECK(bx::isPowerOf2<uint32_t>(alignment), "Invalid alignment value");
	return (sz & (alignment - 1)) == 0;
}

static const uint32_t kAlignedCommandHeaderSize = alignSize(sizeof(CommandHeader), VG_CONFIG_COMMAND_LIST_ALIGNMENT);

inline bool isLocal(uint16_t handleFlags)      { return (handleFlags & HandleFlags::LocalHandle) != 0; }
inline bool isLocal(GradientHandle handle)     { return isLocal(handle.flags); }
inline bool isLocal(ImagePatternHandle handle) { return isLocal(handle.flags); }

//////////////////////////////////////////////////////////////////////////
// Public interface
//
Context* createContext(bx::AllocatorI* allocator, const ContextConfig* userCfg)
{
	static const ContextConfig defaultConfig = {
		64,                          // m_MaxGradients
		64,                          // m_MaxImagePatterns
		8,                           // m_MaxFonts
		32,                          // m_MaxStateStackSize
		16,                          // m_MaxImages
		256,                         // m_MaxCommandLists
		65536,                       // m_MaxVBVertices
		ImageFlags::Filter_Bilinear, // m_FontAtlasImageFlags
		16                           // m_MaxCommandListDepth
	};

	const ContextConfig* cfg = userCfg ? userCfg : &defaultConfig;

	VG_CHECK(cfg->m_MaxVBVertices <= 65536, "Vertex buffers cannot be larger than 64k vertices because indices are always uint16");

	const uint32_t alignment = 8;
	const uint32_t totalMem = 0
		+ alignSize(sizeof(Context), alignment)
		+ alignSize(sizeof(Gradient) * cfg->m_MaxGradients, alignment)
		+ alignSize(sizeof(ImagePattern) * cfg->m_MaxImagePatterns, alignment)
		+ alignSize(sizeof(State) * cfg->m_MaxStateStackSize, alignment)
		+ alignSize(sizeof(FontData) * cfg->m_MaxFonts, alignment)
		+ alignSize(sizeof(CommandList) * cfg->m_MaxCommandLists, alignment);

	uint8_t* mem = (uint8_t*)BX_ALIGNED_ALLOC(allocator, totalMem, alignment);
	bx::memSet(mem, 0, totalMem);

	Context* ctx = (Context*)mem;              mem += alignSize(sizeof(Context), alignment);
	ctx->m_Gradients = (Gradient*)mem;         mem += alignSize(sizeof(Gradient) * cfg->m_MaxGradients, alignment);
	ctx->m_ImagePatterns = (ImagePattern*)mem; mem += alignSize(sizeof(ImagePattern) * cfg->m_MaxImagePatterns, alignment);
	ctx->m_StateStack = (State*)mem;           mem += alignSize(sizeof(State) * cfg->m_MaxStateStackSize, alignment);
	ctx->m_FontData = (FontData*)mem;          mem += alignSize(sizeof(FontData) * cfg->m_MaxFonts, alignment);
	ctx->m_CmdLists = (CommandList*)mem;       mem += alignSize(sizeof(CommandList) * cfg->m_MaxCommandLists, alignment);

#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable = &g_CtxVTable;
#endif

	bx::memCopy(&ctx->m_Config, cfg, sizeof(ContextConfig));
	ctx->m_Allocator = allocator;
	ctx->m_ViewID = 0;
	ctx->m_DevicePixelRatio = 1.0f;
	ctx->m_TesselationTolerance = 0.25f;
	ctx->m_FringeWidth = 1.0f;
	ctx->m_StateStackTop = 0;
	ctx->m_StateStack[0].m_GlobalAlpha = 1.0f;
	resetScissor(ctx);
	transformIdentity(ctx);

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	ctx->m_CmdListCacheStackTop = ~0u;
#endif

#if BX_CONFIG_SUPPORTS_THREADING
	ctx->m_DataPoolMutex = BX_NEW(allocator, bx::Mutex)();
#endif
	ctx->m_Path = createPath(allocator);
	ctx->m_Stroker = createStroker(allocator);

	ctx->m_ImageHandleAlloc = bx::createHandleAlloc(allocator, cfg->m_MaxImages);
	ctx->m_CmdListHandleAlloc = bx::createHandleAlloc(allocator, cfg->m_MaxCommandLists);

	// bgfx setup
	ctx->m_PosVertexDecl.begin().add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float).end();
	ctx->m_ColorVertexDecl.begin().add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true).end();
#if VG_CONFIG_UV_INT16
	ctx->m_UVVertexDecl.begin().add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Int16, true).end();
#else
	ctx->m_UVVertexDecl.begin().add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float).end();
#endif

	// NOTE: A couple of shaders can be shared between programs. Since bgfx
	// cares only whether the program handle changed and not (at least the D3D11 backend 
	// doesn't check shader handles), there's little point in complicating this atm.
	bgfx::RendererType::Enum bgfxRendererType = bgfx::getRendererType();
	ctx->m_ProgramHandle[DrawCommand::Type::Textured] = bgfx::createProgram(
		bgfx::createEmbeddedShader(s_EmbeddedShaders, bgfxRendererType, "vs_textured"),
		bgfx::createEmbeddedShader(s_EmbeddedShaders, bgfxRendererType, "fs_textured"),
		true);

	ctx->m_ProgramHandle[DrawCommand::Type::ColorGradient] = bgfx::createProgram(
		bgfx::createEmbeddedShader(s_EmbeddedShaders, bgfxRendererType, "vs_color_gradient"),
		bgfx::createEmbeddedShader(s_EmbeddedShaders, bgfxRendererType, "fs_color_gradient"),
		true);

	ctx->m_ProgramHandle[DrawCommand::Type::ImagePattern] = bgfx::createProgram(
		bgfx::createEmbeddedShader(s_EmbeddedShaders, bgfxRendererType, "vs_image_pattern"),
		bgfx::createEmbeddedShader(s_EmbeddedShaders, bgfxRendererType, "fs_image_pattern"),
		true);

	ctx->m_ProgramHandle[DrawCommand::Type::Clip] = bgfx::createProgram(
		bgfx::createEmbeddedShader(s_EmbeddedShaders, bgfxRendererType, "vs_stencil"),
		bgfx::createEmbeddedShader(s_EmbeddedShaders, bgfxRendererType, "fs_stencil"),
		true);

	ctx->m_TexUniform = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler, 1);
	ctx->m_PaintMatUniform = bgfx::createUniform("u_paintMat", bgfx::UniformType::Mat3, 1);
	ctx->m_ExtentRadiusFeatherUniform = bgfx::createUniform("u_extentRadiusFeather", bgfx::UniformType::Vec4, 1);
	ctx->m_InnerColorUniform = bgfx::createUniform("u_innerCol", bgfx::UniformType::Vec4, 1);
	ctx->m_OuterColorUniform = bgfx::createUniform("u_outerCol", bgfx::UniformType::Vec4, 1);
	
	// FontStash
	// Init font stash
	const bgfx::Caps* caps = bgfx::getCaps();
	FONSparams fontParams;
	bx::memSet(&fontParams, 0, sizeof(fontParams));
	fontParams.width = VG_CONFIG_MIN_FONT_ATLAS_SIZE;
	fontParams.height = VG_CONFIG_MIN_FONT_ATLAS_SIZE;
	fontParams.flags = FONS_ZERO_TOPLEFT;
#if FONS_CUSTOM_WHITE_RECT
	// NOTE: White rect might get too large but since the atlas limit is the texture size limit
	// it should be that large. Otherwise shapes cached when the atlas was 512x512 will get wrong
	// white pixel UVs when the atlas gets to the texture size limit (should not happen but better
	// be safe).
	fontParams.whiteRectWidth = caps->limits.maxTextureSize / VG_CONFIG_MIN_FONT_ATLAS_SIZE;
	fontParams.whiteRectHeight = caps->limits.maxTextureSize / VG_CONFIG_MIN_FONT_ATLAS_SIZE;
#endif
	fontParams.renderCreate = nullptr;
	fontParams.renderUpdate = nullptr;
	fontParams.renderDraw = nullptr;
	fontParams.renderDelete = nullptr;
	fontParams.userPtr = nullptr;
	ctx->m_FontStashContext = fonsCreateInternal(&fontParams);
	VG_CHECK(ctx->m_FontStashContext != nullptr, "Failed to initialize FontStash");

	for (uint32_t i = 0; i < VG_CONFIG_MAX_FONT_IMAGES; ++i) {
		ctx->m_FontImages[i] = VG_INVALID_HANDLE;
	}

	ctx->m_FontImages[0] = createImage(ctx, (uint16_t)fontParams.width, (uint16_t)fontParams.height, cfg->m_FontAtlasImageFlags, nullptr);
	VG_CHECK(isValid(ctx->m_FontImages[0]), "Failed to initialize font texture");
	
	ctx->m_FontImageID = 0;
	updateWhitePixelUV(ctx);

	fonsInitString(&ctx->m_TextString);

	return ctx;
}

void destroyContext(Context* ctx)
{
	bx::AllocatorI* allocator = ctx->m_Allocator;
	const ContextConfig* cfg = &ctx->m_Config;

	fonsDestroyString(&ctx->m_TextString);

	for (uint32_t i = 0; i < DrawCommand::Type::NumTypes; ++i) {
		if (bgfx::isValid(ctx->m_ProgramHandle[i])) {
			bgfx::destroy(ctx->m_ProgramHandle[i]);
			ctx->m_ProgramHandle[i] = BGFX_INVALID_HANDLE;
		}
	}

	bgfx::destroy(ctx->m_TexUniform);
	bgfx::destroy(ctx->m_PaintMatUniform);
	bgfx::destroy(ctx->m_ExtentRadiusFeatherUniform);
	bgfx::destroy(ctx->m_InnerColorUniform);
	bgfx::destroy(ctx->m_OuterColorUniform);

	for (uint32_t i = 0; i < ctx->m_VertexBufferCapacity; ++i) {
		GPUVertexBuffer* vb = &ctx->m_GPUVertexBuffers[i];
		if (bgfx::isValid(vb->m_PosBufferHandle)) {
			bgfx::destroy(vb->m_PosBufferHandle);
			vb->m_PosBufferHandle = BGFX_INVALID_HANDLE;
		}
		if (bgfx::isValid(vb->m_UVBufferHandle)) {
			bgfx::destroy(vb->m_UVBufferHandle);
			vb->m_UVBufferHandle = BGFX_INVALID_HANDLE;
		}
		if (bgfx::isValid(vb->m_ColorBufferHandle)) {
			bgfx::destroy(vb->m_ColorBufferHandle);
			vb->m_ColorBufferHandle = BGFX_INVALID_HANDLE;
		}
	}
	BX_FREE(allocator, ctx->m_GPUVertexBuffers);
	BX_FREE(allocator, ctx->m_VertexBuffers);
	ctx->m_GPUVertexBuffers = nullptr;
	ctx->m_VertexBuffers = nullptr;
	ctx->m_VertexBufferCapacity = 0;
	ctx->m_NumVertexBuffers = 0;

	for (uint32_t i = 0; i < ctx->m_NumIndexBuffers; ++i) {
		GPUIndexBuffer* gpuib = &ctx->m_GPUIndexBuffers[i];
		if (bgfx::isValid(gpuib->m_bgfxHandle)) {
			bgfx::destroy(gpuib->m_bgfxHandle);
			gpuib->m_bgfxHandle = BGFX_INVALID_HANDLE;
		}

		IndexBuffer* ib = &ctx->m_IndexBuffers[i];
		BX_ALIGNED_FREE(allocator, ib->m_Indices, 16);
		ib->m_Indices = nullptr;
		ib->m_Capacity = 0;
		ib->m_Count = 0;
	}
	BX_FREE(allocator, ctx->m_GPUIndexBuffers);
	BX_FREE(allocator, ctx->m_IndexBuffers);
	ctx->m_GPUIndexBuffers = nullptr;
	ctx->m_IndexBuffers = nullptr;
	ctx->m_ActiveIndexBufferID = UINT16_MAX;

	for (uint32_t i = 0; i < ctx->m_Vec2DataPoolCapacity; ++i) {
		float* buffer = ctx->m_Vec2DataPool[i];
		if (!buffer) {
			continue;
		}

		if ((uintptr_t)buffer & 1) {
			buffer = (float*)((uintptr_t)buffer & ~1);
			BX_ALIGNED_FREE(allocator, buffer, 16);
		}
	}
	BX_FREE(allocator, ctx->m_Vec2DataPool);
	ctx->m_Vec2DataPoolCapacity = 0;

	for (uint32_t i = 0; i < ctx->m_Uint32DataPoolCapacity; ++i) {
		uint32_t* buffer = ctx->m_Uint32DataPool[i];
		if (!buffer) {
			continue;
		}

		if ((uintptr_t)buffer & 1) {
			buffer = (uint32_t*)((uintptr_t)buffer & ~1);
			BX_ALIGNED_FREE(allocator, buffer, 16);
		}
	}
	BX_FREE(allocator, ctx->m_Uint32DataPool);
	ctx->m_Uint32DataPoolCapacity = 0;

#if VG_CONFIG_UV_INT16
	for (uint32_t i = 0; i < ctx->m_UVDataPoolCapacity; ++i) {
		int16_t* buffer = ctx->m_UVDataPool[i];
		if (!buffer) {
			continue;
		}

		if ((uintptr_t)buffer & 1) {
			buffer = (int16_t*)((uintptr_t)buffer & ~1);
			BX_ALIGNED_FREE(allocator, buffer, 16);
		}
	}
	BX_FREE(allocator, ctx->m_UVDataPool);
	ctx->m_UVDataPoolCapacity = 0;
#endif

	BX_FREE(allocator, ctx->m_DrawCommands);
	ctx->m_DrawCommands = nullptr;

	BX_FREE(allocator, ctx->m_ClipCommands);
	ctx->m_ClipCommands = nullptr;

	// Font data
	for (int i = 0; i < cfg->m_MaxFonts; ++i) {
		FontData* fd = &ctx->m_FontData[i];
		if (fd->m_Data && fd->m_Owned) {
			BX_FREE(allocator, fd->m_Data);
			fd->m_Data = nullptr;
			fd->m_Owned = false;
		}
	}

	fonsDeleteInternal(ctx->m_FontStashContext);

	for (uint32_t i = 0; i < VG_CONFIG_MAX_FONT_IMAGES; ++i) {
		destroyImage(ctx, ctx->m_FontImages[i]);
	}

	for (uint32_t i = 0; i < ctx->m_ImageCapacity; ++i) {
		Image* img = &ctx->m_Images[i];
		if (bgfx::isValid(img->m_bgfxHandle)) {
			bgfx::destroy(img->m_bgfxHandle);
		}
	}
	BX_FREE(allocator, ctx->m_Images);
	ctx->m_Images = nullptr;

	bx::destroyHandleAlloc(allocator, ctx->m_ImageHandleAlloc);
	ctx->m_ImageHandleAlloc = nullptr;

	bx::destroyHandleAlloc(allocator, ctx->m_CmdListHandleAlloc);
	ctx->m_CmdListHandleAlloc = nullptr;

	destroyPath(ctx->m_Path);
	ctx->m_Path = nullptr;

	destroyStroker(ctx->m_Stroker);
	ctx->m_Stroker = nullptr;

	BX_ALIGNED_FREE(allocator, ctx->m_TextQuads, 16);
	ctx->m_TextQuads = nullptr;

	BX_ALIGNED_FREE(allocator, ctx->m_TextVertices, 16);
	ctx->m_TextVertices = nullptr;

	BX_ALIGNED_FREE(allocator, ctx->m_TransformedVertices, 16);
	ctx->m_TransformedVertices = nullptr;

#if BX_CONFIG_SUPPORTS_THREADING
	BX_DELETE(allocator, ctx->m_DataPoolMutex);
#endif

	BX_FREE(allocator, ctx);
}

void begin(Context* ctx, uint16_t viewID, uint16_t canvasWidth, uint16_t canvasHeight, float devicePixelRatio)
{
	ctx->m_ViewID = viewID;
	ctx->m_CanvasWidth = canvasWidth;
	ctx->m_CanvasHeight = canvasHeight;
	ctx->m_DevicePixelRatio = devicePixelRatio;
	ctx->m_TesselationTolerance = 0.25f / devicePixelRatio;
	ctx->m_FringeWidth = 1.0f / devicePixelRatio;
	ctx->m_SubmitCmdListRecursionDepth = 0;

#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable = &g_CtxVTable;
	ctx->m_ActiveCommandList = VG_INVALID_HANDLE;
#endif

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	ctx->m_CmdListCacheStackTop = ~0u;
#endif

	VG_CHECK(ctx->m_StateStackTop == 0, "State stack hasn't been properly reset in the previous frame");
	resetScissor(ctx);
	transformIdentity(ctx);

	ctx->m_FirstVertexBufferID = ctx->m_NumVertexBuffers;
	allocVertexBuffer(ctx);

	ctx->m_ActiveIndexBufferID = allocIndexBuffer(ctx);
	VG_CHECK(ctx->m_IndexBuffers[ctx->m_ActiveIndexBufferID].m_Count == 0, "Not empty index buffer");

	ctx->m_NumDrawCommands = 0;
	ctx->m_ForceNewDrawCommand = true;

	ctx->m_NumClipCommands = 0;
	ctx->m_ForceNewClipCommand = true;
	ctx->m_ClipState.m_FirstCmdID = ~0u;
	ctx->m_ClipState.m_NumCmds = 0;
	ctx->m_ClipState.m_Rule = ClipRule::In;

	ctx->m_NextGradientID = 0;
	ctx->m_NextImagePatternID = 0;
}

void end(Context* ctx)
{
	VG_CHECK(ctx->m_StateStackTop == 0, "pushState()/popState() mismatch");
	VG_CHECK(!isValid(ctx->m_ActiveCommandList), "endCommandList() hasn't been called");

	const uint32_t numDrawCommands = ctx->m_NumDrawCommands;
	if (numDrawCommands == 0) {
		// Release the vertex buffer allocated in beginFrame()
		VertexBuffer* vb = &ctx->m_VertexBuffers[ctx->m_FirstVertexBufferID];
		releaseVertexBufferData_Vec2(ctx, vb->m_Pos);
		releaseVertexBufferData_Uint32(ctx, vb->m_Color);

#if VG_CONFIG_UV_INT16
		releaseVertexBufferData_UV(ctx, vb->m_UV);
#else
		releaseVertexBufferData_Vec2(ctx, vb->m_UV);
#endif

		return;
	}

	flushTextAtlas(ctx);

	// Update bgfx vertex buffers...
	const uint32_t numVertexBuffers = ctx->m_NumVertexBuffers;
	for (uint32_t iVB = ctx->m_FirstVertexBufferID; iVB < numVertexBuffers; ++iVB) {
		VertexBuffer* vb = &ctx->m_VertexBuffers[iVB];
		GPUVertexBuffer* gpuvb = &ctx->m_GPUVertexBuffers[iVB];
		
		const uint32_t maxVBVertices = ctx->m_Config.m_MaxVBVertices;
		if (!bgfx::isValid(gpuvb->m_PosBufferHandle)) {
			gpuvb->m_PosBufferHandle = bgfx::createDynamicVertexBuffer(maxVBVertices, ctx->m_PosVertexDecl, 0);
		}
		if (!bgfx::isValid(gpuvb->m_UVBufferHandle)) {
			gpuvb->m_UVBufferHandle = bgfx::createDynamicVertexBuffer(maxVBVertices, ctx->m_UVVertexDecl, 0);
		}
		if (!bgfx::isValid(gpuvb->m_ColorBufferHandle)) {
			gpuvb->m_ColorBufferHandle = bgfx::createDynamicVertexBuffer(maxVBVertices, ctx->m_ColorVertexDecl, 0);
		}

		const bgfx::Memory* posMem = bgfx::makeRef(vb->m_Pos, sizeof(float) * 2 * vb->m_Count, releaseVertexBufferDataCallback_Vec2, ctx);
		const bgfx::Memory* colorMem = bgfx::makeRef(vb->m_Color, sizeof(uint32_t) * vb->m_Count, releaseVertexBufferDataCallback_Uint32, ctx);
#if VG_CONFIG_UV_INT16
		const bgfx::Memory* uvMem = bgfx::makeRef(vb->m_UV, sizeof(int16_t) * 2 * vb->m_Count, releaseVertexBufferDataCallback_UV, ctx);
#else
		const bgfx::Memory* uvMem = bgfx::makeRef(vb->m_UV, sizeof(float) * 2 * vb->m_Count, releaseVertexBufferDataCallback_Vec2, ctx);
#endif

		bgfx::update(gpuvb->m_PosBufferHandle, 0, posMem);
		bgfx::update(gpuvb->m_UVBufferHandle, 0, uvMem);
		bgfx::update(gpuvb->m_ColorBufferHandle, 0, colorMem);

		vb->m_Pos = nullptr;
		vb->m_UV = nullptr;
		vb->m_Color = nullptr;
	}

	// Update bgfx index buffer...
	IndexBuffer* ib = &ctx->m_IndexBuffers[ctx->m_ActiveIndexBufferID];
	GPUIndexBuffer* gpuib = &ctx->m_GPUIndexBuffers[ctx->m_ActiveIndexBufferID];
	const bgfx::Memory* indexMem = bgfx::makeRef(&ib->m_Indices[0], sizeof(uint16_t) * ib->m_Count, releaseIndexBufferCallback, ctx);
	if (!bgfx::isValid(gpuib->m_bgfxHandle)) {
		gpuib->m_bgfxHandle = bgfx::createDynamicIndexBuffer(indexMem, BGFX_BUFFER_ALLOW_RESIZE);
	} else {
		bgfx::update(gpuib->m_bgfxHandle, 0, indexMem);
	}

	const uint16_t viewID = ctx->m_ViewID;
	const uint16_t canvasWidth = ctx->m_CanvasWidth;
	const uint16_t canvasHeight = ctx->m_CanvasHeight;

	float viewMtx[16];
	float projMtx[16];
	bx::mtxIdentity(viewMtx);
	bx::mtxOrtho(projMtx, 0.0f, (float)canvasWidth, (float)canvasHeight, 0.0f, 0.0f, 1.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);
	bgfx::setViewTransform(viewID, viewMtx, projMtx);

	uint16_t prevScissorRect[4] = { 0, 0, canvasWidth, canvasHeight};
	uint16_t prevScissorID = UINT16_MAX;
	uint32_t prevClipCmdID = UINT32_MAX;
	uint32_t stencilState = BGFX_STENCIL_NONE;
	uint8_t nextStencilValue = 1;

	for (uint32_t iCmd = 0; iCmd < numDrawCommands; ++iCmd) {
		DrawCommand* cmd = &ctx->m_DrawCommands[iCmd];

		const ClipState* cmdClipState = &cmd->m_ClipState;
		if (cmdClipState->m_FirstCmdID != prevClipCmdID) {
			prevClipCmdID = cmdClipState->m_FirstCmdID;
			const uint32_t numClipCommands = cmdClipState->m_NumCmds;
			if (numClipCommands) {
				for (uint32_t iClip = 0; iClip < numClipCommands; ++iClip) {
					VG_CHECK(cmdClipState->m_FirstCmdID + iClip < ctx->m_NumClipCommands, "Invalid clip command index");

					DrawCommand* clipCmd = &ctx->m_ClipCommands[cmdClipState->m_FirstCmdID + iClip];

					GPUVertexBuffer* gpuvb = &ctx->m_GPUVertexBuffers[clipCmd->m_VertexBufferID];
					bgfx::setVertexBuffer(0, gpuvb->m_PosBufferHandle, clipCmd->m_FirstVertexID, clipCmd->m_NumVertices);
					bgfx::setIndexBuffer(gpuib->m_bgfxHandle, clipCmd->m_FirstIndexID, clipCmd->m_NumIndices);

					// Set scissor.
					{
						const uint16_t* cmdScissorRect = &clipCmd->m_ScissorRect[0];
						if (!bx::memCmp(cmdScissorRect, &prevScissorRect[0], sizeof(uint16_t) * 4)) {
							bgfx::setScissor(prevScissorID);
						} else {
							prevScissorID = bgfx::setScissor(cmdScissorRect[0], cmdScissorRect[1], cmdScissorRect[2], cmdScissorRect[3]);
							bx::memCopy(prevScissorRect, cmdScissorRect, sizeof(uint16_t) * 4);
						}
					}

					VG_CHECK(clipCmd->m_Type == DrawCommand::Type::Clip, "Invalid clip command");
					VG_CHECK(clipCmd->m_HandleID == UINT16_MAX, "Invalid clip command image handle");

					bgfx::setState(0);
					bgfx::setStencil(0
						| BGFX_STENCIL_TEST_ALWAYS                // pass always
						| BGFX_STENCIL_FUNC_REF(nextStencilValue) // value = nextStencilValue
						| BGFX_STENCIL_FUNC_RMASK(0xff)
						| BGFX_STENCIL_OP_FAIL_S_REPLACE
						| BGFX_STENCIL_OP_FAIL_Z_REPLACE
						| BGFX_STENCIL_OP_PASS_Z_REPLACE, BGFX_STENCIL_NONE);

					// TODO: Check if it's better to use Type_TexturedVertexColor program here to avoid too many 
					// state switches.
					bgfx::submit(viewID, ctx->m_ProgramHandle[DrawCommand::Type::Clip], 0, false);
				}

				stencilState = 0
					| (cmdClipState->m_Rule == ClipRule::In ? BGFX_STENCIL_TEST_EQUAL : BGFX_STENCIL_TEST_NOTEQUAL)
					| BGFX_STENCIL_FUNC_REF(nextStencilValue)
					| BGFX_STENCIL_FUNC_RMASK(0xff)
					| BGFX_STENCIL_OP_FAIL_S_KEEP
					| BGFX_STENCIL_OP_FAIL_Z_KEEP
					| BGFX_STENCIL_OP_PASS_Z_KEEP;

				++nextStencilValue;
			} else {
				stencilState = BGFX_STENCIL_NONE;
			}
		}

		GPUVertexBuffer* gpuvb = &ctx->m_GPUVertexBuffers[cmd->m_VertexBufferID];
		bgfx::setVertexBuffer(0, gpuvb->m_PosBufferHandle, cmd->m_FirstVertexID, cmd->m_NumVertices);
		bgfx::setVertexBuffer(1, gpuvb->m_ColorBufferHandle, cmd->m_FirstVertexID, cmd->m_NumVertices);
		bgfx::setIndexBuffer(gpuib->m_bgfxHandle, cmd->m_FirstIndexID, cmd->m_NumIndices);

		// Set scissor.
		{
			const uint16_t* cmdScissorRect = &cmd->m_ScissorRect[0];
			if (!bx::memCmp(cmdScissorRect, &prevScissorRect[0], sizeof(uint16_t) * 4)) {
				bgfx::setScissor(prevScissorID);
			} else {
				prevScissorID = bgfx::setScissor(cmdScissorRect[0], cmdScissorRect[1], cmdScissorRect[2], cmdScissorRect[3]);
				bx::memCopy(prevScissorRect, cmdScissorRect, sizeof(uint16_t) * 4);
			}
		}

		if (cmd->m_Type == DrawCommand::Type::Textured) {
			VG_CHECK(cmd->m_HandleID != UINT16_MAX, "Invalid image handle");
			Image* tex = &ctx->m_Images[cmd->m_HandleID];

			bgfx::setVertexBuffer(2, gpuvb->m_UVBufferHandle, cmd->m_FirstVertexID, cmd->m_NumVertices);
			bgfx::setTexture(0, ctx->m_TexUniform, tex->m_bgfxHandle, tex->m_Flags);

			bgfx::setState(0
				| BGFX_STATE_WRITE_A
				| BGFX_STATE_WRITE_RGB
				| BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA, BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA));
			bgfx::setStencil(stencilState);

			bgfx::submit(viewID, ctx->m_ProgramHandle[DrawCommand::Type::Textured], 0, false);
		} else if (cmd->m_Type == DrawCommand::Type::ColorGradient) {
			VG_CHECK(cmd->m_HandleID != UINT16_MAX, "Invalid gradient handle");
			Gradient* grad = &ctx->m_Gradients[cmd->m_HandleID];

			bgfx::setUniform(ctx->m_PaintMatUniform, grad->m_Matrix, 1);
			bgfx::setUniform(ctx->m_ExtentRadiusFeatherUniform, grad->m_Params, 1);
			bgfx::setUniform(ctx->m_InnerColorUniform, grad->m_InnerColor, 1);
			bgfx::setUniform(ctx->m_OuterColorUniform, grad->m_OuterColor, 1);

			bgfx::setState(0
				| BGFX_STATE_WRITE_A
				| BGFX_STATE_WRITE_RGB
				| BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA, BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA));
			bgfx::setStencil(stencilState);

			bgfx::submit(viewID, ctx->m_ProgramHandle[DrawCommand::Type::ColorGradient], 0, false);
		} else if(cmd->m_Type == DrawCommand::Type::ImagePattern) {
			VG_CHECK(cmd->m_HandleID != UINT16_MAX, "Invalid image pattern handle");
			ImagePattern* imgPattern = &ctx->m_ImagePatterns[cmd->m_HandleID];

			VG_CHECK(isValid(imgPattern->m_ImageHandle), "Invalid image handle in pattern");
			Image* tex = &ctx->m_Images[imgPattern->m_ImageHandle.idx];

			bgfx::setTexture(0, ctx->m_TexUniform, tex->m_bgfxHandle, tex->m_Flags);
			bgfx::setUniform(ctx->m_PaintMatUniform, imgPattern->m_Matrix, 1);

			bgfx::setState(0
				| BGFX_STATE_WRITE_A
				| BGFX_STATE_WRITE_RGB
				| BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA, BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA));
			bgfx::setStencil(stencilState);

			bgfx::submit(viewID, ctx->m_ProgramHandle[DrawCommand::Type::ImagePattern], 0, false);
		} else {
			VG_CHECK(false, "Unknown draw command type");
		}
	}
}

void frame(Context* ctx)
{
	ctx->m_NumVertexBuffers = 0;

	if (ctx->m_FontImageID != 0) {
		ImageHandle fontImage = ctx->m_FontImages[ctx->m_FontImageID];

		// delete images that smaller than current one
		if (isValid(fontImage)) {
			uint16_t iw, ih;
			getImageSize(ctx, fontImage, &iw, &ih);

			uint32_t j = 0;
			for (uint32_t i = 0; i < ctx->m_FontImageID; i++) {
				if (isValid(ctx->m_FontImages[i])) {
					uint16_t nw, nh;
					getImageSize(ctx, ctx->m_FontImages[i], &nw, &nh);

					if (nw < iw || nh < ih) {
						destroyImage(ctx, ctx->m_FontImages[i]);
					} else {
						ctx->m_FontImages[j++] = ctx->m_FontImages[i];
					}
				}
			}

			// make current font image to first
			ctx->m_FontImages[j++] = ctx->m_FontImages[0];
			ctx->m_FontImages[0] = fontImage;
			ctx->m_FontImageID = 0;
			updateWhitePixelUV(ctx);

			// clear all images after j
			for (int i = j; i < VG_CONFIG_MAX_FONT_IMAGES; i++) {
				ctx->m_FontImages[i] = VG_INVALID_HANDLE;
			}
		}
	}
}

const Stats* getStats(Context* ctx)
{
	return &ctx->m_Stats;
}

void beginPath(Context* ctx)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->beginPath(ctx);
#else
	ctxBeginPath(ctx);
#endif
}

void moveTo(Context* ctx, float x, float y)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->moveTo(ctx, x, y);
#else
	ctxMoveTo(ctx, x, y);
#endif
}

void lineTo(Context* ctx, float x, float y)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->lineTo(ctx, x, y);
#else
	ctxLineTo(ctx, x, y);
#endif
}

void cubicTo(Context* ctx, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->cubicTo(ctx, c1x, c1y, c2x, c2y, x, y);
#else
	ctxCubicTo(ctx, c1x, c1y, c2x, c2y, x, y);
#endif
}

void quadraticTo(Context* ctx, float cx, float cy, float x, float y)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->quadraticTo(ctx, cx, cy, x, y);
#else
	ctxQuadraticTo(ctx, cx, cy, x, y);
#endif
}

void arc(Context* ctx, float cx, float cy, float r, float a0, float a1, Winding::Enum dir)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->arc(ctx, cx, cy, r, a0, a1, dir);
#else
	ctxArc(ctx, cx, cy, r, a0, a1, dir);
#endif
}

void arcTo(Context* ctx, float x1, float y1, float x2, float y2, float r)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->arcTo(ctx, x1, y1, x2, y2, r);
#else
	ctxArcTo(ctx, x1, y1, x2, y2, r);
#endif
}

void rect(Context* ctx, float x, float y, float w, float h)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->rect(ctx, x, y, w, h);
#else
	ctxRect(ctx, x, y, w, h);
#endif
}

void roundedRect(Context* ctx, float x, float y, float w, float h, float r)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->roundedRect(ctx, x, y, w, h, r);
#else
	ctxRoundedRect(ctx, x, y, w, h, r);
#endif
}

void roundedRectVarying(Context* ctx, float x, float y, float w, float h, float rtl, float rtr, float rbr, float rbl)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->roundedRectVarying(ctx, x, y, w, h, rtl, rtr, rbr, rbl);
#else
	ctxRoundedRectVarying(ctx, x, y, w, h, rtl, rtr, rbr, rbl);
#endif
}

void circle(Context* ctx, float cx, float cy, float radius)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->circle(ctx, cx, cy, radius);
#else
	ctxCircle(ctx, cx, cy, radius);
#endif
}

void ellipse(Context* ctx, float cx, float cy, float rx, float ry)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->ellipse(ctx, cx, cy, rx, ry);
#else
	ctxEllipse(ctx, cx, cy, rx, ry);
#endif
}

void polyline(Context* ctx, const float* coords, uint32_t numPoints)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->polyline(ctx, coords, numPoints);
#else
	ctxPolyline(ctx, coords, numPoints);
#endif
}

void closePath(Context* ctx)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->closePath(ctx);
#else
	ctxClosePath(ctx);
#endif
}

void fillPath(Context* ctx, Color color, uint32_t flags)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->fillPathColor(ctx, color, flags);
#else
	ctxFillPathColor(ctx, color, flags);
#endif
}

void fillPath(Context* ctx, GradientHandle gradientHandle, uint32_t flags)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->fillPathGradient(ctx, gradientHandle, flags);
#else
	ctxFillPathGradient(ctx, gradientHandle, flags);
#endif
}

void fillPath(Context* ctx, ImagePatternHandle imgPatternHandle, Color color, uint32_t flags)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->fillPathImagePattern(ctx, imgPatternHandle, color, flags);
#else
	ctxFillPathImagePattern(ctx, imgPatternHandle, color, flags);
#endif
}

void strokePath(Context* ctx, Color color, float width, uint32_t flags)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->strokePathColor(ctx, color, width, flags);
#else
	ctxStrokePathColor(ctx, color, width, flags);
#endif
}

void strokePath(Context* ctx, GradientHandle gradientHandle, float width, uint32_t flags)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->strokePathGradient(ctx, gradientHandle, width, flags);
#else
	ctxStrokePathGradient(ctx, gradientHandle, width, flags);
#endif
}

void strokePath(Context* ctx, ImagePatternHandle imgPatternHandle, Color color, float width, uint32_t flags)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->strokePathImagePattern(ctx, imgPatternHandle, color, width, flags);
#else
	ctxStrokePathImagePattern(ctx, imgPatternHandle, color, width, flags);
#endif
}

void beginClip(Context* ctx, ClipRule::Enum rule)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->beginClip(ctx, rule);
#else
	ctxBeginClip(ctx, rule);
#endif
}

void endClip(Context* ctx)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->endClip(ctx);
#else
	ctxEndClip(ctx);
#endif
}

void resetClip(Context* ctx)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->resetClip(ctx);
#else
	ctxResetClip(ctx);
#endif
}

GradientHandle createLinearGradient(Context* ctx, float sx, float sy, float ex, float ey, Color icol, Color ocol)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	return ctx->m_VTable->createLinearGradient(ctx, sx, sy, ex, ey, icol, ocol);
#else
	return ctxCreateLinearGradient(ctx, sx, sy, ex, ey, icol, ocol);
#endif
}

GradientHandle createBoxGradient(Context* ctx, float x, float y, float w, float h, float r, float f, Color icol, Color ocol)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	return ctx->m_VTable->createBoxGradient(ctx, x, y, w, h, r, f, icol, ocol);
#else
	return ctxCreateBoxGradient(ctx, x, y, w, h, r, f, icol, ocol);
#endif
}

GradientHandle createRadialGradient(Context* ctx, float cx, float cy, float inr, float outr, Color icol, Color ocol)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	return ctx->m_VTable->createRadialGradient(ctx, cx, cy, inr, outr, icol, ocol);
#else
	return ctxCreateRadialGradient(ctx, cx, cy, inr, outr, icol, ocol);
#endif
}

ImagePatternHandle createImagePattern(Context* ctx, float cx, float cy, float w, float h, float angle, ImageHandle image)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	return ctx->m_VTable->createImagePattern(ctx, cx, cy, w, h, angle, image);
#else
	return ctxCreateImagePattern(ctx, cx, cy, w, h, angle, image);
#endif
}

void pushState(Context* ctx)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->pushState(ctx);
#else
	ctxPushState(ctx);
#endif
}

void popState(Context* ctx)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->popState(ctx);
#else
	ctxPopState(ctx);
#endif
}

void resetScissor(Context* ctx)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->resetScissor(ctx);
#else
	ctxResetScissor(ctx);
#endif
}

void setScissor(Context* ctx, float x, float y, float w, float h)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->setScissor(ctx, x, y, w, h);
#else
	ctxSetScissor(ctx, x, y, w, h);
#endif
}

bool intersectScissor(Context* ctx, float x, float y, float w, float h)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	return ctx->m_VTable->intersectScissor(ctx, x, y, w, h);
#else
	return ctxIntersectScissor(ctx, x, y, w, h);
#endif
}

void transformIdentity(Context* ctx)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->transformIdentity(ctx);
#else
	ctxTransformIdentity(ctx);
#endif
}

void transformScale(Context* ctx, float x, float y)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->transformScale(ctx, x, y);
#else
	ctxTransformScale(ctx, x, y);
#endif
}

void transformTranslate(Context* ctx, float x, float y)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->transformTranslate(ctx, x, y);
#else
	ctxTransformTranslate(ctx, x, y);
#endif
}

void transformRotate(Context* ctx, float ang_rad)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->transformRotate(ctx, ang_rad);
#else
	ctxTransformRotate(ctx, ang_rad);
#endif
}

void transformMult(Context* ctx, const float* mtx, TransformOrder::Enum order)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->transformMult(ctx, mtx, order);
#else
	ctxTransformMult(ctx, mtx, order);
#endif
}

void setViewBox(Context* ctx, float x, float y, float w, float h)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->setViewBox(ctx, x, y, w, h);
#else
	ctxSetViewBox(ctx, x, y, w, h);
#endif
}

void indexedTriList(Context* ctx, const float* pos, const uv_t* uv, uint32_t numVertices, const Color* colors, uint32_t numColors, const uint16_t* indices, uint32_t numIndices, ImageHandle img)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->indexedTriList(ctx, pos, uv, numVertices, colors, numColors, indices, numIndices, img);
#else
	ctxIndexedTriList(ctx, pos, uv, numVertices, colors, numColors, indices, numIndices, img);
#endif
}

void text(Context* ctx, const TextConfig& cfg, float x, float y, const char* str, const char* end)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->text(ctx, cfg, x, y, str, end);
#else
	ctxText(ctx, cfg, x, y, str, end);
#endif
}

void textBox(Context* ctx, const TextConfig& cfg, float x, float y, float breakWidth, const char* str, const char* end, uint32_t textboxFlags)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->textBox(ctx, cfg, x, y, breakWidth, str, end, textboxFlags);
#else
	ctxTextBox(ctx, cfg, x, y, breakWidth, str, end, textboxFlags);
#endif
}

void submitCommandList(Context* ctx, CommandListHandle handle)
{
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	ctx->m_VTable->submitCommandList(ctx, handle);
#else
	ctxSubmitCommandList(ctx, handle);
#endif
}

void setGlobalAlpha(Context* ctx, float alpha)
{
	State* state = getState(ctx);
	state->m_GlobalAlpha = alpha;
}

void getTransform(Context* ctx, float* mtx)
{
	const State* state = getState(ctx);
	bx::memCopy(mtx, state->m_TransformMtx, sizeof(float) * 6);
}

void getScissor(Context* ctx, float* rect)
{
	const State* state = getState(ctx);
	bx::memCopy(rect, state->m_ScissorRect, sizeof(float) * 4);
}

FontHandle createFont(Context* ctx, const char* name, uint8_t* data, uint32_t size, uint32_t flags)
{
	if (ctx->m_NextFontID == ctx->m_Config.m_MaxFonts) {
		return VG_INVALID_HANDLE;
	}

	const bool copyData = (flags & FontFlags::DontCopyData) == 0;

	uint8_t* fontData = nullptr;
	if (copyData) {
		fontData = (uint8_t*)BX_ALLOC(ctx->m_Allocator, size);
		bx::memCopy(fontData, data, size);
	} else {
		fontData = data;
	}

	int fonsHandle = fonsAddFontMem(ctx->m_FontStashContext, name, fontData, size, 0);
	if (fonsHandle == FONS_INVALID) {
		if (copyData) {
			BX_FREE(ctx->m_Allocator, fontData);
		}
		return VG_INVALID_HANDLE;
	}

	FontData* fd = &ctx->m_FontData[ctx->m_NextFontID++];
	fd->m_FonsHandle = fonsHandle;
	fd->m_Data = fontData;
	fd->m_Owned = copyData;

	return { (uint16_t)fonsHandle };
}

FontHandle getFontByName(Context* ctx, const char* name)
{
	const int fonsHandle = fonsGetFontByName(ctx->m_FontStashContext, name);
	return { (uint16_t)fonsHandle };
}

bool setFallbackFont(Context* ctx, FontHandle base, FontHandle fallback)
{
	VG_CHECK(isValid(base) && isValid(fallback), "Invalid font handle");

	FONScontext* fons = ctx->m_FontStashContext;
	return fonsAddFallbackFont(fons, base.idx, fallback.idx) == 1;
}

float measureText(Context* ctx, const TextConfig& cfg, float x, float y, const char* str, const char* end, float* bounds)
{
	// nvgTextBounds()
	const State* state = getState(ctx);
	const float scale = state->m_FontScale * ctx->m_DevicePixelRatio;
	const float invscale = 1.0f / scale;

	FONScontext* fons = ctx->m_FontStashContext;
	fonsSetSize(fons, cfg.m_FontSize * scale);
	fonsSetAlign(fons, cfg.m_Alignment);
	fonsSetFont(fons, cfg.m_FontHandle.idx);

	float width = fonsTextBounds(fons, x * scale, y * scale, str, end, bounds);
	if (bounds != nullptr) {
		// Use line bounds for height.
		fonsLineBounds(fons, y * scale, &bounds[1], &bounds[3]);
		bounds[0] *= invscale;
		bounds[1] *= invscale;
		bounds[2] *= invscale;
		bounds[3] *= invscale;
	}

	return width * invscale;
}

void measureTextBox(Context* ctx, const TextConfig& cfg, float x, float y, float breakWidth, const char* text, const char* end, float* bounds, uint32_t flags)
{
	const State* state = getState(ctx);
	const float scale = state->m_FontScale * ctx->m_DevicePixelRatio;
	const float invscale = 1.0f / scale;

	const uint32_t alignment = cfg.m_Alignment;

	const uint32_t halign = alignment & (FONS_ALIGN_LEFT | FONS_ALIGN_CENTER | FONS_ALIGN_RIGHT);
	const uint32_t valign = alignment & (FONS_ALIGN_TOP | FONS_ALIGN_MIDDLE | FONS_ALIGN_BOTTOM | FONS_ALIGN_BASELINE);

	const uint32_t newAlignment = FONS_ALIGN_LEFT | valign;

	FONScontext* fons = ctx->m_FontStashContext;
	fonsSetAlign(fons, newAlignment);
	fonsSetSize(fons, cfg.m_FontSize * scale);
	fonsSetFont(fons, cfg.m_FontHandle.idx);

	float lineh;
	fonsVertMetrics(fons, nullptr, nullptr, &lineh);
	lineh *= invscale;

	float rminy = 0, rmaxy = 0;
	fonsLineBounds(fons, 0, &rminy, &rmaxy);
	rminy *= invscale;
	rmaxy *= invscale;

	float minx, miny, maxx, maxy;
	minx = maxx = x;
	miny = maxy = y;

	TextRow rows[2];
	int nrows = 0;

	const TextConfig newTextCfg = { cfg.m_FontHandle, cfg.m_FontSize, newAlignment, vg::Colors::Transparent };
	while ((nrows = textBreakLines(ctx, newTextCfg, text, end, breakWidth, rows, 2, flags))) {
		for (uint32_t i = 0; i < (uint32_t)nrows; i++) {
			const TextRow* row = &rows[i];

			// Horizontal bounds
			float dx = 0.0f; // Assume left align
			if (halign & FONS_ALIGN_CENTER) {
				dx = breakWidth * 0.5f - row->width * 0.5f;
			} else if (halign & FONS_ALIGN_RIGHT) {
				dx = breakWidth - row->width;
			}

			const float rminx = x + row->minx + dx;
			const float rmaxx = x + row->maxx + dx;

			minx = bx::min<float>(minx, rminx);
			maxx = bx::max<float>(maxx, rmaxx);

			// Vertical bounds.
			miny = bx::min<float>(miny, y + rminy);
			maxy = bx::max<float>(maxy, y + rmaxy);

			y += lineh; // Assume line height multiplier of 1.0
		}

		text = rows[nrows - 1].next;
	}

	if (bounds != nullptr) {
		bounds[0] = minx;
		bounds[1] = miny;
		bounds[2] = maxx;
		bounds[3] = maxy;
	}
}

float getTextLineHeight(Context* ctx, const TextConfig& cfg)
{
	const State* state = getState(ctx);
	const float scale = state->m_FontScale * ctx->m_DevicePixelRatio;
	const float invscale = 1.0f / scale;

	FONScontext* fons = ctx->m_FontStashContext;
	fonsSetSize(fons, cfg.m_FontSize * scale);
	fonsSetAlign(fons, cfg.m_Alignment);
	fonsSetFont(fons, cfg.m_FontHandle.idx);

	float lineh;
	fonsVertMetrics(fons, nullptr, nullptr, &lineh);
	lineh *= invscale;

	return lineh;
}

int textBreakLines(Context* ctx, const TextConfig& cfg, const char* str, const char* end, float breakRowWidth, TextRow* rows, int maxRows, uint32_t flags)
{
	// nvgTextBreakLines()
#define CP_SPACE  0
#define CP_NEW_LINE  1
#define CP_CHAR 2

	const State* state = getState(ctx);
	const float scale = state->m_FontScale * ctx->m_DevicePixelRatio;
	const float invscale = 1.0f / scale;

	if (maxRows == 0) {
		return 0;
	}

	if (end == nullptr) {
		end = str + bx::strLen(str);
	}

	if (str == end) {
		return 0;
	}

	FONScontext* fons = ctx->m_FontStashContext;
	fonsSetSize(fons, cfg.m_FontSize * scale);
	fonsSetAlign(fons, cfg.m_Alignment);
	fonsSetFont(fons, cfg.m_FontHandle.idx);

	breakRowWidth *= scale;

	int nrows = 0;
	float rowStartX = 0;
	float rowWidth = 0;
	float rowMinX = 0;
	float rowMaxX = 0;
	const char* rowStart = nullptr;
	const char* rowEnd = nullptr;
	const char* wordStart = nullptr;
	float wordStartX = 0;
	float wordMinX = 0;
	const char* breakEnd = nullptr;
	float breakWidth = 0;
	float breakMaxX = 0;
	int type = CP_SPACE, ptype = CP_SPACE;
	unsigned int pcodepoint = 0;

	FONStextIter iter, prevIter;
	fonsTextIterInit(fons, &iter, 0, 0, str, end, FONS_GLYPH_BITMAP_OPTIONAL);
	prevIter = iter;

	FONSquad q;
	while (fonsTextIterNext(fons, &iter, &q)) {
		if (iter.prevGlyphIndex < 0 && allocTextAtlas(ctx)) {
			// can not retrieve glyph?
			iter = prevIter;
			fonsTextIterNext(fons, &iter, &q); // try again
		}

		prevIter = iter;
		switch (iter.codepoint) {
		case 9:			// \t
		case 11:		// \v
		case 12:		// \f
		case 0x00a0:	// NBSP
			type = CP_SPACE;
			break;
		case 32:		// space 
			// JD: Treat spaces as regular characters in order to be able to have pre and post spaces in an edit box.
			if (flags & TextBoxFlags::KeepSpaces) {
				type = CP_CHAR;
			} else {
				type = CP_SPACE;
			}
			break;
		case 10:		// \n
			type = pcodepoint == 13 ? CP_SPACE : CP_NEW_LINE;
			break;
		case 13:		// \r
			type = pcodepoint == 10 ? CP_SPACE : CP_NEW_LINE;
			break;
		case 0x0085:	// NEL
			type = CP_NEW_LINE;
			break;
		default:
			type = CP_CHAR;
			break;
		}

		if (type == CP_NEW_LINE) {
			// Always handle new lines.
			rows[nrows].start = rowStart != nullptr ? rowStart : iter.str;
			rows[nrows].end = rowEnd != nullptr ? rowEnd : iter.str;
			rows[nrows].width = rowWidth * invscale;
			rows[nrows].minx = rowMinX * invscale;
			rows[nrows].maxx = rowMaxX * invscale;
			rows[nrows].next = iter.next;
			nrows++;
			if (nrows >= maxRows) {
				return nrows;
			}

			// Set null break point
			breakEnd = rowStart;
			breakWidth = 0.0;
			breakMaxX = 0.0;

			// Indicate to skip the white space at the beginning of the row.
			rowStart = nullptr;
			rowEnd = nullptr;
			rowWidth = 0;
			rowMinX = rowMaxX = 0;
		} else {
			if (rowStart == nullptr) {
				// Skip white space until the beginning of the line
				if (type == CP_CHAR) {
					// The current char is the row so far
					rowStartX = iter.x;
					rowStart = iter.str;
					rowEnd = iter.next;
					rowWidth = iter.nextx - rowStartX; // q.x1 - rowStartX;
					rowMinX = q.x0 - rowStartX;
					rowMaxX = q.x1 - rowStartX;
					wordStart = iter.str;
					wordStartX = iter.x;
					wordMinX = q.x0 - rowStartX;

					// Set null break point
					breakEnd = rowStart;
					breakWidth = 0.0;
					breakMaxX = 0.0;
				}
			} else {
				float nextWidth = iter.nextx - rowStartX;

				// track last non-white space character
				if (type == CP_CHAR) {
					rowEnd = iter.next;
//					rowWidth = iter.nextx - rowStartX;
					rowMaxX = q.x1 - rowStartX;
				}

				// track last end of a word
				if (ptype == CP_CHAR && type == CP_SPACE) {
					breakEnd = iter.str;
					breakWidth = nextWidth; // rowWidth;
					breakMaxX = rowMaxX;
				}

				// track last beginning of a word
				if (ptype == CP_SPACE && type == CP_CHAR) {
					wordStart = iter.str;
					wordStartX = iter.x;
					wordMinX = q.x0 - rowStartX;
				}

				// Break to new line when a character is beyond break width.
				if (type == CP_CHAR && nextWidth > breakRowWidth) {
					// The run length is too long, need to break to new line.
					if (breakEnd == rowStart) {
						// The current word is longer than the row length, just break it from here.
						rows[nrows].start = rowStart;
						rows[nrows].end = iter.str;
						rows[nrows].width = rowWidth * invscale;
						rows[nrows].minx = rowMinX * invscale;
						rows[nrows].maxx = rowMaxX * invscale;
						rows[nrows].next = iter.str;
						nrows++;
						if (nrows >= maxRows) {
							return nrows;
						}
						rowStartX = iter.x;
						rowStart = iter.str;
						rowEnd = iter.next;
						rowWidth = iter.nextx - rowStartX;
						rowMinX = q.x0 - rowStartX;
						rowMaxX = q.x1 - rowStartX;
						wordStart = iter.str;
						wordStartX = iter.x;
						wordMinX = q.x0 - rowStartX;
					} else {
						// Break the line from the end of the last word, and start new line from the beginning of the new.
						rows[nrows].start = rowStart;
						rows[nrows].end = breakEnd;
						rows[nrows].width = breakWidth * invscale;
						rows[nrows].minx = rowMinX * invscale;
						rows[nrows].maxx = breakMaxX * invscale;
						rows[nrows].next = wordStart;
						nrows++;
						if (nrows >= maxRows) {
							return nrows;
						}
						rowStartX = wordStartX;
						rowStart = wordStart;
						rowEnd = iter.next;
						rowWidth = iter.nextx - rowStartX;
						rowMinX = wordMinX;
						rowMaxX = q.x1 - rowStartX;
						// No change to the word start
					}
					// Set null break point
					breakEnd = rowStart;
					breakWidth = 0.0;
					breakMaxX = 0.0;
				} else {
					rowWidth = nextWidth;
				}
			}
		}

		pcodepoint = iter.codepoint;
		ptype = type;
	}

	// Break the line from the end of the last word, and start new line from the beginning of the new.
	if (rowStart != nullptr) {
		rows[nrows].start = rowStart;
		rows[nrows].end = rowEnd;
		rows[nrows].width = rowWidth * invscale;
		rows[nrows].minx = rowMinX * invscale;
		rows[nrows].maxx = rowMaxX * invscale;
		rows[nrows].next = end;
		nrows++;
	}

	return nrows;

#undef CP_SPACE
#undef CP_NEW_LINE
#undef CP_CHAR
}

int textGlyphPositions(Context* ctx, const TextConfig& cfg, float x, float y, const char* str, const char* end, GlyphPosition* positions, int maxPositions)
{
	const State* state = getState(ctx);
	const float scale = state->m_FontScale * ctx->m_DevicePixelRatio;
	const float invscale = 1.0f / scale;

	if (!end) {
		end = str + bx::strLen(str);
	}

	if (str == end) {
		return 0;
	}

	FONScontext* fons = ctx->m_FontStashContext;
	fonsSetSize(fons, cfg.m_FontSize * scale);
	fonsSetAlign(fons, cfg.m_Alignment);
	fonsSetFont(fons, cfg.m_FontHandle.idx);

	FONStextIter iter, prevIter;
	fonsTextIterInit(fons, &iter, x * scale, y * scale, str, end, FONS_GLYPH_BITMAP_OPTIONAL);
	prevIter = iter;

	FONSquad q;
	int npos = 0;
	while (fonsTextIterNext(fons, &iter, &q)) {
		if (iter.prevGlyphIndex < 0 && allocTextAtlas(ctx)) {
			iter = prevIter;
			fonsTextIterNext(fons, &iter, &q);
		}

		prevIter = iter;
		positions[npos].str = iter.str;
		positions[npos].x = iter.x * invscale;
		positions[npos].minx = bx::min<float>(iter.x, q.x0) * invscale;
		positions[npos].maxx = bx::max<float>(iter.nextx, q.x1) * invscale;

		npos++;
		if (npos >= maxPositions) {
			break;
		}
	}

	return npos;
}

bool getImageSize(Context* ctx, ImageHandle handle, uint16_t* w, uint16_t* h)
{
	if (!isValid(handle)) {
		*w = UINT16_MAX;
		*h = UINT16_MAX;
		return false;
	}

	Image* img = &ctx->m_Images[handle.idx];
	if (!bgfx::isValid(img->m_bgfxHandle)) {
		*w = UINT16_MAX;
		*h = UINT16_MAX;
		return false;
	}

	*w = img->m_Width;
	*h = img->m_Height;

	return true;
}

ImageHandle createImage(Context* ctx, uint16_t w, uint16_t h, uint32_t flags, const uint8_t* data)
{
	ImageHandle handle = allocImage(ctx);
	if (!isValid(handle)) {
		return VG_INVALID_HANDLE;
	}

	Image* tex = &ctx->m_Images[handle.idx];
	tex->m_Width = w;
	tex->m_Height = h;

	uint32_t bgfxFlags = BGFX_SAMPLER_NONE;

#if BX_PLATFORM_EMSCRIPTEN
	if (!bx::isPowerOf2(w) || !bx::isPowerOf2(h)) {
		flags = ImageFlags::Filter_NearestUV | ImageFlags::Filter_NearestW;
		bgfxFlags |= BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_W_CLAMP;
	}
#endif

	if (flags & ImageFlags::Filter_NearestUV) {
		bgfxFlags |= BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT;
	}
	if (flags & ImageFlags::Filter_NearestW) {
		bgfxFlags |= BGFX_SAMPLER_MIP_POINT;
	}
	tex->m_Flags = bgfxFlags;

	tex->m_bgfxHandle = bgfx::createTexture2D(tex->m_Width, tex->m_Height, false, 1, bgfx::TextureFormat::RGBA8, bgfxFlags);

	if (bgfx::isValid(tex->m_bgfxHandle) && data) {
		const uint32_t bytesPerPixel = 4;
		const uint32_t pitch = tex->m_Width * bytesPerPixel;
		const bgfx::Memory* mem = bgfx::copy(data, tex->m_Height * pitch);

		bgfx::updateTexture2D(tex->m_bgfxHandle, 0, 0, 0, 0, tex->m_Width, tex->m_Height, mem);
	}

	return handle;
}

ImageHandle createImage(Context* ctx, uint32_t flags, const bgfx::TextureHandle& bgfxTextureHandle)
{
	VG_CHECK(bgfx::isValid(bgfxTextureHandle), "Invalid bgfx texture handle");

	ImageHandle handle = allocImage(ctx);
	if (!isValid(handle)) {
		return VG_INVALID_HANDLE;
	}

	Image* tex = &ctx->m_Images[handle.idx];
	tex->m_Width = UINT16_MAX;
	tex->m_Height = UINT16_MAX;

	uint32_t bgfxFlags = BGFX_TEXTURE_NONE;

	if (flags & ImageFlags::Filter_NearestUV) {
		bgfxFlags |= BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT;
	}
	if (flags & ImageFlags::Filter_NearestW) {
		bgfxFlags |= BGFX_SAMPLER_MIP_POINT;
	}
	tex->m_Flags = bgfxFlags;

	tex->m_bgfxHandle.idx = bgfxTextureHandle.idx;

	return handle;
}

bool updateImage(Context* ctx, ImageHandle image, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t* data)
{
	if (!isValid(image)) {
		return false;
	}

	Image* tex = &ctx->m_Images[image.idx];
	VG_CHECK(bgfx::isValid(tex->m_bgfxHandle), "Invalid texture handle");

	const uint32_t bytesPerPixel = 4;
	const uint32_t pitch = tex->m_Width * bytesPerPixel;

	const bgfx::Memory* mem = bgfx::alloc(w * h * bytesPerPixel);
	bx::gather(mem->data, data + y * pitch + x * bytesPerPixel, pitch, w * bytesPerPixel, h);

	bgfx::updateTexture2D(tex->m_bgfxHandle, 0, 0, x, y, w, h, mem, UINT16_MAX);

	return true;
}

bool destroyImage(Context* ctx, ImageHandle img)
{
	if (!isValid(img)) {
		return false;
	}

	Image* tex = &ctx->m_Images[img.idx];
	VG_CHECK(bgfx::isValid(tex->m_bgfxHandle), "Invalid texture handle");
	bgfx::destroy(tex->m_bgfxHandle);
	resetImage(tex);

	ctx->m_ImageHandleAlloc->free(img.idx);

	return true;
}

bool isImageValid(Context* ctx, ImageHandle image)
{
	if (!isValid(image)) {
		return false;
	}

	Image* tex = &ctx->m_Images[image.idx];
	return bgfx::isValid(tex->m_bgfxHandle);
}

CommandListHandle createCommandList(Context* ctx, uint32_t flags)
{
	VG_CHECK(!isValid(ctx->m_ActiveCommandList), "Cannot create command list while inside a beginCommandList()/endCommandList() block");

	CommandListHandle handle = allocCommandList(ctx);
	if (!isValid(handle)) {
		return VG_INVALID_HANDLE;
	}

	CommandList* cl = &ctx->m_CmdLists[handle.idx];
	cl->m_Flags = flags;

	return handle;
}

void destroyCommandList(Context* ctx, CommandListHandle handle)
{
	VG_CHECK(!isValid(ctx->m_ActiveCommandList), "Cannot destroy command list while inside a beginCommandList()/endCommandList() block");
	VG_CHECK(isValid(handle), "Invalid command list handle");

	bx::AllocatorI* allocator = ctx->m_Allocator;

	CommandList* cl = &ctx->m_CmdLists[handle.idx];

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	if (cl->m_Cache) {
		freeCommandListCache(ctx, cl->m_Cache);
	}
#endif

	ctx->m_Stats.m_CmdListMemoryTotal -= cl->m_CommandBufferCapacity;
	ctx->m_Stats.m_CmdListMemoryUsed -= cl->m_CommandBufferPos;

	BX_ALIGNED_FREE(allocator, cl->m_CommandBuffer, VG_CONFIG_COMMAND_LIST_ALIGNMENT);
	BX_FREE(allocator, cl->m_StringBuffer);
	bx::memSet(cl, 0, sizeof(CommandList));

	ctx->m_CmdListHandleAlloc->free(handle.idx);
}

void resetCommandList(Context* ctx, CommandListHandle handle)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	if (cl->m_Cache) {
		clCacheReset(ctx, cl->m_Cache);
	}
#else
	BX_UNUSED(ctx);
#endif

	ctx->m_Stats.m_CmdListMemoryUsed -= cl->m_CommandBufferPos;
	cl->m_CommandBufferPos = 0;
	cl->m_StringBufferPos = 0;
	cl->m_NumImagePatterns = 0;
	cl->m_NumGradients = 0;
}

#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
void beginCommandList(Context* ctx, CommandListHandle handle)
{
	VG_CHECK(!isValid(ctx->m_ActiveCommandList), "Cannot call beginCommandList() while inside a beginCommandList()/endCommandList() block");
	VG_CHECK(isValid(handle), "Invalid command list handle");

	ctx->m_ActiveCommandList = handle;

	ctx->m_VTable = &g_ActiveCmdListVTable;
}

void endCommandList(Context* ctx)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "beginCommandList() hasn't been called yet.");
	ctx->m_ActiveCommandList = VG_INVALID_HANDLE;

	ctx->m_VTable = &g_CtxVTable;
}
#endif

void clBeginPath(Context* ctx, CommandListHandle handle)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	clAllocCommand(ctx, cl, CommandType::BeginPath, 0);
}

void clMoveTo(Context* ctx, CommandListHandle handle, float x, float y)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::MoveTo, sizeof(float) * 2);
	CMD_WRITE(ptr, float, x);
	CMD_WRITE(ptr, float, y);
}

void clLineTo(Context* ctx, CommandListHandle handle, float x, float y)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::LineTo, sizeof(float) * 2);
	CMD_WRITE(ptr, float, x);
	CMD_WRITE(ptr, float, y);
}

void clCubicTo(Context* ctx, CommandListHandle handle, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::CubicTo, sizeof(float) * 6);
	CMD_WRITE(ptr, float, c1x);
	CMD_WRITE(ptr, float, c1y);
	CMD_WRITE(ptr, float, c2x);
	CMD_WRITE(ptr, float, c2y);
	CMD_WRITE(ptr, float, x);
	CMD_WRITE(ptr, float, y);
}

void clQuadraticTo(Context* ctx, CommandListHandle handle, float cx, float cy, float x, float y)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::QuadraticTo, sizeof(float) * 4);
	CMD_WRITE(ptr, float, cx);
	CMD_WRITE(ptr, float, cy);
	CMD_WRITE(ptr, float, x);
	CMD_WRITE(ptr, float, y);
}

void clArc(Context* ctx, CommandListHandle handle, float cx, float cy, float r, float a0, float a1, Winding::Enum dir)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::Arc, sizeof(float) * 5 + sizeof(Winding::Enum));
	CMD_WRITE(ptr, float, cx);
	CMD_WRITE(ptr, float, cy);
	CMD_WRITE(ptr, float, r);
	CMD_WRITE(ptr, float, a0);
	CMD_WRITE(ptr, float, a1);
	CMD_WRITE(ptr, Winding::Enum, dir);
}

void clArcTo(Context* ctx, CommandListHandle handle, float x1, float y1, float x2, float y2, float r)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::ArcTo, sizeof(float) * 5);
	CMD_WRITE(ptr, float, x1);
	CMD_WRITE(ptr, float, y1);
	CMD_WRITE(ptr, float, x2);
	CMD_WRITE(ptr, float, y2);
	CMD_WRITE(ptr, float, r);
}

void clRect(Context* ctx, CommandListHandle handle, float x, float y, float w, float h)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::Rect, sizeof(float) * 4);
	CMD_WRITE(ptr, float, x);
	CMD_WRITE(ptr, float, y);
	CMD_WRITE(ptr, float, w);
	CMD_WRITE(ptr, float, h);
}

void clRoundedRect(Context* ctx, CommandListHandle handle, float x, float y, float w, float h, float r)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::RoundedRect, sizeof(float) * 5);
	CMD_WRITE(ptr, float, x);
	CMD_WRITE(ptr, float, y);
	CMD_WRITE(ptr, float, w);
	CMD_WRITE(ptr, float, h);
	CMD_WRITE(ptr, float, r);
}

void clRoundedRectVarying(Context* ctx, CommandListHandle handle, float x, float y, float w, float h, float rtl, float rtr, float rbr, float rbl)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::RoundedRectVarying, sizeof(float) * 8);
	CMD_WRITE(ptr, float, x);
	CMD_WRITE(ptr, float, y);
	CMD_WRITE(ptr, float, w);
	CMD_WRITE(ptr, float, h);
	CMD_WRITE(ptr, float, rtl);
	CMD_WRITE(ptr, float, rtr);
	CMD_WRITE(ptr, float, rbr);
	CMD_WRITE(ptr, float, rbl);
}

void clCircle(Context* ctx, CommandListHandle handle, float cx, float cy, float radius)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::Circle, sizeof(float) * 3);
	CMD_WRITE(ptr, float, cx);
	CMD_WRITE(ptr, float, cy);
	CMD_WRITE(ptr, float, radius);
}

void clEllipse(Context* ctx, CommandListHandle handle, float cx, float cy, float rx, float ry)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::Ellipse, sizeof(float) * 4);
	CMD_WRITE(ptr, float, cx);
	CMD_WRITE(ptr, float, cy);
	CMD_WRITE(ptr, float, rx);
	CMD_WRITE(ptr, float, ry);
}

void clPolyline(Context* ctx, CommandListHandle handle, const float* coords, uint32_t numPoints)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::Polyline, sizeof(uint32_t) + sizeof(float) * 2 * numPoints);
	CMD_WRITE(ptr, uint32_t, numPoints);
	bx::memCopy(ptr, coords, sizeof(float) * 2 * numPoints);
}

void clClosePath(Context* ctx, CommandListHandle handle)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	clAllocCommand(ctx, cl, CommandType::ClosePath, 0);
}

void clIndexedTriList(Context* ctx, CommandListHandle handle, const float* pos, const uv_t* uv, uint32_t numVertices, const Color* color, uint32_t numColors, const uint16_t* indices, uint32_t numIndices, ImageHandle img)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	const uint32_t dataSize = 0
		+ sizeof(uint32_t) // num positions
		+ sizeof(float) * 2 * numVertices // positions
		+ sizeof(uint32_t) // num UVs
		+ (uv != nullptr ? (sizeof(uv_t) * 2 * numVertices) : 0) // UVs
		+ sizeof(uint32_t) // num colors
		+ sizeof(Color) * numColors // colors
		+ sizeof(uint32_t) // num indices
		+ sizeof(uint16_t) * numIndices // indices
		+ sizeof(uint16_t) // image handle
		;

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::IndexedTriList, dataSize);

	// positions
	CMD_WRITE(ptr, uint32_t, numVertices);
	bx::memCopy(ptr, pos, sizeof(float) * 2 * numVertices);
	ptr += sizeof(float) * 2 * numVertices;

	// UVs
	if (uv) {
		CMD_WRITE(ptr, uint32_t, numVertices);
		bx::memCopy(ptr, uv, sizeof(uv_t) * 2 * numVertices);
		ptr += sizeof(uv_t) * 2 * numVertices;
	} else {
		CMD_WRITE(ptr, uint32_t, 0);
	}

	// Colors
	CMD_WRITE(ptr, uint32_t, numColors);
	bx::memCopy(ptr, color, sizeof(Color) * numColors);
	ptr += sizeof(Color) * numColors;

	// Indices
	CMD_WRITE(ptr, uint32_t, numIndices);
	bx::memCopy(ptr, indices, sizeof(uint16_t) * numIndices);
	ptr += sizeof(uint16_t) * numIndices;

	// Image
	CMD_WRITE(ptr, uint16_t, img.idx);
}

void clFillPath(Context* ctx, CommandListHandle handle, Color color, uint32_t flags)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::FillPathColor, sizeof(uint32_t) + sizeof(Color));
	CMD_WRITE(ptr, uint32_t, flags);
	CMD_WRITE(ptr, Color, color);
}

void clFillPath(Context* ctx, CommandListHandle handle, GradientHandle gradient, uint32_t flags)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	VG_CHECK(isValid(gradient), "Invalid gradient handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	VG_CHECK(isValid(gradient), "Invalid gradient handle");

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::FillPathGradient, sizeof(uint32_t) + sizeof(uint16_t) * 2);
	CMD_WRITE(ptr, uint32_t, flags);
	CMD_WRITE(ptr, uint16_t, gradient.idx);
	CMD_WRITE(ptr, uint16_t, gradient.flags);
}

void clFillPath(Context* ctx, CommandListHandle handle, ImagePatternHandle img, Color color, uint32_t flags)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	VG_CHECK(isValid(img), "Invalid image pattern handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	VG_CHECK(isValid(img), "Invalid image pattern handle");

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::FillPathImagePattern, sizeof(uint32_t) + sizeof(Color) + sizeof(uint16_t) * 2);
	CMD_WRITE(ptr, uint32_t, flags);
	CMD_WRITE(ptr, Color, color);
	CMD_WRITE(ptr, uint16_t, img.idx);
	CMD_WRITE(ptr, uint16_t, img.flags);
}

void clStrokePath(Context* ctx, CommandListHandle handle, Color color, float width, uint32_t flags)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::StrokePathColor, sizeof(float) + sizeof(uint32_t) + sizeof(Color));
	CMD_WRITE(ptr, float, width);
	CMD_WRITE(ptr, uint32_t, flags);
	CMD_WRITE(ptr, Color, color);
}

void clStrokePath(Context* ctx, CommandListHandle handle, GradientHandle gradient, float width, uint32_t flags)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	VG_CHECK(isValid(gradient), "Invalid gradient handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	VG_CHECK(isValid(gradient), "Invalid gradient handle");

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::StrokePathGradient, sizeof(float) + sizeof(uint32_t) + sizeof(uint16_t) * 2);
	CMD_WRITE(ptr, float, width);
	CMD_WRITE(ptr, uint32_t, flags);
	CMD_WRITE(ptr, uint16_t, gradient.idx);
	CMD_WRITE(ptr, uint16_t, gradient.flags);
}

void clStrokePath(Context* ctx, CommandListHandle handle, ImagePatternHandle img, Color color, float width, uint32_t flags)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	VG_CHECK(isValid(img), "Invalid image pattern handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	VG_CHECK(isValid(img), "Invalid image pattern handle");

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::StrokePathImagePattern, sizeof(float) + sizeof(uint32_t) + sizeof(Color) + sizeof(uint16_t) * 2);
	CMD_WRITE(ptr, float, width);
	CMD_WRITE(ptr, uint32_t, flags);
	CMD_WRITE(ptr, Color, color);
	CMD_WRITE(ptr, uint16_t, img.idx);
	CMD_WRITE(ptr, uint16_t, img.flags);
}

void clBeginClip(Context* ctx, CommandListHandle handle, ClipRule::Enum rule)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::BeginClip, sizeof(ClipRule::Enum));
	CMD_WRITE(ptr, ClipRule::Enum, rule);
}

void clEndClip(Context* ctx, CommandListHandle handle)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	clAllocCommand(ctx, cl, CommandType::EndClip, 0);
}

void clResetClip(Context* ctx, CommandListHandle handle)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	clAllocCommand(ctx, cl, CommandType::ResetClip, 0);
}

GradientHandle clCreateLinearGradient(Context* ctx, CommandListHandle handle, float sx, float sy, float ex, float ey, Color icol, Color ocol)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::CreateLinearGradient, sizeof(float) * 4 + sizeof(Color) * 2);
	CMD_WRITE(ptr, float, sx);
	CMD_WRITE(ptr, float, sy);
	CMD_WRITE(ptr, float, ex);
	CMD_WRITE(ptr, float, ey);
	CMD_WRITE(ptr, Color, icol);
	CMD_WRITE(ptr, Color, ocol);

	const uint16_t gradientHandle = cl->m_NumGradients;
	cl->m_NumGradients++;
	return { gradientHandle, HandleFlags::LocalHandle };
}

GradientHandle clCreateBoxGradient(Context* ctx, CommandListHandle handle, float x, float y, float w, float h, float r, float f, Color icol, Color ocol)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::CreateBoxGradient, sizeof(float) * 6 + sizeof(Color) * 2);
	CMD_WRITE(ptr, float, x);
	CMD_WRITE(ptr, float, y);
	CMD_WRITE(ptr, float, w);
	CMD_WRITE(ptr, float, h);
	CMD_WRITE(ptr, float, r);
	CMD_WRITE(ptr, float, f);
	CMD_WRITE(ptr, Color, icol);
	CMD_WRITE(ptr, Color, ocol);

	const uint16_t gradientHandle = cl->m_NumGradients;
	cl->m_NumGradients++;
	return { gradientHandle, HandleFlags::LocalHandle };
}

GradientHandle clCreateRadialGradient(Context* ctx, CommandListHandle handle, float cx, float cy, float inr, float outr, Color icol, Color ocol)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::CreateRadialGradient, sizeof(float) * 4 + sizeof(Color) * 2);
	CMD_WRITE(ptr, float, cx);
	CMD_WRITE(ptr, float, cy);
	CMD_WRITE(ptr, float, inr);
	CMD_WRITE(ptr, float, outr);
	CMD_WRITE(ptr, Color, icol);
	CMD_WRITE(ptr, Color, ocol);

	const uint16_t gradientHandle = cl->m_NumGradients;
	cl->m_NumGradients++;
	return { gradientHandle, HandleFlags::LocalHandle };
}

ImagePatternHandle clCreateImagePattern(Context* ctx, CommandListHandle handle, float cx, float cy, float w, float h, float angle, ImageHandle image)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	VG_CHECK(isValid(image), "Invalid image handle");

	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	VG_CHECK(isValid(image), "Invalid image handle");

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::CreateImagePattern, sizeof(float) * 5 + sizeof(uint16_t));
	CMD_WRITE(ptr, float, cx);
	CMD_WRITE(ptr, float, cy);
	CMD_WRITE(ptr, float, w);
	CMD_WRITE(ptr, float, h);
	CMD_WRITE(ptr, float, angle);
	CMD_WRITE(ptr, uint16_t, image.idx);

	const uint16_t patternHandle = cl->m_NumImagePatterns;
	cl->m_NumImagePatterns++;
	return { patternHandle, HandleFlags::LocalHandle };
}

void clPushState(Context* ctx, CommandListHandle handle)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	clAllocCommand(ctx, cl, CommandType::PushState, 0);
}

void clPopState(Context* ctx, CommandListHandle handle)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	clAllocCommand(ctx, cl, CommandType::PopState, 0);
}

void clResetScissor(Context* ctx, CommandListHandle handle)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	clAllocCommand(ctx, cl, CommandType::ResetScissor, 0);
}

void clSetScissor(Context* ctx, CommandListHandle handle, float x, float y, float w, float h)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::SetScissor, sizeof(float) * 4);
	CMD_WRITE(ptr, float, x);
	CMD_WRITE(ptr, float, y);
	CMD_WRITE(ptr, float, w);
	CMD_WRITE(ptr, float, h);
}

void clIntersectScissor(Context* ctx, CommandListHandle handle, float x, float y, float w, float h)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::IntersectScissor, sizeof(float) * 4);
	CMD_WRITE(ptr, float, x);
	CMD_WRITE(ptr, float, y);
	CMD_WRITE(ptr, float, w);
	CMD_WRITE(ptr, float, h);
}

void clTransformIdentity(Context* ctx, CommandListHandle handle)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	clAllocCommand(ctx, cl, CommandType::TransformIdentity, 0);
}

void clTransformScale(Context* ctx, CommandListHandle handle, float x, float y)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::TransformScale, sizeof(float) * 2);
	CMD_WRITE(ptr, float, x);
	CMD_WRITE(ptr, float, y);
}

void clTransformTranslate(Context* ctx, CommandListHandle handle, float x, float y)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::TransformTranslate, sizeof(float) * 2);
	CMD_WRITE(ptr, float, x);
	CMD_WRITE(ptr, float, y);
}

void clTransformRotate(Context* ctx, CommandListHandle handle, float ang_rad)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::TransformRotate, sizeof(float));
	CMD_WRITE(ptr, float, ang_rad);
}

void clTransformMult(Context* ctx, CommandListHandle handle, const float* mtx, TransformOrder::Enum order)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::TransformMult, sizeof(float) * 6 + sizeof(TransformOrder::Enum));
	bx::memCopy(ptr, mtx, sizeof(float) * 6);
	ptr += sizeof(float) * 6;
	CMD_WRITE(ptr, TransformOrder::Enum, order);
}

void clSetViewBox(Context* ctx, CommandListHandle handle, float x, float y, float w, float h)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::SetViewBox, sizeof(float) * 4);
	CMD_WRITE(ptr, float, x);
	CMD_WRITE(ptr, float, y);
	CMD_WRITE(ptr, float, w);
	CMD_WRITE(ptr, float, h);
}

void clText(Context* ctx, CommandListHandle handle, const TextConfig& cfg, float x, float y, const char* str, const char* end)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	const uint32_t len = end ? (uint32_t)(end - str) : (uint32_t)bx::strLen(str);
	if (len == 0) {
		return;
	}

	const uint32_t offset = clStoreString(ctx, cl, str, len);

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::Text, sizeof(TextConfig) + sizeof(float) * 2 + sizeof(uint32_t) * 2);
	bx::memCopy(ptr, &cfg, sizeof(TextConfig));
	ptr += sizeof(TextConfig);
	CMD_WRITE(ptr, float, x);
	CMD_WRITE(ptr, float, y);
	CMD_WRITE(ptr, uint32_t, offset);
	CMD_WRITE(ptr, uint32_t, len);
}

void clTextBox(Context* ctx, CommandListHandle handle, const TextConfig& cfg, float x, float y, float breakWidth, const char* str, const char* end, uint32_t textboxFlags)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	const uint32_t len = end ? (uint32_t)(end - str) : (uint32_t)bx::strLen(str);
	if (len == 0) {
		return;
	}

	const uint32_t offset = clStoreString(ctx, cl, str, len);

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::TextBox, sizeof(TextConfig) + sizeof(float) * 3 + sizeof(uint32_t) * 3);
	bx::memCopy(ptr, &cfg, sizeof(TextConfig));
	ptr += sizeof(TextConfig);
	CMD_WRITE(ptr, float, x);
	CMD_WRITE(ptr, float, y);
	CMD_WRITE(ptr, float, breakWidth);
	CMD_WRITE(ptr, uint32_t, offset);
	CMD_WRITE(ptr, uint32_t, len);
	CMD_WRITE(ptr, uint32_t, textboxFlags);
}

void clSubmitCommandList(Context* ctx, CommandListHandle parent, CommandListHandle child)
{
	VG_CHECK(isValid(parent), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[parent.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::SubmitCommandList, sizeof(uint16_t));
	CMD_WRITE(ptr, uint16_t, child.idx);
}

// Context
static void ctxBeginPath(Context* ctx)
{
	const State* state = getState(ctx);
	const float avgScale = state->m_AvgScale;
	const float testTol = ctx->m_TesselationTolerance;
	const float fringeWidth = ctx->m_FringeWidth;
	Path* path = ctx->m_Path;
	Stroker* stroker = ctx->m_Stroker;

	pathReset(path, avgScale, testTol);
	strokerReset(stroker, avgScale, testTol, fringeWidth);
	ctx->m_PathTransformed = false;
}

static void ctxMoveTo(Context* ctx, float x, float y)
{
	VG_CHECK(!ctx->m_PathTransformed, "Call beginPath() before starting a new path");
	pathMoveTo(ctx->m_Path, x, y);
}

static void ctxLineTo(Context* ctx, float x, float y)
{
	VG_CHECK(!ctx->m_PathTransformed, "Call beginPath() before starting a new path");
	pathLineTo(ctx->m_Path, x, y);
}

static void ctxCubicTo(Context* ctx, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
	VG_CHECK(!ctx->m_PathTransformed, "Call beginPath() before starting a new path");
	pathCubicTo(ctx->m_Path, c1x, c1y, c2x, c2y, x, y);
}

static void ctxQuadraticTo(Context* ctx, float cx, float cy, float x, float y)
{
	VG_CHECK(!ctx->m_PathTransformed, "Call beginPath() before starting a new path");
	pathQuadraticTo(ctx->m_Path, cx, cy, x, y);
}

static void ctxArc(Context* ctx, float cx, float cy, float r, float a0, float a1, Winding::Enum dir)
{
	VG_CHECK(!ctx->m_PathTransformed, "Call beginPath() before starting a new path");
	pathArc(ctx->m_Path, cx, cy, r, a0, a1, dir);
}

static void ctxArcTo(Context* ctx, float x1, float y1, float x2, float y2, float r)
{
	VG_CHECK(!ctx->m_PathTransformed, "Call beginPath() before starting a new path");
	pathArcTo(ctx->m_Path, x1, y1, x2, y2, r);
}

static void ctxRect(Context* ctx, float x, float y, float w, float h)
{
	VG_CHECK(!ctx->m_PathTransformed, "Call beginPath() before starting a new path");
	pathRect(ctx->m_Path, x, y, w, h);
}

static void ctxRoundedRect(Context* ctx, float x, float y, float w, float h, float r)
{
	VG_CHECK(!ctx->m_PathTransformed, "Call beginPath() before starting a new path");
	pathRoundedRect(ctx->m_Path, x, y, w, h, r);
}

static void ctxRoundedRectVarying(Context* ctx, float x, float y, float w, float h, float rtl, float rtr, float rbr, float rbl)
{
	VG_CHECK(!ctx->m_PathTransformed, "Call beginPath() before starting a new path");
	pathRoundedRectVarying(ctx->m_Path, x, y, w, h, rtl, rtr, rbr, rbl);
}

static void ctxCircle(Context* ctx, float cx, float cy, float radius)
{
	VG_CHECK(!ctx->m_PathTransformed, "Call beginPath() before starting a new path");
	pathCircle(ctx->m_Path, cx, cy, radius);
}

static void ctxEllipse(Context* ctx, float cx, float cy, float rx, float ry)
{
	VG_CHECK(!ctx->m_PathTransformed, "Call beginPath() before starting a new path");
	pathEllipse(ctx->m_Path, cx, cy, rx, ry);
}

static void ctxPolyline(Context* ctx, const float* coords, uint32_t numPoints)
{
	VG_CHECK(!ctx->m_PathTransformed, "Call beginPath() before starting a new path");
	pathPolyline(ctx->m_Path, coords, numPoints);
}

static void ctxClosePath(Context* ctx)
{
	VG_CHECK(!ctx->m_PathTransformed, "Call beginPath() before starting a new path");
	pathClose(ctx->m_Path);
}

static void ctxFillPathColor(Context* ctx, Color color, uint32_t flags)
{
	const bool recordClipCommands = ctx->m_RecordClipCommands;
#if VG_CONFIG_ENABLE_SHAPE_CACHING
	const bool hasCache = getCommandListCacheStackTop(ctx) != nullptr;
#else
	const bool hasCache = false;
#endif

	const State* state = getState(ctx);
	const float globalAlpha = hasCache ? 1.0f : state->m_GlobalAlpha;
	const Color col = recordClipCommands ? Colors::Black : colorSetAlpha(color, (uint8_t)(globalAlpha * colorGetAlpha(color)));
	if (!hasCache && colorGetAlpha(col) == 0) {
		return;
	}

	const float* pathVertices = transformPath(ctx);

#if VG_CONFIG_FORCE_AA_OFF
	const bool aa = false;
#else
	const bool aa = recordClipCommands ? false : VG_FILL_FLAGS_AA(flags);
#endif
	const PathType::Enum pathType = VG_FILL_FLAGS_PATH_TYPE(flags);
	const FillRule::Enum fillRule = VG_FILL_FLAGS_RULE(flags);

	const Path* path = ctx->m_Path;
	const uint32_t numSubPaths = pathGetNumSubPaths(path);
	const SubPath* subPaths = pathGetSubPaths(path);
	Stroker* stroker = ctx->m_Stroker;

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	if (hasCache) {
		beginCachedCommand(ctx);
	}
#endif

	if (pathType == PathType::Convex) {
		for (uint32_t i = 0; i < numSubPaths; ++i) {
			const SubPath* subPath = &subPaths[i];
			if (subPath->m_NumVertices < 3) {
				continue;
			}

			const float* vtx = &pathVertices[subPath->m_FirstVertexID << 1];
			const uint32_t numPathVertices = subPath->m_NumVertices;

			Mesh mesh;
			const uint32_t* colors = &col;
			uint32_t numColors = 1;

			if (aa) {
				strokerConvexFillAA(stroker, &mesh, vtx, numPathVertices, col);
				colors = mesh.m_ColorBuffer;
				numColors = mesh.m_NumVertices;
			} else {
				strokerConvexFill(stroker, &mesh, vtx, numPathVertices);
			}

#if VG_CONFIG_ENABLE_SHAPE_CACHING
			if (hasCache) {
				addCachedCommand(ctx, mesh.m_PosBuffer, mesh.m_NumVertices, colors, numColors, mesh.m_IndexBuffer, mesh.m_NumIndices);
			}
#endif

			if (recordClipCommands) {
				createDrawCommand_Clip(ctx, mesh.m_PosBuffer, mesh.m_NumVertices, mesh.m_IndexBuffer, mesh.m_NumIndices);
			} else {
				createDrawCommand_VertexColor(ctx, mesh.m_PosBuffer, mesh.m_NumVertices, colors, numColors, mesh.m_IndexBuffer, mesh.m_NumIndices);
			}
		}
	} else if (pathType == PathType::Concave) {
		strokerConcaveFillBegin(stroker);
		for (uint32_t i = 0; i < numSubPaths; ++i) {
			const SubPath* subPath = &subPaths[i];
			if (subPath->m_NumVertices < 3) {
				return;
			}

			const float* vtx = &pathVertices[subPath->m_FirstVertexID << 1];
			const uint32_t numPathVertices = subPath->m_NumVertices;
			strokerConcaveFillAddContour(stroker, vtx, numPathVertices);
		}

		Mesh mesh;
		const uint32_t* colors = &col;
		uint32_t numColors = 1;

		bool decomposed = false;
		if (aa) {
			decomposed = strokerConcaveFillEndAA(stroker, &mesh, col, fillRule);
			colors = mesh.m_ColorBuffer;
			numColors = mesh.m_NumVertices;
		} else {
			decomposed = strokerConcaveFillEnd(stroker, &mesh, fillRule);
		}

		VG_WARN(decomposed, "Failed to triangulate concave polygon");
		if (decomposed) {
#if VG_CONFIG_ENABLE_SHAPE_CACHING
			if (hasCache) {
				addCachedCommand(ctx, mesh.m_PosBuffer, mesh.m_NumVertices, colors, numColors, mesh.m_IndexBuffer, mesh.m_NumIndices);
			}
#endif

			if (recordClipCommands) {
				createDrawCommand_Clip(ctx, mesh.m_PosBuffer, mesh.m_NumVertices, mesh.m_IndexBuffer, mesh.m_NumIndices);
			} else {
				createDrawCommand_VertexColor(ctx, mesh.m_PosBuffer, mesh.m_NumVertices, colors, numColors, mesh.m_IndexBuffer, mesh.m_NumIndices);
			}
		}
	}

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	if (hasCache) {
		endCachedCommand(ctx);
	}
#endif
}

static void ctxFillPathGradient(Context* ctx, GradientHandle gradientHandle, uint32_t flags)
{
	VG_CHECK(!ctx->m_RecordClipCommands, "Only fillPath(Color) is supported inside BeginClip()/EndClip()");
	VG_CHECK(isValid(gradientHandle), "Invalid gradient handle");
	VG_CHECK(!isLocal(gradientHandle), "Invalid gradient handle");

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	const bool hasCache = getCommandListCacheStackTop(ctx) != nullptr;
#endif

	const float* pathVertices = transformPath(ctx);

	const PathType::Enum pathType = VG_FILL_FLAGS_PATH_TYPE(flags);
	const FillRule::Enum fillRule = VG_FILL_FLAGS_RULE(flags);
#if VG_CONFIG_FORCE_AA_OFF
	const bool aa = false;
#else
	const bool aa = VG_FILL_FLAGS_AA(flags);
#endif

	Stroker* stroker = ctx->m_Stroker;
	const Path* path = ctx->m_Path;
	const uint32_t numSubPaths = pathGetNumSubPaths(path);
	const SubPath* subPaths = pathGetSubPaths(path);

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	if (hasCache) {
		beginCachedCommand(ctx);
	}
#endif

	if (pathType == PathType::Convex) {
		for (uint32_t i = 0; i < numSubPaths; ++i) {
			const SubPath* subPath = &subPaths[i];
			if (subPath->m_NumVertices < 3) {
				continue;
			}

			const float* vtx = &pathVertices[subPath->m_FirstVertexID << 1];
			const uint32_t numPathVertices = subPath->m_NumVertices;

			Mesh mesh;
			const uint32_t black = Colors::Black;
			const uint32_t* colors = &black;
			uint32_t numColors = 1;

			if (aa) {
				strokerConvexFillAA(stroker, &mesh, vtx, numPathVertices, Colors::Black);
				colors = mesh.m_ColorBuffer;
				numColors = mesh.m_NumVertices;
			} else {
				strokerConvexFill(stroker, &mesh, vtx, numPathVertices);
			}

#if VG_CONFIG_ENABLE_SHAPE_CACHING
			if (hasCache) {
				addCachedCommand(ctx, mesh.m_PosBuffer, mesh.m_NumVertices, colors, numColors, mesh.m_IndexBuffer, mesh.m_NumIndices);
			}
#endif

			createDrawCommand_ColorGradient(ctx, gradientHandle, mesh.m_PosBuffer, mesh.m_NumVertices, colors, numColors, mesh.m_IndexBuffer, mesh.m_NumIndices);
		}
	} else if (pathType == PathType::Concave) {
		strokerConcaveFillBegin(stroker);
		for (uint32_t i = 0; i < numSubPaths; ++i) {
			const SubPath* subPath = &subPaths[i];
			if (subPath->m_NumVertices < 3) {
				return;
			}

			const float* vtx = &pathVertices[subPath->m_FirstVertexID << 1];
			const uint32_t numPathVertices = subPath->m_NumVertices;
			strokerConcaveFillAddContour(stroker, vtx, numPathVertices);
		}

		const Color black = Colors::Black;
		Mesh mesh;
		const uint32_t* colors = &black;
		uint32_t numColors = 1;

		bool decomposed = false;
		if (aa) {
			decomposed = strokerConcaveFillEndAA(stroker, &mesh, black, fillRule);
			colors = mesh.m_ColorBuffer;
			numColors = mesh.m_NumVertices;
		} else {
			decomposed = strokerConcaveFillEnd(stroker, &mesh, fillRule);
		}

		VG_WARN(decomposed, "Failed to triangulate concave polygon");
		if (decomposed) {
#if VG_CONFIG_ENABLE_SHAPE_CACHING
			if (hasCache) {
				addCachedCommand(ctx, mesh.m_PosBuffer, mesh.m_NumVertices, colors, numColors, mesh.m_IndexBuffer, mesh.m_NumIndices);
			}
#endif

			createDrawCommand_ColorGradient(ctx, gradientHandle, mesh.m_PosBuffer, mesh.m_NumVertices, colors, numColors, mesh.m_IndexBuffer, mesh.m_NumIndices);
		}
	}

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	if (hasCache) {
		endCachedCommand(ctx);
	}
#endif
}

static void ctxFillPathImagePattern(Context* ctx, ImagePatternHandle imgPatternHandle, Color color, uint32_t flags)
{
	VG_CHECK(!ctx->m_RecordClipCommands, "Only fillPath(Color) is supported inside BeginClip()/EndClip()");
	VG_CHECK(isValid(imgPatternHandle), "Invalid image pattern handle");
	VG_CHECK(!isLocal(imgPatternHandle), "Invalid gradient handle");

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	const bool hasCache = getCommandListCacheStackTop(ctx) != nullptr;
#else
	const bool hasCache = false;
#endif

	const State* state = getState(ctx);
	const float globalAlpha = hasCache ? 1.0f : state->m_GlobalAlpha;
	const Color col = colorSetAlpha(color, (uint8_t)(globalAlpha * colorGetAlpha(color)));
	if (!hasCache && colorGetAlpha(col) == 0) {
		return;
	}

	const PathType::Enum pathType = VG_FILL_FLAGS_PATH_TYPE(flags);
	const FillRule::Enum fillRule = VG_FILL_FLAGS_RULE(flags);
#if VG_CONFIG_FORCE_AA_OFF
	const bool aa = false;
#else
	const bool aa = VG_FILL_FLAGS_AA(flags);
#endif

	const float* pathVertices = transformPath(ctx);

	Stroker* stroker = ctx->m_Stroker;
	const Path* path = ctx->m_Path;
	const uint32_t numSubPaths = pathGetNumSubPaths(path);
	const SubPath* subPaths = pathGetSubPaths(path);

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	if (hasCache) {
		beginCachedCommand(ctx);
	}
#endif

	if (pathType == PathType::Convex) {
		for (uint32_t i = 0; i < numSubPaths; ++i) {
			const SubPath* subPath = &subPaths[i];
			if (subPath->m_NumVertices < 3) {
				continue;
			}

			const float* vtx = &pathVertices[subPath->m_FirstVertexID << 1];
			const uint32_t numPathVertices = subPath->m_NumVertices;

			Mesh mesh;
			const uint32_t* colors = &col;
			uint32_t numColors = 1;

			if (aa) {
				strokerConvexFillAA(stroker, &mesh, vtx, numPathVertices, col);
				colors = mesh.m_ColorBuffer;
				numColors = mesh.m_NumVertices;
			} else {
				strokerConvexFill(stroker, &mesh, vtx, numPathVertices);
			}

#if VG_CONFIG_ENABLE_SHAPE_CACHING
			if (hasCache) {
				addCachedCommand(ctx, mesh.m_PosBuffer, mesh.m_NumVertices, colors, numColors, mesh.m_IndexBuffer, mesh.m_NumIndices);
			}
#endif

			createDrawCommand_ImagePattern(ctx, imgPatternHandle, mesh.m_PosBuffer, mesh.m_NumVertices, colors, numColors, mesh.m_IndexBuffer, mesh.m_NumIndices);
		}
	} else if (pathType == PathType::Concave) {
		strokerConcaveFillBegin(stroker);
		for (uint32_t i = 0; i < numSubPaths; ++i) {
			const SubPath* subPath = &subPaths[i];
			if (subPath->m_NumVertices < 3) {
				return;
			}

			const float* vtx = &pathVertices[subPath->m_FirstVertexID << 1];
			const uint32_t numPathVertices = subPath->m_NumVertices;
			strokerConcaveFillAddContour(stroker, vtx, numPathVertices);
		}

		Mesh mesh;
		const uint32_t* colors = &col;
		uint32_t numColors = 1;

		bool decomposed = false;
		if (aa) {
			decomposed = strokerConcaveFillEndAA(stroker, &mesh, col, fillRule);
			colors = mesh.m_ColorBuffer;
			numColors = mesh.m_NumVertices;
		} else {
			decomposed = strokerConcaveFillEnd(stroker, &mesh, fillRule);
		}

		VG_WARN(decomposed, "Failed to triangulate concave polygon");
		if (decomposed) {
#if VG_CONFIG_ENABLE_SHAPE_CACHING
			if (hasCache) {
				addCachedCommand(ctx, mesh.m_PosBuffer, mesh.m_NumVertices, colors, numColors, mesh.m_IndexBuffer, mesh.m_NumIndices);
			}
#endif

			createDrawCommand_ImagePattern(ctx, imgPatternHandle, mesh.m_PosBuffer, mesh.m_NumVertices, colors, numColors, mesh.m_IndexBuffer, mesh.m_NumIndices);
		}
	}

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	if (hasCache) {
		endCachedCommand(ctx);
	}
#endif
}

static void ctxStrokePathColor(Context* ctx, Color color, float width, uint32_t flags)
{
	const bool recordClipCommands = ctx->m_RecordClipCommands;

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	const bool hasCache = getCommandListCacheStackTop(ctx) != nullptr;
#else
	const bool hasCache = false;
#endif

	const State* state = getState(ctx);
	const float avgScale = state->m_AvgScale;
	const float globalAlpha = hasCache ? 1.0f : state->m_GlobalAlpha;
	const float fringeWidth = ctx->m_FringeWidth;

	const float scaledStrokeWidth = bx::clamp<float>(width * avgScale, 0.0f, 200.0f);
	const bool isThin = scaledStrokeWidth <= fringeWidth;

	const float alphaScale = !isThin ? globalAlpha : globalAlpha * bx::square(bx::clamp<float>(scaledStrokeWidth, 0.0f, fringeWidth));
	const Color col = recordClipCommands ? Colors::Black : colorSetAlpha(color, (uint8_t)(alphaScale * colorGetAlpha(color)));
	if (!hasCache && colorGetAlpha(col) == 0) {
		return;
	}

	const LineJoin::Enum lineJoin = VG_STROKE_FLAGS_LINE_JOIN(flags);
	const LineCap::Enum lineCap = VG_STROKE_FLAGS_LINE_CAP(flags);
#if VG_CONFIG_FORCE_AA_OFF
	const bool aa = false;
#else
	const bool aa = recordClipCommands ? false : VG_STROKE_FLAGS_AA(flags);
#endif

	const float strokeWidth = isThin ? fringeWidth : scaledStrokeWidth;

	const float* pathVertices = transformPath(ctx);

	const Path* path = ctx->m_Path;
	const uint32_t numSubPaths = pathGetNumSubPaths(path);
	const SubPath* subPaths = pathGetSubPaths(path);
	Stroker* stroker = ctx->m_Stroker;

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	if (hasCache) {
		beginCachedCommand(ctx);
	}
#endif

	for (uint32_t iSubPath = 0; iSubPath < numSubPaths; ++iSubPath) {
		const SubPath* subPath = &subPaths[iSubPath];
		if (subPath->m_NumVertices < 2) {
			continue;
		}

		const float* vtx = &pathVertices[subPath->m_FirstVertexID << 1];
		const uint32_t numPathVertices = subPath->m_NumVertices;
		const bool isClosed = subPath->m_IsClosed;

		Mesh mesh;
		const uint32_t* colors = &col;
		uint32_t numColors = 1;
		if (aa) {
			if (isThin) {
				strokerPolylineStrokeAAThin(stroker, &mesh, vtx, numPathVertices, isClosed, col, lineCap, lineJoin);
			} else {
				strokerPolylineStrokeAA(stroker, &mesh, vtx, numPathVertices, isClosed, col, strokeWidth, lineCap, lineJoin);
			}

			colors = mesh.m_ColorBuffer;
			numColors = mesh.m_NumVertices;
		} else {
			strokerPolylineStroke(stroker, &mesh, vtx, numPathVertices, isClosed, strokeWidth, lineCap, lineJoin);
		}

#if VG_CONFIG_ENABLE_SHAPE_CACHING
		if (hasCache) {
			addCachedCommand(ctx, mesh.m_PosBuffer, mesh.m_NumVertices, colors, numColors, mesh.m_IndexBuffer, mesh.m_NumIndices);
		}
#endif

		if (recordClipCommands) {
			createDrawCommand_Clip(ctx, mesh.m_PosBuffer, mesh.m_NumVertices, mesh.m_IndexBuffer, mesh.m_NumIndices);
		} else {
			createDrawCommand_VertexColor(ctx, mesh.m_PosBuffer, mesh.m_NumVertices, colors, numColors, mesh.m_IndexBuffer, mesh.m_NumIndices);
		}
	}

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	if (hasCache) {
		endCachedCommand(ctx);
	}
#endif
}

static void ctxStrokePathGradient(Context* ctx, GradientHandle gradientHandle, float width, uint32_t flags)
{
	VG_CHECK(!ctx->m_RecordClipCommands, "Only strokePath(Color) is supported inside BeginClip()/EndClip()");
	VG_CHECK(isValid(gradientHandle), "Invalid gradient handle");
	VG_CHECK(!isLocal(gradientHandle), "Invalid gradient handle");

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	const bool hasCache = getCommandListCacheStackTop(ctx) != nullptr;
#endif

	const LineJoin::Enum lineJoin = VG_STROKE_FLAGS_LINE_JOIN(flags);
	const LineCap::Enum lineCap = VG_STROKE_FLAGS_LINE_CAP(flags);
#if VG_CONFIG_FORCE_AA_OFF
	const bool aa = false;
#else
	const bool aa = VG_STROKE_FLAGS_AA(flags);
#endif

	const float* pathVertices = transformPath(ctx);

	const State* state = getState(ctx);
	const float avgScale = state->m_AvgScale;
	float strokeWidth = bx::clamp<float>(width * avgScale, 0.0f, 200.0f);
	bool isThin = false;
	if (strokeWidth <= ctx->m_FringeWidth) {
		strokeWidth = ctx->m_FringeWidth;
		isThin = true;
	}

	Stroker* stroker = ctx->m_Stroker;
	const Path* path = ctx->m_Path;
	const uint32_t numSubPaths = pathGetNumSubPaths(path);
	const SubPath* subPaths = pathGetSubPaths(path);

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	if (hasCache) {
		beginCachedCommand(ctx);
	}
#endif

	for (uint32_t iSubPath = 0; iSubPath < numSubPaths; ++iSubPath) {
		const SubPath* subPath = &subPaths[iSubPath];
		if (subPath->m_NumVertices < 2) {
			continue;
		}

		const float* vtx = &pathVertices[subPath->m_FirstVertexID << 1];
		const uint32_t numPathVertices = subPath->m_NumVertices;
		const bool isClosed = subPath->m_IsClosed;

		Mesh mesh;
		const uint32_t black = Colors::Black;
		const uint32_t* colors = &black;
		uint32_t numColors = 1;

		if (aa) {
			if (isThin) {
				strokerPolylineStrokeAAThin(stroker, &mesh, vtx, numPathVertices, isClosed, vg::Colors::Black, lineCap, lineJoin);
			} else {
				strokerPolylineStrokeAA(stroker, &mesh, vtx, numPathVertices, isClosed, vg::Colors::Black, strokeWidth, lineCap, lineJoin);
			}

			colors = mesh.m_ColorBuffer;
			numColors = mesh.m_NumVertices;
		} else {
			strokerPolylineStroke(stroker, &mesh, vtx, numPathVertices, isClosed, strokeWidth, lineCap, lineJoin);
		}

#if VG_CONFIG_ENABLE_SHAPE_CACHING
		if (hasCache) {
			addCachedCommand(ctx, mesh.m_PosBuffer, mesh.m_NumVertices, colors, numColors, mesh.m_IndexBuffer, mesh.m_NumIndices);
		}
#endif

		createDrawCommand_ColorGradient(ctx, gradientHandle, mesh.m_PosBuffer, mesh.m_NumVertices, colors, numColors, mesh.m_IndexBuffer, mesh.m_NumIndices);
	}

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	if (hasCache) {
		endCachedCommand(ctx);
	}
#endif
}

static void ctxStrokePathImagePattern(Context* ctx, ImagePatternHandle imgPatternHandle, Color color, float width, uint32_t flags)
{
	VG_CHECK(!ctx->m_RecordClipCommands, "Only strokePath(Color) is supported inside BeginClip()/EndClip()");
	VG_CHECK(isValid(imgPatternHandle), "Invalid image pattern handle");
	VG_CHECK(!isLocal(imgPatternHandle), "Invalid gradient handle");

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	const bool hasCache = getCommandListCacheStackTop(ctx) != nullptr;
#else
	const bool hasCache = false;
#endif

	const State* state = getState(ctx);
	const float avgScale = state->m_AvgScale;
	const float globalAlpha = hasCache ? 1.0f : state->m_GlobalAlpha;
	const float fringeWidth = ctx->m_FringeWidth;

	const float scaledStrokeWidth = bx::clamp<float>(width * avgScale, 0.0f, 200.0f);
	const bool isThin = scaledStrokeWidth <= fringeWidth;

	const float alphaScale = isThin ? globalAlpha : globalAlpha * bx::square(bx::clamp<float>(scaledStrokeWidth, 0.0f, fringeWidth));
	const Color col = colorSetAlpha(color, (uint8_t)(alphaScale * colorGetAlpha(color)));
	if (!hasCache && colorGetAlpha(col) == 0) {
		return;
	}

	const LineJoin::Enum lineJoin = VG_STROKE_FLAGS_LINE_JOIN(flags);
	const LineCap::Enum lineCap = VG_STROKE_FLAGS_LINE_CAP(flags);
#if VG_CONFIG_FORCE_AA_OFF
	const bool aa = false;
#else
	const bool aa = VG_STROKE_FLAGS_AA(flags);
#endif

	const float strokeWidth = isThin ? fringeWidth : scaledStrokeWidth;

	const float* pathVertices = transformPath(ctx);

	Stroker* stroker = ctx->m_Stroker;
	const Path* path = ctx->m_Path;
	const uint32_t numSubPaths = pathGetNumSubPaths(path);
	const SubPath* subPaths = pathGetSubPaths(path);

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	if (hasCache) {
		beginCachedCommand(ctx);
	}
#endif

	for (uint32_t iSubPath = 0; iSubPath < numSubPaths; ++iSubPath) {
		const SubPath* subPath = &subPaths[iSubPath];
		if (subPath->m_NumVertices < 2) {
			continue;
		}

		const float* vtx = &pathVertices[subPath->m_FirstVertexID << 1];
		const uint32_t numPathVertices = subPath->m_NumVertices;
		const bool isClosed = subPath->m_IsClosed;

		Mesh mesh;
		const uint32_t* colors = &col;
		uint32_t numColors = 1;

		if (aa) {
			if (isThin) {
				strokerPolylineStrokeAAThin(stroker, &mesh, vtx, numPathVertices, isClosed, col, lineCap, lineJoin);
			} else {
				strokerPolylineStrokeAA(stroker, &mesh, vtx, numPathVertices, isClosed, col, strokeWidth, lineCap, lineJoin);
			}

			colors = mesh.m_ColorBuffer;
			numColors = mesh.m_NumVertices;
		} else {
			strokerPolylineStroke(stroker, &mesh, vtx, numPathVertices, isClosed, strokeWidth, lineCap, lineJoin);
		}

#if VG_CONFIG_ENABLE_SHAPE_CACHING
		if (hasCache) {
			addCachedCommand(ctx, mesh.m_PosBuffer, mesh.m_NumVertices, colors, numColors, mesh.m_IndexBuffer, mesh.m_NumIndices);
		}
#endif

		createDrawCommand_ImagePattern(ctx, imgPatternHandle, mesh.m_PosBuffer, mesh.m_NumVertices, colors, numColors, mesh.m_IndexBuffer, mesh.m_NumIndices);
	}

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	if (hasCache) {
		endCachedCommand(ctx);
	}
#endif
}

static void ctxBeginClip(Context* ctx, ClipRule::Enum rule)
{
	VG_CHECK(!ctx->m_RecordClipCommands, "Already inside beginClip()/endClip() block");

	ClipState* clipState = &ctx->m_ClipState;
	const uint32_t nextClipCmdID = ctx->m_NumClipCommands;

	clipState->m_Rule = rule;
	clipState->m_FirstCmdID = nextClipCmdID;
	clipState->m_NumCmds = 0;

	ctx->m_RecordClipCommands = true;
	ctx->m_ForceNewClipCommand = true;
}

static void ctxEndClip(Context* ctx)
{
	VG_CHECK(ctx->m_RecordClipCommands, "Must be called once after beginClip()");

	ClipState* clipState = &ctx->m_ClipState;
	const uint32_t nextClipCmdID = ctx->m_NumClipCommands;

	clipState->m_NumCmds = nextClipCmdID - clipState->m_FirstCmdID;

	ctx->m_RecordClipCommands = false;
	ctx->m_ForceNewDrawCommand = true;
}

static void ctxResetClip(Context* ctx)
{
	VG_CHECK(!ctx->m_RecordClipCommands, "Must be called outside beginClip()/endClip() pair.");

	ClipState* clipState = &ctx->m_ClipState;

	if (clipState->m_FirstCmdID != ~0u) {
		clipState->m_FirstCmdID = ~0u;
		clipState->m_NumCmds = 0;

		ctx->m_ForceNewDrawCommand = true;
	}
}

static GradientHandle ctxCreateLinearGradient(Context* ctx, float sx, float sy, float ex, float ey, Color icol, Color ocol)
{
	if (ctx->m_NextGradientID >= ctx->m_Config.m_MaxGradients) {
		return VG_INVALID_HANDLE32;
	}

	GradientHandle handle = { (uint16_t)ctx->m_NextGradientID++, 0 };

	const float large = 1e5;
	float dx = ex - sx;
	float dy = ey - sy;
	float d = bx::sqrt(dx * dx + dy * dy);
	if (d > 0.0001f) {
		dx /= d;
		dy /= d;
	} else {
		dx = 0;
		dy = 1;
	}

	float gradientMatrix[6];
	gradientMatrix[0] = dy;
	gradientMatrix[1] = -dx;
	gradientMatrix[2] = dx;
	gradientMatrix[3] = dy;
	gradientMatrix[4] = sx - dx * large;
	gradientMatrix[5] = sy - dy * large;

	const State* state = getState(ctx);
	const float* stateTransform = state->m_TransformMtx;

	float patternMatrix[6];
	vgutil::multiplyMatrix3(stateTransform, gradientMatrix, patternMatrix);

	float inversePatternMatrix[6];
	vgutil::invertMatrix3(patternMatrix, inversePatternMatrix);

	Gradient* grad = &ctx->m_Gradients[handle.idx];
	grad->m_Matrix[0] = inversePatternMatrix[0];
	grad->m_Matrix[1] = inversePatternMatrix[1];
	grad->m_Matrix[2] = 0.0f;
	grad->m_Matrix[3] = inversePatternMatrix[2];
	grad->m_Matrix[4] = inversePatternMatrix[3];
	grad->m_Matrix[5] = 0.0f;
	grad->m_Matrix[6] = inversePatternMatrix[4];
	grad->m_Matrix[7] = inversePatternMatrix[5];
	grad->m_Matrix[8] = 1.0f;
	grad->m_Params[0] = large;
	grad->m_Params[1] = large + d * 0.5f;
	grad->m_Params[2] = 0.0f;
	grad->m_Params[3] = bx::max<float>(1.0f, d);
	grad->m_InnerColor[0] = colorGetRed(icol) / 255.0f;
	grad->m_InnerColor[1] = colorGetGreen(icol) / 255.0f;
	grad->m_InnerColor[2] = colorGetBlue(icol) / 255.0f;
	grad->m_InnerColor[3] = colorGetAlpha(icol) / 255.0f;
	grad->m_OuterColor[0] = colorGetRed(ocol) / 255.0f;
	grad->m_OuterColor[1] = colorGetGreen(ocol) / 255.0f;
	grad->m_OuterColor[2] = colorGetBlue(ocol) / 255.0f;
	grad->m_OuterColor[3] = colorGetAlpha(ocol) / 255.0f;

	return handle;
}

static GradientHandle ctxCreateBoxGradient(Context* ctx, float x, float y, float w, float h, float r, float f, Color icol, Color ocol)
{
	if (ctx->m_NextGradientID >= ctx->m_Config.m_MaxGradients) {
		return VG_INVALID_HANDLE32;
	}

	GradientHandle handle = { (uint16_t)ctx->m_NextGradientID++, 0 };

	float gradientMatrix[6];
	gradientMatrix[0] = 1.0f;
	gradientMatrix[1] = 0.0f;
	gradientMatrix[2] = 0.0f;
	gradientMatrix[3] = 1.0f;
	gradientMatrix[4] = x + w * 0.5f;
	gradientMatrix[5] = y + h * 0.5f;

	const State* state = getState(ctx);
	const float* stateTransform = state->m_TransformMtx;

	float patternMatrix[6];
	vgutil::multiplyMatrix3(stateTransform, gradientMatrix, patternMatrix);

	float inversePatternMatrix[6];
	vgutil::invertMatrix3(patternMatrix, inversePatternMatrix);

	Gradient* grad = &ctx->m_Gradients[handle.idx];
	grad->m_Matrix[0] = inversePatternMatrix[0];
	grad->m_Matrix[1] = inversePatternMatrix[1];
	grad->m_Matrix[2] = 0.0f;
	grad->m_Matrix[3] = inversePatternMatrix[2];
	grad->m_Matrix[4] = inversePatternMatrix[3];
	grad->m_Matrix[5] = 0.0f;
	grad->m_Matrix[6] = inversePatternMatrix[4];
	grad->m_Matrix[7] = inversePatternMatrix[5];
	grad->m_Matrix[8] = 1.0f;
	grad->m_Params[0] = w * 0.5f;
	grad->m_Params[1] = h * 0.5f;
	grad->m_Params[2] = r;
	grad->m_Params[3] = bx::max<float>(1.0f, f);
	grad->m_InnerColor[0] = colorGetRed(icol) / 255.0f;
	grad->m_InnerColor[1] = colorGetGreen(icol) / 255.0f;
	grad->m_InnerColor[2] = colorGetBlue(icol) / 255.0f;
	grad->m_InnerColor[3] = colorGetAlpha(icol) / 255.0f;
	grad->m_OuterColor[0] = colorGetRed(ocol) / 255.0f;
	grad->m_OuterColor[1] = colorGetGreen(ocol) / 255.0f;
	grad->m_OuterColor[2] = colorGetBlue(ocol) / 255.0f;
	grad->m_OuterColor[3] = colorGetAlpha(ocol) / 255.0f;

	return handle;
}

static GradientHandle ctxCreateRadialGradient(Context* ctx, float cx, float cy, float inr, float outr, Color icol, Color ocol)
{
	if (ctx->m_NextGradientID >= ctx->m_Config.m_MaxGradients) {
		return VG_INVALID_HANDLE32;
	}

	GradientHandle handle = { (uint16_t)ctx->m_NextGradientID++, 0 };

	float gradientMatrix[6];
	gradientMatrix[0] = 1.0f;
	gradientMatrix[1] = 0.0f;
	gradientMatrix[2] = 0.0f;
	gradientMatrix[3] = 1.0f;
	gradientMatrix[4] = cx;
	gradientMatrix[5] = cy;

	const State* state = getState(ctx);
	const float* stateTransform = state->m_TransformMtx;

	float patternMatrix[6];
	vgutil::multiplyMatrix3(stateTransform, gradientMatrix, patternMatrix);

	float inversePatternMatrix[6];
	vgutil::invertMatrix3(patternMatrix, inversePatternMatrix);

	const float r = (inr + outr) * 0.5f;
	const float f = (outr - inr);

	Gradient* grad = &ctx->m_Gradients[handle.idx];
	grad->m_Matrix[0] = inversePatternMatrix[0];
	grad->m_Matrix[1] = inversePatternMatrix[1];
	grad->m_Matrix[2] = 0.0f;
	grad->m_Matrix[3] = inversePatternMatrix[2];
	grad->m_Matrix[4] = inversePatternMatrix[3];
	grad->m_Matrix[5] = 0.0f;
	grad->m_Matrix[6] = inversePatternMatrix[4];
	grad->m_Matrix[7] = inversePatternMatrix[5];
	grad->m_Matrix[8] = 1.0f;
	grad->m_Params[0] = r;
	grad->m_Params[1] = r;
	grad->m_Params[2] = r;
	grad->m_Params[3] = bx::max<float>(1.0f, f);
	grad->m_InnerColor[0] = colorGetRed(icol) / 255.0f;
	grad->m_InnerColor[1] = colorGetGreen(icol) / 255.0f;
	grad->m_InnerColor[2] = colorGetBlue(icol) / 255.0f;
	grad->m_InnerColor[3] = colorGetAlpha(icol) / 255.0f;
	grad->m_OuterColor[0] = colorGetRed(ocol) / 255.0f;
	grad->m_OuterColor[1] = colorGetGreen(ocol) / 255.0f;
	grad->m_OuterColor[2] = colorGetBlue(ocol) / 255.0f;
	grad->m_OuterColor[3] = colorGetAlpha(ocol) / 255.0f;

	return handle;
}

static ImagePatternHandle ctxCreateImagePattern(Context* ctx, float cx, float cy, float w, float h, float angle, ImageHandle image)
{
	if (!isValid(image)) {
		return VG_INVALID_HANDLE32;
	}

	if (ctx->m_NextImagePatternID >= ctx->m_Config.m_MaxImagePatterns) {
		return VG_INVALID_HANDLE32;
	}

	ImagePatternHandle handle = { (uint16_t)ctx->m_NextImagePatternID++, 0 };

	const float cs = bx::cos(angle);
	const float sn = bx::sin(angle);

	float mtx[6];
	mtx[0] = cs;
	mtx[1] = sn;
	mtx[2] = -sn;
	mtx[3] = cs;
	mtx[4] = cx;
	mtx[5] = cy;

	const State* state = getState(ctx);
	const float* stateTransform = state->m_TransformMtx;

	float patternMatrix[6];
	vgutil::multiplyMatrix3(stateTransform, mtx, patternMatrix);

	float inversePatternMatrix[6];
	vgutil::invertMatrix3(patternMatrix, inversePatternMatrix);

	inversePatternMatrix[0] /= w;
	inversePatternMatrix[1] /= h;
	inversePatternMatrix[2] /= w;
	inversePatternMatrix[3] /= h;
	inversePatternMatrix[4] /= w;
	inversePatternMatrix[5] /= h;

	ImagePattern* pattern = &ctx->m_ImagePatterns[handle.idx];
	pattern->m_Matrix[0] = inversePatternMatrix[0];
	pattern->m_Matrix[1] = inversePatternMatrix[1];
	pattern->m_Matrix[2] = 0.0f;
	pattern->m_Matrix[3] = inversePatternMatrix[2];
	pattern->m_Matrix[4] = inversePatternMatrix[3];
	pattern->m_Matrix[5] = 0.0f;
	pattern->m_Matrix[6] = inversePatternMatrix[4];
	pattern->m_Matrix[7] = inversePatternMatrix[5];
	pattern->m_Matrix[8] = 1.0f;
	pattern->m_ImageHandle = image;

	return handle;
}

static void ctxPushState(Context* ctx)
{
	VG_CHECK(ctx->m_StateStackTop < (uint32_t)(ctx->m_Config.m_MaxStateStackSize - 1), "State stack overflow");

	const uint32_t top = ctx->m_StateStackTop;
	const State* curState = &ctx->m_StateStack[top];
	State* newState = &ctx->m_StateStack[top + 1];
	bx::memCopy(newState, curState, sizeof(State));
	++ctx->m_StateStackTop;
}

static void ctxPopState(Context* ctx)
{
	VG_CHECK(ctx->m_StateStackTop > 0, "State stack underflow");
	--ctx->m_StateStackTop;

	// If the new state has a different scissor rect than the last draw command 
	// force creating a new command.
	const uint32_t numDrawCommands = ctx->m_NumDrawCommands;
	if (numDrawCommands != 0) {
		const State* state = getState(ctx);
		const DrawCommand* lastDrawCommand = &ctx->m_DrawCommands[numDrawCommands - 1];
		const uint16_t* lastScissor = &lastDrawCommand->m_ScissorRect[0];
		const float* stateScissor = &state->m_ScissorRect[0];
		if (lastScissor[0] != (uint16_t)stateScissor[0] ||
			lastScissor[1] != (uint16_t)stateScissor[1] ||
			lastScissor[2] != (uint16_t)stateScissor[2] ||
			lastScissor[3] != (uint16_t)stateScissor[3]) {
			ctx->m_ForceNewDrawCommand = true;
			ctx->m_ForceNewClipCommand = true;
		}
	}
}

static void ctxResetScissor(Context* ctx)
{
	State* state = getState(ctx);
	state->m_ScissorRect[0] = state->m_ScissorRect[1] = 0.0f;
	state->m_ScissorRect[2] = (float)ctx->m_CanvasWidth;
	state->m_ScissorRect[3] = (float)ctx->m_CanvasHeight;
	ctx->m_ForceNewDrawCommand = true;
	ctx->m_ForceNewClipCommand = true;
}

static void ctxSetScissor(Context* ctx, float x, float y, float w, float h)
{
	State* state = getState(ctx);
	const float* stateTransform = state->m_TransformMtx;
	const float canvasWidth = (float)ctx->m_CanvasWidth;
	const float canvasHeight = (float)ctx->m_CanvasHeight;

	float pos[2], size[2];
	vgutil::transformPos2D(x, y, stateTransform, &pos[0]);
	vgutil::transformVec2D(w, h, stateTransform, &size[0]);

	const float minx = bx::clamp<float>(pos[0], 0.0f, canvasWidth);
	const float miny = bx::clamp<float>(pos[1], 0.0f, canvasHeight);
	const float maxx = bx::clamp<float>(pos[0] + size[0], 0.0f, canvasWidth);
	const float maxy = bx::clamp<float>(pos[1] + size[1], 0.0f, canvasHeight);

	state->m_ScissorRect[0] = minx;
	state->m_ScissorRect[1] = miny;
	state->m_ScissorRect[2] = maxx - minx;
	state->m_ScissorRect[3] = maxy - miny;
	ctx->m_ForceNewDrawCommand = true;
	ctx->m_ForceNewClipCommand = true;
}

static bool ctxIntersectScissor(Context* ctx, float x, float y, float w, float h)
{
	State* state = getState(ctx);
	const float* stateTransform = state->m_TransformMtx;
	const float* scissorRect = state->m_ScissorRect;

	float pos[2], size[2];
	vgutil::transformPos2D(x, y, stateTransform, &pos[0]);
	vgutil::transformVec2D(w, h, stateTransform, &size[0]);

	const float minx = bx::max<float>(pos[0], scissorRect[0]);
	const float miny = bx::max<float>(pos[1], scissorRect[1]);
	const float maxx = bx::min<float>(pos[0] + size[0], scissorRect[0] + scissorRect[2]);
	const float maxy = bx::min<float>(pos[1] + size[1], scissorRect[1] + scissorRect[3]);

	const float newRectWidth = bx::max<float>(0.0f, maxx - minx);
	const float newRectHeight = bx::max<float>(0.0f, maxy - miny);

	state->m_ScissorRect[0] = minx;
	state->m_ScissorRect[1] = miny;
	state->m_ScissorRect[2] = newRectWidth;
	state->m_ScissorRect[3] = newRectHeight;

	ctx->m_ForceNewDrawCommand = true;
	ctx->m_ForceNewClipCommand = true;

	return newRectWidth >= 1.0f && newRectHeight >= 1.0f;
}

static void ctxTransformIdentity(Context* ctx)
{
	State* state = getState(ctx);
	state->m_TransformMtx[0] = 1.0f;
	state->m_TransformMtx[1] = 0.0f;
	state->m_TransformMtx[2] = 0.0f;
	state->m_TransformMtx[3] = 1.0f;
	state->m_TransformMtx[4] = 0.0f;
	state->m_TransformMtx[5] = 0.0f;

	updateState(state);
}

static void ctxTransformScale(Context* ctx, float x, float y)
{
	State* state = getState(ctx);
	state->m_TransformMtx[0] = x * state->m_TransformMtx[0];
	state->m_TransformMtx[1] = x * state->m_TransformMtx[1];
	state->m_TransformMtx[2] = y * state->m_TransformMtx[2];
	state->m_TransformMtx[3] = y * state->m_TransformMtx[3];

	updateState(state);
}

static void ctxTransformTranslate(Context* ctx, float x, float y)
{
	State* state = getState(ctx);
	state->m_TransformMtx[4] += state->m_TransformMtx[0] * x + state->m_TransformMtx[2] * y;
	state->m_TransformMtx[5] += state->m_TransformMtx[1] * x + state->m_TransformMtx[3] * y;

	updateState(state);
}

static void ctxTransformRotate(Context* ctx, float ang_rad)
{
	const float c = bx::cos(ang_rad);
	const float s = bx::sin(ang_rad);

	State* state = getState(ctx);
	const float* stateTransform = state->m_TransformMtx;

	float mtx[6];
	mtx[0] = c * stateTransform[0] + s * stateTransform[2];
	mtx[1] = c * stateTransform[1] + s * stateTransform[3];
	mtx[2] = -s * stateTransform[0] + c * stateTransform[2];
	mtx[3] = -s * stateTransform[1] + c * stateTransform[3];
	mtx[4] = stateTransform[4];
	mtx[5] = stateTransform[5];
	bx::memCopy(state->m_TransformMtx, mtx, sizeof(float) * 6);

	updateState(state);
}

static void ctxTransformMult(Context* ctx, const float* mtx, TransformOrder::Enum order)
{
	State* state = getState(ctx);
	const float* stateTransform = state->m_TransformMtx;

	float res[6];
	if (order == TransformOrder::Post) {
		vgutil::multiplyMatrix3(stateTransform, mtx, res);
	} else {
		VG_CHECK(order == TransformOrder::Pre, "Unknown TransformOrder::Enum");
		vgutil::multiplyMatrix3(mtx, stateTransform, res);
	}

	bx::memCopy(state->m_TransformMtx, res, sizeof(float) * 6);

	updateState(state);
}

static void ctxSetViewBox(Context* ctx, float x, float y, float w, float h)
{
	const float scaleX = (float)ctx->m_CanvasWidth / w;
	const float scaleY = (float)ctx->m_CanvasHeight / h;

	State* state = getState(ctx);
	float* stateTransform = &state->m_TransformMtx[0];

	// ctxTransformScale(ctx, scaleX, scaleY);
	stateTransform[0] = scaleX * stateTransform[0];
	stateTransform[1] = scaleX * stateTransform[1];
	stateTransform[2] = scaleY * stateTransform[2];
	stateTransform[3] = scaleY * stateTransform[3];

	// ctxTransformTranslate(ctx, -x, -y);
	stateTransform[4] -= stateTransform[0] * x + stateTransform[2] * y;
	stateTransform[5] -= stateTransform[1] * x + stateTransform[3] * y;

	updateState(state);
}

static void ctxIndexedTriList(Context* ctx, const float* pos, const uv_t* uv, uint32_t numVertices, const Color* colors, uint32_t numColors, const uint16_t* indices, uint32_t numIndices, ImageHandle img)
{
	if (!isValid(img)) {
		img = ctx->m_FontImages[0];
	}

	const State* state = getState(ctx);
	const float* stateTransform = state->m_TransformMtx;

	DrawCommand* cmd = allocDrawCommand(ctx, numVertices, numIndices, DrawCommand::Type::Textured, img.idx);

	// Vertex buffer
	VertexBuffer* vb = &ctx->m_VertexBuffers[cmd->m_VertexBufferID];
	const uint32_t vbOffset = cmd->m_FirstVertexID + cmd->m_NumVertices;

	float* dstPos = &vb->m_Pos[vbOffset << 1];
	vgutil::batchTransformPositions(pos, numVertices, dstPos, stateTransform);

	uv_t* dstUV = &vb->m_UV[vbOffset << 1];
	if (uv) {
		bx::memCopy(dstUV, uv, sizeof(uv_t) * 2 * numVertices);
	} else {
		const uv_t* whiteRectUV = getWhitePixelUV(ctx);

#if VG_CONFIG_UV_INT16
		vgutil::memset32(dstUV, numVertices, &whiteRectUV[0]);
#else
		vgutil::memset64(dstUV, numVertices, &whiteRectUV[0]);
#endif
	}

	uint32_t* dstColor = &vb->m_Color[vbOffset];
	if (numColors == numVertices) {
		bx::memCopy(dstColor, colors, sizeof(uint32_t) * numVertices);
	} else {
		VG_CHECK(numColors == 1, "Invalid size of color array passed.");
		vgutil::memset32(dstColor, numVertices, colors);
	}

	// Index buffer
	IndexBuffer* ib = &ctx->m_IndexBuffers[ctx->m_ActiveIndexBufferID];
	uint16_t* dstIndex = &ib->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];
	vgutil::batchTransformDrawIndices(indices, numIndices, dstIndex, (uint16_t)cmd->m_NumVertices);

	cmd->m_NumVertices += numVertices;
	cmd->m_NumIndices += numIndices;
}

static void ctxText(Context* ctx, const TextConfig& cfg, float x, float y, const char* str, const char* end)
{
	VG_CHECK(isValid(cfg.m_FontHandle), "Invalid font handle");

	const State* state = getState(ctx);
	const float scale = state->m_FontScale * ctx->m_DevicePixelRatio;
	const float scaledFontSize = cfg.m_FontSize * scale;
	if (scaledFontSize < VG_CONFIG_MIN_FONT_SIZE) {
		return;
	}

	end = end ? end : (str + bx::strLen(str));
	if (end == str) {
		return;
	}

	FONSstring* vgs = &ctx->m_TextString;
	FONScontext* fons = ctx->m_FontStashContext;
	fonsSetSize(fons, scaledFontSize);
	fonsSetFont(fons, cfg.m_FontHandle.idx);

	fonsResetString(fons, vgs, str, end);

	int numBakedChars = fonsBakeString(fons, vgs);
	if (numBakedChars == -1) {
		// Atlas full? Retry
		if (!allocTextAtlas(ctx)) {
			VG_WARN(false, "Failed to allocate enough text atlas space for string");
			return;
		}

		numBakedChars = fonsBakeString(fons, vgs);
	}

	if (numBakedChars <= 0) {
		return;
	}

	if (ctx->m_TextQuadCapacity < (uint32_t)numBakedChars) {
		bx::AllocatorI* allocator = ctx->m_Allocator;

		ctx->m_TextQuadCapacity = (uint32_t)numBakedChars;
		ctx->m_TextQuads = (FONSquad*)BX_ALIGNED_REALLOC(allocator, ctx->m_TextQuads, sizeof(FONSquad) * ctx->m_TextQuadCapacity, 16);
		ctx->m_TextVertices = (float*)BX_ALIGNED_REALLOC(allocator, ctx->m_TextVertices, sizeof(float) * 2 * (ctx->m_TextQuadCapacity * 4), 16);
	}

	bx::memCopy(ctx->m_TextQuads, vgs->m_Quads, sizeof(FONSquad) * numBakedChars);

	float dx = 0.0f, dy = 0.0f;
	fonsAlignString(fons, vgs, cfg.m_Alignment, &dx, &dy);

	ctxPushState(ctx);
	ctxTransformTranslate(ctx, x + dx / scale, y + dy / scale);
	renderTextQuads(ctx, numBakedChars, cfg.m_Color);
	ctxPopState(ctx);
}

static void ctxTextBox(Context* ctx, const TextConfig& cfg, float x, float y, float breakWidth, const char* str, const char* end, uint32_t textboxFlags)
{
	VG_CHECK(isValid(cfg.m_FontHandle), "Invalid font handle");

	const State* state = getState(ctx);
	const float scale = state->m_FontScale * ctx->m_DevicePixelRatio;
	const float scaledFontSize = cfg.m_FontSize * scale;
	if (scaledFontSize < VG_CONFIG_MIN_FONT_SIZE) {
		return;
	}

	const uint32_t alignment = cfg.m_Alignment;
	const int halign = alignment & (FONS_ALIGN_LEFT | FONS_ALIGN_CENTER | FONS_ALIGN_RIGHT);
	const int valign = alignment & (FONS_ALIGN_TOP | FONS_ALIGN_MIDDLE | FONS_ALIGN_BOTTOM | FONS_ALIGN_BASELINE);
	const float lineh = getTextLineHeight(ctx, cfg);

	const TextConfig newCfg = makeTextConfig(ctx, cfg.m_FontHandle, cfg.m_FontSize, FONS_ALIGN_LEFT | valign, cfg.m_Color);

	TextRow rows[2];
	int nrows;
	while ((nrows = textBreakLines(ctx, cfg, str, end, breakWidth, rows, 2, textboxFlags))) {
		for (int i = 0; i < nrows; ++i) {
			TextRow* row = &rows[i];

			if (halign & FONS_ALIGN_LEFT) {
				ctxText(ctx, newCfg, x, y, row->start, row->end);
			} else if (halign & FONS_ALIGN_CENTER) {
				ctxText(ctx, newCfg, x + (breakWidth - row->width) * 0.5f, y, row->start, row->end);
			} else if (halign & FONS_ALIGN_RIGHT) {
				ctxText(ctx, newCfg, x + breakWidth - row->width, y, row->start, row->end);
			}

			y += lineh; // Assume line height multiplier to be 1.0 (NanoVG allows the user to change it, but I don't use it).
		}

		str = rows[nrows - 1].next;
	}
}

static void ctxSubmitCommandList(Context* ctx, CommandListHandle handle)
{
	VG_CHECK(isCommandListHandleValid(ctx, handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];

	if (ctx->m_SubmitCmdListRecursionDepth >= ctx->m_Config.m_MaxCommandListDepth) {
		VG_CHECK(false, "SubmitCommandList recursion depth limit reached.");
		return;
	}
	++ctx->m_SubmitCmdListRecursionDepth;

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	CommandListCache* clCache = clGetCache(ctx, cl);
	if(clCache) {
		const State* state = getState(ctx);

		const float cachedScale = clCache->m_AvgScale;
		const float stateScale = state->m_AvgScale;
		if (cachedScale == stateScale) {
			clCacheRender(ctx, cl);
			--ctx->m_SubmitCmdListRecursionDepth;
			return;
		} else {
			clCacheReset(ctx, clCache);

			clCache->m_AvgScale = stateScale;
		}
	}
#else
	CommandListCache* clCache = nullptr;
#endif

	// Don't cull commands during caching.
	const uint32_t clFlags = cl->m_Flags;
	const bool cullCmds = !clCache && ((clFlags & CommandListFlags::AllowCommandCulling) != 0);

	const uint16_t firstGradientID = (uint16_t)ctx->m_NextGradientID;
	const uint16_t firstImagePatternID = (uint16_t)ctx->m_NextImagePatternID;
	VG_CHECK(firstGradientID + cl->m_NumGradients <= ctx->m_Config.m_MaxGradients, "Not enough free gradients for command list. Increase ContextConfig::m_MaxGradients");
	VG_CHECK(firstImagePatternID + cl->m_NumImagePatterns <= ctx->m_Config.m_MaxImagePatterns, "Not enough free image patterns for command list. Increase ContextConfig::m_MaxImagePatterns");

	const uint8_t* cmd = cl->m_CommandBuffer;
	const uint8_t* cmdListEnd = cl->m_CommandBuffer + cl->m_CommandBufferPos;
	if (cmd == cmdListEnd) {
		--ctx->m_SubmitCmdListRecursionDepth;
		return;
	}

	const char* stringBuffer = cl->m_StringBuffer;

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	pushCommandListCache(ctx, clCache);
#endif

	bool skipCmds = false;
#if VG_CONFIG_COMMAND_LIST_PRESERVE_STATE
	ctxPushState(ctx);
#endif

	while (cmd < cmdListEnd) {
		const CommandHeader* cmdHeader = (CommandHeader*)cmd;
		cmd += kAlignedCommandHeaderSize;

		const uint8_t* nextCmd = cmd + cmdHeader->m_Size;
		
		if (skipCmds && cmdHeader->m_Type >= CommandType::FirstStrokerCommand && cmdHeader->m_Type <= CommandType::LastStrokerCommand) {
			cmd = nextCmd;
			continue;
		}

		switch (cmdHeader->m_Type) {
		case CommandType::BeginPath: {
			ctxBeginPath(ctx);
		} break;
		case CommandType::ClosePath: {
			ctxClosePath(ctx);
		} break;
		case CommandType::MoveTo: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 2;
			ctxMoveTo(ctx, coords[0], coords[1]);
		} break;
		case CommandType::LineTo: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 2;
			ctxLineTo(ctx, coords[0], coords[1]);
		} break;
		case CommandType::CubicTo: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 6;
			ctxCubicTo(ctx, coords[0], coords[1], coords[2], coords[3], coords[4], coords[5]);
		} break;
		case CommandType::QuadraticTo: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 4;
			ctxQuadraticTo(ctx, coords[0], coords[1], coords[2], coords[3]);
		} break;
		case CommandType::Arc: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 5;
			const Winding::Enum dir = CMD_READ(cmd, Winding::Enum);
			ctxArc(ctx, coords[0], coords[1], coords[2], coords[3], coords[4], dir);
		} break;
		case CommandType::ArcTo: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 5;
			ctxArcTo(ctx, coords[0], coords[1], coords[2], coords[3], coords[4]);
		} break;
		case CommandType::Rect: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 4;
			ctxRect(ctx, coords[0], coords[1], coords[2], coords[3]);
		} break;
		case CommandType::RoundedRect: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 5;
			ctxRoundedRect(ctx, coords[0], coords[1], coords[2], coords[3], coords[4]);
		} break;
		case CommandType::RoundedRectVarying: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 8;
			ctxRoundedRectVarying(ctx, coords[0], coords[1], coords[2], coords[3], coords[4], coords[5], coords[6], coords[7]);
		} break;
		case CommandType::Circle: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 3;
			ctxCircle(ctx, coords[0], coords[1], coords[2]);
		} break;
		case CommandType::Ellipse: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 4;
			ctxEllipse(ctx, coords[0], coords[1], coords[2], coords[3]);
		} break;
		case CommandType::Polyline: {
			const uint32_t numPoints = CMD_READ(cmd, uint32_t);
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 2 * numPoints;
			ctxPolyline(ctx, coords, numPoints);
		} break;
		case CommandType::FillPathColor: {
			const uint32_t flags = CMD_READ(cmd, uint32_t);
			const Color color = CMD_READ(cmd, Color);

			ctxFillPathColor(ctx, color, flags);
		} break;
		case CommandType::FillPathGradient: {
			const uint32_t flags = CMD_READ(cmd, uint32_t);
			const uint16_t gradientHandle = CMD_READ(cmd, uint16_t);
			const uint16_t gradientFlags = CMD_READ(cmd, uint16_t);

			const GradientHandle gradient = { isLocal(gradientFlags) ? (uint16_t)(gradientHandle + firstGradientID) : gradientHandle, 0 };
			ctxFillPathGradient(ctx, gradient, flags);
		} break;
		case CommandType::FillPathImagePattern: {
			const uint32_t flags = CMD_READ(cmd, uint32_t);
			const Color color = CMD_READ(cmd, Color);
			const uint16_t imgPatternHandle = CMD_READ(cmd, uint16_t);
			const uint16_t imgPatternFlags = CMD_READ(cmd, uint16_t);

			const ImagePatternHandle imgPattern = { isLocal(imgPatternFlags) ? (uint16_t)(imgPatternHandle + firstImagePatternID) : imgPatternHandle, 0 };
			ctxFillPathImagePattern(ctx, imgPattern, color, flags);
		} break;
		case CommandType::StrokePathColor: {
			const float width = CMD_READ(cmd, float);
			const uint32_t flags = CMD_READ(cmd, uint32_t);
			const Color color = CMD_READ(cmd, Color);

			ctxStrokePathColor(ctx, color, width, flags);
		} break;
		case CommandType::StrokePathGradient: {
			const float width = CMD_READ(cmd, float);
			const uint32_t flags = CMD_READ(cmd, uint32_t);
			const uint16_t gradientHandle = CMD_READ(cmd, uint16_t);
			const uint16_t gradientFlags = CMD_READ(cmd, uint16_t);

			const GradientHandle gradient = { isLocal(gradientFlags) ? (uint16_t)(gradientHandle + firstGradientID) : gradientHandle, 0 };
			ctxStrokePathGradient(ctx, gradient, width, flags);
		} break;
		case CommandType::StrokePathImagePattern: {
			const float width = CMD_READ(cmd, float);
			const uint32_t flags = CMD_READ(cmd, uint32_t);
			const Color color = CMD_READ(cmd, Color);
			const uint16_t imgPatternHandle = CMD_READ(cmd, uint16_t);
			const uint16_t imgPatternFlags = CMD_READ(cmd, uint16_t);

			const ImagePatternHandle imgPattern = { isLocal(imgPatternFlags) ? (uint16_t)(imgPatternHandle + firstImagePatternID) : imgPatternHandle, 0 };
			ctxStrokePathImagePattern(ctx, imgPattern, color, width, flags);
		} break;
		case CommandType::IndexedTriList: {
			const uint32_t numVertices = CMD_READ(cmd, uint32_t);
			const float* positions = (float*)cmd;
			cmd += sizeof(float) * 2 * numVertices;
			const uint32_t numUVs = CMD_READ(cmd, uint32_t);
			const uv_t* uv = (uv_t*)cmd;
			cmd += sizeof(uv_t) * 2 * numUVs;
			const uint32_t numColors = CMD_READ(cmd, uint32_t);
			const Color* colors = (Color*)cmd;
			cmd += sizeof(Color) * numColors;
			const uint32_t numIndices = CMD_READ(cmd, uint32_t);
			const uint16_t* indices = (uint16_t*)cmd;
			cmd += sizeof(uint16_t) * numIndices;
			const uint16_t imgHandle = CMD_READ(cmd, uint16_t);

			ctxIndexedTriList(ctx, positions, numUVs ? uv : nullptr, numVertices, colors, numColors, indices, numIndices, { imgHandle });
		} break;
		case CommandType::CreateLinearGradient: {
			const float* params = (float*)cmd;
			cmd += sizeof(float) * 4;
			const Color* colors = (Color*)cmd;
			cmd += sizeof(Color) * 2;
			ctxCreateLinearGradient(ctx, params[0], params[1], params[2], params[3], colors[0], colors[1]);
		} break;
		case CommandType::CreateBoxGradient: {
			const float* params = (float*)cmd;
			cmd += sizeof(float) * 6;
			const Color* colors = (Color*)cmd;
			cmd += sizeof(Color) * 2;
			ctxCreateBoxGradient(ctx, params[0], params[1], params[2], params[3], params[4], params[5], colors[0], colors[1]);
		} break;
		case CommandType::CreateRadialGradient: {
			const float* params = (float*)cmd;
			cmd += sizeof(float) * 4;
			const Color* colors = (Color*)cmd;
			cmd += sizeof(Color) * 2;
			ctxCreateRadialGradient(ctx, params[0], params[1], params[2], params[3], colors[0], colors[1]);
		} break;
		case CommandType::CreateImagePattern: {
			const float* params = (float*)cmd;
			cmd += sizeof(float) * 5;
			const ImageHandle img = CMD_READ(cmd, ImageHandle);
			ctxCreateImagePattern(ctx, params[0], params[1], params[2], params[3], params[4], img);
		} break;
		case CommandType::Text: {
			const TextConfig* txtCfg = (TextConfig*)cmd;
			cmd += sizeof(TextConfig);
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 2;
			const uint32_t stringOffset = CMD_READ(cmd, uint32_t);
			const uint32_t stringLen = CMD_READ(cmd, uint32_t);
			VG_CHECK(stringOffset < cl->m_StringBufferPos, "Invalid string offset");
			VG_CHECK(stringOffset + stringLen <= cl->m_StringBufferPos, "Invalid string length");

			const char* str = stringBuffer + stringOffset;
			const char* end = str + stringLen;
			ctxText(ctx, *txtCfg, coords[0], coords[1], str, end);
		} break;
		case CommandType::TextBox: {
			const TextConfig* txtCfg = (TextConfig*)cmd;
			cmd += sizeof(TextConfig);
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 3; // x, y, breakWidth
			const uint32_t stringOffset = CMD_READ(cmd, uint32_t);
			const uint32_t stringLen = CMD_READ(cmd, uint32_t);
			const uint32_t textboxFlags = CMD_READ(cmd, uint32_t);
			VG_CHECK(stringOffset < cl->m_StringBufferPos, "Invalid string offset");
			VG_CHECK(stringOffset + stringLen <= cl->m_StringBufferPos, "Invalid string length");

			const char* str = stringBuffer + stringOffset;
			const char* end = str + stringLen;
			ctxTextBox(ctx, *txtCfg, coords[0], coords[1], coords[2], str, end, textboxFlags);
		} break;
		case CommandType::ResetScissor: {
			ctxResetScissor(ctx);
			skipCmds = false;
		} break;
		case CommandType::SetScissor: {
			const float* rect = (float*)cmd;
			cmd += sizeof(float) * 4;
			ctxSetScissor(ctx, rect[0], rect[1], rect[2], rect[3]);

			if (cullCmds) {
				const State* state = getState(ctx);
				const float* scissorRect = &state->m_ScissorRect[0];
				skipCmds = (scissorRect[2] < 1.0f) || (scissorRect[3] < 1.0f);
			}
		} break;
		case CommandType::IntersectScissor: {
			const float* rect = (float*)cmd;
			cmd += sizeof(float) * 4;
			
			const bool zeroRect = !ctxIntersectScissor(ctx, rect[0], rect[1], rect[2], rect[3]);
			if (cullCmds) {
				skipCmds = zeroRect;
			}
		} break;
		case CommandType::PushState: {
			ctxPushState(ctx);
		} break;
		case CommandType::PopState: {
			ctxPopState(ctx);
			if (cullCmds) {
				const State* state = getState(ctx);
				const float* scissorRect = &state->m_ScissorRect[0];
				skipCmds = (scissorRect[2] < 1.0f) || (scissorRect[3] < 1.0f);
			}
		} break;
		case CommandType::TransformIdentity: {
			ctxTransformIdentity(ctx);
		} break;
		case CommandType::TransformRotate: {
			const float ang_rad = CMD_READ(cmd, float);
			ctxTransformRotate(ctx, ang_rad);
		} break;
		case CommandType::TransformTranslate: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 2;
			ctxTransformTranslate(ctx, coords[0], coords[1]);
		} break;
		case CommandType::TransformScale: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 2;
			ctxTransformScale(ctx, coords[0], coords[1]);
		} break;
		case CommandType::TransformMult: {
			const float* mtx = (float*)cmd;
			cmd += sizeof(float) * 6;
			const TransformOrder::Enum order = CMD_READ(cmd, TransformOrder::Enum);
			ctxTransformMult(ctx, mtx, order);
		} break;
		case CommandType::SetViewBox: {
			const float* viewBox = (float*)cmd;
			cmd += sizeof(float) * 4;
			ctxSetViewBox(ctx, viewBox[0], viewBox[1], viewBox[2], viewBox[3]);
		} break;
		case CommandType::BeginClip: {
			const ClipRule::Enum rule = CMD_READ(cmd, ClipRule::Enum);
			ctxBeginClip(ctx, rule);
		} break;
		case CommandType::EndClip: {
			ctxEndClip(ctx);
		} break;
		case CommandType::ResetClip: {
			ctxResetClip(ctx);
		} break;
		case CommandType::SubmitCommandList: {
			const uint16_t cmdListID = CMD_READ(cmd, uint16_t);
			const CommandListHandle cmdListHandle = { cmdListID };

			if (isCommandListHandleValid(ctx, cmdListHandle)) {
				ctxSubmitCommandList(ctx, cmdListHandle);
			}
		} break;
		default: {
			VG_CHECK(false, "Unknown command");
		} break;
		}

		cmd = nextCmd;
	}

#if VG_CONFIG_COMMAND_LIST_PRESERVE_STATE
	ctxPopState(ctx);
	ctxResetClip(ctx);
#endif

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	popCommandListCache(ctx);
#endif

	--ctx->m_SubmitCmdListRecursionDepth;
}

#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
// Active command list
static void aclBeginPath(Context* ctx)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clBeginPath(ctx, ctx->m_ActiveCommandList);
}

static void aclMoveTo(Context* ctx, float x, float y)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clMoveTo(ctx, ctx->m_ActiveCommandList, x, y);
}

static void aclLineTo(Context* ctx, float x, float y)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clLineTo(ctx, ctx->m_ActiveCommandList, x, y);
}

static void aclCubicTo(Context* ctx, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clCubicTo(ctx, ctx->m_ActiveCommandList, c1x, c1y, c2x, c2y, x, y);
}

static void aclQuadraticTo(Context* ctx, float cx, float cy, float x, float y)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clQuadraticTo(ctx, ctx->m_ActiveCommandList, cx, cy, x, y);
}

static void aclArc(Context* ctx, float cx, float cy, float r, float a0, float a1, Winding::Enum dir)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clArc(ctx, ctx->m_ActiveCommandList, cx, cy, r, a0, a1, dir);
}

static void aclArcTo(Context* ctx, float x1, float y1, float x2, float y2, float r)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clArcTo(ctx, ctx->m_ActiveCommandList, x1, y1, x2, y2, r);
}

static void aclRect(Context* ctx, float x, float y, float w, float h)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clRect(ctx, ctx->m_ActiveCommandList, x, y, w, h);
}

static void aclRoundedRect(Context* ctx, float x, float y, float w, float h, float r)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clRoundedRect(ctx, ctx->m_ActiveCommandList, x, y, w, h, r);
}

static void aclRoundedRectVarying(Context* ctx, float x, float y, float w, float h, float rtl, float rtr, float rbr, float rbl)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clRoundedRectVarying(ctx, ctx->m_ActiveCommandList, x, y, w, h, rtl, rtr, rbr, rbl);
}

static void aclCircle(Context* ctx, float cx, float cy, float radius)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clCircle(ctx, ctx->m_ActiveCommandList, cx, cy, radius);
}

static void aclEllipse(Context* ctx, float cx, float cy, float rx, float ry)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clEllipse(ctx, ctx->m_ActiveCommandList, cx, cy, rx, ry);
}

static void aclPolyline(Context* ctx, const float* coords, uint32_t numPoints)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clPolyline(ctx, ctx->m_ActiveCommandList, coords, numPoints);
}

static void aclClosePath(Context* ctx)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clClosePath(ctx, ctx->m_ActiveCommandList);
}

static void aclFillPathColor(Context* ctx, Color color, uint32_t flags)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clFillPath(ctx, ctx->m_ActiveCommandList, color, flags);
}

static void aclFillPathGradient(Context* ctx, GradientHandle gradientHandle, uint32_t flags)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clFillPath(ctx, ctx->m_ActiveCommandList, gradientHandle, flags);
}

static void aclFillPathImagePattern(Context* ctx, ImagePatternHandle imgPatternHandle, Color color, uint32_t flags)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clFillPath(ctx, ctx->m_ActiveCommandList, imgPatternHandle, color, flags);
}

static void aclStrokePathColor(Context* ctx, Color color, float width, uint32_t flags)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clStrokePath(ctx, ctx->m_ActiveCommandList, color, width, flags);
}

static void aclStrokePathGradient(Context* ctx, GradientHandle gradientHandle, float width, uint32_t flags)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clStrokePath(ctx, ctx->m_ActiveCommandList, gradientHandle, width, flags);
}

static void aclStrokePathImagePattern(Context* ctx, ImagePatternHandle imgPatternHandle, Color color, float width, uint32_t flags)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clStrokePath(ctx, ctx->m_ActiveCommandList, imgPatternHandle, color, width, flags);
}

static void aclBeginClip(Context* ctx, ClipRule::Enum rule)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clBeginClip(ctx, ctx->m_ActiveCommandList, rule);
}

static void aclEndClip(Context* ctx)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clEndClip(ctx, ctx->m_ActiveCommandList);
}

static void aclResetClip(Context* ctx)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clResetClip(ctx, ctx->m_ActiveCommandList);
}

static GradientHandle aclCreateLinearGradient(Context* ctx, float sx, float sy, float ex, float ey, Color icol, Color ocol)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	return clCreateLinearGradient(ctx, ctx->m_ActiveCommandList, sx, sy, ex, ey, icol, ocol);
}

static GradientHandle aclCreateBoxGradient(Context* ctx, float x, float y, float w, float h, float r, float f, Color icol, Color ocol)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	return clCreateBoxGradient(ctx, ctx->m_ActiveCommandList, x, y, w, h, r, f, icol, ocol);
}

static GradientHandle aclCreateRadialGradient(Context* ctx, float cx, float cy, float inr, float outr, Color icol, Color ocol)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	return clCreateRadialGradient(ctx, ctx->m_ActiveCommandList, cx, cy, inr, outr, icol, ocol);
}

static ImagePatternHandle aclCreateImagePattern(Context* ctx, float cx, float cy, float w, float h, float angle, ImageHandle image)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	return clCreateImagePattern(ctx, ctx->m_ActiveCommandList, cx, cy, w, h, angle, image);
}

static void aclPushState(Context* ctx)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clPushState(ctx, ctx->m_ActiveCommandList);
}

static void aclPopState(Context* ctx)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clPopState(ctx, ctx->m_ActiveCommandList);
}

static void aclResetScissor(Context* ctx)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clResetScissor(ctx, ctx->m_ActiveCommandList);
}

static void aclSetScissor(Context* ctx, float x, float y, float w, float h)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clSetScissor(ctx, ctx->m_ActiveCommandList, x, y, w, h);
}

static bool aclIntersectScissor(Context* ctx, float x, float y, float w, float h)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clIntersectScissor(ctx, ctx->m_ActiveCommandList, x, y, w, h);
	return true;
}

static void aclTransformIdentity(Context* ctx)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clTransformIdentity(ctx, ctx->m_ActiveCommandList);
}

static void aclTransformScale(Context* ctx, float x, float y)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clTransformScale(ctx, ctx->m_ActiveCommandList, x, y);
}

static void aclTransformTranslate(Context* ctx, float x, float y)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clTransformTranslate(ctx, ctx->m_ActiveCommandList, x, y);
}

static void aclTransformRotate(Context* ctx, float ang_rad)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clTransformRotate(ctx, ctx->m_ActiveCommandList, ang_rad);
}

static void aclTransformMult(Context* ctx, const float* mtx, TransformOrder::Enum order)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clTransformMult(ctx, ctx->m_ActiveCommandList, mtx, order);
}

static void aclSetViewBox(Context* ctx, float x, float y, float w, float h)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clSetViewBox(ctx, ctx->m_ActiveCommandList, x, y, w, h);
}

static void aclIndexedTriList(Context* ctx, const float* pos, const uv_t* uv, uint32_t numVertices, const Color* colors, uint32_t numColors, const uint16_t* indices, uint32_t numIndices, ImageHandle img)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clIndexedTriList(ctx, ctx->m_ActiveCommandList, pos, uv, numVertices, colors, numColors, indices, numIndices, img);
}

static void aclText(Context* ctx, const TextConfig& cfg, float x, float y, const char* str, const char* end)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clText(ctx, ctx->m_ActiveCommandList, cfg, x, y, str, end);
}

static void aclTextBox(Context* ctx, const TextConfig& cfg, float x, float y, float breakWidth, const char* str, const char* end, uint32_t textboxFlags)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clTextBox(ctx, ctx->m_ActiveCommandList, cfg, x, y, breakWidth, str, end, textboxFlags);
}

static void aclSubmitCommandList(Context* ctx, CommandListHandle handle)
{
	VG_CHECK(isValid(ctx->m_ActiveCommandList), "Invalid Context state");
	clSubmitCommandList(ctx, ctx->m_ActiveCommandList, handle);
}
#endif // VG_CONFIG_COMMAND_LIST_BEGIN_END_API

// Internal
static inline const uv_t* getWhitePixelUV(Context* ctx)
{
	return ctx->m_FontImageWhitePixelUV;
}

static void updateWhitePixelUV(Context* ctx)
{
	uint16_t w, h;
	getImageSize(ctx, ctx->m_FontImages[0], &w, &h);

#if VG_CONFIG_UV_INT16
	ctx->m_FontImageWhitePixelUV[0] = INT16_MAX / (int16_t)w;
	ctx->m_FontImageWhitePixelUV[1] = INT16_MAX / (int16_t)h;
#else	#else
	ctx->m_FontImageWhitePixelUV[0] = 0.5f / (float)w;
	ctx->m_FontImageWhitePixelUV[1] = 0.5f / (float)h;
#endif
}

static State* getState(Context* ctx)
{
	const uint32_t top = ctx->m_StateStackTop;
	return &ctx->m_StateStack[top];
}

static void updateState(State* state)
{
	const float* stateTransform = state->m_TransformMtx;

	const float sx = bx::sqrt(stateTransform[0] * stateTransform[0] + stateTransform[2] * stateTransform[2]);
	const float sy = bx::sqrt(stateTransform[1] * stateTransform[1] + stateTransform[3] * stateTransform[3]);
	const float avgScale = (sx + sy) * 0.5f;

	state->m_AvgScale = avgScale;

	const float quantFactor = 0.1f;
	const float quantScale = (bx::floor((avgScale / quantFactor) + 0.5f)) * quantFactor;
	state->m_FontScale = bx::clamp<float>(quantScale, VG_CONFIG_MIN_FONT_SCALE, VG_CONFIG_MAX_FONT_SCALE);
}

static float* allocTransformedVertices(Context* ctx, uint32_t numVertices)
{
	if (numVertices > ctx->m_TransformedVertexCapacity) {
		bx::AllocatorI* allocator = ctx->m_Allocator;
		ctx->m_TransformedVertices = (float*)BX_ALIGNED_REALLOC(allocator, ctx->m_TransformedVertices, sizeof(float) * 2 * numVertices, 16);
		ctx->m_TransformedVertexCapacity = numVertices;
	}

	return ctx->m_TransformedVertices;
}

static const float* transformPath(Context* ctx)
{
	if (ctx->m_PathTransformed) {
		return ctx->m_TransformedVertices;
	}

	Path* path = ctx->m_Path;

	const uint32_t numPathVertices = pathGetNumVertices(path);
	float* transformedVertices = allocTransformedVertices(ctx, numPathVertices);

	const State* state = getState(ctx);
	const float* stateTransform = state->m_TransformMtx;
	const float* pathVertices = pathGetVertices(path);
	vgutil::batchTransformPositions(pathVertices, numPathVertices, transformedVertices, stateTransform);
	ctx->m_PathTransformed = true;

	return transformedVertices;
}

static VertexBuffer* allocVertexBuffer(Context* ctx)
{
	if (ctx->m_NumVertexBuffers + 1 > ctx->m_VertexBufferCapacity) {
		ctx->m_VertexBufferCapacity++;
		ctx->m_VertexBuffers = (VertexBuffer*)BX_REALLOC(ctx->m_Allocator, ctx->m_VertexBuffers, sizeof(VertexBuffer) * ctx->m_VertexBufferCapacity);
		ctx->m_GPUVertexBuffers = (GPUVertexBuffer*)BX_REALLOC(ctx->m_Allocator, ctx->m_GPUVertexBuffers, sizeof(GPUVertexBuffer) * ctx->m_VertexBufferCapacity);

		GPUVertexBuffer* gpuvb = &ctx->m_GPUVertexBuffers[ctx->m_VertexBufferCapacity - 1];

		gpuvb->m_PosBufferHandle = BGFX_INVALID_HANDLE;
		gpuvb->m_UVBufferHandle = BGFX_INVALID_HANDLE;
		gpuvb->m_ColorBufferHandle = BGFX_INVALID_HANDLE;
	}

	VertexBuffer* vb = &ctx->m_VertexBuffers[ctx->m_NumVertexBuffers++];
	vb->m_Pos = allocVertexBufferData_Vec2(ctx);
#if VG_CONFIG_UV_INT16
	vb->m_UV = allocVertexBufferData_UV(ctx);
#else
	vb->m_UV = allocVertexBufferData_Vec2(ctx);
#endif
	vb->m_Color = allocVertexBufferData_Uint32(ctx);
	vb->m_Count = 0;

	return vb;
}

static uint16_t allocIndexBuffer(Context* ctx)
{
#if BX_CONFIG_SUPPORTS_THREADING
	bx::MutexScope ms(*ctx->m_DataPoolMutex);
#endif

	uint16_t ibID = UINT16_MAX;
	const uint32_t numIB = ctx->m_NumIndexBuffers;
	for (uint32_t i = 0; i < numIB; ++i) {
		if (ctx->m_IndexBuffers[i].m_Count == 0) {
			ibID = (uint16_t)i;
			break;
		}
	}

	if (ibID == UINT16_MAX) {
		ctx->m_NumIndexBuffers++;
		ctx->m_IndexBuffers = (IndexBuffer*)BX_REALLOC(ctx->m_Allocator, ctx->m_IndexBuffers, sizeof(IndexBuffer) * ctx->m_NumIndexBuffers);
		ctx->m_GPUIndexBuffers = (GPUIndexBuffer*)BX_REALLOC(ctx->m_Allocator, ctx->m_GPUIndexBuffers, sizeof(GPUIndexBuffer) * ctx->m_NumIndexBuffers);

		ibID = (uint16_t)(ctx->m_NumIndexBuffers - 1);

		IndexBuffer* ib = &ctx->m_IndexBuffers[ibID];
		ib->m_Capacity = 0;
		ib->m_Count = 0;
		ib->m_Indices = nullptr;

		GPUIndexBuffer* gpuib = &ctx->m_GPUIndexBuffers[ibID];
		gpuib->m_bgfxHandle = BGFX_INVALID_HANDLE;
	}

	return ibID;
}

static float* allocVertexBufferData_Vec2(Context* ctx)
{
#if BX_CONFIG_SUPPORTS_THREADING
	bx::MutexScope ms(*ctx->m_DataPoolMutex);
#endif

	bx::AllocatorI* allocator = ctx->m_Allocator;

	const uint32_t capacity = ctx->m_Vec2DataPoolCapacity;
	for (uint32_t i = 0; i < capacity; ++i) {
		float* data = ctx->m_Vec2DataPool[i];
		// If LSB of pointer is set it means that the ptr is valid and the buffer is free for reuse.
		if (((uintptr_t)data) & 1) {
			// Remove the free flag
			data = (float*)((uintptr_t)data & ~1);;
			ctx->m_Vec2DataPool[i] = data;
			return data;
		} else if (data == nullptr) {
			data = (float*)BX_ALIGNED_ALLOC(allocator, sizeof(float) * 2 * ctx->m_Config.m_MaxVBVertices, 16);
			ctx->m_Vec2DataPool[i] = data;
			return data;
		}
	}

	ctx->m_Vec2DataPoolCapacity += 8;
	ctx->m_Vec2DataPool = (float**)BX_REALLOC(allocator, ctx->m_Vec2DataPool, sizeof(float*) * ctx->m_Vec2DataPoolCapacity);
	bx::memSet(&ctx->m_Vec2DataPool[capacity], 0, sizeof(float*) * (ctx->m_Vec2DataPoolCapacity - capacity));

	ctx->m_Vec2DataPool[capacity] = (float*)BX_ALIGNED_ALLOC(allocator, sizeof(float) * 2 * ctx->m_Config.m_MaxVBVertices, 16);
	return ctx->m_Vec2DataPool[capacity];
}

static uint32_t* allocVertexBufferData_Uint32(Context* ctx)
{
#if BX_CONFIG_SUPPORTS_THREADING
	bx::MutexScope ms(*ctx->m_DataPoolMutex);
#endif

	bx::AllocatorI* allocator = ctx->m_Allocator;

	const uint32_t capacity = ctx->m_Uint32DataPoolCapacity;
	for (uint32_t i = 0; i < capacity; ++i) {
		uint32_t* data = ctx->m_Uint32DataPool[i];
		// If LSB of pointer is set it means that the ptr is valid and the buffer is free for reuse.
		if (((uintptr_t)data) & 1) {
			// Remove the free flag
			data = (uint32_t*)((uintptr_t)data & ~1);
			ctx->m_Uint32DataPool[i] = data;
			return data;
		} else if (data == nullptr) {
			data = (uint32_t*)BX_ALIGNED_ALLOC(allocator, sizeof(uint32_t) * ctx->m_Config.m_MaxVBVertices, 16);
			ctx->m_Uint32DataPool[i] = data;
			return data;
		}
	}

	ctx->m_Uint32DataPoolCapacity += 8;
	ctx->m_Uint32DataPool = (uint32_t**)BX_REALLOC(allocator, ctx->m_Uint32DataPool, sizeof(uint32_t*) * ctx->m_Uint32DataPoolCapacity);
	bx::memSet(&ctx->m_Uint32DataPool[capacity], 0, sizeof(uint32_t*) * (ctx->m_Uint32DataPoolCapacity - capacity));

	ctx->m_Uint32DataPool[capacity] = (uint32_t*)BX_ALIGNED_ALLOC(allocator, sizeof(uint32_t) * ctx->m_Config.m_MaxVBVertices, 16);
	return ctx->m_Uint32DataPool[capacity];
}

#if VG_CONFIG_UV_INT16
static int16_t* allocVertexBufferData_UV(Context* ctx)
{
#if BX_CONFIG_SUPPORTS_THREADING
	bx::MutexScope ms(*ctx->m_DataPoolMutex);
#endif

	bx::AllocatorI* allocator = ctx->m_Allocator;

	const uint32_t capacity = ctx->m_UVDataPoolCapacity;
	for (uint32_t i = 0; i < capacity; ++i) {
		int16_t* data = ctx->m_UVDataPool[i];

		// If LSB of pointer is set it means that the ptr is valid and the buffer is free for reuse.
		if (((uintptr_t)data) & 1) {
			// Remove the free flag
			data = (int16_t*)((uintptr_t)data & ~1);
			ctx->m_UVDataPool[i] = data;
			return data;
		} else if (data == nullptr) {
			data = (int16_t*)BX_ALIGNED_ALLOC(allocator, sizeof(int16_t) * 2 * ctx->m_Config.m_MaxVBVertices, 16);
			ctx->m_UVDataPool[i] = data;
			return data;
		}
	}

	ctx->m_UVDataPoolCapacity += 8;
	ctx->m_UVDataPool = (int16_t**)BX_REALLOC(allocator, ctx->m_UVDataPool, sizeof(int16_t*) * ctx->m_UVDataPoolCapacity);
	bx::memSet(&ctx->m_UVDataPool[capacity], 0, sizeof(int16_t*) * (ctx->m_UVDataPoolCapacity - capacity));

	ctx->m_UVDataPool[capacity] = (int16_t*)BX_ALIGNED_ALLOC(allocator, sizeof(int16_t) * 2 * ctx->m_Config.m_MaxVBVertices, 16);
	return ctx->m_UVDataPool[capacity];
}
#endif

static void releaseVertexBufferData_Vec2(Context* ctx, float* data)
{
#if BX_CONFIG_SUPPORTS_THREADING
	bx::MutexScope ms(*ctx->m_DataPoolMutex);
#endif

	VG_CHECK(data != nullptr, "Tried to release a null vertex buffer");
	const uint32_t capacity = ctx->m_Vec2DataPoolCapacity;
	for (uint32_t i = 0; i < capacity; ++i) {
		if (ctx->m_Vec2DataPool[i] == data) {
			// Mark buffer as free by setting the LSB of the ptr to 1.
			ctx->m_Vec2DataPool[i] = (float*)((uintptr_t)ctx->m_Vec2DataPool[i] | 1);
			break;
		}
	}
}

static void releaseVertexBufferData_Uint32(Context* ctx, uint32_t* data)
{
#if BX_CONFIG_SUPPORTS_THREADING
	bx::MutexScope ms(*ctx->m_DataPoolMutex);
#endif

	VG_CHECK(data != nullptr, "Tried to release a null vertex buffer");
	const uint32_t capacity = ctx->m_Uint32DataPoolCapacity;
	for (uint32_t i = 0; i < capacity; ++i) {
		if (ctx->m_Uint32DataPool[i] == data) {
			// Mark buffer as free by setting the LSB of the ptr to 1.
			ctx->m_Uint32DataPool[i] = (uint32_t*)((uintptr_t)ctx->m_Uint32DataPool[i] | 1);
			break;
		}
	}
}

#if VG_CONFIG_UV_INT16
static void releaseVertexBufferData_UV(Context* ctx, int16_t* data)
{
#if BX_CONFIG_SUPPORTS_THREADING
	bx::MutexScope ms(*ctx->m_DataPoolMutex);
#endif

	VG_CHECK(data != nullptr, "Tried to release a null vertex buffer");
	const uint32_t capacity = ctx->m_UVDataPoolCapacity;
	for (uint32_t i = 0; i < capacity; ++i) {
		if (ctx->m_UVDataPool[i] == data) {
			// Mark buffer as free by setting the LSB of the ptr to 1.
			ctx->m_UVDataPool[i] = (int16_t*)((uintptr_t)ctx->m_UVDataPool[i] | 1);
			break;
		}
	}
}
#endif

static void releaseIndexBuffer(Context* ctx, uint16_t* data)
{
#if BX_CONFIG_SUPPORTS_THREADING
	bx::MutexScope ms(*ctx->m_DataPoolMutex);
#endif

	VG_CHECK(data != nullptr, "Tried to release a null vertex buffer");
	const uint32_t numIB = ctx->m_NumIndexBuffers;
	for (uint32_t i = 0; i < numIB; ++i) {
		if (ctx->m_IndexBuffers[i].m_Indices == data) {
			// Reset the ib for reuse.
			ctx->m_IndexBuffers[i].m_Count = 0;
			break;
		}
	}
}

static void createDrawCommand_VertexColor(Context* ctx, const float* vtx, uint32_t numVertices, const uint32_t* colors, uint32_t numColors, const uint16_t* indices, uint32_t numIndices)
{
	// Allocate the draw command
	const ImageHandle fontImg = ctx->m_FontImages[0];
	DrawCommand* cmd = allocDrawCommand(ctx, numVertices, numIndices, DrawCommand::Type::Textured, fontImg.idx);

	// Vertex buffer
	VertexBuffer* vb = &ctx->m_VertexBuffers[cmd->m_VertexBufferID];
	const uint32_t vbOffset = cmd->m_FirstVertexID + cmd->m_NumVertices;

	float* dstPos = &vb->m_Pos[vbOffset << 1];
	bx::memCopy(dstPos, vtx, sizeof(float) * 2 * numVertices);

	const uv_t* uv = getWhitePixelUV(ctx);

	uv_t* dstUV = &vb->m_UV[vbOffset << 1];
#if VG_CONFIG_UV_INT16
	vgutil::memset32(dstUV, numVertices, &uv[0]);
#else
	vgutil::memset64(dstUV, numVertices, &uv[0]);
#endif

	uint32_t* dstColor = &vb->m_Color[vbOffset];
	if (numColors == numVertices) {
		bx::memCopy(dstColor, colors, sizeof(uint32_t) * numVertices);
	} else {
		VG_CHECK(numColors == 1, "Invalid size of color array passed.");
		vgutil::memset32(dstColor, numVertices, colors);
	}

	// Index buffer
	IndexBuffer* ib = &ctx->m_IndexBuffers[ctx->m_ActiveIndexBufferID];
	uint16_t* dstIndex = &ib->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];
	vgutil::batchTransformDrawIndices(indices, numIndices, dstIndex, (uint16_t)cmd->m_NumVertices);

	cmd->m_NumVertices += numVertices;
	cmd->m_NumIndices += numIndices;
}

static void createDrawCommand_ImagePattern(Context* ctx, ImagePatternHandle imgPatternHandle, const float* vtx, uint32_t numVertices, const uint32_t* colors, uint32_t numColors, const uint16_t* indices, uint32_t numIndices)
{
	DrawCommand* cmd = allocDrawCommand(ctx, numVertices, numIndices, DrawCommand::Type::ImagePattern, imgPatternHandle.idx);

	VertexBuffer* vb = &ctx->m_VertexBuffers[cmd->m_VertexBufferID];
	const uint32_t vbOffset = cmd->m_FirstVertexID + cmd->m_NumVertices;

	float* dstPos = &vb->m_Pos[vbOffset << 1];
	bx::memCopy(dstPos, vtx, sizeof(float) * 2 * numVertices);

	uint32_t* dstColor = &vb->m_Color[vbOffset];
	if (numColors == numVertices) {
		bx::memCopy(dstColor, colors, sizeof(uint32_t) * numVertices);
	} else {
		VG_CHECK(numColors == 1, "Invalid size of color array passed.");
		vgutil::memset32(dstColor, numVertices, colors);
	}

	IndexBuffer* ib = &ctx->m_IndexBuffers[ctx->m_ActiveIndexBufferID];
	uint16_t* dstIndex = &ib->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];
	vgutil::batchTransformDrawIndices(indices, numIndices, dstIndex, (uint16_t)cmd->m_NumVertices);

	cmd->m_NumVertices += numVertices;
	cmd->m_NumIndices += numIndices;
}

static void createDrawCommand_ColorGradient(Context* ctx, GradientHandle gradientHandle, const float* vtx, uint32_t numVertices, const uint32_t* colors, uint32_t numColors, const uint16_t* indices, uint32_t numIndices)
{
	DrawCommand* cmd = allocDrawCommand(ctx, numVertices, numIndices, DrawCommand::Type::ColorGradient, gradientHandle.idx);

	VertexBuffer* vb = &ctx->m_VertexBuffers[cmd->m_VertexBufferID];
	const uint32_t vbOffset = cmd->m_FirstVertexID + cmd->m_NumVertices;

	float* dstPos = &vb->m_Pos[vbOffset << 1];
	bx::memCopy(dstPos, vtx, sizeof(float) * 2 * numVertices);

	uint32_t* dstColor = &vb->m_Color[vbOffset];
	if (numColors == numVertices) {
		bx::memCopy(dstColor, colors, sizeof(uint32_t) * numVertices);
	} else {
		VG_CHECK(numColors == 1, "Invalid size of color array passed.");
		vgutil::memset32(dstColor, numVertices, colors);
	}

	IndexBuffer* ib = &ctx->m_IndexBuffers[ctx->m_ActiveIndexBufferID];
	uint16_t* dstIndex = &ib->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];
	vgutil::batchTransformDrawIndices(indices, numIndices, dstIndex, (uint16_t)cmd->m_NumVertices);

	cmd->m_NumVertices += numVertices;
	cmd->m_NumIndices += numIndices;
}

static void createDrawCommand_Clip(Context* ctx, const float* vtx, uint32_t numVertices, const uint16_t* indices, uint32_t numIndices)
{
	// Allocate the draw command
	DrawCommand* cmd = allocClipCommand(ctx, numVertices, numIndices);

	// Vertex buffer
	VertexBuffer* vb = &ctx->m_VertexBuffers[cmd->m_VertexBufferID];
	const uint32_t vbOffset = cmd->m_FirstVertexID + cmd->m_NumVertices;

	float* dstPos = &vb->m_Pos[vbOffset << 1];
	bx::memCopy(dstPos, vtx, sizeof(float) * 2 * numVertices);

	// Index buffer
	IndexBuffer* ib = &ctx->m_IndexBuffers[ctx->m_ActiveIndexBufferID];
	uint16_t* dstIndex = &ib->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];
	vgutil::batchTransformDrawIndices(indices, numIndices, dstIndex, (uint16_t)cmd->m_NumVertices);

	cmd->m_NumVertices += numVertices;
	cmd->m_NumIndices += numIndices;
}

// NOTE: Side effect: Resets m_ForceNewDrawCommand and m_ForceNewClipCommand if the current
// vertex buffer cannot hold the specified amount of vertices.
static uint32_t allocVertices(Context* ctx, uint32_t numVertices, uint32_t* vbID)
{
	VG_CHECK(numVertices < ctx->m_Config.m_MaxVBVertices, "A single draw call cannot have more than %d vertices", ctx->m_Config.m_MaxVBVertices);

	// Check if the current vertex buffer can hold the specified amount of vertices
	VertexBuffer* vb = &ctx->m_VertexBuffers[ctx->m_NumVertexBuffers - 1];
	if (vb->m_Count + numVertices > ctx->m_Config.m_MaxVBVertices) {
		// It cannot. Allocate a new vb.
		vb = allocVertexBuffer(ctx);
		VG_CHECK(vb, "Failed to allocate new Vertex Buffer");

		// The currently active vertex buffer has changed so force a new draw command.
		ctx->m_ForceNewDrawCommand = true;
		ctx->m_ForceNewClipCommand = true;
	}

	*vbID = (uint32_t)(vb - ctx->m_VertexBuffers);
	
	const uint32_t firstVertexID = vb->m_Count;
	vb->m_Count += numVertices;
	return firstVertexID;
}

static uint32_t allocIndices(Context* ctx, uint32_t numIndices)
{
	IndexBuffer* ib = &ctx->m_IndexBuffers[ctx->m_ActiveIndexBufferID];
	if (ib->m_Count + numIndices > ib->m_Capacity) {
		const uint32_t nextCapacity = ib->m_Capacity != 0 ? (ib->m_Capacity * 3) / 2 : 32;

		ib->m_Capacity = bx::uint32_max(nextCapacity, ib->m_Count + numIndices);
		ib->m_Indices = (uint16_t*)BX_ALIGNED_REALLOC(ctx->m_Allocator, ib->m_Indices, sizeof(uint16_t) * ib->m_Capacity, 16);
	}

	const uint32_t firstIndexID = ib->m_Count;
	ib->m_Count += numIndices;
	return firstIndexID;
}

static DrawCommand* allocDrawCommand(Context* ctx, uint32_t numVertices, uint32_t numIndices, DrawCommand::Type::Enum type, uint16_t handle)
{
	uint32_t vertexBufferID;
	const uint32_t firstVertexID = allocVertices(ctx, numVertices, &vertexBufferID);
	const uint32_t firstIndexID = allocIndices(ctx, numIndices);

	const State* state = getState(ctx);
	const float* scissor = state->m_ScissorRect;

	if (!ctx->m_ForceNewDrawCommand && ctx->m_NumDrawCommands != 0) {
		DrawCommand* prevCmd = &ctx->m_DrawCommands[ctx->m_NumDrawCommands - 1];

		VG_CHECK(prevCmd->m_VertexBufferID == vertexBufferID, "Cannot merge draw commands with different vertex buffers");
		VG_CHECK(prevCmd->m_ScissorRect[0] == (uint16_t)scissor[0]
		      && prevCmd->m_ScissorRect[1] == (uint16_t)scissor[1] 
		      && prevCmd->m_ScissorRect[2] == (uint16_t)scissor[2] 
		      && prevCmd->m_ScissorRect[3] == (uint16_t)scissor[3], "Invalid scissor rect");

		if (prevCmd->m_Type == type && prevCmd->m_HandleID == handle) {
			return prevCmd;
		}
	}

	// The new draw command cannot be combined with the previous one. Create a new one.
	if (ctx->m_NumDrawCommands == ctx->m_DrawCommandCapacity) {
		ctx->m_DrawCommandCapacity = ctx->m_DrawCommandCapacity + 32;
		ctx->m_DrawCommands = (DrawCommand*)BX_REALLOC(ctx->m_Allocator, ctx->m_DrawCommands, sizeof(DrawCommand) * ctx->m_DrawCommandCapacity);
	}

	DrawCommand* cmd = &ctx->m_DrawCommands[ctx->m_NumDrawCommands];
	ctx->m_NumDrawCommands++;

	cmd->m_VertexBufferID = vertexBufferID;
	cmd->m_FirstVertexID = firstVertexID;
	cmd->m_FirstIndexID = firstIndexID;
	cmd->m_NumVertices = 0;
	cmd->m_NumIndices = 0;
	cmd->m_Type = type;
	cmd->m_HandleID = handle;
	cmd->m_ScissorRect[0] = (uint16_t)scissor[0];
	cmd->m_ScissorRect[1] = (uint16_t)scissor[1];
	cmd->m_ScissorRect[2] = (uint16_t)scissor[2];
	cmd->m_ScissorRect[3] = (uint16_t)scissor[3];
	bx::memCopy(&cmd->m_ClipState, &ctx->m_ClipState, sizeof(ClipState));

	ctx->m_ForceNewDrawCommand = false;

	return cmd;
}

static DrawCommand* allocClipCommand(Context* ctx, uint32_t numVertices, uint32_t numIndices)
{
	uint32_t vertexBufferID;
	const uint32_t firstVertexID = allocVertices(ctx, numVertices, &vertexBufferID);
	const uint32_t firstIndexID = allocIndices(ctx, numIndices);
	
	const State* state = getState(ctx);
	const float* scissor = state->m_ScissorRect;

	if (!ctx->m_ForceNewClipCommand && ctx->m_NumClipCommands != 0) {
		DrawCommand* prevCmd = &ctx->m_ClipCommands[ctx->m_NumClipCommands - 1];

		VG_CHECK(prevCmd->m_VertexBufferID == vertexBufferID, "Cannot merge clip commands with different vertex buffers");
		VG_CHECK(prevCmd->m_ScissorRect[0] == (uint16_t)scissor[0] 
		      && prevCmd->m_ScissorRect[1] == (uint16_t)scissor[1] 
		      && prevCmd->m_ScissorRect[2] == (uint16_t)scissor[2] 
		      && prevCmd->m_ScissorRect[3] == (uint16_t)scissor[3], "Invalid scissor rect");
		VG_CHECK(prevCmd->m_Type == DrawCommand::Type::Clip, "Invalid draw command type");

		return prevCmd;
	}

	// The new clip command cannot be combined with the previous one. Create a new one.
	if (ctx->m_NumClipCommands == ctx->m_ClipCommandCapacity) {
		ctx->m_ClipCommandCapacity = ctx->m_ClipCommandCapacity + 32;
		ctx->m_ClipCommands = (DrawCommand*)BX_REALLOC(ctx->m_Allocator, ctx->m_ClipCommands, sizeof(DrawCommand) * ctx->m_ClipCommandCapacity);
	}

	DrawCommand* cmd = &ctx->m_ClipCommands[ctx->m_NumClipCommands];
	ctx->m_NumClipCommands++;

	cmd->m_VertexBufferID = vertexBufferID;
	cmd->m_FirstVertexID = firstVertexID;
	cmd->m_FirstIndexID = firstIndexID;
	cmd->m_NumVertices = 0;
	cmd->m_NumIndices = 0;
	cmd->m_Type = DrawCommand::Type::Clip;
	cmd->m_HandleID = UINT16_MAX;
	cmd->m_ScissorRect[0] = (uint16_t)scissor[0];
	cmd->m_ScissorRect[1] = (uint16_t)scissor[1];
	cmd->m_ScissorRect[2] = (uint16_t)scissor[2];
	cmd->m_ScissorRect[3] = (uint16_t)scissor[3];
	cmd->m_ClipState.m_FirstCmdID = ~0u;
	cmd->m_ClipState.m_NumCmds = 0;

	ctx->m_ForceNewClipCommand = false;

	return cmd;
}

static void resetImage(Image* img)
{
	img->m_bgfxHandle = BGFX_INVALID_HANDLE;
	img->m_Width = 0;
	img->m_Height = 0;
	img->m_Flags = 0;
}

static ImageHandle allocImage(Context* ctx)
{
	ImageHandle handle = { ctx->m_ImageHandleAlloc->alloc() };
	if (!isValid(handle)) {
		return VG_INVALID_HANDLE;
	}

	if (handle.idx >= ctx->m_ImageCapacity) {
		const uint32_t oldCapacity = ctx->m_ImageCapacity;

		ctx->m_ImageCapacity = bx::uint32_max(ctx->m_ImageCapacity + 4, handle.idx + 1);
		ctx->m_Images = (Image*)BX_REALLOC(ctx->m_Allocator, ctx->m_Images, sizeof(Image) * ctx->m_ImageCapacity);
		if (!ctx->m_Images) {
			return VG_INVALID_HANDLE;
		}

		// Reset all new textures...
		const uint32_t capacity = ctx->m_ImageCapacity;
		for (uint32_t i = oldCapacity; i < capacity; ++i) {
			resetImage(&ctx->m_Images[i]);
		}
	}

	VG_CHECK(handle.idx < ctx->m_ImageCapacity, "Allocated invalid image handle");
	Image* tex = &ctx->m_Images[handle.idx];
	VG_CHECK(!bgfx::isValid(tex->m_bgfxHandle), "Allocated texture is already in use");
	resetImage(tex);

	return handle;
}

static bool allocTextAtlas(Context* ctx)
{
	flushTextAtlas(ctx);

	if (ctx->m_FontImageID + 1 >= VG_CONFIG_MAX_FONT_IMAGES) {
		VG_WARN(false, "No more text atlases for this frame");
		return false;
	}

	// if next fontImage already have a texture
	uint16_t iw, ih;
	if (isValid(ctx->m_FontImages[ctx->m_FontImageID + 1])) {
		getImageSize(ctx, ctx->m_FontImages[ctx->m_FontImageID + 1], &iw, &ih);
	} else {
		// calculate the new font image size and create it.
		bool imgSizeValid = getImageSize(ctx, ctx->m_FontImages[ctx->m_FontImageID], &iw, &ih);
		VG_CHECK(imgSizeValid, "Invalid font atlas dimensions");
		BX_UNUSED(imgSizeValid);

		if (iw > ih) {
			ih *= 2;
		} else {
			iw *= 2;
		}

		const uint32_t maxTextureSize = bgfx::getCaps()->limits.maxTextureSize;
		if (iw > maxTextureSize || ih > maxTextureSize) {
			iw = ih = (uint16_t)maxTextureSize;
		}

		ctx->m_FontImages[ctx->m_FontImageID + 1] = createImage(ctx, iw, ih, ctx->m_Config.m_FontAtlasImageFlags, nullptr);
	}

	++ctx->m_FontImageID;
	updateWhitePixelUV(ctx);

	fonsResetAtlas(ctx->m_FontStashContext, iw, ih);

	return true;
}

static void renderTextQuads(Context* ctx, uint32_t numQuads, Color color)
{
	const State* state = getState(ctx);
	const float scale = state->m_FontScale * ctx->m_DevicePixelRatio;
	const float invscale = 1.0f / scale;

	const uint32_t c = colorSetAlpha(color, (uint8_t)(state->m_GlobalAlpha * colorGetAlpha(color)));
	if (colorGetAlpha(c) == 0) {
		return;
	}

	float mtx[6];
	mtx[0] = state->m_TransformMtx[0] * invscale;
	mtx[1] = state->m_TransformMtx[1] * invscale;
	mtx[2] = state->m_TransformMtx[2] * invscale;
	mtx[3] = state->m_TransformMtx[3] * invscale;
	mtx[4] = state->m_TransformMtx[4];
	mtx[5] = state->m_TransformMtx[5];

	// TODO: Calculate bounding rect of the quads.
	vgutil::batchTransformTextQuads(&ctx->m_TextQuads->x0, numQuads, mtx, ctx->m_TextVertices);

	const uint32_t numDrawVertices = numQuads * 4;
	const uint32_t numDrawIndices = numQuads * 6;

	DrawCommand* cmd = allocDrawCommand(ctx, numDrawVertices, numDrawIndices, DrawCommand::Type::Textured, ctx->m_FontImages[ctx->m_FontImageID].idx);

	VertexBuffer* vb = &ctx->m_VertexBuffers[cmd->m_VertexBufferID];
	const uint32_t vbOffset = cmd->m_FirstVertexID + cmd->m_NumVertices;

	float* dstPos = &vb->m_Pos[vbOffset << 1];
	bx::memCopy(dstPos, ctx->m_TextVertices, sizeof(float) * 2 * numDrawVertices);

	uint32_t* dstColor = &vb->m_Color[vbOffset];
	vgutil::memset32(dstColor, numDrawVertices, &c);

#if VG_CONFIG_UV_INT16
	int16_t* dstUV = &vb->m_UV[vbOffset << 1];
	const FONSquad* q = ctx->m_TextQuads;
	uint32_t nq = numQuads;
	while (nq-- > 0) {
		const float s0 = q->s0;
		const float s1 = q->s1;
		const float t0 = q->t0;
		const float t1 = q->t1;

		dstUV[0] = (int16_t)(s0 * INT16_MAX); dstUV[1] = (int16_t)(t0 * INT16_MAX);
		dstUV[2] = (int16_t)(s1 * INT16_MAX); dstUV[3] = (int16_t)(t0 * INT16_MAX);
		dstUV[4] = (int16_t)(s1 * INT16_MAX); dstUV[5] = (int16_t)(t1 * INT16_MAX);
		dstUV[6] = (int16_t)(s0 * INT16_MAX); dstUV[7] = (int16_t)(t1 * INT16_MAX);

		dstUV += 8;
		++q;
	}
#else
	float* dstUV = &vb->m_UV[vbOffset << 1];
	const FONSquad* q = ctx->m_TextQuads;
	uint32_t nq = numQuads;
	while (nq-- > 0) {
		const float s0 = q->s0;
		const float s1 = q->s1;
		const float t0 = q->t0;
		const float t1 = q->t1;

		dstUV[0] = s0; dstUV[1] = t0;
		dstUV[2] = s1; dstUV[3] = t0;
		dstUV[4] = s1; dstUV[5] = t1;
		dstUV[6] = s0; dstUV[7] = t1;

		dstUV += 8;
		++q;
	}
#endif

	IndexBuffer* ib = &ctx->m_IndexBuffers[ctx->m_ActiveIndexBufferID];
	uint16_t* dstIndex = &ib->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];
	vgutil::genQuadIndices_unaligned(dstIndex, numQuads, (uint16_t)cmd->m_NumVertices);

	cmd->m_NumVertices += numDrawVertices;
	cmd->m_NumIndices += numDrawIndices;
}

static void flushTextAtlas(Context* ctx)
{
	FONScontext* fons = ctx->m_FontStashContext;

	int dirty[4];
	if (!fonsValidateTexture(fons, dirty)) {
		return;
	}

	// Update texture
	ImageHandle fontImage = ctx->m_FontImages[ctx->m_FontImageID];
	if (!isValid(fontImage)) {
		return;
	}

	int iw, ih;
	const uint8_t* a8Data = fonsGetTextureData(fons, &iw, &ih);
	VG_CHECK(iw > 0 && ih > 0, "Invalid font atlas dimensions");

	// TODO: Convert only the dirty part of the texture (it's the only part that will be uploaded to the backend)
	uint32_t* rgbaData = (uint32_t*)BX_ALLOC(ctx->m_Allocator, sizeof(uint32_t) * iw * ih);
	vgutil::convertA8_to_RGBA8(rgbaData, a8Data, (uint32_t)iw, (uint32_t)ih, 0x00FFFFFF);

	int x = dirty[0];
	int y = dirty[1];
	int w = dirty[2] - dirty[0];
	int h = dirty[3] - dirty[1];
	updateImage(ctx, fontImage, (uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, (const uint8_t*)rgbaData);

	BX_FREE(ctx->m_Allocator, rgbaData);
}

static CommandListHandle allocCommandList(Context* ctx)
{
	CommandListHandle handle = { ctx->m_CmdListHandleAlloc->alloc() };
	if (!isValid(handle)) {
		return VG_INVALID_HANDLE;
	}

	VG_CHECK(handle.idx < ctx->m_Config.m_MaxCommandLists, "Allocated invalid command list handle");
	CommandList* cl = &ctx->m_CmdLists[handle.idx];
	bx::memSet(cl, 0, sizeof(CommandList));

	return handle;
}

static inline bool isCommandListHandleValid(Context* ctx, CommandListHandle handle)
{
	return isValid(handle) && ctx->m_CmdListHandleAlloc->isValid(handle.idx);
}

#if VG_CONFIG_ENABLE_SHAPE_CACHING
static CommandListCache* allocCommandListCache(Context* ctx)
{
	bx::AllocatorI* allocator = ctx->m_Allocator;

	CommandListCache* cache = (CommandListCache*)BX_ALLOC(allocator, sizeof(CommandListCache));
	bx::memSet(cache, 0, sizeof(CommandListCache));

	return cache;
}

static void freeCommandListCache(Context* ctx, CommandListCache* cache)
{
	bx::AllocatorI* allocator = ctx->m_Allocator;

	clCacheReset(ctx, cache);
	BX_FREE(allocator, cache);
}
#endif

static uint8_t* clAllocCommand(Context* ctx, CommandList* cl, CommandType::Enum cmdType, uint32_t dataSize)
{
	const uint32_t alignedDataSize = alignSize(dataSize, VG_CONFIG_COMMAND_LIST_ALIGNMENT);
	const uint32_t totalSize = 0
		+ kAlignedCommandHeaderSize
		+ alignedDataSize;

	const uint32_t pos = cl->m_CommandBufferPos;
	VG_CHECK(isAligned(pos, VG_CONFIG_COMMAND_LIST_ALIGNMENT), "Unaligned command buffer position");

	if (pos + totalSize > cl->m_CommandBufferCapacity) {
		const uint32_t capacityDelta = bx::max<uint32_t>(totalSize, 256);
		cl->m_CommandBufferCapacity += capacityDelta;
		cl->m_CommandBuffer = (uint8_t*)BX_ALIGNED_REALLOC(ctx->m_Allocator, cl->m_CommandBuffer, cl->m_CommandBufferCapacity, VG_CONFIG_COMMAND_LIST_ALIGNMENT);

		ctx->m_Stats.m_CmdListMemoryTotal += capacityDelta;
	}

	uint8_t* ptr = &cl->m_CommandBuffer[pos];
	cl->m_CommandBufferPos += totalSize;
	ctx->m_Stats.m_CmdListMemoryUsed += totalSize;

	CommandHeader* hdr = (CommandHeader*)ptr;
	ptr += kAlignedCommandHeaderSize;

	hdr->m_Type = cmdType;
	hdr->m_Size = alignedDataSize;

	return ptr;
}

static uint32_t clStoreString(Context* ctx, CommandList* cl, const char* str, uint32_t len)
{
	if (cl->m_StringBufferPos + len > cl->m_StringBufferCapacity) {
		cl->m_StringBufferCapacity += bx::max<uint32_t>(len, 128);
		cl->m_StringBuffer = (char*)BX_REALLOC(ctx->m_Allocator, cl->m_StringBuffer, cl->m_StringBufferCapacity);
	}

	const uint32_t offset = cl->m_StringBufferPos;
	bx::memCopy(cl->m_StringBuffer + offset, str, len);
	cl->m_StringBufferPos += len;
	return offset;
}

#if VG_CONFIG_ENABLE_SHAPE_CACHING
static CommandListCache* clGetCache(Context* ctx, CommandList* cl)
{
	if ((cl->m_Flags & CommandListFlags::Cacheable) == 0) {
		return nullptr;
	}

	CommandListCache* cache = cl->m_Cache;
	if (!cache) {
		cache = allocCommandListCache(ctx);
		cl->m_Cache = cache;
	}

	return cache;
}

static void pushCommandListCache(Context* ctx, CommandListCache* cache)
{
	VG_CHECK(ctx->m_CmdListCacheStackTop + 1 < VG_CONFIG_COMMAND_LIST_CACHE_STACK_SIZE, "Command list cache stack overflow");
	++ctx->m_CmdListCacheStackTop;
	ctx->m_CmdListCacheStack[ctx->m_CmdListCacheStackTop] = cache;
}

static void popCommandListCache(Context* ctx)
{
	VG_CHECK(ctx->m_CmdListCacheStackTop != ~0u, "Command list cache stack underflow");
	--ctx->m_CmdListCacheStackTop;
}

static CommandListCache* getCommandListCacheStackTop(Context* ctx)
{
	const uint32_t top = ctx->m_CmdListCacheStackTop;
	return top == ~0u ? nullptr : ctx->m_CmdListCacheStack[top];
}

static void beginCachedCommand(Context* ctx)
{
	CommandListCache* cache = getCommandListCacheStackTop(ctx);
	VG_CHECK(cache, "No bound CommandListCache");

	bx::AllocatorI* allocator = ctx->m_Allocator;

	cache->m_NumCommands++;
	cache->m_Commands = (CachedCommand*)BX_REALLOC(allocator, cache->m_Commands, sizeof(CachedCommand) * cache->m_NumCommands);

	CachedCommand* lastCmd = &cache->m_Commands[cache->m_NumCommands - 1];
	lastCmd->m_FirstMeshID = (uint16_t)cache->m_NumMeshes;
	lastCmd->m_NumMeshes = 0;

	const State* state = getState(ctx);
	vgutil::invertMatrix3(state->m_TransformMtx, lastCmd->m_InvTransformMtx);
}

static void endCachedCommand(Context* ctx)
{
	CommandListCache* cache = getCommandListCacheStackTop(ctx);
	VG_CHECK(cache, "No bound CommandListCache");

	VG_CHECK(cache->m_NumCommands != 0, "beginCachedCommand() hasn't been called");

	CachedCommand* lastCmd = &cache->m_Commands[cache->m_NumCommands - 1];
	VG_CHECK(lastCmd->m_NumMeshes == 0, "endCachedCommand() called too many times");
	lastCmd->m_NumMeshes = (uint16_t)(cache->m_NumMeshes - lastCmd->m_FirstMeshID);
}

static void addCachedCommand(Context* ctx, const float* pos, uint32_t numVertices, const uint32_t* colors, uint32_t numColors, const uint16_t* indices, uint32_t numIndices)
{
	CommandListCache* cache = getCommandListCacheStackTop(ctx);
	VG_CHECK(cache, "No bound CommandListCache");

	bx::AllocatorI* allocator = ctx->m_Allocator;

	cache->m_NumMeshes++;
	cache->m_Meshes = (CachedMesh*)BX_REALLOC(allocator, cache->m_Meshes, sizeof(CachedMesh) * cache->m_NumMeshes);

	CachedMesh* mesh = &cache->m_Meshes[cache->m_NumMeshes - 1];

	const uint32_t totalMem = 0
		+ alignSize(sizeof(float) * 2 * numVertices, 16)
		+ ((numColors != 1) ? alignSize(sizeof(uint32_t) * numVertices, 16) : 0)
		+ alignSize(sizeof(uint16_t) * numIndices, 16);

	uint8_t* mem = (uint8_t*)BX_ALIGNED_ALLOC(allocator, totalMem, 16);
	mesh->m_Pos = (float*)mem;
	mem += alignSize(sizeof(float) * 2 * numVertices, 16);
	
	const float* invMtx = cache->m_Commands[cache->m_NumCommands - 1].m_InvTransformMtx;
	vgutil::batchTransformPositions(pos, numVertices, mesh->m_Pos, invMtx);
	mesh->m_NumVertices = numVertices;

	if (numColors == 1) {
		mesh->m_Colors = nullptr;
	} else {
		VG_CHECK(numColors == numVertices, "Invalid number of colors");
		mesh->m_Colors = (uint32_t*)mem;
		mem += alignSize(sizeof(uint32_t) * numVertices, 16);

		bx::memCopy(mesh->m_Colors, colors, sizeof(uint32_t) * numColors);
	}

	mesh->m_Indices = (uint16_t*)mem;
	bx::memCopy(mesh->m_Indices, indices, sizeof(uint16_t) * numIndices);
	mesh->m_NumIndices = numIndices;
}

// Walk the command list; avoid Path commands and use CachedMesh(es) on Stroker commands. 
// Everything else (state, clip, text) is executed similarly to the uncached version (see submitCommandList).
static void clCacheRender(Context* ctx, CommandList* cl)
{
	const uint16_t numGradients = cl->m_NumGradients;
	const uint16_t numImagePatterns = cl->m_NumImagePatterns;
	const uint32_t clFlags = cl->m_Flags;

	const bool cullCmds = (clFlags & CommandListFlags::AllowCommandCulling) != 0;

	CommandListCache* clCache = cl->m_Cache;
	VG_CHECK(clCache != nullptr, "No CommandListCache in CommandList; this function shouldn't have been called!");
	
	const uint16_t firstGradientID = (uint16_t)ctx->m_NextGradientID;
	const uint16_t firstImagePatternID = (uint16_t)ctx->m_NextImagePatternID;
	VG_CHECK(firstGradientID + numGradients <= ctx->m_Config.m_MaxGradients, "Not enough free gradients for command list. Increase ContextConfig::m_MaxGradients");
	VG_CHECK(firstImagePatternID + numImagePatterns <= ctx->m_Config.m_MaxImagePatterns, "Not enough free image patterns for command list. Increase ContextConfig::m_MaxImagePatterns");
	BX_UNUSED(numGradients, numImagePatterns); // For Release builds.

	const uint8_t* cmd = cl->m_CommandBuffer;
	const uint8_t* cmdListEnd = cl->m_CommandBuffer + cl->m_CommandBufferPos;
	if (cmd == cmdListEnd) {
		return;
	}

	const char* stringBuffer = cl->m_StringBuffer;
	CachedCommand* nextCachedCommand = &clCache->m_Commands[0];

	bool skipCmds = false;

#if VG_CONFIG_COMMAND_LIST_PRESERVE_STATE
	ctxPushState(ctx);
#endif

	while (cmd < cmdListEnd) {
		const CommandHeader* cmdHeader = (CommandHeader*)cmd;
		cmd += kAlignedCommandHeaderSize;

		const uint8_t* nextCmd = cmd + cmdHeader->m_Size;

		// Skip path commands.
		if (cmdHeader->m_Type >= CommandType::FirstPathCommand && cmdHeader->m_Type <= CommandType::LastPathCommand) {
			cmd = nextCmd;
			continue;
		}

		if (skipCmds && cmdHeader->m_Type >= CommandType::FirstStrokerCommand && cmdHeader->m_Type <= CommandType::LastStrokerCommand) {
			cmd = nextCmd;
			++nextCachedCommand;
			continue;
		}

		switch (cmdHeader->m_Type) {
		case CommandType::FillPathColor: {
			const uint32_t flags = CMD_READ(cmd, uint32_t);
			const Color color = CMD_READ(cmd, Color);
			BX_UNUSED(flags);
			submitCachedMesh(ctx, color, &clCache->m_Meshes[nextCachedCommand->m_FirstMeshID], nextCachedCommand->m_NumMeshes);
			++nextCachedCommand;
		} break;
		case CommandType::FillPathGradient: {
			const uint32_t flags = CMD_READ(cmd, uint32_t);
			const uint16_t gradientHandle = CMD_READ(cmd, uint16_t);
			const uint16_t gradientFlags = CMD_READ(cmd, uint16_t);
			BX_UNUSED(flags);

			const GradientHandle gradient = { isLocal(gradientFlags) ? (uint16_t)(gradientHandle + firstGradientID) : gradientHandle, 0 };
			submitCachedMesh(ctx, gradient, &clCache->m_Meshes[nextCachedCommand->m_FirstMeshID], nextCachedCommand->m_NumMeshes);
			++nextCachedCommand;
		} break;
		case CommandType::FillPathImagePattern: {
			const uint32_t flags = CMD_READ(cmd, uint32_t);
			const Color color = CMD_READ(cmd, Color);
			const uint16_t imgPatternHandle = CMD_READ(cmd, uint16_t);
			const uint16_t imgPatternFlags = CMD_READ(cmd, uint16_t);
			BX_UNUSED(flags);

			const ImagePatternHandle imgPattern = { isLocal(imgPatternFlags) ? (uint16_t)(imgPatternHandle + firstImagePatternID) : imgPatternHandle, 0 };
			submitCachedMesh(ctx, imgPattern, color, &clCache->m_Meshes[nextCachedCommand->m_FirstMeshID], nextCachedCommand->m_NumMeshes);
			++nextCachedCommand;
		} break;
		case CommandType::StrokePathColor: {
			const float width = CMD_READ(cmd, float);
			const uint32_t flags = CMD_READ(cmd, uint32_t);
			const Color color = CMD_READ(cmd, Color);
			BX_UNUSED(flags, width);

			submitCachedMesh(ctx, color, &clCache->m_Meshes[nextCachedCommand->m_FirstMeshID], nextCachedCommand->m_NumMeshes);
			++nextCachedCommand;
		} break;
		case CommandType::StrokePathGradient: {
			const float width = CMD_READ(cmd, float);
			const uint32_t flags = CMD_READ(cmd, uint32_t);
			const uint16_t gradientHandle = CMD_READ(cmd, uint16_t);
			const uint16_t gradientFlags = CMD_READ(cmd, uint16_t);
			BX_UNUSED(flags, width);

			const GradientHandle gradient = { isLocal(gradientFlags) ? (uint16_t)(gradientHandle + firstGradientID) : gradientHandle, 0 };
			submitCachedMesh(ctx, gradient, &clCache->m_Meshes[nextCachedCommand->m_FirstMeshID], nextCachedCommand->m_NumMeshes);
			++nextCachedCommand;
		} break;
		case CommandType::StrokePathImagePattern: {
			const float width = CMD_READ(cmd, float);
			const uint32_t flags = CMD_READ(cmd, uint32_t);
			const Color color = CMD_READ(cmd, Color);
			const uint16_t imgPatternHandle = CMD_READ(cmd, uint16_t);
			const uint16_t imgPatternFlags = CMD_READ(cmd, uint16_t);
			BX_UNUSED(flags, width);

			const ImagePatternHandle imgPattern = { isLocal(imgPatternFlags) ? (uint16_t)(imgPatternHandle + firstImagePatternID) : imgPatternHandle, 0 };
			submitCachedMesh(ctx, imgPattern, color, &clCache->m_Meshes[nextCachedCommand->m_FirstMeshID], nextCachedCommand->m_NumMeshes);
			++nextCachedCommand;
		} break;
		case CommandType::IndexedTriList: {
			const uint32_t numVertices = CMD_READ(cmd, uint32_t);
			const float* positions = (float*)cmd;
			cmd += sizeof(float) * 2 * numVertices;
			const uint32_t numUVs = CMD_READ(cmd, uint32_t);
			const uv_t* uv = (uv_t*)cmd;
			cmd += sizeof(uv_t) * 2 * numUVs;
			const uint32_t numColors = CMD_READ(cmd, uint32_t);
			const Color* colors = (Color*)cmd;
			cmd += sizeof(Color) * numColors;
			const uint32_t numIndices = CMD_READ(cmd, uint32_t);
			const uint16_t* indices = (uint16_t*)cmd;
			cmd += sizeof(uint16_t) * numIndices;
			const uint16_t imgHandle = CMD_READ(cmd, uint16_t);

			ctxIndexedTriList(ctx, positions, numUVs ? uv : nullptr, numVertices, colors, numColors, indices, numIndices, { imgHandle });
		} break;
		case CommandType::CreateLinearGradient: {
			const float* params = (float*)cmd;
			cmd += sizeof(float) * 4;
			const Color* colors = (Color*)cmd;
			cmd += sizeof(Color) * 2;
			ctxCreateLinearGradient(ctx, params[0], params[1], params[2], params[3], colors[0], colors[1]);
		} break;
		case CommandType::CreateBoxGradient: {
			const float* params = (float*)cmd;
			cmd += sizeof(float) * 6;
			const Color* colors = (Color*)cmd;
			cmd += sizeof(Color) * 2;
			ctxCreateBoxGradient(ctx, params[0], params[1], params[2], params[3], params[4], params[5], colors[0], colors[1]);
		} break;
		case CommandType::CreateRadialGradient: {
			const float* params = (float*)cmd;
			cmd += sizeof(float) * 4;
			const Color* colors = (Color*)cmd;
			cmd += sizeof(Color) * 2;
			ctxCreateRadialGradient(ctx, params[0], params[1], params[2], params[3], colors[0], colors[1]);
		} break;
		case CommandType::CreateImagePattern: {
			const float* params = (float*)cmd;
			cmd += sizeof(float) * 5;
			const ImageHandle img = CMD_READ(cmd, ImageHandle);
			ctxCreateImagePattern(ctx, params[0], params[1], params[2], params[3], params[4], img);
		} break;
		case CommandType::Text: {
			const TextConfig* txtCfg = (TextConfig*)cmd;
			cmd += sizeof(TextConfig);
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 2;
			const uint32_t stringOffset = CMD_READ(cmd, uint32_t);
			const uint32_t stringLen = CMD_READ(cmd, uint32_t);
			VG_CHECK(stringOffset < cl->m_StringBufferPos, "Invalid string offset");
			VG_CHECK(stringOffset + stringLen <= cl->m_StringBufferPos, "Invalid string length");

			const char* str = stringBuffer + stringOffset;
			const char* end = str + stringLen;
			ctxText(ctx, *txtCfg, coords[0], coords[1], str, end);
		} break;
		case CommandType::TextBox: {
			const TextConfig* txtCfg = (TextConfig*)cmd;
			cmd += sizeof(TextConfig);
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 3; // x, y, breakWidth
			const uint32_t stringOffset = CMD_READ(cmd, uint32_t);
			const uint32_t stringLen = CMD_READ(cmd, uint32_t);
			const uint32_t textboxFlags = CMD_READ(cmd, uint32_t);
			VG_CHECK(stringOffset < cl->m_StringBufferPos, "Invalid string offset");
			VG_CHECK(stringOffset + stringLen <= cl->m_StringBufferPos, "Invalid string length");

			const char* str = stringBuffer + stringOffset;
			const char* end = str + stringLen;
			ctxTextBox(ctx, *txtCfg, coords[0], coords[1], coords[2], str, end, textboxFlags);
		} break;
		case CommandType::ResetScissor: {
			ctxResetScissor(ctx);
			skipCmds = false;
		} break;
		case CommandType::SetScissor: {
			const float* rect = (float*)cmd;
			cmd += sizeof(float) * 4;
			ctxSetScissor(ctx, rect[0], rect[1], rect[2], rect[3]);

			if (cullCmds) {
				skipCmds = (rect[2] < 1.0f) || (rect[3] < 1.0f);
			}
		} break;
		case CommandType::IntersectScissor: {
			const float* rect = (float*)cmd;
			cmd += sizeof(float) * 4;

			const bool zeroRect = !ctxIntersectScissor(ctx, rect[0], rect[1], rect[2], rect[3]);
			if (cullCmds) {
				skipCmds = zeroRect;
			}
		} break;
		case CommandType::PushState: {
			ctxPushState(ctx);
		} break;
		case CommandType::PopState: {
			ctxPopState(ctx);
			if (cullCmds) {
				const State* state = getState(ctx);
				const float* scissorRect = &state->m_ScissorRect[0];
				skipCmds = (scissorRect[2] < 1.0f) || (scissorRect[3] < 1.0f);
			}
		} break;
		case CommandType::TransformIdentity: {
			ctxTransformIdentity(ctx);
		} break;
		case CommandType::TransformRotate: {
			const float ang_rad = CMD_READ(cmd, float);
			ctxTransformRotate(ctx, ang_rad);
		} break;
		case CommandType::TransformTranslate: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 2;
			ctxTransformTranslate(ctx, coords[0], coords[1]);
		} break;
		case CommandType::TransformScale: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 2;
			ctxTransformScale(ctx, coords[0], coords[1]);
		} break;
		case CommandType::TransformMult: {
			const float* mtx = (float*)cmd;
			cmd += sizeof(float) * 6;
			const TransformOrder::Enum order = CMD_READ(cmd, TransformOrder::Enum);
			ctxTransformMult(ctx, mtx, order);
		} break;
		case CommandType::SetViewBox: {
			const float* viewBox = (float*)cmd;
			cmd += sizeof(float) * 4;
			ctxSetViewBox(ctx, viewBox[0], viewBox[1], viewBox[2], viewBox[3]);
		} break;
		case CommandType::BeginClip: {
			const ClipRule::Enum rule = CMD_READ(cmd, ClipRule::Enum);
			ctxBeginClip(ctx, rule);
		} break;
		case CommandType::EndClip: {
			ctxEndClip(ctx);
		} break;
		case CommandType::ResetClip: {
			ctxResetClip(ctx);
		} break;
		case CommandType::SubmitCommandList: {
			const uint16_t cmdListID = CMD_READ(cmd, uint16_t);
			const CommandListHandle cmdListHandle = { cmdListID };

			if (isCommandListHandleValid(ctx, cmdListHandle)) {
				ctxSubmitCommandList(ctx, cmdListHandle);
			}
		} break;
		default: {
			VG_CHECK(false, "Unknown cached command");
		} break;
		}

		cmd = nextCmd;
	}

#if VG_CONFIG_COMMAND_LIST_PRESERVE_STATE
	ctxPopState(ctx);
	ctxResetClip(ctx);
#endif
}

static void clCacheReset(Context* ctx, CommandListCache* cache)
{
	bx::AllocatorI* allocator = ctx->m_Allocator;

	const uint32_t numMeshes = cache->m_NumMeshes;
	for (uint32_t i = 0; i < numMeshes; ++i) {
		CachedMesh* mesh = &cache->m_Meshes[i];
		BX_ALIGNED_FREE(allocator, mesh->m_Pos, 16);
	}
	BX_FREE(allocator, cache->m_Meshes);
	BX_FREE(allocator, cache->m_Commands);

	bx::memSet(cache, 0, sizeof(CommandListCache));
}

static void submitCachedMesh(Context* ctx, Color col, const CachedMesh* meshList, uint32_t numMeshes)
{
	const bool recordClipCommands = ctx->m_RecordClipCommands;

	const State* state = getState(ctx);
	const float* mtx = state->m_TransformMtx;

	if (recordClipCommands) {
		for (uint32_t i = 0; i < numMeshes; ++i) {
			const CachedMesh* mesh = &meshList[i];
			const uint32_t numVertices = mesh->m_NumVertices;
			float* transformedVertices = allocTransformedVertices(ctx, numVertices);

			vgutil::batchTransformPositions(mesh->m_Pos, numVertices, transformedVertices, mtx);
			createDrawCommand_Clip(ctx, transformedVertices, numVertices, mesh->m_Indices, mesh->m_NumIndices);
		}
	} else {
		for (uint32_t i = 0; i < numMeshes; ++i) {
			const CachedMesh* mesh = &meshList[i];
			const uint32_t numVertices = mesh->m_NumVertices;
			float* transformedVertices = allocTransformedVertices(ctx, numVertices);

			const uint32_t* colors = mesh->m_Colors ? mesh->m_Colors : &col;
			const uint32_t numColors = mesh->m_Colors ? numVertices : 1;
			
			vgutil::batchTransformPositions(mesh->m_Pos, numVertices, transformedVertices, mtx);
			createDrawCommand_VertexColor(ctx, transformedVertices, numVertices, colors, numColors, mesh->m_Indices, mesh->m_NumIndices);
		}
	}
}

static void submitCachedMesh(Context* ctx, GradientHandle gradientHandle, const CachedMesh* meshList, uint32_t numMeshes)
{
	VG_CHECK(!ctx->m_RecordClipCommands, "Only submitCachedMesh(Color) is supported inside BeginClip()/EndClip()");
	VG_CHECK(isValid(gradientHandle), "Invalid gradient handle");
	VG_CHECK(!isLocal(gradientHandle), "Invalid gradient handle");

	const State* state = getState(ctx);
	const float* mtx = state->m_TransformMtx;

	const uint32_t black = Colors::Black;
	for (uint32_t i = 0; i < numMeshes; ++i) {
		const CachedMesh* mesh = &meshList[i];
		const uint32_t numVertices = mesh->m_NumVertices;
		float* transformedVertices = allocTransformedVertices(ctx, numVertices);

		const uint32_t* colors = mesh->m_Colors ? mesh->m_Colors : &black;
		const uint32_t numColors = mesh->m_Colors ? numVertices : 1;

		vgutil::batchTransformPositions(mesh->m_Pos, numVertices, transformedVertices, mtx);
		createDrawCommand_ColorGradient(ctx, gradientHandle, transformedVertices, numVertices, colors, numColors, mesh->m_Indices, mesh->m_NumIndices);
	}
}

static void submitCachedMesh(Context* ctx, ImagePatternHandle imgPattern, Color col, const CachedMesh* meshList, uint32_t numMeshes)
{
	VG_CHECK(!ctx->m_RecordClipCommands, "Only submitCachedMesh(Color) is supported inside BeginClip()/EndClip()");
	VG_CHECK(isValid(imgPattern), "Invalid image pattern handle");
	VG_CHECK(!isLocal(imgPattern), "Invalid image pattern handle");

	const State* state = getState(ctx);
	const float* mtx = state->m_TransformMtx;

	for (uint32_t i = 0; i < numMeshes; ++i) {
		const CachedMesh* mesh = &meshList[i];
		const uint32_t numVertices = mesh->m_NumVertices;
		float* transformedVertices = allocTransformedVertices(ctx, numVertices);

		const uint32_t* colors = mesh->m_Colors ? mesh->m_Colors : &col;
		const uint32_t numColors = mesh->m_Colors ? numVertices : 1;

		vgutil::batchTransformPositions(mesh->m_Pos, numVertices, transformedVertices, mtx);
		createDrawCommand_ImagePattern(ctx, imgPattern, transformedVertices, numVertices, colors, numColors, mesh->m_Indices, mesh->m_NumIndices);
	}
}
#endif // VG_CONFIG_ENABLE_SHAPE_CACHING

static void releaseVertexBufferDataCallback_Vec2(void* ptr, void* userData)
{
	Context* ctx = (Context*)userData;
	releaseVertexBufferData_Vec2(ctx, (float*)ptr);
}

static void releaseVertexBufferDataCallback_Uint32(void* ptr, void* userData)
{
	Context* ctx = (Context*)userData;
	releaseVertexBufferData_Uint32(ctx, (uint32_t*)ptr);
}

#if VG_CONFIG_UV_INT16
static void releaseVertexBufferDataCallback_UV(void* ptr, void* userData)
{
	Context* ctx = (Context*)userData;
	releaseVertexBufferData_UV(ctx, (int16_t*)ptr);
}
#endif

static void releaseIndexBufferCallback(void* ptr, void* userData)
{
	Context* ctx = (Context*)userData;
	releaseIndexBuffer(ctx, (uint16_t*)ptr);
}
}
