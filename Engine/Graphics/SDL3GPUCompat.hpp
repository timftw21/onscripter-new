/**
 *  SDL3GPUCompat.hpp
 *  ONScripter-RU
 *
 *  SDL2_gpu-shaped compatibility surface for the SDL3_GPU renderer path.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#pragma once

#if defined(ONS_USE_SDL3)

#include "Support/SDLCompat.hpp"

#include <SDL3/SDL_gpu.h>

#include <vector>

#ifndef GPU_FALSE
#define GPU_FALSE false
#endif

#ifndef GPU_TRUE
#define GPU_TRUE true
#endif

using GPU_bool = bool;
using GPU_RendererEnum = Uint32;
using GPU_WindowFlagEnum = SDL_WindowFlags;
using GPU_InitFlagEnum = Uint32;
using GPU_BatchFlagEnum = Uint32;

static const GPU_RendererEnum GPU_RENDERER_UNKNOWN  = 0;
static const GPU_RendererEnum GPU_RENDERER_OPENGL_2 = 3;
static const GPU_RendererEnum GPU_RENDERER_GLES_2   = 12;
static const GPU_RendererEnum GPU_RENDERER_GLES_3   = 13;
static const GPU_RendererEnum GPU_RENDERER_SDL3_GPU = 1000;

static const GPU_InitFlagEnum GPU_INIT_ENABLE_VSYNC                         = 0x1;
static const GPU_InitFlagEnum GPU_INIT_DISABLE_VSYNC                        = 0x2;
static const GPU_InitFlagEnum GPU_INIT_USE_ROW_BY_ROW_TEXTURE_UPLOAD_FALLBACK = 0x20;
static const GPU_InitFlagEnum GPU_INIT_USE_COPY_TEXTURE_UPLOAD_FALLBACK     = 0x40;

#define GPU_DEFAULT_INIT_FLAGS 0

static const GPU_BatchFlagEnum GPU_BATCH_XY  = 0x1;
static const GPU_BatchFlagEnum GPU_BATCH_XYZ = 0x2;
static const GPU_BatchFlagEnum GPU_BATCH_ST  = 0x4;
#define GPU_BATCH_XY_ST (GPU_BATCH_XY | GPU_BATCH_ST)
#define GPU_BATCH_XYZ_ST (GPU_BATCH_XYZ | GPU_BATCH_ST)

#define GPU_MODELVIEW 0
#define GPU_PROJECTION 1

struct GPU_Renderer;
struct GPU_Target;

typedef struct GPU_Rect {
	float x;
	float y;
	float w;
	float h;
} GPU_Rect;

typedef struct GPU_RendererID {
	const char *name;
	GPU_RendererEnum renderer;
	int major_version;
	int minor_version;
} GPU_RendererID;

typedef enum {
	GPU_FUNC_ZERO                = 0,
	GPU_FUNC_ONE                 = 1,
	GPU_FUNC_SRC_COLOR           = 0x0300,
	GPU_FUNC_DST_COLOR           = 0x0306,
	GPU_FUNC_ONE_MINUS_SRC       = 0x0301,
	GPU_FUNC_ONE_MINUS_DST       = 0x0307,
	GPU_FUNC_SRC_ALPHA           = 0x0302,
	GPU_FUNC_DST_ALPHA           = 0x0304,
	GPU_FUNC_ONE_MINUS_SRC_ALPHA = 0x0303,
	GPU_FUNC_ONE_MINUS_DST_ALPHA = 0x0305
} GPU_BlendFuncEnum;

typedef enum {
	GPU_EQ_ADD              = 0x8006,
	GPU_EQ_SUBTRACT         = 0x800A,
	GPU_EQ_REVERSE_SUBTRACT = 0x800B
} GPU_BlendEqEnum;

typedef struct GPU_BlendMode {
	GPU_BlendFuncEnum source_color;
	GPU_BlendFuncEnum dest_color;
	GPU_BlendFuncEnum source_alpha;
	GPU_BlendFuncEnum dest_alpha;
	GPU_BlendEqEnum color_equation;
	GPU_BlendEqEnum alpha_equation;
} GPU_BlendMode;

typedef enum {
	GPU_BLEND_NORMAL = 0,
	GPU_BLEND_PREMULTIPLIED_ALPHA = 1,
	GPU_BLEND_MULTIPLY = 2,
	GPU_BLEND_ADD = 3,
	GPU_BLEND_SUBTRACT = 4,
	GPU_BLEND_MOD_ALPHA = 5,
	GPU_BLEND_SET_ALPHA = 6,
	GPU_BLEND_SET = 7,
	GPU_BLEND_NORMAL_KEEP_ALPHA = 8,
	GPU_BLEND_NORMAL_ADD_ALPHA = 9,
	GPU_BLEND_NORMAL_FACTOR_ALPHA = 10
} GPU_BlendPresetEnum;

typedef enum {
	GPU_FILTER_NEAREST = 0,
	GPU_FILTER_LINEAR = 1,
	GPU_FILTER_LINEAR_MIPMAP = 2
} GPU_FilterEnum;

typedef enum {
	GPU_SNAP_NONE = 0,
	GPU_SNAP_POSITION = 1,
	GPU_SNAP_DIMENSIONS = 2,
	GPU_SNAP_POSITION_AND_DIMENSIONS = 3
} GPU_SnapEnum;

typedef enum {
	GPU_FORMAT_LUMINANCE = 1,
	GPU_FORMAT_LUMINANCE_ALPHA = 2,
	GPU_FORMAT_RGB = 3,
	GPU_FORMAT_RGBA = 4,
	GPU_FORMAT_ALPHA = 5,
	GPU_FORMAT_RG = 6,
	GPU_FORMAT_YCbCr422 = 7,
	GPU_FORMAT_YCbCr420P = 8,
	GPU_FORMAT_BGR = 9,
	GPU_FORMAT_BGRA = 10,
	GPU_FORMAT_ABGR = 11
} GPU_FormatEnum;

typedef enum {
	GPU_FILE_AUTO = 0,
	GPU_FILE_PNG,
	GPU_FILE_BMP,
	GPU_FILE_TGA
} GPU_FileFormatEnum;

typedef enum {
	GPU_DEBUG_LEVEL_0 = 0,
	GPU_DEBUG_LEVEL_1 = 1,
	GPU_DEBUG_LEVEL_2 = 2,
	GPU_DEBUG_LEVEL_MAX = 3
} GPU_DebugLevelEnum;

typedef enum {
	GPU_VERTEX_SHADER = 0,
	GPU_FRAGMENT_SHADER = 1,
	GPU_PIXEL_SHADER = 1,
	GPU_GEOMETRY_SHADER = 2
} GPU_ShaderEnum;

typedef struct GPU_ShaderBlock {
	int position_loc;
	int texcoord_loc;
	int color_loc;
	int modelViewProjection_loc;
} GPU_ShaderBlock;

typedef struct GPU_Context {
	void *context;
	GPU_bool failed;
	Uint32 windowID;
	int window_w;
	int window_h;
	int drawable_w;
	int drawable_h;
	Uint32 current_shader_program;
	GPU_ShaderBlock current_shader_block;
	GPU_bool shapes_use_blending;
	GPU_BlendMode shapes_blend_mode;
	int matrix_mode;
	int refcount;
	void *data;
} GPU_Context;

typedef struct GPU_Image {
	GPU_Renderer *renderer;
	GPU_Target *context_target;
	GPU_Target *target;
	Uint16 w;
	Uint16 h;
	GPU_bool using_virtual_resolution;
	GPU_FormatEnum format;
	int num_layers;
	int bytes_per_pixel;
	Uint16 base_w;
	Uint16 base_h;
	Uint16 texture_w;
	Uint16 texture_h;
	GPU_bool has_mipmaps;
	float anchor_x;
	float anchor_y;
	SDL_Color color;
	GPU_bool use_blending;
	GPU_BlendMode blend_mode;
	GPU_FilterEnum filter_mode;
	GPU_SnapEnum snap_mode;
	Uint32 mip_level_count;
	void *data;
	int refcount;
	GPU_bool is_alias;
	SDL_GPUTexture *texture;
	std::vector<Uint8> pixels;
	int pitch;
	GPU_bool pixels_dirty;
	GPU_bool pixels_solid;
	SDL_Color solid_color;
	GPU_bool texture_initialized;
} GPU_Image;

struct GPU_Target {
	GPU_Renderer *renderer;
	GPU_Target *context_target;
	GPU_Image *image;
	void *data;
	Uint16 w;
	Uint16 h;
	GPU_bool using_virtual_resolution;
	Uint16 base_w;
	Uint16 base_h;
	GPU_bool use_clip_rect;
	GPU_Rect clip_rect;
	GPU_bool use_color;
	SDL_Color color;
	GPU_Rect viewport;
	GPU_Context *context;
	int refcount;
	GPU_bool is_alias;
	SDL_GPUTexture *texture;
	GPU_bool is_window;
};

struct GPU_Renderer {
	GPU_RendererID id;
	GPU_Target *current_context_target;
	SDL_GPUDevice *device;
	SDL_Window *window;
	GPU_InitFlagEnum preinit_flags;
	GPU_DebugLevelEnum debug_level;
	Uint32 enabled_features;
	SDL_GPUTextureFormat swapchain_format;
	SDL_GPUPresentMode present_mode;
};

struct GPU_TriangleBatchVertex {
	float x;
	float y;
	float r;
	float g;
	float b;
	float a;
	float s;
	float t;
};

GPU_RendererID SDLCALL GPU_MakeRendererID(const char *name, GPU_RendererEnum renderer, int major_version, int minor_version);
void SDLCALL GPU_SetPreInitFlags(GPU_InitFlagEnum GPU_flags);
GPU_Target *SDLCALL GPU_InitRendererByID(GPU_RendererID renderer_request, Uint16 w, Uint16 h, GPU_WindowFlagEnum SDL_flags);
void SDLCALL GPU_PrintTelemetry(void);
void SDLCALL GPU_Quit(void);
void SDLCALL GPU_SetDebugLevel(GPU_DebugLevelEnum level);
GPU_Renderer *SDLCALL GPU_GetCurrentRenderer(void);
GPU_Target *SDLCALL GPU_GetContextTarget(void);
GPU_bool SDLCALL GPU_SetWindowResolution(Uint16 w, Uint16 h);
void SDLCALL GPU_SetShapeBlending(GPU_bool enable);
GPU_Target *SDLCALL GPU_GetTarget(GPU_Image *image);
void SDLCALL GPU_SetVirtualResolution(GPU_Target *target, Uint16 w, Uint16 h);
GPU_Rect SDLCALL GPU_SetClipRect(GPU_Target *target, GPU_Rect rect);
void SDLCALL GPU_UnsetClip(GPU_Target *target);
GPU_Image *SDLCALL GPU_CreateImage(Uint16 w, Uint16 h, GPU_FormatEnum format);
GPU_Image *SDLCALL GPU_CopyImage(GPU_Image *image);
void SDLCALL GPU_FreeImage(GPU_Image *image);
void SDLCALL GPU_UpdateImage(GPU_Image *image, const GPU_Rect *image_rect, SDL_Surface *surface, const GPU_Rect *surface_rect);
void SDLCALL GPU_UpdateImageBytes(GPU_Image *image, const GPU_Rect *image_rect, const unsigned char *bytes, int bytes_per_row);
GPU_bool SDLCALL GPU_SaveImage(GPU_Image *image, const char *filename, GPU_FileFormatEnum format);
GPU_bool SDLCALL GPU_SaveImage_RW(GPU_Image *image, SDL_RWops *rwops, GPU_bool free_rwops, GPU_FileFormatEnum format);
void SDLCALL GPU_GenerateMipmaps(GPU_Image *image);
void SDLCALL GPU_SetRGBA(GPU_Image *image, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
void SDLCALL GPU_SetBlending(GPU_Image *image, GPU_bool enable);
void SDLCALL GPU_SetBlendMode(GPU_Image *image, GPU_BlendPresetEnum mode);
void SDLCALL GPU_SetImageFilter(GPU_Image *image, GPU_FilterEnum filter);
void SDLCALL GPU_SetSnapMode(GPU_Image *image, GPU_SnapEnum mode);
GPU_Image *SDLCALL GPU_CopyImageFromSurface(SDL_Surface *surface);
GPU_Image *SDLCALL GPU_CopyImageFromTarget(GPU_Target *target);
SDL_Surface *SDLCALL GPU_CopySurfaceFromImage(GPU_Image *image);
void SDLCALL GPU_MatrixMode(int matrix_mode);
void SDLCALL GPU_PushMatrix(void);
void SDLCALL GPU_PopMatrix(void);
void SDLCALL GPU_LoadIdentity(void);
void SDLCALL GPU_Frustum(float left, float right, float bottom, float top, float z_near, float z_far);
void SDLCALL GPU_ClearRGBA(GPU_Target *target, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
void SDLCALL GPU_Blit(GPU_Image *image, GPU_Rect *src_rect, GPU_Target *target, float x, float y);
void SDLCALL GPU_BlitRotate(GPU_Image *image, GPU_Rect *src_rect, GPU_Target *target, float x, float y, float degrees);
void SDLCALL GPU_BlitScale(GPU_Image *image, GPU_Rect *src_rect, GPU_Target *target, float x, float y, float scaleX, float scaleY);
void SDLCALL GPU_BlitTransform(GPU_Image *image, GPU_Rect *src_rect, GPU_Target *target, float x, float y, float degrees, float scaleX, float scaleY);
void SDLCALL GPU_TriangleBatch(GPU_Image *image, GPU_Target *target, unsigned short num_vertices, float *values, unsigned int num_indices, unsigned short *indices, GPU_BatchFlagEnum flags);
void SDLCALL GPU_TriangleBatchRGBA(GPU_Image *image, GPU_Target *target, unsigned short num_vertices, const GPU_TriangleBatchVertex *vertices, unsigned int num_indices, const unsigned short *indices);
void SDLCALL GPU_FlushBlitBuffer(void);
void SDLCALL GPU_Flip(GPU_Target *target);

#if defined(DROID)
/**
 * Suspends presentation while the Android surface is gone.
 *
 * Android destroys the native window when the app is backgrounded. SDL only
 * waits for the render thread to finish on OpenGL windows, so with SDL_GPU on
 * Vulkan the surface can vanish mid-frame and SDL_AcquireGPUSwapchainTexture
 * then tries to rebuild it from a released ANativeWindow, which segfaults
 * inside libvulkan.
 *
 * Set from an SDL event watch, which SDL documents as the place to do lifecycle
 * handling: the background event is delivered to watches as it is queued, and
 * the app blocks immediately afterwards, so an app that only reads its own
 * event queue never sees it in time.
 */
