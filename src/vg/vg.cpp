// TODO:
// - More than 254 clip regions: Either use another view (extra parameter in createContext)
// or draw a fullscreen quad to reset the stencil buffer to 0.
// 
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

#define VG_CONFIG_MAX_FONT_SCALE             4.0f
#define VG_CONFIG_MAX_FONT_IMAGES            4
#define VG_CONFIG_MIN_FONT_ATLAS_SIZE        512

// Minimum font size (after scaling with the current transformation matrix),
// below which no text will be rendered.
#define VG_CONFIG_MIN_FONT_SIZE              1.0f

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
		FirstPathCommand = 0,
		BeginPath = FirstPathCommand,
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
		LastPathCommand = ClosePath,

		// Stroker commands
		FirstStrokerCommand,
		FillPathColor = FirstStrokerCommand,
		FillPathGradient,
		FillPathImagePattern,
		StrokePathColor,
		StrokePathGradient,
		StrokePathImagePattern,
		IndexedTriList,
		LastStrokerCommand = IndexedTriList,

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

		// Text
		Text,
		TextBox,
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

struct Context
{
	ContextConfig m_Config;
	bx::AllocatorI* m_Allocator;
	uint16_t m_ViewID;
	uint16_t m_CanvasWidth;
	uint16_t m_CanvasHeight;
	float m_DevicePixelRatio;
	float m_TesselationTolerance;
	float m_FringeWidth;

	Stroker* m_Stroker;
	Path* m_Path;

	VertexBuffer* m_VertexBuffers;
	GPUVertexBuffer* m_GPUVertexBuffers;
	uint32_t m_NumVertexBuffers;
	uint32_t m_VertexBufferCapacity;

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

	CommandList* m_CommandLists;
	uint32_t m_CommandListCapacity;
	bx::HandleAlloc* m_CommandListHandleAlloc;
	CommandListCache* m_CommandListCache;

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

	float* m_TextVertices;
	FONSquad* m_TextQuads;
	uint32_t m_TextQuadCapacity;
	FONSstring m_TextString;
	FontData* m_FontData;
	uint32_t m_NextFontID;

	bgfx::VertexDecl m_PosVertexDecl;
	bgfx::VertexDecl m_UVVertexDecl;
	bgfx::VertexDecl m_ColorVertexDecl;
	bgfx::ProgramHandle m_ProgramHandle[DrawCommand::Type::NumTypes];
	bgfx::UniformHandle m_TexUniform;
	bgfx::UniformHandle m_PaintMatUniform;
	bgfx::UniformHandle m_ExtentRadiusFeatherUniform;
	bgfx::UniformHandle m_InnerColorUniform;
	bgfx::UniformHandle m_OuterColorUniform;
};

static State* getState(Context* ctx);
static void updateState(State* state);
static void getWhitePixelUV(Context* ctx, uv_t* uv);

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

static DrawCommand* allocDrawCommand_Textured(Context* ctx, uint32_t numVertices, uint32_t numIndices, ImageHandle img);
static DrawCommand* allocDrawCommand_ImagePattern(Context* ctx, uint32_t numVertices, uint32_t numIndices, ImagePatternHandle img);
static DrawCommand* allocDrawCommand_ColorGradient(Context* ctx, uint32_t numVertices, uint32_t numIndices, GradientHandle gradientHandle);
static DrawCommand* allocDrawCommand_Clip(Context* ctx, uint32_t numVertices, uint32_t numIndices);
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
static uint8_t* clAllocCommand(Context* ctx, CommandList* cl, CommandType::Enum cmdType, uint32_t dataSize);
static uint32_t clStoreString(Context* ctx, CommandList* cl, const char* str, uint32_t len);
static void clCacheRender(Context* ctx, CommandList* cl);
static void clCacheReset(Context* ctx, CommandListCache* cache);

#if VG_CONFIG_ENABLE_SHAPE_CACHING
static CommandListCache* clGetCache(Context* ctx, CommandList* cl);
static CommandListCache* allocCommandListCache(Context* ctx);
static void freeCommandListCache(Context* ctx, CommandListCache* cache);
static void bindCommandListCache(Context* ctx, CommandListCache* cache);
static void beginCachedCommand(Context* ctx);
static void endCachedCommand(Context* ctx);
static void addCachedCommand(Context* ctx, const float* pos, uint32_t numVertices, const uint32_t* colors, uint32_t numColors, const uint16_t* indices, uint32_t numIndices);
static void submitCachedMesh(Context* ctx, Color col, const float* invTransform, const CachedMesh* meshList, uint32_t numMeshes);
static void submitCachedMesh(Context* ctx, GradientHandle gradientHandle, const float* invTransform, const CachedMesh* meshList, uint32_t numMeshes);
static void submitCachedMesh(Context* ctx, ImagePatternHandle imgPatter, Color color, const float* invTransform, const CachedMesh* meshList, uint32_t numMeshes);
#endif

#define CMD_WRITE(ptr, type, value) *(type*)(ptr) = (value); ptr += sizeof(type)
#define CMD_READ(ptr, type) *(type*)(ptr); ptr += sizeof(type)

inline uint32_t alignSize(uint32_t sz, uint32_t alignment)
{
	VG_CHECK(bx::isPowerOf2<uint32_t>(alignment), "Invalid alignment value");
	return (sz & (~(alignment - 1))) + ((sz & (alignment - 1)) != 0 ? alignment : 0);
}

//////////////////////////////////////////////////////////////////////////
// Public interface
//
Context* createContext(uint16_t viewID, bx::AllocatorI* allocator, const ContextConfig* userCfg)
{
	static const ContextConfig defaultConfig = {
		64,                          // m_MaxGradients
		64,                          // m_MaxImagePatterns
		8,                           // m_MaxFonts
		32,                          // m_MaxStateStackSize
		16,                          // m_MaxImages
		256,                         // m_MaxCommandLists
		65536,                       // m_MaxVBVertices
		ImageFlags::Filter_Bilinear  // m_FontAtlasImageFlags
	};

	const ContextConfig* cfg = userCfg ? userCfg : &defaultConfig;

	VG_CHECK(cfg->m_MaxVBVertices <= 65536, "Vertex buffers cannot be larger than 64k vertices because indices are always uint16");

	const uint32_t defaultAlignment = 8;
	const uint32_t totalMem = 0
		+ alignSize(sizeof(Context), defaultAlignment)
		+ alignSize(sizeof(Gradient) * cfg->m_MaxGradients, defaultAlignment)
		+ alignSize(sizeof(ImagePattern) * cfg->m_MaxImagePatterns, defaultAlignment)
		+ alignSize(sizeof(State) * cfg->m_MaxStateStackSize, defaultAlignment)
		+ alignSize(sizeof(FontData) * cfg->m_MaxFonts, defaultAlignment);

	uint8_t* mem = (uint8_t*)BX_ALIGNED_ALLOC(allocator, totalMem, defaultAlignment);
	bx::memSet(mem, 0, totalMem);

	Context* ctx = (Context*)mem;
	mem += alignSize(sizeof(Context), defaultAlignment);
	ctx->m_Gradients = (Gradient*)mem;
	mem += alignSize(sizeof(Gradient) * cfg->m_MaxGradients, defaultAlignment);
	ctx->m_ImagePatterns = (ImagePattern*)mem;
	mem += alignSize(sizeof(ImagePattern) * cfg->m_MaxImagePatterns, defaultAlignment);
	ctx->m_StateStack = (State*)mem;
	mem += alignSize(sizeof(State) * cfg->m_MaxStateStackSize, defaultAlignment);
	ctx->m_FontData = (FontData*)mem;
	mem += alignSize(sizeof(FontData) * cfg->m_MaxFonts, defaultAlignment);

	bx::memCopy(&ctx->m_Config, cfg, sizeof(ContextConfig));
	ctx->m_Allocator = allocator;
	ctx->m_ViewID = viewID;
	ctx->m_DevicePixelRatio = 1.0f;
	ctx->m_TesselationTolerance = 0.25f;
	ctx->m_FringeWidth = 1.0f;
	ctx->m_StateStackTop = 0;
	ctx->m_StateStack[0].m_GlobalAlpha = 1.0f;
	resetScissor(ctx);
	transformIdentity(ctx);

#if BX_CONFIG_SUPPORTS_THREADING
	ctx->m_DataPoolMutex = BX_NEW(allocator, bx::Mutex)();
#endif
	ctx->m_Path = createPath(allocator);
	ctx->m_Stroker = createStroker(allocator);

	ctx->m_ImageHandleAlloc = bx::createHandleAlloc(allocator, cfg->m_MaxImages);
	ctx->m_CommandListHandleAlloc = bx::createHandleAlloc(allocator, cfg->m_MaxCommandLists);

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

	ctx->m_TexUniform = bgfx::createUniform("s_tex", bgfx::UniformType::Int1, 1);
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
		deleteImage(ctx, ctx->m_FontImages[i]);
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

	BX_FREE(allocator, ctx->m_CommandLists);
	ctx->m_CommandLists = nullptr;

	bx::destroyHandleAlloc(allocator, ctx->m_CommandListHandleAlloc);
	ctx->m_CommandListHandleAlloc = nullptr;

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

	BX_DELETE(allocator, ctx->m_DataPoolMutex);

	BX_FREE(allocator, ctx);
}

void beginFrame(Context* ctx, uint16_t canvasWidth, uint16_t canvasHeight, float devicePixelRatio)
{
	ctx->m_CanvasWidth = canvasWidth;
	ctx->m_CanvasHeight = canvasHeight;
	ctx->m_DevicePixelRatio = devicePixelRatio;
	ctx->m_TesselationTolerance = 0.25f / devicePixelRatio;
	ctx->m_FringeWidth = 1.0f / devicePixelRatio;
	ctx->m_CommandListCache = nullptr;

	VG_CHECK(ctx->m_StateStackTop == 0, "State stack hasn't been properly reset in the previous frame");
	resetScissor(ctx);
	transformIdentity(ctx);

	ctx->m_NumVertexBuffers = 0;
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

void endFrame(Context* ctx)
{
	VG_CHECK(ctx->m_StateStackTop == 0, "PushState()/PopState() mismatch");
	const uint32_t numDrawCommands = ctx->m_NumDrawCommands;
	if (numDrawCommands == 0) {
		// Release the vertex buffer allocated in beginFrame()
		VertexBuffer* vb = &ctx->m_VertexBuffers[0];
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
	for (uint32_t iVB = 0; iVB < numVertexBuffers; ++iVB) {
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

		bgfx::updateDynamicVertexBuffer(gpuvb->m_PosBufferHandle, 0, posMem);
		bgfx::updateDynamicVertexBuffer(gpuvb->m_UVBufferHandle, 0, uvMem);
		bgfx::updateDynamicVertexBuffer(gpuvb->m_ColorBufferHandle, 0, colorMem);

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
		bgfx::updateDynamicIndexBuffer(gpuib->m_bgfxHandle, 0, indexMem);
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

					int cmdDepth = 0; // TODO: Use depth to sort draw calls into layers.
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
					bgfx::submit(viewID, ctx->m_ProgramHandle[DrawCommand::Type::Clip], cmdDepth, false);
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
		bgfx::setVertexBuffer(1, gpuvb->m_UVBufferHandle, cmd->m_FirstVertexID, cmd->m_NumVertices);
		bgfx::setVertexBuffer(2, gpuvb->m_ColorBufferHandle, cmd->m_FirstVertexID, cmd->m_NumVertices);
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

			bgfx::setTexture(0, ctx->m_TexUniform, tex->m_bgfxHandle, tex->m_Flags);

			int cmdDepth = 0; // TODO: Use depth to sort draw calls into layers.
			bgfx::setState(0
				| BGFX_STATE_ALPHA_WRITE 
				| BGFX_STATE_RGB_WRITE 
				| BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA));
			bgfx::setStencil(stencilState);

			bgfx::submit(viewID, ctx->m_ProgramHandle[DrawCommand::Type::Textured], cmdDepth, false);
		} else if (cmd->m_Type == DrawCommand::Type::ColorGradient) {
			VG_CHECK(cmd->m_HandleID != UINT16_MAX, "Invalid gradient handle");
			Gradient* grad = &ctx->m_Gradients[cmd->m_HandleID];

			bgfx::setUniform(ctx->m_PaintMatUniform, grad->m_Matrix, 1);
			bgfx::setUniform(ctx->m_ExtentRadiusFeatherUniform, grad->m_Params, 1);
			bgfx::setUniform(ctx->m_InnerColorUniform, grad->m_InnerColor, 1);
			bgfx::setUniform(ctx->m_OuterColorUniform, grad->m_OuterColor, 1);

			int cmdDepth = 0; // TODO: Use depth to sort draw calls into layers.
			bgfx::setState(0
				| BGFX_STATE_ALPHA_WRITE
				| BGFX_STATE_RGB_WRITE
				| BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA));
			bgfx::setStencil(stencilState);

			bgfx::submit(viewID, ctx->m_ProgramHandle[DrawCommand::Type::ColorGradient], cmdDepth, false);
		} else if(cmd->m_Type == DrawCommand::Type::ImagePattern) {
			VG_CHECK(cmd->m_HandleID != UINT16_MAX, "Invalid image pattern handle");
			ImagePattern* imgPattern = &ctx->m_ImagePatterns[cmd->m_HandleID];

			VG_CHECK(isValid(imgPattern->m_ImageHandle), "Invalid image handle in pattern");
			Image* tex = &ctx->m_Images[imgPattern->m_ImageHandle.idx];

			bgfx::setTexture(0, ctx->m_TexUniform, tex->m_bgfxHandle, tex->m_Flags);
			bgfx::setUniform(ctx->m_PaintMatUniform, imgPattern->m_Matrix, 1);

			int cmdDepth = 0; // TODO: Use depth to sort draw calls into layers.
			bgfx::setState(0
				| BGFX_STATE_ALPHA_WRITE
				| BGFX_STATE_RGB_WRITE
				| BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA));
			bgfx::setStencil(stencilState);

			bgfx::submit(viewID, ctx->m_ProgramHandle[DrawCommand::Type::ImagePattern], cmdDepth, false);
		} else {
			VG_CHECK(false, "Unknown draw command type");
		}
	}

	// nvgEndFrame
	// TODO: Move to a separate function?
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
						deleteImage(ctx, ctx->m_FontImages[i]);
					} else {
						ctx->m_FontImages[j++] = ctx->m_FontImages[i];
					}
				}
			}

			// make current font image to first
			ctx->m_FontImages[j++] = ctx->m_FontImages[0];
			ctx->m_FontImages[0] = fontImage;
			ctx->m_FontImageID = 0;

			// clear all images after j
			for (int i = j; i < VG_CONFIG_MAX_FONT_IMAGES; i++) {
				ctx->m_FontImages[i] = VG_INVALID_HANDLE;
			}
		}
	}
}

void beginPath(Context* ctx)
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

void moveTo(Context* ctx, float x, float y)
{
	VG_CHECK(!ctx->m_PathTransformed, "Call beginPath() before starting a new path");
	Path* path = ctx->m_Path;
	pathMoveTo(path, x, y);
}

void lineTo(Context* ctx, float x, float y)
{
	VG_CHECK(!ctx->m_PathTransformed, "Call beginPath() before starting a new path");
	Path* path = ctx->m_Path;
	pathLineTo(path, x, y);
}

void cubicTo(Context* ctx, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
	VG_CHECK(!ctx->m_PathTransformed, "Call beginPath() before starting a new path");
	Path* path = ctx->m_Path;
	pathCubicTo(path, c1x, c1y, c2x, c2y, x, y);
}

void quadraticTo(Context* ctx, float cx, float cy, float x, float y)
{
	VG_CHECK(!ctx->m_PathTransformed, "Call beginPath() before starting a new path");
	Path* path = ctx->m_Path;
	pathQuadraticTo(path, cx, cy, x, y);
}

void arc(Context* ctx, float cx, float cy, float r, float a0, float a1, Winding::Enum dir)
{
	VG_CHECK(!ctx->m_PathTransformed, "Call beginPath() before starting a new path");
	Path* path = ctx->m_Path;
	pathArc(path, cx, cy, r, a0, a1, dir);
}

void arcTo(Context* ctx, float x1, float y1, float x2, float y2, float r)
{
	VG_CHECK(!ctx->m_PathTransformed, "Call beginPath() before starting a new path");
	Path* path = ctx->m_Path;
	pathArcTo(path, x1, y1, x2, y2, r);
}

void rect(Context* ctx, float x, float y, float w, float h)
{
	VG_CHECK(!ctx->m_PathTransformed, "Call beginPath() before starting a new path");
	Path* path = ctx->m_Path;
	pathRect(path, x, y, w, h);
}

void roundedRect(Context* ctx, float x, float y, float w, float h, float r)
{
	VG_CHECK(!ctx->m_PathTransformed, "Call beginPath() before starting a new path");
	Path* path = ctx->m_Path;
	pathRoundedRect(path, x, y, w, h, r);
}

void roundedRectVarying(Context* ctx, float x, float y, float w, float h, float rtl, float rbl, float rbr, float rtr)
{
	VG_CHECK(!ctx->m_PathTransformed, "Call beginPath() before starting a new path");
	Path* path = ctx->m_Path;
	pathRoundedRectVarying(path, x, y, w, h, rtl, rbl, rbr, rtr);
}

void circle(Context* ctx, float cx, float cy, float radius)
{
	VG_CHECK(!ctx->m_PathTransformed, "Call beginPath() before starting a new path");
	Path* path = ctx->m_Path;
	pathCircle(path, cx, cy, radius);
}

void ellipse(Context* ctx, float cx, float cy, float rx, float ry)
{
	VG_CHECK(!ctx->m_PathTransformed, "Call beginPath() before starting a new path");
	Path* path = ctx->m_Path;
	pathEllipse(path, cx, cy, rx, ry);
}

void polyline(Context* ctx, const float* coords, uint32_t numPoints)
{
	VG_CHECK(!ctx->m_PathTransformed, "Call beginPath() before starting a new path");
	Path* path = ctx->m_Path;
	pathPolyline(path, coords, numPoints);
}

void closePath(Context* ctx)
{
	VG_CHECK(!ctx->m_PathTransformed, "Call beginPath() before starting a new path");
	Path* path = ctx->m_Path;
	pathClose(path);
}