void GPU_SetPresentationSuspended(bool suspended);
// True once per detected mismatch between canvas and swapchain aspect.
bool GPU_TakeSurfaceGeometryStale();

#endif

// Live GPU image accounting. textureBytes is what the driver holds, pixelBytes
// the CPU-side copies the engine keeps alongside them. Not Android-specific --
// the definitions never were, and the performance counter reports them on every
// platform.
void GPU_GetLiveImageMemory(size_t &images, size_t &textureBytes, size_t &pixelBytes);
void GPU_LogLargestLiveImages(size_t count);
void SDLCALL GPU_RectangleFilled2(GPU_Target *target, GPU_Rect rect, SDL_Color color);
Uint32 SDLCALL GPU_CompileShader_RW(GPU_ShaderEnum shader_type, SDL_RWops *shader_source, GPU_bool free_rwops);
Uint32 SDLCALL GPU_LinkShaders(Uint32 shader_object1, Uint32 shader_object2);
Uint32 SDLCALL GPU_LinkManyShaders(Uint32 *shader_objects, int count);
GPU_bool SDLCALL GPU_LinkShaderProgram(Uint32 program_object);
void SDLCALL GPU_ActivateShaderProgram(Uint32 program_object, GPU_ShaderBlock *block);
void SDLCALL GPU_DeactivateShaderProgram(void);
const char *SDLCALL GPU_GetShaderMessage(void);
int SDLCALL GPU_GetUniformLocation(Uint32 program_object, const char *uniform_name);
GPU_ShaderBlock SDLCALL GPU_LoadShaderBlock(Uint32 program_object, const char *position_name, const char *texcoord_name, const char *color_name, const char *modelViewMatrix_name);
void SDLCALL GPU_SetShaderImage(GPU_Image *image, int location, int image_unit);
void SDLCALL GPU_SetUniformi(int location, int value);
void SDLCALL GPU_SetUniformf(int location, float value);
void SDLCALL GPU_SetUniformfv(int location, int num_elements_per_value, int num_values, float *values);
GPU_bool SDLCALL GPU_MultiplyAlpha(GPU_Image *image, const GPU_Rect *dst_clip);
void SDLCALL GPU_DiscardImagePixels(GPU_Image *image);
int SDLCALL GPU_RunSDL3Benchmark(int iterations, int width, int height, const char *outputPath);
int SDLCALL GPU_RunMusicBoxBenchmark(int iterations, int width, int height, const char *outputPath);
void SDLCALL GPU_PushTelemetryScope(const char *source);
void SDLCALL GPU_PopTelemetryScope(void);

class GPU_TelemetryScope {
public:
	explicit GPU_TelemetryScope(const char *source) {
		GPU_PushTelemetryScope(source);
	}

	~GPU_TelemetryScope() {
		GPU_PopTelemetryScope();
	}

	GPU_TelemetryScope(const GPU_TelemetryScope &) = delete;
	GPU_TelemetryScope &operator=(const GPU_TelemetryScope &) = delete;
};

#endif