void fillPath(Context* ctx, Color color, uint32_t flags)
{
	const bool recordClipCommands = ctx->m_RecordClipCommands;
	const bool hasCache = ctx->m_CommandListCache != nullptr;

	const State* state = getState(ctx);
	const float globalAlpha = hasCache ? 1.0f : state->m_GlobalAlpha;
	const Color col = recordClipCommands ? Colors::Black : colorSetAlpha(color, (uint8_t)(globalAlpha * colorGetAlpha(color)));
	if (colorGetAlpha(col) == 0) {
		return;
	}

	const float* pathVertices = transformPath(ctx);

#if VG_CONFIG_FORCE_AA_OFF
	const bool aa = false;
#else
	const bool aa = recordClipCommands ? false : VG_FILL_FLAGS_AA(flags);
#endif
	const PathType::Enum pathType = VG_FILL_FLAGS_PATH_TYPE(flags);

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
		for (uint32_t i = 0; i < numSubPaths; ++i) {
			const SubPath* subPath = &subPaths[i];
			if (subPath->m_NumVertices < 3) {
				return;
			}

			const float* vtx = &pathVertices[subPath->m_FirstVertexID << 1];
			const uint32_t numPathVertices = subPath->m_NumVertices;

			Mesh mesh;
			const uint32_t* colors = &col;
			uint32_t numColors = 1;

			bool decomposed = false;
			if (aa) {
				decomposed = strokerConcaveFillAA(stroker, &mesh, vtx, numPathVertices, col);
				colors = mesh.m_ColorBuffer;
				numColors = mesh.m_NumVertices;
			} else {
				decomposed = strokerConcaveFill(stroker, &mesh, vtx, numPathVertices);
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
	}

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	if (hasCache) {
		endCachedCommand(ctx);
	}
#endif
}

void fillPath(Context* ctx, GradientHandle gradientHandle, uint32_t flags)
{
	VG_CHECK(!ctx->m_RecordClipCommands, "Only fillPath(Color) is supported inside BeginClip()/EndClip()");
	VG_CHECK(isValid(gradientHandle), "Invalid gradient handle");

	const bool hasCache = ctx->m_CommandListCache != nullptr;

	const float* pathVertices = transformPath(ctx);

	const PathType::Enum pathType = VG_FILL_FLAGS_PATH_TYPE(flags);
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
		for (uint32_t i = 0; i < numSubPaths; ++i) {
			const SubPath* subPath = &subPaths[i];
			if (subPath->m_NumVertices < 3) {
				continue;
			}

			const float* vtx = &pathVertices[subPath->m_FirstVertexID << 1];
			const uint32_t numPathVertices = subPath->m_NumVertices;

			const Color black = Colors::Black;

			Mesh mesh;
			const uint32_t* colors = &black;
			uint32_t numColors = 1;

			bool decomposed = false;
			if (aa) {
				decomposed = strokerConcaveFillAA(stroker, &mesh, vtx, numPathVertices, vg::Colors::Black);
				colors = mesh.m_ColorBuffer;
				numColors = mesh.m_NumVertices;
			} else {
				decomposed = strokerConcaveFill(stroker, &mesh, vtx, numPathVertices);
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
	}

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	if (hasCache) {
		endCachedCommand(ctx);
	}
#endif
}

void fillPath(Context* ctx, ImagePatternHandle imgPatternHandle, Color color, uint32_t flags)
{
	VG_CHECK(!ctx->m_RecordClipCommands, "Only fillPath(Color) is supported inside BeginClip()/EndClip()");
	VG_CHECK(isValid(imgPatternHandle), "Invalid image pattern handle");

	const bool hasCache = ctx->m_CommandListCache != nullptr;

	const State* state = getState(ctx);
	const float globalAlpha = hasCache ? 1.0f : state->m_GlobalAlpha;
	const Color col = colorSetAlpha(color, (uint8_t)(globalAlpha * colorGetAlpha(color)));
	if (colorGetAlpha(col) == 0) {
		return;
	}

	const PathType::Enum pathType = VG_FILL_FLAGS_PATH_TYPE(flags);
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

			bool decomposed = false;
			if (aa) {
				decomposed = strokerConcaveFillAA(stroker, &mesh, vtx, numPathVertices, col);
				colors = mesh.m_ColorBuffer;
				numColors = mesh.m_NumVertices;
			} else {
				decomposed = strokerConcaveFill(stroker, &mesh, vtx, numPathVertices);
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
	}

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	if (hasCache) {
		endCachedCommand(ctx);
	}
#endif
}

void strokePath(Context* ctx, Color color, float width, uint32_t flags)
{
	const bool recordClipCommands = ctx->m_RecordClipCommands;
	const bool hasCache = ctx->m_CommandListCache != nullptr;

	const State* state = getState(ctx);
	const float avgScale = state->m_AvgScale;
	const float globalAlpha = hasCache ? 1.0f : state->m_GlobalAlpha;
	const float fringeWidth = ctx->m_FringeWidth;

	const float scaledStrokeWidth = bx::clamp<float>(width * avgScale, 0.0f, 200.0f);
	const bool isThin = scaledStrokeWidth <= fringeWidth;

	const float alphaScale = !isThin ? globalAlpha : globalAlpha * bx::square(bx::clamp<float>(scaledStrokeWidth, 0.0f, fringeWidth));
	const Color col = recordClipCommands ? Colors::Black : colorSetAlpha(color, (uint8_t)(alphaScale * colorGetAlpha(color)));
	if (colorGetAlpha(col) == 0) {
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

void strokePath(Context* ctx, GradientHandle gradientHandle, float width, uint32_t flags)
{
	VG_CHECK(!ctx->m_RecordClipCommands, "Only strokePath(Color) is supported inside BeginClip()/EndClip()");
	VG_CHECK(isValid(gradientHandle), "Invalid gradient handle");

	const bool hasCache = ctx->m_CommandListCache != nullptr;

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

void strokePath(Context* ctx, ImagePatternHandle imgPatternHandle, Color color, float width, uint32_t flags)
{
	VG_CHECK(!ctx->m_RecordClipCommands, "Only strokePath(Color) is supported inside BeginClip()/EndClip()");
	VG_CHECK(isValid(imgPatternHandle), "Invalid image pattern handle");

	const bool hasCache = ctx->m_CommandListCache != nullptr;

	const State* state = getState(ctx);
	const float avgScale = state->m_AvgScale;
	const float globalAlpha = hasCache ? 1.0f : state->m_GlobalAlpha;
	const float fringeWidth = ctx->m_FringeWidth;

	const float scaledStrokeWidth = bx::clamp<float>(width * avgScale, 0.0f, 200.0f);
	const bool isThin = scaledStrokeWidth <= fringeWidth;

	const float alphaScale = isThin ? globalAlpha : globalAlpha * bx::square(bx::clamp<float>(scaledStrokeWidth, 0.0f, fringeWidth));
	const Color col = colorSetAlpha(color, (uint8_t)(alphaScale * colorGetAlpha(color)));
	if (colorGetAlpha(col) == 0) {
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

void beginClip(Context* ctx, ClipRule::Enum rule)
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

void endClip(Context* ctx)
{
	VG_CHECK(ctx->m_RecordClipCommands, "Must be called once after beginClip()");
	
	ClipState* clipState = &ctx->m_ClipState;
	const uint32_t nextClipCmdID = ctx->m_NumClipCommands;

	clipState->m_NumCmds = nextClipCmdID - clipState->m_FirstCmdID;
	
	ctx->m_RecordClipCommands = false;
	ctx->m_ForceNewDrawCommand = true;
}

void resetClip(Context* ctx)
{
	VG_CHECK(!ctx->m_RecordClipCommands, "Must be called outside beginClip()/endClip() pair.");

	ClipState* clipState = &ctx->m_ClipState;
	
	if (clipState->m_FirstCmdID != ~0u) {
		clipState->m_FirstCmdID = ~0u;
		clipState->m_NumCmds = 0;

		ctx->m_ForceNewDrawCommand = true;
	}
}

GradientHandle createLinearGradient(Context* ctx, float sx, float sy, float ex, float ey, Color icol, Color ocol)
{
	if (ctx->m_NextGradientID >= ctx->m_Config.m_MaxGradients) {
		return VG_INVALID_HANDLE;
	}

	GradientHandle handle = { (uint16_t)ctx->m_NextGradientID++ };

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

GradientHandle createBoxGradient(Context* ctx, float x, float y, float w, float h, float r, float f, Color icol, Color ocol)
{
	if (ctx->m_NextGradientID >= ctx->m_Config.m_MaxGradients) {
		return VG_INVALID_HANDLE;
	}

	GradientHandle handle = { (uint16_t)ctx->m_NextGradientID++ };

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

GradientHandle createRadialGradient(Context* ctx, float cx, float cy, float inr, float outr, Color icol, Color ocol)
{
	if (ctx->m_NextGradientID >= ctx->m_Config.m_MaxGradients) {
		return VG_INVALID_HANDLE;
	}

	GradientHandle handle = { (uint16_t)ctx->m_NextGradientID++ };

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

ImagePatternHandle createImagePattern(Context* ctx, float cx, float cy, float w, float h, float angle, ImageHandle image)
{
	if (!isValid(image)) {
		return VG_INVALID_HANDLE;
	}

	if (ctx->m_NextImagePatternID >= ctx->m_Config.m_MaxImagePatterns) {
		return VG_INVALID_HANDLE;
	}

	ImagePatternHandle handle = { (uint16_t)ctx->m_NextImagePatternID++ };

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

void setGlobalAlpha(Context* ctx, float alpha)
{
	State* state = getState(ctx);
	state->m_GlobalAlpha = alpha;
}

void pushState(Context* ctx)
{
	VG_CHECK(ctx->m_StateStackTop < (uint32_t)(ctx->m_Config.m_MaxStateStackSize - 1), "State stack overflow");

	const uint32_t top = ctx->m_StateStackTop;
	const State* curState = &ctx->m_StateStack[top];
	State* newState = &ctx->m_StateStack[top + 1];
	bx::memCopy(newState, curState, sizeof(State));
	++ctx->m_StateStackTop;
}

void popState(Context* ctx)
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
			lastScissor[3] != (uint16_t)stateScissor[3]) 
		{
			ctx->m_ForceNewDrawCommand = true;
			ctx->m_ForceNewClipCommand = true;
		}
	}
}

void resetScissor(Context* ctx)
{
	State* state = getState(ctx);
	state->m_ScissorRect[0] = state->m_ScissorRect[1] = 0.0f;
	state->m_ScissorRect[2] = (float)ctx->m_CanvasWidth;
	state->m_ScissorRect[3] = (float)ctx->m_CanvasHeight;
	ctx->m_ForceNewDrawCommand = true;
	ctx->m_ForceNewClipCommand = true;
}

void setScissor(Context* ctx, float x, float y, float w, float h)
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

bool intersectScissor(Context* ctx, float x, float y, float w, float h)
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

void transformIdentity(Context* ctx)
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

void transformScale(Context* ctx, float x, float y)
{
	State* state = getState(ctx);
	state->m_TransformMtx[0] = x * state->m_TransformMtx[0];
	state->m_TransformMtx[1] = x * state->m_TransformMtx[1];
	state->m_TransformMtx[2] = y * state->m_TransformMtx[2];
	state->m_TransformMtx[3] = y * state->m_TransformMtx[3];

	updateState(state);
}

void transformTranslate(Context* ctx, float x, float y)
{
	State* state = getState(ctx);
	state->m_TransformMtx[4] += state->m_TransformMtx[0] * x + state->m_TransformMtx[2] * y;
	state->m_TransformMtx[5] += state->m_TransformMtx[1] * x + state->m_TransformMtx[3] * y;

	updateState(state);
}

void transformRotate(Context* ctx, float ang_rad)
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

void transformMult(Context* ctx, const float* mtx, bool pre)
{
	State* state = getState(ctx);
	const float* stateTransform = state->m_TransformMtx;

	float res[6];
	if (pre) {
		vgutil::multiplyMatrix3(stateTransform, mtx, res);
	} else {
		vgutil::multiplyMatrix3(mtx, stateTransform, res);
	}

	bx::memCopy(state->m_TransformMtx, res, sizeof(float) * 6);

	updateState(state);
}

void indexedTriList(Context* ctx, const float* pos, const uv_t* uv, uint32_t numVertices, const Color* colors, uint32_t numColors, const uint16_t* indices, uint32_t numIndices, ImageHandle img)
{
	if (!isValid(img)) {
		img = ctx->m_FontImages[0];
	}

	const State* state = getState(ctx);
	const float* stateTransform = state->m_TransformMtx;

	DrawCommand* cmd = allocDrawCommand_Textured(ctx, numVertices, numIndices, img);

	// Vertex buffer
	VertexBuffer* vb = &ctx->m_VertexBuffers[cmd->m_VertexBufferID];
	const uint32_t vbOffset = cmd->m_FirstVertexID + cmd->m_NumVertices;

	float* dstPos = &vb->m_Pos[vbOffset << 1];
	vgutil::batchTransformPositions_Unaligned(pos, numVertices, dstPos, stateTransform);

	uv_t* dstUV = &vb->m_UV[vbOffset << 1];
	if (uv) {
		bx::memCopy(dstUV, uv, sizeof(uv_t) * 2 * numVertices);
	} else {
		uv_t whiteRectUV[2];
		getWhitePixelUV(ctx, &whiteRectUV[0]);

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

void text(Context* ctx, const TextConfig& cfg, float x, float y, const char* str, const char* end)
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

	pushState(ctx);
	transformTranslate(ctx, x + dx / scale, y + dy / scale);
	renderTextQuads(ctx, numBakedChars, cfg.m_Color);
	popState(ctx);
}

void textBox(Context* ctx, const TextConfig& cfg, float x, float y, float breakWidth, const char* str, const char* end)
{
	uint32_t alignment = cfg.m_Alignment;

	int halign = alignment & (FONS_ALIGN_LEFT | FONS_ALIGN_CENTER | FONS_ALIGN_RIGHT);
	int valign = alignment & (FONS_ALIGN_TOP | FONS_ALIGN_MIDDLE | FONS_ALIGN_BOTTOM | FONS_ALIGN_BASELINE);

	float lineh = getTextLineHeight(ctx, cfg);

	alignment = FONS_ALIGN_LEFT | valign;

	TextRow rows[2];
	int nrows;
	while ((nrows = textBreakLines(ctx, cfg, str, end, breakWidth, rows, 2, 0))) {
		for (int i = 0; i < nrows; ++i) {
			TextRow* row = &rows[i];

			if (halign & FONS_ALIGN_LEFT) {
				text(ctx, cfg, x, y, row->start, row->end);
			} else if (halign & FONS_ALIGN_CENTER) {
				text(ctx, cfg, x + breakWidth * 0.5f - row->width * 0.5f, y, row->start, row->end);
			} else if (halign & FONS_ALIGN_RIGHT) {
				text(ctx, cfg, x + breakWidth - row->width, y, row->start, row->end);
			}

			y += lineh; // Assume line height multiplier to be 1.0 (NanoVG allows the user to change it, but I don't use it).
		}

		str = rows[nrows - 1].next;
	}
}

float measureText(Context* ctx, const TextConfig& cfg, float x, float y, const char* str, const char* end, float* bounds)
{
	// nvgTextBounds()
	// TODO: fonsTextBounds() calls fons__getGlyph() which in turn bakes the glyph into the atlas.
	// This shouldn't happen at this point. Measuring text should do the absolute minimum work to 
	// calculate text size.
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
	fonsTextIterInit(fons, &iter, 0, 0, str, end);
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
			if (flags & TextBreakFlags::SpacesAsChars) {
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
					rowWidth = iter.nextx - rowStartX;
					rowMaxX = q.x1 - rowStartX;
				}

				// track last end of a word
				if (ptype == CP_CHAR && type == CP_SPACE) {
					breakEnd = iter.str;
					breakWidth = rowWidth;
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
	fonsTextIterInit(fons, &iter, x * scale, y * scale, str, end);
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

	uint32_t bgfxFlags = BGFX_TEXTURE_NONE;

#if BX_PLATFORM_EMSCRIPTEN
	if (!isPowerOf2(w) || !isPowerOf2(h)) {
		flags = ImageFlags::Filter_NearestUV | ImageFlags::Filter_NearestW;
		bgfxFlags |= BGFX_TEXTURE_U_CLAMP | BGFX_TEXTURE_V_CLAMP | BGFX_TEXTURE_W_CLAMP;
	}
#endif

	if (flags & ImageFlags::Filter_NearestUV) {
		bgfxFlags |= BGFX_TEXTURE_MIN_POINT | BGFX_TEXTURE_MAG_POINT;
	}
	if (flags & ImageFlags::Filter_NearestW) {
		bgfxFlags |= BGFX_TEXTURE_MIP_POINT;
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
	bx::gather(mem->data, data + y * pitch + x * bytesPerPixel, w * bytesPerPixel, h, pitch);

	bgfx::updateTexture2D(tex->m_bgfxHandle, 0, 0, x, y, w, h, mem, UINT16_MAX);

	return true;
}

bool deleteImage(Context* ctx, ImageHandle img)
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


//////////////////////////////////////////////////////////////////////////
// Command lists
//
CommandListHandle createCommandList(Context* ctx, uint32_t flags)
{
	CommandListHandle handle = allocCommandList(ctx);
	if (!isValid(handle)) {
		return VG_INVALID_HANDLE;
	}

	CommandList* cl = &ctx->m_CommandLists[handle.idx];
	cl->m_Flags = flags;

	return handle;
}

void destroyCommandList(Context* ctx, CommandListHandle handle)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");

	bx::AllocatorI* allocator = ctx->m_Allocator;

	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	if (cl->m_Cache) {
		freeCommandListCache(ctx, cl->m_Cache);
	}

	BX_FREE(allocator, cl->m_CommandBuffer);
	BX_FREE(allocator, cl->m_StringBuffer);
	bx::memSet(cl, 0, sizeof(CommandList));

	ctx->m_CommandListHandleAlloc->free(handle.idx);
}

void clReset(Context* ctx, CommandListHandle handle)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	cl->m_CommandBufferPos = 0;
	cl->m_StringBufferPos = 0;
	cl->m_NumImagePatterns = 0;
	cl->m_NumGradients = 0;
}

void clBeginPath(Context* ctx, CommandListHandle handle)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];
	
	clAllocCommand(ctx, cl, CommandType::BeginPath, 0);
}

void clMoveTo(Context* ctx, CommandListHandle handle, float x, float y)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::MoveTo, sizeof(float) * 2);
	CMD_WRITE(ptr, float, x);
	CMD_WRITE(ptr, float, y);
}

void clLineTo(Context* ctx, CommandListHandle handle, float x, float y)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::LineTo, sizeof(float) * 2);
	CMD_WRITE(ptr, float, x);
	CMD_WRITE(ptr, float, y);
}

void clCubicTo(Context* ctx, CommandListHandle handle, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

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
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::QuadraticTo, sizeof(float) * 4);
	CMD_WRITE(ptr, float, cx);
	CMD_WRITE(ptr, float, cy);
	CMD_WRITE(ptr, float, x);
	CMD_WRITE(ptr, float, y);
}

void clArc(Context* ctx, CommandListHandle handle, float cx, float cy, float r, float a0, float a1, Winding::Enum dir)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

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
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

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
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::Rect, sizeof(float) * 4);
	CMD_WRITE(ptr, float, x);
	CMD_WRITE(ptr, float, y);
	CMD_WRITE(ptr, float, w);
	CMD_WRITE(ptr, float, h);
}

void clRoundedRect(Context* ctx, CommandListHandle handle, float x, float y, float w, float h, float r)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::RoundedRect, sizeof(float) * 5);
	CMD_WRITE(ptr, float, x);
	CMD_WRITE(ptr, float, y);
	CMD_WRITE(ptr, float, w);
	CMD_WRITE(ptr, float, h);
	CMD_WRITE(ptr, float, r);
}

void clRoundedRectVarying(Context* ctx, CommandListHandle handle, float x, float y, float w, float h, float rtl, float rbl, float rbr, float rtr)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::RoundedRectVarying, sizeof(float) * 8);
	CMD_WRITE(ptr, float, x);
	CMD_WRITE(ptr, float, y);
	CMD_WRITE(ptr, float, w);
	CMD_WRITE(ptr, float, h);
	CMD_WRITE(ptr, float, rtl);
	CMD_WRITE(ptr, float, rbl);
	CMD_WRITE(ptr, float, rbr);
	CMD_WRITE(ptr, float, rtr);
}

void clCircle(Context* ctx, CommandListHandle handle, float cx, float cy, float radius)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::Circle, sizeof(float) * 3);
	CMD_WRITE(ptr, float, cx);
	CMD_WRITE(ptr, float, cy);
	CMD_WRITE(ptr, float, radius);
}

void clEllipse(Context* ctx, CommandListHandle handle, float cx, float cy, float rx, float ry)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::Ellipse, sizeof(float) * 4);
	CMD_WRITE(ptr, float, cx);
	CMD_WRITE(ptr, float, cy);
	CMD_WRITE(ptr, float, rx);
	CMD_WRITE(ptr, float, ry);
}

void clPolyline(Context* ctx, CommandListHandle handle, const float* coords, uint32_t numPoints)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::Polyline, sizeof(float) * 2 * numPoints);
	bx::memCopy(ptr, coords, sizeof(float) * 2 * numPoints);
}

void clClosePath(Context* ctx, CommandListHandle handle)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	clAllocCommand(ctx, cl, CommandType::ClosePath, 0);
}

void clIndexedTriList(Context* ctx, CommandListHandle handle, const float* pos, const uv_t* uv, uint32_t numVertices, const Color* color, uint32_t numColors, const uint16_t* indices, uint32_t numIndices, ImageHandle img)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

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
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::FillPathColor, sizeof(uint32_t) + sizeof(Color));
	CMD_WRITE(ptr, uint32_t, flags);
	CMD_WRITE(ptr, Color, color);
}

void clFillPath(Context* ctx, CommandListHandle handle, GradientHandle gradient, uint32_t flags)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	VG_CHECK(isValid(gradient), "Invalid gradient handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::FillPathGradient, sizeof(uint32_t) + sizeof(uint16_t) + sizeof(bool));
	CMD_WRITE(ptr, uint32_t, flags);
	CMD_WRITE(ptr, uint16_t, gradient.idx);
	CMD_WRITE(ptr, bool, false);
}

void clFillPath(Context* ctx, CommandListHandle handle, LocalGradientHandle gradient, uint32_t flags)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	VG_CHECK(isValid(gradient), "Invalid gradient handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::FillPathGradient, sizeof(uint32_t) + sizeof(uint16_t) + sizeof(bool));
	CMD_WRITE(ptr, uint32_t, flags);
	CMD_WRITE(ptr, uint16_t, gradient.idx);
	CMD_WRITE(ptr, bool, true);
}

void clFillPath(Context* ctx, CommandListHandle handle, ImagePatternHandle img, Color color, uint32_t flags)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	VG_CHECK(isValid(img), "Invalid image pattern handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::FillPathImagePattern, sizeof(uint32_t) + sizeof(Color) + sizeof(uint16_t) + sizeof(bool));
	CMD_WRITE(ptr, uint32_t, flags);
	CMD_WRITE(ptr, Color, color);
	CMD_WRITE(ptr, uint16_t, img.idx);
	CMD_WRITE(ptr, bool, false);
}

void clFillPath(Context* ctx, CommandListHandle handle, LocalImagePatternHandle img, Color color, uint32_t flags)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	VG_CHECK(isValid(img), "Invalid image pattern handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::FillPathImagePattern, sizeof(uint32_t) + sizeof(Color) + sizeof(uint16_t) + sizeof(bool));
	CMD_WRITE(ptr, uint32_t, flags);
	CMD_WRITE(ptr, Color, color);
	CMD_WRITE(ptr, uint16_t, img.idx);
	CMD_WRITE(ptr, bool, true);
}

void clStrokePath(Context* ctx, CommandListHandle handle, Color color, float width, uint32_t flags)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::StrokePathColor, sizeof(float) + sizeof(uint32_t) + sizeof(Color));
	CMD_WRITE(ptr, float, width);
	CMD_WRITE(ptr, uint32_t, flags);
	CMD_WRITE(ptr, Color, color);
}

void clStrokePath(Context* ctx, CommandListHandle handle, GradientHandle gradient, float width, uint32_t flags)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	VG_CHECK(isValid(gradient), "Invalid gradient handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::StrokePathGradient, sizeof(float) + sizeof(uint32_t) + sizeof(uint16_t) + sizeof(bool));
	CMD_WRITE(ptr, float, width);
	CMD_WRITE(ptr, uint32_t, flags);
	CMD_WRITE(ptr, uint16_t, gradient.idx);
	CMD_WRITE(ptr, bool, false);
}

void clStrokePath(Context* ctx, CommandListHandle handle, LocalGradientHandle gradient, float width, uint32_t flags)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	VG_CHECK(isValid(gradient), "Invalid gradient handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::StrokePathGradient, sizeof(float) + sizeof(uint32_t) + sizeof(uint16_t) + sizeof(bool));
	CMD_WRITE(ptr, float, width);
	CMD_WRITE(ptr, uint32_t, flags);
	CMD_WRITE(ptr, uint16_t, gradient.idx);
	CMD_WRITE(ptr, bool, true);
}

void clStrokePath(Context* ctx, CommandListHandle handle, ImagePatternHandle img, Color color, float width, uint32_t flags)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	VG_CHECK(isValid(img), "Invalid image pattern handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::StrokePathImagePattern, sizeof(float) + sizeof(uint32_t) + sizeof(Color) + sizeof(uint16_t) + sizeof(bool));
	CMD_WRITE(ptr, float, width);
	CMD_WRITE(ptr, uint32_t, flags);
	CMD_WRITE(ptr, Color, color);
	CMD_WRITE(ptr, uint16_t, img.idx);
	CMD_WRITE(ptr, bool, false);
}

void clStrokePath(Context* ctx, CommandListHandle handle, LocalImagePatternHandle img, Color color, float width, uint32_t flags)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	VG_CHECK(isValid(img), "Invalid image pattern handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::StrokePathImagePattern, sizeof(float) + sizeof(uint32_t) + sizeof(Color) + sizeof(uint16_t) + sizeof(bool));
	CMD_WRITE(ptr, float, width);
	CMD_WRITE(ptr, uint32_t, flags);
	CMD_WRITE(ptr, Color, color);
	CMD_WRITE(ptr, uint16_t, img.idx);
	CMD_WRITE(ptr, bool, true);
}

void clBeginClip(Context* ctx, CommandListHandle handle, ClipRule::Enum rule)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::BeginClip, sizeof(ClipRule::Enum));
	CMD_WRITE(ptr, ClipRule::Enum, rule);
}

void clEndClip(Context* ctx, CommandListHandle handle)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	clAllocCommand(ctx, cl, CommandType::EndClip, 0);
}

void clResetClip(Context* ctx, CommandListHandle handle)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	clAllocCommand(ctx, cl, CommandType::ResetClip, 0);
}

LocalGradientHandle clCreateLinearGradient(Context* ctx, CommandListHandle handle, float sx, float sy, float ex, float ey, Color icol, Color ocol)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::CreateLinearGradient, sizeof(float) * 4 + sizeof(Color) * 2);
	CMD_WRITE(ptr, float, sx);
	CMD_WRITE(ptr, float, sy);
	CMD_WRITE(ptr, float, ex);
	CMD_WRITE(ptr, float, ey);
	CMD_WRITE(ptr, Color, icol);
	CMD_WRITE(ptr, Color, ocol);

	const uint16_t gradientHandle = cl->m_NumGradients;
	cl->m_NumGradients++;
	return { gradientHandle };
}

LocalGradientHandle clCreateBoxGradient(Context* ctx, CommandListHandle handle, float x, float y, float w, float h, float r, float f, Color icol, Color ocol)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

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
	return { gradientHandle };
}

LocalGradientHandle clCreateRadialGradient(Context* ctx, CommandListHandle handle, float cx, float cy, float inr, float outr, Color icol, Color ocol)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::CreateRadialGradient, sizeof(float) * 4 + sizeof(Color) * 2);
	CMD_WRITE(ptr, float, cx);
	CMD_WRITE(ptr, float, cy);
	CMD_WRITE(ptr, float, inr);
	CMD_WRITE(ptr, float, outr);
	CMD_WRITE(ptr, Color, icol);
	CMD_WRITE(ptr, Color, ocol);

	const uint16_t gradientHandle = cl->m_NumGradients;
	cl->m_NumGradients++;
	return { gradientHandle };
}

LocalImagePatternHandle clCreateImagePattern(Context* ctx, CommandListHandle handle, float cx, float cy, float w, float h, float angle, ImageHandle image)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	VG_CHECK(isValid(image), "Invalid image handle");

	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::CreateImagePattern, sizeof(float) * 5 + sizeof(uint16_t));
	CMD_WRITE(ptr, float, cx);
	CMD_WRITE(ptr, float, cy);
	CMD_WRITE(ptr, float, w);
	CMD_WRITE(ptr, float, h);
	CMD_WRITE(ptr, float, angle);
	CMD_WRITE(ptr, uint16_t, image.idx);

	const uint16_t patternHandle = cl->m_NumImagePatterns;
	cl->m_NumImagePatterns++;
	return { patternHandle };
}

void clPushState(Context* ctx, CommandListHandle handle)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	clAllocCommand(ctx, cl, CommandType::PushState, 0);
}

void clPopState(Context* ctx, CommandListHandle handle)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	clAllocCommand(ctx, cl, CommandType::PopState, 0);
}

void clResetScissor(Context* ctx, CommandListHandle handle)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	clAllocCommand(ctx, cl, CommandType::ResetScissor, 0);
}

void clSetScissor(Context* ctx, CommandListHandle handle, float x, float y, float w, float h)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::SetScissor, sizeof(float) * 4);
	CMD_WRITE(ptr, float, x);
	CMD_WRITE(ptr, float, y);
	CMD_WRITE(ptr, float, w);
	CMD_WRITE(ptr, float, h);
}

void clIntersectScissor(Context* ctx, CommandListHandle handle, float x, float y, float w, float h)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::IntersectScissor, sizeof(float) * 4);
	CMD_WRITE(ptr, float, x);
	CMD_WRITE(ptr, float, y);
	CMD_WRITE(ptr, float, w);
	CMD_WRITE(ptr, float, h);
}

void clTransformIdentity(Context* ctx, CommandListHandle handle)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	clAllocCommand(ctx, cl, CommandType::TransformIdentity, 0);
}

void clTransformScale(Context* ctx, CommandListHandle handle, float x, float y)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::TransformScale, sizeof(float) * 2);
	CMD_WRITE(ptr, float, x);
	CMD_WRITE(ptr, float, y);
}

void clTransformTranslate(Context* ctx, CommandListHandle handle, float x, float y)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::TransformTranslate, sizeof(float) * 2);
	CMD_WRITE(ptr, float, x);
	CMD_WRITE(ptr, float, y);
}

void clTransformRotate(Context* ctx, CommandListHandle handle, float ang_rad)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::TransformRotate, sizeof(float));
	CMD_WRITE(ptr, float, ang_rad);
}

void clTransformMult(Context* ctx, CommandListHandle handle, const float* mtx, bool pre)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::TransformMult, sizeof(float) * 6 + sizeof(bool));
	bx::memCopy(ptr, mtx, sizeof(float) * 6);
	ptr += sizeof(float) * 6;
	CMD_WRITE(ptr, bool, pre);
}

void clText(Context* ctx, CommandListHandle handle, const TextConfig& cfg, float x, float y, const char* str, const char* end)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

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

void clTextBox(Context* ctx, CommandListHandle handle, const TextConfig& cfg, float x, float y, float breakWidth, const char* str, const char* end)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	const uint32_t len = end ? (uint32_t)(end - str) : (uint32_t)bx::strLen(str);
	if (len == 0) {
		return;
	}

	const uint32_t offset = clStoreString(ctx, cl, str, len);

	uint8_t* ptr = clAllocCommand(ctx, cl, CommandType::TextBox, sizeof(TextConfig) + sizeof(float) * 3 + sizeof(uint32_t) * 2);
	bx::memCopy(ptr, &cfg, sizeof(TextConfig));
	ptr += sizeof(TextConfig);
	CMD_WRITE(ptr, float, x);
	CMD_WRITE(ptr, float, y);
	CMD_WRITE(ptr, float, breakWidth);
	CMD_WRITE(ptr, uint32_t, offset);
	CMD_WRITE(ptr, uint32_t, len);
}

void submitCommandList(Context* ctx, CommandListHandle handle)
{
	VG_CHECK(isValid(handle), "Invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];

	const uint16_t numGradients = cl->m_NumGradients;
	const uint16_t numImagePatterns = cl->m_NumImagePatterns;
	const uint32_t clFlags = cl->m_Flags;

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	CommandListCache* clCache = clGetCache(ctx, cl);
	if(clCache) {
		const State* state = getState(ctx);

		const float cachedScale = clCache->m_AvgScale;
		const float stateScale = state->m_AvgScale;
		if (cachedScale == stateScale) {
			clCacheRender(ctx, cl);
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
	const bool cullCmds = !clCache && ((clFlags & CommandListFlags::AllowCommandCulling) != 0);

	const uint16_t firstGradientID = (uint16_t)ctx->m_NextGradientID;
	const uint16_t firstImagePatternID = (uint16_t)ctx->m_NextImagePatternID;
	VG_CHECK(firstGradientID + numGradients <= ctx->m_Config.m_MaxGradients, "Not enough free gradients for command list. Increase ContextConfig::m_MaxGradients");
	VG_CHECK(firstImagePatternID + numImagePatterns <= ctx->m_Config.m_MaxImagePatterns, "Not enough free image patterns for command list. Increase ContextConfig::m_MaxImagePatterns");

	uint8_t* cmd = cl->m_CommandBuffer;
	const uint8_t* cmdListEnd = cl->m_CommandBuffer + cl->m_CommandBufferPos;
	if (cmd == cmdListEnd) {
		return;
	}

	const char* stringBuffer = cl->m_StringBuffer;

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	bindCommandListCache(ctx, clCache);
#endif

	bool skipCmds = false;
	pushState(ctx);
	while (cmd < cmdListEnd) {
		const CommandHeader* cmdHeader = (CommandHeader*)cmd;
		cmd += sizeof(CommandHeader);
		
		if (skipCmds && cmdHeader->m_Type >= CommandType::FirstStrokerCommand && cmdHeader->m_Type <= CommandType::LastStrokerCommand) {
			cmd += cmdHeader->m_Size;
			continue;
		}

		const uint8_t* cmdEnd = cmd + cmdHeader->m_Size;

		switch (cmdHeader->m_Type) {
		case CommandType::BeginPath: {
			beginPath(ctx);
		} break;
		case CommandType::ClosePath: {
			closePath(ctx);
		} break;
		case CommandType::MoveTo: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 2;
			moveTo(ctx, coords[0], coords[1]);
		} break;
		case CommandType::LineTo: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 2;
			lineTo(ctx, coords[0], coords[1]);
		} break;
		case CommandType::CubicTo: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 6;
			cubicTo(ctx, coords[0], coords[1], coords[2], coords[3], coords[4], coords[5]);
		} break;
		case CommandType::QuadraticTo: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 4;
			quadraticTo(ctx, coords[0], coords[1], coords[2], coords[3]);
		} break;
		case CommandType::Arc: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 5;
			const Winding::Enum dir = CMD_READ(cmd, Winding::Enum);
			arc(ctx, coords[0], coords[1], coords[2], coords[3], coords[4], dir);
		} break;
		case CommandType::ArcTo: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 5;
			arcTo(ctx, coords[0], coords[1], coords[2], coords[3], coords[4]);
		} break;
		case CommandType::Rect: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 4;
			rect(ctx, coords[0], coords[1], coords[2], coords[3]);
		} break;
		case CommandType::RoundedRect: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 5;
			roundedRect(ctx, coords[0], coords[1], coords[2], coords[3], coords[4]);
		} break;
		case CommandType::RoundedRectVarying: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 8;
			roundedRectVarying(ctx, coords[0], coords[1], coords[2], coords[3], coords[4], coords[5], coords[6], coords[7]);
		} break;
		case CommandType::Circle: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 3;
			circle(ctx, coords[0], coords[1], coords[2]);
		} break;
		case CommandType::Ellipse: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 4;
			ellipse(ctx, coords[0], coords[1], coords[2], coords[3]);
		} break;
		case CommandType::FillPathColor: {
			const uint32_t flags = CMD_READ(cmd, uint32_t);
			const Color color = CMD_READ(cmd, Color);

			fillPath(ctx, color, flags);
		} break;
		case CommandType::FillPathGradient: {
			const uint32_t flags = CMD_READ(cmd, uint32_t);
			const uint16_t gradientHandle = CMD_READ(cmd, uint16_t);
			const bool isLocal = CMD_READ(cmd, bool);

			const GradientHandle gradient = { isLocal ? (uint16_t)(gradientHandle + firstGradientID) : gradientHandle };
			fillPath(ctx, gradient, flags);
		} break;
		case CommandType::FillPathImagePattern: {
			const uint32_t flags = CMD_READ(cmd, uint32_t);
			const Color color = CMD_READ(cmd, Color);
			const uint16_t imgPatternHandle = CMD_READ(cmd, uint16_t);
			const bool isLocal = CMD_READ(cmd, bool);

			const ImagePatternHandle imgPattern = { isLocal ? (uint16_t)(imgPatternHandle + firstImagePatternID) : imgPatternHandle };
			fillPath(ctx, imgPattern, color, flags);
		} break;
		case CommandType::StrokePathColor: {
			const float width = CMD_READ(cmd, float);
			const uint32_t flags = CMD_READ(cmd, uint32_t);
			const Color color = CMD_READ(cmd, Color);

			strokePath(ctx, color, width, flags);
		} break;
		case CommandType::StrokePathGradient: {
			const float width = CMD_READ(cmd, float);
			const uint32_t flags = CMD_READ(cmd, uint32_t);
			const uint16_t gradientHandle = CMD_READ(cmd, uint16_t);
			const bool isLocal = CMD_READ(cmd, bool);

			const GradientHandle gradient = { isLocal ? (uint16_t)(gradientHandle + firstGradientID) : gradientHandle };
			strokePath(ctx, gradient, width, flags);
		} break;
		case CommandType::StrokePathImagePattern: {
			const float width = CMD_READ(cmd, float);
			const uint32_t flags = CMD_READ(cmd, uint32_t);
			const Color color = CMD_READ(cmd, Color);
			const uint16_t imgPatternHandle = CMD_READ(cmd, uint16_t);
			const bool isLocal = CMD_READ(cmd, bool);

			const ImagePatternHandle imgPattern = { isLocal ? (uint16_t)(imgPatternHandle + firstImagePatternID) : imgPatternHandle };
			strokePath(ctx, imgPattern, color, width, flags);
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

			indexedTriList(ctx, positions, numUVs ? uv : nullptr, numVertices, colors, numColors, indices, numIndices, { imgHandle });
		} break;
		case CommandType::CreateLinearGradient: {
			const float* params = (float*)cmd;
			cmd += sizeof(float) * 4;
			const Color* colors = (Color*)cmd;
			cmd += sizeof(Color) * 2;
			createLinearGradient(ctx, params[0], params[1], params[2], params[3], colors[0], colors[1]);
		} break;
		case CommandType::CreateBoxGradient: {
			const float* params = (float*)cmd;
			cmd += sizeof(float) * 6;
			const Color* colors = (Color*)cmd;
			cmd += sizeof(Color) * 2;
			createBoxGradient(ctx, params[0], params[1], params[2], params[3], params[4], params[5], colors[0], colors[1]);
		} break;
		case CommandType::CreateRadialGradient: {
			const float* params = (float*)cmd;
			cmd += sizeof(float) * 4;
			const Color* colors = (Color*)cmd;
			cmd += sizeof(Color) * 2;
			createRadialGradient(ctx, params[0], params[1], params[2], params[3], colors[0], colors[1]);
		} break;
		case CommandType::CreateImagePattern: {
			const float* params = (float*)cmd;
			cmd += sizeof(float) * 5;
			const ImageHandle img = CMD_READ(cmd, ImageHandle);
			createImagePattern(ctx, params[0], params[1], params[2], params[3], params[4], img);
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
			text(ctx, *txtCfg, coords[0], coords[1], str, end);
		} break;
		case CommandType::TextBox: {
			const TextConfig* txtCfg = (TextConfig*)cmd;
			cmd += sizeof(TextConfig);
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 3; // x, y, breakWidth
			const uint32_t stringOffset = CMD_READ(cmd, uint32_t);
			const uint32_t stringLen = CMD_READ(cmd, uint32_t);
			VG_CHECK(stringOffset < cl->m_StringBufferPos, "Invalid string offset");
			VG_CHECK(stringOffset + stringLen <= cl->m_StringBufferPos, "Invalid string length");

			const char* str = stringBuffer + stringOffset;
			const char* end = str + stringLen;
			textBox(ctx, *txtCfg, coords[0], coords[1], coords[2], str, end);
		} break;
		case CommandType::ResetScissor: {
			resetScissor(ctx);
			skipCmds = false;
		} break;
		case CommandType::SetScissor: {
			const float* rect = (float*)cmd;
			cmd += sizeof(float) * 4;
			setScissor(ctx, rect[0], rect[1], rect[2], rect[3]);

			if (cullCmds) {
				const State* state = getState(ctx);
				const float* scissorRect = &state->m_ScissorRect[0];
				skipCmds = (scissorRect[2] < 1.0f) || (scissorRect[3] < 1.0f);
			}
		} break;
		case CommandType::IntersectScissor: {
			const float* rect = (float*)cmd;
			cmd += sizeof(float) * 4;
			
			const bool zeroRect = !intersectScissor(ctx, rect[0], rect[1], rect[2], rect[3]);
			if (cullCmds) {
				skipCmds = zeroRect;
			}
		} break;
		case CommandType::PushState: {
			pushState(ctx);
		} break;
		case CommandType::PopState: {
			popState(ctx);
			if (cullCmds) {
				const State* state = getState(ctx);
				const float* scissorRect = &state->m_ScissorRect[0];
				skipCmds = (scissorRect[2] < 1.0f) || (scissorRect[3] < 1.0f);
			}
		} break;
		case CommandType::TransformRotate: {
			const float ang_rad = CMD_READ(cmd, float);
			transformRotate(ctx, ang_rad);
		} break;
		case CommandType::TransformTranslate: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 2;
			transformTranslate(ctx, coords[0], coords[1]);
		} break;
		case CommandType::TransformScale: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 2;
			transformScale(ctx, coords[0], coords[1]);
		} break;
		case CommandType::TransformMult: {
			const float* mtx = (float*)cmd;
			cmd += sizeof(float) * 6;
			const bool pre = CMD_READ(cmd, bool);
			transformMult(ctx, mtx, pre);
		} break;
		case CommandType::BeginClip: {
			const ClipRule::Enum rule = CMD_READ(cmd, ClipRule::Enum);
			beginClip(ctx, rule);
		} break;
		case CommandType::EndClip: {
			endClip(ctx);
		} break;
		case CommandType::ResetClip: {
			resetClip(ctx);
		} break;
		default: {
			VG_CHECK(false, "Unknown command");
		} break;
		}

		VG_CHECK(cmd == cmdEnd, "Incomplete command parsing");
		BX_UNUSED(cmdEnd); // For release builds
	}
	popState(ctx);
	resetClip(ctx);

#if VG_CONFIG_ENABLE_SHAPE_CACHING
	bindCommandListCache(ctx, nullptr);
#endif
}

//////////////////////////////////////////////////////////////////////////
// Internal
//
static void getWhitePixelUV(Context* ctx, uv_t* uv)
{
	uint16_t w, h;
	getImageSize(ctx, ctx->m_FontImages[0], &w, &h);

#if VG_CONFIG_UV_INT16
	uv[0] = INT16_MAX / (int16_t)w;
	uv[1] = INT16_MAX / (int16_t)h;
#else
	uv[0] = 0.5f / (float)w;
	uv[1] = 0.5f / (float)h;
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
	state->m_FontScale = bx::min<float>(quantScale, VG_CONFIG_MAX_FONT_SCALE);
}

float* allocTransformedVertices(Context* ctx, uint32_t numVertices)
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
	DrawCommand* cmd = allocDrawCommand_Textured(ctx, numVertices, numIndices, fontImg);

	// Vertex buffer
	VertexBuffer* vb = &ctx->m_VertexBuffers[cmd->m_VertexBufferID];
	const uint32_t vbOffset = cmd->m_FirstVertexID + cmd->m_NumVertices;

	float* dstPos = &vb->m_Pos[vbOffset << 1];
	bx::memCopy(dstPos, vtx, sizeof(float) * 2 * numVertices);

	uv_t uv[2];
	getWhitePixelUV(ctx, &uv[0]);

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
	DrawCommand* cmd = allocDrawCommand_ImagePattern(ctx, numVertices, numIndices, imgPatternHandle);

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
	DrawCommand* cmd = allocDrawCommand_ColorGradient(ctx, numVertices, numIndices, gradientHandle);

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
	DrawCommand* cmd = allocDrawCommand_Clip(ctx, numVertices, numIndices);

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

static DrawCommand* allocDrawCommand(Context* ctx)
{
	if (ctx->m_NumDrawCommands + 1 >= ctx->m_DrawCommandCapacity) {
		ctx->m_DrawCommandCapacity = ctx->m_DrawCommandCapacity + 32;
		ctx->m_DrawCommands = (DrawCommand*)BX_REALLOC(ctx->m_Allocator, ctx->m_DrawCommands, sizeof(DrawCommand) * ctx->m_DrawCommandCapacity);
	}

	DrawCommand* cmd = &ctx->m_DrawCommands[ctx->m_NumDrawCommands];
	ctx->m_NumDrawCommands++;
	return cmd;
}

static DrawCommand* allocClipCommand(Context* ctx)
{
	if (ctx->m_NumClipCommands + 1 >= ctx->m_ClipCommandCapacity) {
		ctx->m_ClipCommandCapacity = ctx->m_ClipCommandCapacity + 32;
		ctx->m_ClipCommands = (DrawCommand*)BX_REALLOC(ctx->m_Allocator, ctx->m_ClipCommands, sizeof(DrawCommand) * ctx->m_ClipCommandCapacity);
	}

	DrawCommand* cmd = &ctx->m_ClipCommands[ctx->m_NumClipCommands];
	ctx->m_NumClipCommands++;
	return cmd;
}

static DrawCommand* allocDrawCommand_Textured(Context* ctx, uint32_t numVertices, uint32_t numIndices, ImageHandle imgHandle)
{
	VG_CHECK(isValid(imgHandle), "Invalid image handle");

	uint32_t vertexBufferID;
	const uint32_t firstVertexID = allocVertices(ctx, numVertices, &vertexBufferID);
	const uint32_t firstIndexID = allocIndices(ctx, numIndices);

	const State* state = getState(ctx);
	const float* scissor = state->m_ScissorRect;

	if (!ctx->m_ForceNewDrawCommand && ctx->m_NumDrawCommands != 0) {
		DrawCommand* prevCmd = &ctx->m_DrawCommands[ctx->m_NumDrawCommands - 1];

		VG_CHECK(prevCmd->m_VertexBufferID == vertexBufferID, "Cannot merge draw commands with different vertex buffers");
		VG_CHECK(prevCmd->m_ScissorRect[0] == (uint16_t)scissor[0] && 
		         prevCmd->m_ScissorRect[1] == (uint16_t)scissor[1] && 
		         prevCmd->m_ScissorRect[2] == (uint16_t)scissor[2] && 
		         prevCmd->m_ScissorRect[3] == (uint16_t)scissor[3], "Invalid scissor rect");

		if (prevCmd->m_Type == DrawCommand::Type::Textured &&
			prevCmd->m_HandleID == imgHandle.idx) 
		{
			return prevCmd;
		}
	}

	// The new draw command cannot be combined with the previous one. Create a new one.
	DrawCommand* cmd = allocDrawCommand(ctx);
	cmd->m_VertexBufferID = vertexBufferID;
	cmd->m_FirstVertexID = firstVertexID;
	cmd->m_FirstIndexID = firstIndexID;
	cmd->m_NumVertices = 0;
	cmd->m_NumIndices = 0;
	cmd->m_Type = DrawCommand::Type::Textured;
	cmd->m_HandleID = imgHandle.idx;
	cmd->m_ScissorRect[0] = (uint16_t)scissor[0];
	cmd->m_ScissorRect[1] = (uint16_t)scissor[1];
	cmd->m_ScissorRect[2] = (uint16_t)scissor[2];
	cmd->m_ScissorRect[3] = (uint16_t)scissor[3];
	bx::memCopy(&cmd->m_ClipState, &ctx->m_ClipState, sizeof(ClipState));

	ctx->m_ForceNewDrawCommand = false;

	return cmd;
}

static DrawCommand* allocDrawCommand_ImagePattern(Context* ctx, uint32_t numVertices, uint32_t numIndices, ImagePatternHandle handle)
{
	VG_CHECK(isValid(handle), "Invalid image pattern handle");

	uint32_t vertexBufferID;
	const uint32_t firstVertexID = allocVertices(ctx, numVertices, &vertexBufferID);
	const uint32_t firstIndexID = allocIndices(ctx, numIndices);

	const State* state = getState(ctx);
	const float* scissor = state->m_ScissorRect;

	if (!ctx->m_ForceNewDrawCommand && ctx->m_NumDrawCommands != 0) {
		DrawCommand* prevCmd = &ctx->m_DrawCommands[ctx->m_NumDrawCommands - 1];

		VG_CHECK(prevCmd->m_VertexBufferID == vertexBufferID, "Cannot merge draw commands with different vertex buffers");
		VG_CHECK(prevCmd->m_ScissorRect[0] == (uint16_t)scissor[0] &&
			prevCmd->m_ScissorRect[1] == (uint16_t)scissor[1] &&
			prevCmd->m_ScissorRect[2] == (uint16_t)scissor[2] &&
			prevCmd->m_ScissorRect[3] == (uint16_t)scissor[3], "Invalid scissor rect");

		if (prevCmd->m_Type == DrawCommand::Type::ImagePattern &&
			prevCmd->m_HandleID == handle.idx) {
			return prevCmd;
		}
	}

	// The new draw command cannot be combined with the previous one. Create a new one.
	DrawCommand* cmd = allocDrawCommand(ctx);
	cmd->m_VertexBufferID = vertexBufferID;
	cmd->m_FirstVertexID = firstVertexID;
	cmd->m_FirstIndexID = firstIndexID;
	cmd->m_NumVertices = 0;
	cmd->m_NumIndices = 0;
	cmd->m_Type = DrawCommand::Type::ImagePattern;
	cmd->m_HandleID = handle.idx;
	cmd->m_ScissorRect[0] = (uint16_t)scissor[0];
	cmd->m_ScissorRect[1] = (uint16_t)scissor[1];
	cmd->m_ScissorRect[2] = (uint16_t)scissor[2];
	cmd->m_ScissorRect[3] = (uint16_t)scissor[3];
	bx::memCopy(&cmd->m_ClipState, &ctx->m_ClipState, sizeof(ClipState));

	ctx->m_ForceNewDrawCommand = false;

	return cmd;
}

static DrawCommand* allocDrawCommand_ColorGradient(Context* ctx, uint32_t numVertices, uint32_t numIndices, GradientHandle gradientHandle)
{
	VG_CHECK(isValid(gradientHandle), "Invalid gradient handle");

	uint32_t vertexBufferID;
	const uint32_t firstVertexID = allocVertices(ctx, numVertices, &vertexBufferID);
	const uint32_t firstIndexID = allocIndices(ctx, numIndices);

	const State* state = getState(ctx);
	const float* scissor = state->m_ScissorRect;

	if (!ctx->m_ForceNewDrawCommand && ctx->m_NumDrawCommands != 0) {
		DrawCommand* prevCmd = &ctx->m_DrawCommands[ctx->m_NumDrawCommands - 1];

		VG_CHECK(prevCmd->m_VertexBufferID == vertexBufferID, "Cannot merge draw commands with different vertex buffers");
		VG_CHECK(prevCmd->m_ScissorRect[0] == (uint16_t)scissor[0] && 
		         prevCmd->m_ScissorRect[1] == (uint16_t)scissor[1] &&
		         prevCmd->m_ScissorRect[2] == (uint16_t)scissor[2] && 
		         prevCmd->m_ScissorRect[3] == (uint16_t)scissor[3], "Invalid scissor rect");

		if (prevCmd->m_Type == DrawCommand::Type::ColorGradient &&
			prevCmd->m_HandleID == gradientHandle.idx) 
		{
			return prevCmd;
		}
	}

	// The new draw command cannot be combined with the previous one. Create a new one.
	DrawCommand* cmd = allocDrawCommand(ctx);
	cmd->m_VertexBufferID = vertexBufferID;
	cmd->m_FirstVertexID = firstVertexID;
	cmd->m_FirstIndexID = firstIndexID;
	cmd->m_NumVertices = 0;
	cmd->m_NumIndices = 0;
	cmd->m_Type = DrawCommand::Type::ColorGradient;
	cmd->m_HandleID = gradientHandle.idx;
	cmd->m_ScissorRect[0] = (uint16_t)scissor[0];
	cmd->m_ScissorRect[1] = (uint16_t)scissor[1];
	cmd->m_ScissorRect[2] = (uint16_t)scissor[2];
	cmd->m_ScissorRect[3] = (uint16_t)scissor[3];
	bx::memCopy(&cmd->m_ClipState, &ctx->m_ClipState, sizeof(ClipState));

	ctx->m_ForceNewDrawCommand = false;

	return cmd;
}

static DrawCommand* allocDrawCommand_Clip(Context* ctx, uint32_t numVertices, uint32_t numIndices)
{
	uint32_t vertexBufferID;
	const uint32_t firstVertexID = allocVertices(ctx, numVertices, &vertexBufferID);
	const uint32_t firstIndexID = allocIndices(ctx, numIndices);
	
	const State* state = getState(ctx);
	const float* scissor = state->m_ScissorRect;

	if (!ctx->m_ForceNewClipCommand && ctx->m_NumClipCommands != 0) {
		DrawCommand* prevCmd = &ctx->m_ClipCommands[ctx->m_NumClipCommands - 1];

		VG_CHECK(prevCmd->m_VertexBufferID == vertexBufferID, "Cannot merge clip commands with different vertex buffers");
		VG_CHECK(prevCmd->m_ScissorRect[0] == (uint16_t)scissor[0] && 
		         prevCmd->m_ScissorRect[1] == (uint16_t)scissor[1] && 
		         prevCmd->m_ScissorRect[2] == (uint16_t)scissor[2] && 
		         prevCmd->m_ScissorRect[3] == (uint16_t)scissor[3], "Invalid scissor rect");
		VG_CHECK(prevCmd->m_Type == DrawCommand::Type::Clip, "Invalid draw command type");

		return prevCmd;
	}

	// The new clip command cannot be combined with the previous one. Create a new one.
	DrawCommand* cmd = allocClipCommand(ctx);
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

	DrawCommand* cmd = allocDrawCommand_Textured(ctx, numDrawVertices, numDrawIndices, ctx->m_FontImages[ctx->m_FontImageID]);

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

	ImageHandle fontImage = ctx->m_FontImages[ctx->m_FontImageID];

	// Update texture
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
	CommandListHandle handle = { ctx->m_CommandListHandleAlloc->alloc() };
	if (!isValid(handle)) {
		return VG_INVALID_HANDLE;
	}

	if (handle.idx >= ctx->m_CommandListCapacity) {
		const uint32_t oldCapacity = ctx->m_CommandListCapacity;

		ctx->m_CommandListCapacity += 8;
		ctx->m_CommandLists = (CommandList*)BX_REALLOC(ctx->m_Allocator, ctx->m_CommandLists, sizeof(CommandList) * ctx->m_CommandListCapacity);
		if (!ctx->m_CommandLists) {
			VG_WARN(false, "Failed to allocator command list memory");
			return VG_INVALID_HANDLE;
		}
	}

	VG_CHECK(handle.idx < ctx->m_CommandListCapacity, "Allocated invalid command list handle");
	CommandList* cl = &ctx->m_CommandLists[handle.idx];
	bx::memSet(cl, 0, sizeof(CommandList));

	return handle;
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
	const uint32_t totalSize = dataSize + sizeof(CommandType::Enum) + sizeof(uint32_t);
	const uint32_t pos = cl->m_CommandBufferPos;
	if (pos + totalSize > cl->m_CommandBufferCapacity) {
		cl->m_CommandBufferCapacity += bx::max<uint32_t>(totalSize, 256);
		cl->m_CommandBuffer = (uint8_t*)BX_REALLOC(ctx->m_Allocator, cl->m_CommandBuffer, cl->m_CommandBufferCapacity);
	}

	uint8_t* ptr = &cl->m_CommandBuffer[pos];
	cl->m_CommandBufferPos += totalSize;

	const CommandHeader header = { cmdType, dataSize };
	bx::memCopy(ptr, &header, sizeof(CommandHeader));
	ptr += sizeof(CommandHeader);

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

static void bindCommandListCache(Context* ctx, CommandListCache* cache)
{
	ctx->m_CommandListCache = cache;
}

static void beginCachedCommand(Context* ctx)
{
	CommandListCache* cache = ctx->m_CommandListCache;
	VG_CHECK(cache != nullptr, "No bound CommandListCache");

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
	CommandListCache* cache = ctx->m_CommandListCache;
	VG_CHECK(cache != nullptr, "No bound CommandListCache");
	VG_CHECK(cache->m_NumCommands != 0, "beginCachedCommand() hasn't been called");

	CachedCommand* lastCmd = &cache->m_Commands[cache->m_NumCommands - 1];
	VG_CHECK(lastCmd->m_NumMeshes == 0, "endCachedCommand() called too many times");
	lastCmd->m_NumMeshes = (uint16_t)(cache->m_NumMeshes - lastCmd->m_FirstMeshID);
}

static void addCachedCommand(Context* ctx, const float* pos, uint32_t numVertices, const uint32_t* colors, uint32_t numColors, const uint16_t* indices, uint32_t numIndices)
{
	CommandListCache* cache = ctx->m_CommandListCache;
	VG_CHECK(cache != nullptr, "No bound CommandListCache");

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
	
	bx::memCopy(mesh->m_Pos, pos, sizeof(float) * 2 * numVertices);
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

	uint8_t* cmd = cl->m_CommandBuffer;
	const uint8_t* cmdListEnd = cl->m_CommandBuffer + cl->m_CommandBufferPos;
	if (cmd == cmdListEnd) {
		return;
	}

	const char* stringBuffer = cl->m_StringBuffer;
	CachedCommand* nextCachedCommand = &clCache->m_Commands[0];

	bool skipCmds = false;
	pushState(ctx);
	while (cmd < cmdListEnd) {
		const CommandHeader* cmdHeader = (CommandHeader*)cmd;
		cmd += sizeof(CommandHeader);

		// Skip path commands.
		if (cmdHeader->m_Type >= CommandType::FirstPathCommand && cmdHeader->m_Type <= CommandType::LastPathCommand) {
			cmd += cmdHeader->m_Size;
			continue;
		}

		if (skipCmds && cmdHeader->m_Type >= CommandType::FirstStrokerCommand && cmdHeader->m_Type <= CommandType::LastStrokerCommand) {
			cmd += cmdHeader->m_Size;
			++nextCachedCommand;
			continue;
		}

		const uint8_t* cmdEnd = cmd + cmdHeader->m_Size;

		switch (cmdHeader->m_Type) {
		case CommandType::FillPathColor: {
			const uint32_t flags = CMD_READ(cmd, uint32_t);
			const Color color = CMD_READ(cmd, Color);
			submitCachedMesh(ctx, color, nextCachedCommand->m_InvTransformMtx, &clCache->m_Meshes[nextCachedCommand->m_FirstMeshID], nextCachedCommand->m_NumMeshes);
			++nextCachedCommand;
		} break;
		case CommandType::FillPathGradient: {
			const uint32_t flags = CMD_READ(cmd, uint32_t);
			const uint16_t gradientHandle = CMD_READ(cmd, uint16_t);
			const bool isLocal = CMD_READ(cmd, bool);

			const GradientHandle gradient = { isLocal ? (uint16_t)(gradientHandle + firstGradientID) : gradientHandle };
			submitCachedMesh(ctx, gradient, nextCachedCommand->m_InvTransformMtx, &clCache->m_Meshes[nextCachedCommand->m_FirstMeshID], nextCachedCommand->m_NumMeshes);
			++nextCachedCommand;
		} break;
		case CommandType::FillPathImagePattern: {
			const uint32_t flags = CMD_READ(cmd, uint32_t);
			const Color color = CMD_READ(cmd, Color);
			const uint16_t imgPatternHandle = CMD_READ(cmd, uint16_t);
			const bool isLocal = CMD_READ(cmd, bool);

			const ImagePatternHandle imgPattern = { isLocal ? (uint16_t)(imgPatternHandle + firstImagePatternID) : imgPatternHandle };
			submitCachedMesh(ctx, imgPattern, color, nextCachedCommand->m_InvTransformMtx, &clCache->m_Meshes[nextCachedCommand->m_FirstMeshID], nextCachedCommand->m_NumMeshes);
			++nextCachedCommand;
		} break;
		case CommandType::StrokePathColor: {
			const float width = CMD_READ(cmd, float);
			const uint32_t flags = CMD_READ(cmd, uint32_t);
			const Color color = CMD_READ(cmd, Color);
			submitCachedMesh(ctx, color, nextCachedCommand->m_InvTransformMtx, &clCache->m_Meshes[nextCachedCommand->m_FirstMeshID], nextCachedCommand->m_NumMeshes);
			++nextCachedCommand;
		} break;
		case CommandType::StrokePathGradient: {
			const float width = CMD_READ(cmd, float);
			const uint32_t flags = CMD_READ(cmd, uint32_t);
			const uint16_t gradientHandle = CMD_READ(cmd, uint16_t);
			const bool isLocal = CMD_READ(cmd, bool);

			const GradientHandle gradient = { isLocal ? (uint16_t)(gradientHandle + firstGradientID) : gradientHandle };
			submitCachedMesh(ctx, gradient, nextCachedCommand->m_InvTransformMtx, &clCache->m_Meshes[nextCachedCommand->m_FirstMeshID], nextCachedCommand->m_NumMeshes);
			++nextCachedCommand;
		} break;
		case CommandType::StrokePathImagePattern: {
			const float width = CMD_READ(cmd, float);
			const uint32_t flags = CMD_READ(cmd, uint32_t);
			const Color color = CMD_READ(cmd, Color);
			const uint16_t imgPatternHandle = CMD_READ(cmd, uint16_t);
			const bool isLocal = CMD_READ(cmd, bool);

			const ImagePatternHandle imgPattern = { isLocal ? (uint16_t)(imgPatternHandle + firstImagePatternID) : imgPatternHandle };
			submitCachedMesh(ctx, imgPattern, color, nextCachedCommand->m_InvTransformMtx, &clCache->m_Meshes[nextCachedCommand->m_FirstMeshID], nextCachedCommand->m_NumMeshes);
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

			indexedTriList(ctx, positions, numUVs ? uv : nullptr, numVertices, colors, numColors, indices, numIndices, { imgHandle });
		} break;
		case CommandType::CreateLinearGradient: {
			const float* params = (float*)cmd;
			cmd += sizeof(float) * 4;
			const Color* colors = (Color*)cmd;
			cmd += sizeof(Color) * 2;
			createLinearGradient(ctx, params[0], params[1], params[2], params[3], colors[0], colors[1]);
		} break;
		case CommandType::CreateBoxGradient: {
			const float* params = (float*)cmd;
			cmd += sizeof(float) * 6;
			const Color* colors = (Color*)cmd;
			cmd += sizeof(Color) * 2;
			createBoxGradient(ctx, params[0], params[1], params[2], params[3], params[4], params[5], colors[0], colors[1]);
		} break;
		case CommandType::CreateRadialGradient: {
			const float* params = (float*)cmd;
			cmd += sizeof(float) * 4;
			const Color* colors = (Color*)cmd;
			cmd += sizeof(Color) * 2;
			createRadialGradient(ctx, params[0], params[1], params[2], params[3], colors[0], colors[1]);
		} break;
		case CommandType::CreateImagePattern: {
			const float* params = (float*)cmd;
			cmd += sizeof(float) * 5;
			const ImageHandle img = CMD_READ(cmd, ImageHandle);
			createImagePattern(ctx, params[0], params[1], params[2], params[3], params[4], img);
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
			text(ctx, *txtCfg, coords[0], coords[1], str, end);
		} break;
		case CommandType::TextBox: {
			const TextConfig* txtCfg = (TextConfig*)cmd;
			cmd += sizeof(TextConfig);
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 3; // x, y, breakWidth
			const uint32_t stringOffset = CMD_READ(cmd, uint32_t);
			const uint32_t stringLen = CMD_READ(cmd, uint32_t);
			VG_CHECK(stringOffset < cl->m_StringBufferPos, "Invalid string offset");
			VG_CHECK(stringOffset + stringLen <= cl->m_StringBufferPos, "Invalid string length");

			const char* str = stringBuffer + stringOffset;
			const char* end = str + stringLen;
			textBox(ctx, *txtCfg, coords[0], coords[1], coords[2], str, end);
		} break;
		case CommandType::ResetScissor: {
			resetScissor(ctx);
			skipCmds = false;
		} break;
		case CommandType::SetScissor: {
			const float* rect = (float*)cmd;
			cmd += sizeof(float) * 4;
			setScissor(ctx, rect[0], rect[1], rect[2], rect[3]);

			if (cullCmds) {
				skipCmds = (rect[2] < 1.0f) || (rect[3] < 1.0f);
			}
		} break;
		case CommandType::IntersectScissor: {
			const float* rect = (float*)cmd;
			cmd += sizeof(float) * 4;

			const bool zeroRect = !intersectScissor(ctx, rect[0], rect[1], rect[2], rect[3]);
			if (cullCmds) {
				skipCmds = zeroRect;
			}
		} break;
		case CommandType::PushState: {
			pushState(ctx);
		} break;
		case CommandType::PopState: {
			popState(ctx);
			if (cullCmds) {
				const State* state = getState(ctx);
				const float* scissorRect = &state->m_ScissorRect[0];
				skipCmds = (scissorRect[2] < 1.0f) || (scissorRect[3] < 1.0f);
			}
		} break;
		case CommandType::TransformRotate: {
			const float ang_rad = CMD_READ(cmd, float);
			transformRotate(ctx, ang_rad);
		} break;
		case CommandType::TransformTranslate: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 2;
			transformTranslate(ctx, coords[0], coords[1]);
		} break;
		case CommandType::TransformScale: {
			const float* coords = (float*)cmd;
			cmd += sizeof(float) * 2;
			transformScale(ctx, coords[0], coords[1]);
		} break;
		case CommandType::TransformMult: {
			const float* mtx = (float*)cmd;
			cmd += sizeof(float) * 6;
			const bool pre = CMD_READ(cmd, bool);
			transformMult(ctx, mtx, pre);
		} break;
		case CommandType::BeginClip: {
			const ClipRule::Enum rule = CMD_READ(cmd, ClipRule::Enum);
			beginClip(ctx, rule);
		} break;
		case CommandType::EndClip: {
			endClip(ctx);
		} break;
		case CommandType::ResetClip: {
			resetClip(ctx);
		} break;
		default: {
			VG_CHECK(false, "Unknown cached command");
		} break;
		}

		VG_CHECK(cmd == cmdEnd, "Incomplete command parsing");
		BX_UNUSED(cmdEnd); // For release builds
	}
	popState(ctx);
	resetClip(ctx);
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

static void submitCachedMesh(Context* ctx, Color col, const float* invTransform, const CachedMesh* meshList, uint32_t numMeshes)
{
	const bool recordClipCommands = ctx->m_RecordClipCommands;

	const State* state = getState(ctx);
	const float* stateTransform = state->m_TransformMtx;

	float mtx[6];
	vgutil::multiplyMatrix3(stateTransform, invTransform, mtx);

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

static void submitCachedMesh(Context* ctx, GradientHandle gradientHandle, const float* invTransform, const CachedMesh* meshList, uint32_t numMeshes)
{
	VG_CHECK(!ctx->m_RecordClipCommands, "Only submitCachedMesh(Color) is supported inside BeginClip()/EndClip()");
	VG_CHECK(isValid(gradientHandle), "Invalid gradient handle");

	const State* state = getState(ctx);
	const float* stateTransform = state->m_TransformMtx;

	float mtx[6];
	vgutil::multiplyMatrix3(stateTransform, invTransform, mtx);

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

static void submitCachedMesh(Context* ctx, ImagePatternHandle imgPattern, Color col, const float* invTransform, const CachedMesh* meshList, uint32_t numMeshes)
{
	VG_CHECK(!ctx->m_RecordClipCommands, "Only submitCachedMesh(Color) is supported inside BeginClip()/EndClip()");
	VG_CHECK(isValid(imgPattern), "Invalid gradient handle");

	const State* state = getState(ctx);
	const float* stateTransform = state->m_TransformMtx;

	float mtx[6];
	vgutil::multiplyMatrix3(stateTransform, invTransform, mtx);

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
