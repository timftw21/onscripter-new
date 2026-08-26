/**
 *  SDL3GPUCompat.cpp
 *  ONScripter-RU
 *
 *  SDL2_gpu-shaped compatibility surface for the SDL3_GPU renderer path.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#include <atomic>
#include "Engine/Graphics/SDL3GPUCompat.hpp"

#if defined(ONS_USE_SDL3)

#include "Engine/Graphics/GPU.hpp"
#include "Engine/Graphics/SDL3GPUShaders/SDL3GPUShaders.hpp"
#include "Support/FileDefs.hpp"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <png.h>

#if defined(ONS_USE_SDL3_SHADERCROSS)
#include <SDL3_shadercross/SDL_shadercross.h>
#if defined(ONS_USE_SDL3_SHADERC)
#include <shaderc/shaderc.h>
#endif
#endif

namespace {
GPU_Renderer rendererState{};
GPU_InitFlagEnum pendingPreinitFlags{GPU_DEFAULT_INIT_FLAGS};
char shaderMessage[256]{"SDL3_GPU compatibility layer is active"};
Uint32 nextShaderObject{1};

using SDL3GPUVertex = GPU_TriangleBatchVertex;

struct SDL3GPUShaderBytecode {
	const Uint8 *code{nullptr};
	size_t size{0};
	SDL_GPUShaderFormat format{SDL_GPU_SHADERFORMAT_INVALID};
	const char *entrypoint{"main"};
};

struct SDL3GPUPipelineEntry {
	SDL_GPUTextureFormat targetFormat{SDL_GPU_TEXTUREFORMAT_INVALID};
	GPU_bool useBlending{false};
	GPU_BlendMode blendMode{};
	SDL_GPUShader *vertexShader{nullptr};
	SDL_GPUShader *fragmentShader{nullptr};
	SDL_GPUGraphicsPipeline *pipeline{nullptr};
};

enum class SDL3GPUShaderKind {
	Unknown,
	DefaultVertex,
	AlphaOutsideTextures,
	BlendByMask,
	BlurH,
	BlurV,
	Breakup,
	ColorModification,
	ColourConversion,
	CropByMask,
	EffectTrvswave,
	EffectWarp,
	EffectWhirl,
	GlassSmash,
	GlyphGradient,
	MergeAlpha,
	MultiplyAlpha,
	Pixelate,
	RenderSubtitles,
	TextFade,
	Count
};

enum class SDL3GPUUniformType {
	Int,
	Bool,
	Float,
	FloatVec
};

struct SDL3GPUNativeResourceInfo {
	Uint32 numSamplers{0};
	Uint32 numStorageTextures{0};
	Uint32 numStorageBuffers{0};
	Uint32 numUniformBuffers{0};
};

struct SDL3GPUNativeUniform {
	std::string name;
	SDL3GPUUniformType type{SDL3GPUUniformType::Float};
	int components{1};
	int arraySize{1};
	Uint32 registerIndex{0};
};

struct SDL3GPUNativeUniformRegister {
	Uint32 words[4]{0, 0, 0, 0};
};

struct SDL3GPUShaderObject {
	GPU_ShaderEnum type{GPU_FRAGMENT_SHADER};
	SDL3GPUShaderKind kind{SDL3GPUShaderKind::Unknown};
	std::string source;
	SDL_GPUShader *nativeShader{nullptr};
	SDL3GPUNativeResourceInfo nativeResources{};
	std::vector<SDL3GPUNativeUniform> nativeUniforms;
	std::array<int, 8> nativeSamplerImageUnits{{-1, -1, -1, -1, -1, -1, -1, -1}};
	bool translatedLegacyGLSL{false};
};

struct SDL3GPUUniformValue {
	SDL3GPUUniformType type{SDL3GPUUniformType::Float};
	int intValue{0};
	float values[4]{0.0f, 0.0f, 0.0f, 0.0f};
	int components{1};
};

struct SDL3GPUProgramObject {
	SDL3GPUShaderKind kind{SDL3GPUShaderKind::Unknown};
	std::vector<Uint32> shaders;
	std::unordered_map<std::string, int> uniformLocations;
	std::unordered_map<int, std::string> locationNames;
	std::unordered_map<std::string, SDL3GPUUniformValue> uniforms;
	std::array<GPU_Image *, 8> images{};
	SDL_GPUShader *nativeVertexShader{nullptr};
	SDL_GPUShader *nativeFragmentShader{nullptr};
	SDL3GPUNativeResourceInfo nativeFragmentResources{};
	std::vector<SDL3GPUNativeUniform> nativeUniforms;
	std::unordered_map<std::string, size_t> nativeUniformLookup;
	std::vector<SDL3GPUNativeUniformRegister> nativeUniformRegisters;
	std::array<int, 8> nativeSamplerImageUnits{{-1, -1, -1, -1, -1, -1, -1, -1}};
};

struct SDL3GPUColorF {
	float r{0.0f};
	float g{0.0f};
	float b{0.0f};
	float a{0.0f};
};

struct SDL3GPUReusableTransferBuffer {
	SDL_GPUTransferBuffer *transfer{nullptr};
	Uint32 capacity{0};
	SDL_GPUTransferBufferUsage usage{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD};
};

struct SDL3GPUReusableUploadedBuffer {
	SDL_GPUBuffer *buffer{nullptr};
	SDL_GPUTransferBuffer *transfer{nullptr};
	Uint32 capacity{0};
	Uint32 size{0};
	SDL_GPUBufferUsageFlags usage{0};
};

struct SDL3GPUSamplerSet {
	std::array<SDL_GPUTexture *, 8> textures{};
	std::array<SDL_GPUSampler *, 8> samplers{};
	size_t count{0};

	void clear() {
		textures.fill(nullptr);
		samplers.fill(nullptr);
		count = 0;
	}

	bool push(SDL_GPUTexture *texture, SDL_GPUSampler *sampler) {
		if (count >= textures.size())
			return false;
		textures[count] = texture;
		samplers[count] = sampler;
		++count;
		return true;
	}
};

bool samplerSetsEqual(const SDL3GPUSamplerSet &a, const SDL3GPUSamplerSet &b) {
	if (a.count != b.count)
		return false;
	for (size_t i = 0; i < a.count; ++i) {
		if (a.textures[i] != b.textures[i] || a.samplers[i] != b.samplers[i])
			return false;
	}
	return true;
}

struct SDL3GPUNativeBlitBatch {
	struct DrawGroup {
		SDL_GPUTexture *sourceTexture{nullptr};
		SDL_GPUSampler *sampler{nullptr};
		Uint32 firstIndex{0};
		Uint32 indexCount{0};
		Uint32 vertexCount{0};
		std::string telemetrySource;
	};

	GPU_Target *target{nullptr};
	GPU_Image *targetImage{nullptr};
	SDL_GPUTexture *targetTexture{nullptr};
	SDL_GPUGraphicsPipeline *pipeline{nullptr};
	SDL_GPUViewport viewport{};
	SDL_Rect scissor{};
	GPU_bool useBlending{false};
	GPU_BlendMode blendMode{};
	std::vector<SDL3GPUVertex> vertices;
	std::vector<Uint16> indices;
	std::vector<DrawGroup> drawGroups;
};

struct SDL3GPUNativeTriangleBatch {
	GPU_Target *target{nullptr};
	GPU_Image *targetImage{nullptr};
	SDL_GPUTexture *targetTexture{nullptr};
	SDL_GPUGraphicsPipeline *pipeline{nullptr};
	SDL_GPUViewport viewport{};
	SDL_Rect scissor{};
	GPU_bool useBlending{false};
	GPU_BlendMode blendMode{};
	bool nativeShaderProgram{false};
	SDL3GPUShaderKind shaderKind{SDL3GPUShaderKind::Unknown};
	bool pushColorScale{false};
	SDL3GPUSamplerSet samplerSet;
	std::vector<SDL3GPUNativeUniformRegister> fragmentUniformRegisters;
	std::vector<SDL3GPUVertex> vertices;
	std::vector<Uint16> indices;
};

struct SDL3GPUShaderTelemetry {
	Uint64 nativeCompiles{0};
	Uint64 compatibilityCompiles{0};
	Uint64 nativeDraws{0};
	Uint64 cpuFallbackDraws{0};
	Uint64 cpuFallbackPixels{0};
};

struct SDL3GPUTransferTelemetry {
	std::string source;
	Uint64 textureUploads{0};
	Uint64 textureUploadBytes{0};
	Uint64 readbacks{0};
	Uint64 readbackBytes{0};
};

struct SDL3GPUFixedDrawTelemetry {
	std::string source;
	Uint64 nativeFixedDraws{0};
	Uint64 nativeFixedVertices{0};
};

struct SDL3GPUTelemetry {
	bool initialized{false};
	bool enabled{false};
	bool hasData{false};
	Uint64 commandBuffersSubmitted{0};
	Uint64 textureUploads{0};
	Uint64 textureUploadBytes{0};
	Uint64 readbacks{0};
	Uint64 readbackBytes{0};
	Uint64 nativeFixedDraws{0};
	Uint64 nativeFixedVertices{0};
	Uint64 nativeShaderDraws{0};
	Uint64 nativeShaderVertices{0};
	Uint64 cpuBlitDraws{0};
	Uint64 cpuBlitPixels{0};
	Uint64 cpuShaderDraws{0};
	Uint64 cpuShaderPixels{0};
	Uint64 blockingGPUWaits{0};
	Uint64 submissionFences{0};
	Uint64 submissionFenceWaits{0};
	Uint64 submissionBacklogPeak{0};
	Uint64 nativeBlitCulls{0};
	Uint64 nativeBlitClips{0};
	Uint64 nativeShaderCompiles{0};
	Uint64 compatibilityShaderCompiles{0};
	std::array<SDL3GPUShaderTelemetry, static_cast<size_t>(SDL3GPUShaderKind::Count)> shaders{};
	std::vector<SDL3GPUTransferTelemetry> transfers;
	std::vector<SDL3GPUFixedDrawTelemetry> fixedDraws;
};

struct SDL3GPUSubmissionFence {
	SDL_GPUFence *fence{nullptr};
	Uint64 commandBuffers{0};
};

SDL_GPUShader *texturedVertexShader{nullptr};
SDL_GPUShader *texturedFragmentShader{nullptr};
SDL_GPUTexture *solidWhiteTexture{nullptr};
SDL_GPUSampler *nearestSampler{nullptr};
SDL_GPUSampler *linearSampler{nullptr};
SDL3GPUReusableTransferBuffer textureUploadBuffer;
SDL3GPUReusableTransferBuffer textureDownloadBuffer;
SDL3GPUReusableUploadedBuffer vertexUploadBuffer;
SDL3GPUReusableUploadedBuffer indexUploadBuffer;
SDL3GPUNativeBlitBatch nativeBlitBatch;
SDL3GPUNativeTriangleBatch nativeTriangleBatch;
SDL3GPUTelemetry telemetry;
std::vector<std::string> telemetrySourceStack;
std::vector<SDL3GPUPipelineEntry> pipelineCache;
std::unordered_map<Uint32, SDL3GPUShaderObject> shaderObjects;
std::unordered_map<Uint32, SDL3GPUProgramObject> programObjects;
std::unordered_map<int, Uint32> uniformLocationOwners;
std::unordered_set<GPU_Image *> liveTextureImages;
Uint64 nextLiveTextureTelemetryBytes{256ull * 1024ull * 1024ull};
std::deque<SDL3GPUSubmissionFence> submissionFences;
Uint64 unretiredCommandBuffers{0};
Uint64 commandBuffersSinceSubmissionFence{0};
// Completed-fence polling alone does not force every backend to release its
// deferred command/upload allocations. Track actual waits independently.
Uint64 commandBuffersSinceSubmissionWait{0};
int nextUniformLocation{1};
#if defined(ONS_USE_SDL3_SHADERCROSS)
bool shaderCrossInitialized{false};
#endif

void setShaderMessage(const char *message);
bool flushNativeBlitBatch();
bool flushNativeBlitBatchOnly();
bool flushNativeTriangleBatch();
bool queueNativeTriangleDraw(GPU_Image *image,
                             GPU_Target *target,
                             SDL_GPUGraphicsPipeline *pipeline,
                             const SDL_GPUViewport &viewport,
                             const SDL_Rect &scissor,
                             const SDL3GPUSamplerSet &samplerSet,
                             bool nativeShaderProgram,
                             SDL3GPUShaderKind shaderKind,
                             bool pushColorScale,
                             const std::vector<SDL3GPUNativeUniformRegister> &fragmentUniformRegisters,
                             const SDL3GPUVertex *vertices,
                             Uint32 numVertices,
                             const Uint16 *indices,
                             Uint32 numIndices);
void fillImagePixels(GPU_Image *image, SDL_Rect bounds, SDL_Color color);
void materializeSolidPixels(GPU_Image *image);
bool ensureImagePixelStorage(GPU_Image *image);
void discardCleanImagePixels(GPU_Image *image);
SDL_Rect clampImageUploadRect(const GPU_Image *image, SDL_Rect rect);
bool imageRectCoversImage(const GPU_Image *image, const SDL_Rect &rect);
void printAndResetTelemetry();

bool telemetryValueEnabled(const char *value) {
	if (!value || !*value)
		return false;
	if (std::strcmp(value, "0") == 0)
		return false;
	if (std::strcmp(value, "false") == 0 || std::strcmp(value, "FALSE") == 0)
		return false;
	if (std::strcmp(value, "off") == 0 || std::strcmp(value, "OFF") == 0)
		return false;
	return true;
}

bool telemetryEnabled() {
	if (!telemetry.initialized) {
		telemetry.enabled = telemetryValueEnabled(onsSDLGetEnv("ONS_SDL3_GPU_TELEMETRY"));
		telemetry.initialized = true;
	}
	return telemetry.enabled;
}

void noteCommandBufferSubmitted() {
	if (!telemetryEnabled())
		return;
	++telemetry.commandBuffersSubmitted;
	telemetry.hasData = true;
}

void noteBlockingGPUWait() {
	if (telemetryEnabled()) {
		++telemetry.blockingGPUWaits;
		telemetry.hasData = true;
	}
}

Uint64 maxQueuedCommandBuffersBeforeWait() {
	static bool initialized = false;
	static Uint64 value     = 64;
	if (initialized)
		return value;

	initialized       = true;
	const char *limit = onsSDLGetEnv("ONS_SDL3_GPU_MAX_QUEUED_COMMAND_BUFFERS");
	if (!limit || !*limit)
		return value;

	char *end = nullptr;
	const auto parsed = std::strtoull(limit, &end, 10);
	if (end != limit)
		value = parsed;
	return value;
}

Uint64 submissionFenceInterval(Uint64 maxQueuedCommandBuffers) {
	constexpr Uint64 DefaultInterval = 32;
	return std::min(DefaultInterval, maxQueuedCommandBuffers);
}

void noteSubmissionBacklog() {
	if (!telemetryEnabled())
		return;
	telemetry.submissionBacklogPeak = std::max(telemetry.submissionBacklogPeak, unretiredCommandBuffers);
	telemetry.hasData = true;
}

void retireOldestSubmissionFence() {
	if (submissionFences.empty())
		return;

	const SDL3GPUSubmissionFence completed = submissionFences.front();
	submissionFences.pop_front();
	SDL_ReleaseGPUFence(rendererState.device, completed.fence);
	unretiredCommandBuffers -= std::min(unretiredCommandBuffers, completed.commandBuffers);
}

void reapCompletedSubmissionFences() {
	while (!submissionFences.empty() && SDL_QueryGPUFence(rendererState.device, submissionFences.front().fence))
		retireOldestSubmissionFence();
}

void retireSubmissionBacklogAfterCompletedFence() {
	while (!submissionFences.empty())
		retireOldestSubmissionFence();
	unretiredCommandBuffers = 0;
	commandBuffersSinceSubmissionFence = 0;
	commandBuffersSinceSubmissionWait = 0;
}

void releaseSubmissionFences() {
	while (!submissionFences.empty()) {
		SDL_ReleaseGPUFence(rendererState.device, submissionFences.front().fence);
		submissionFences.pop_front();
	}
	unretiredCommandBuffers = 0;
	commandBuffersSinceSubmissionFence = 0;
	commandBuffersSinceSubmissionWait = 0;
}

void throttleGPUSubmissionBacklog() {
	if (!rendererState.device)
		return;

	const Uint64 maxQueuedCommandBuffers = maxQueuedCommandBuffersBeforeWait();
	if (maxQueuedCommandBuffers == 0)
		return;

	const bool submittedCheckpoint = commandBuffersSinceSubmissionFence == 0;
	const bool periodicWaitRequired = commandBuffersSinceSubmissionWait >= maxQueuedCommandBuffers;
	if (!submittedCheckpoint && !periodicWaitRequired)
		return;

	if (periodicWaitRequired && !submissionFences.empty()) {
		// The newest checkpoint covers all earlier submissions. Waiting for it
		// bounds backend-retained memory without idling unrelated later work.
		SDL_GPUFence *fence = submissionFences.back().fence;
		const bool waited = SDL_WaitForGPUFences(rendererState.device, true, &fence, 1);
		noteBlockingGPUWait();
		if (telemetryEnabled()) {
			++telemetry.submissionFenceWaits;
			telemetry.hasData = true;
		}
		if (waited)
			retireSubmissionBacklogAfterCompletedFence();
		return;
	}

	reapCompletedSubmissionFences();
}

bool submitGPUCommandBuffer(SDL_GPUCommandBuffer *commands) {
	const Uint64 maxQueuedCommandBuffers = maxQueuedCommandBuffersBeforeWait();
	const Uint64 fenceInterval = maxQueuedCommandBuffers == 0 ? 0 : submissionFenceInterval(maxQueuedCommandBuffers);
	const bool acquireFence = fenceInterval != 0 &&
	                          (commandBuffersSinceSubmissionFence + 1 >= fenceInterval ||
	                           commandBuffersSinceSubmissionWait + 1 >= maxQueuedCommandBuffers);

	SDL_GPUFence *fence = nullptr;
	if (acquireFence) {
		fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
		if (!fence)
			return false;
	} else if (!SDL_SubmitGPUCommandBuffer(commands)) {
		return false;
	}

	noteCommandBufferSubmitted();
	if (maxQueuedCommandBuffers != 0) {
		++unretiredCommandBuffers;
		++commandBuffersSinceSubmissionFence;
		++commandBuffersSinceSubmissionWait;
		if (fence) {
			submissionFences.push_back({fence, commandBuffersSinceSubmissionFence});
			commandBuffersSinceSubmissionFence = 0;
			if (telemetryEnabled()) {
				++telemetry.submissionFences;
				telemetry.hasData = true;
			}
		}
		noteSubmissionBacklog();
		throttleGPUSubmissionBacklog();
	}
	return true;
}

SDL_GPUFence *submitGPUCommandBufferAndAcquireFence(SDL_GPUCommandBuffer *commands) {
	SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
	if (fence) {
		noteCommandBufferSubmitted();
		if (maxQueuedCommandBuffersBeforeWait() != 0) {
			++unretiredCommandBuffers;
			++commandBuffersSinceSubmissionFence;
			++commandBuffersSinceSubmissionWait;
			noteSubmissionBacklog();
			throttleGPUSubmissionBacklog();
		}
	}
	return fence;
}

std::string normalizedTelemetrySource(const char *source) {
	return (source && *source) ? source : "unscoped";
}

std::string currentTelemetrySource(const char *fallback) {
	if (!telemetrySourceStack.empty())
		return telemetrySourceStack.back();
	return normalizedTelemetrySource(fallback);
}

SDL3GPUTransferTelemetry &transferTelemetryForSource(const std::string &source) {
	for (auto &stats : telemetry.transfers) {
		if (stats.source == source)
			return stats;
	}
	telemetry.transfers.emplace_back();
	telemetry.transfers.back().source = source;
	return telemetry.transfers.back();
}

SDL3GPUFixedDrawTelemetry &fixedDrawTelemetryForSource(const std::string &source) {
	for (auto &stats : telemetry.fixedDraws) {
		if (stats.source == source)
			return stats;
	}
	telemetry.fixedDraws.emplace_back();
	telemetry.fixedDraws.back().source = source;
	return telemetry.fixedDraws.back();
}

void noteTextureUpload(Uint64 bytes, const char *source = nullptr) {
	if (!telemetryEnabled())
		return;
	++telemetry.textureUploads;
	telemetry.textureUploadBytes += bytes;
	auto &transfer = transferTelemetryForSource(currentTelemetrySource(source));
	++transfer.textureUploads;
	transfer.textureUploadBytes += bytes;
	telemetry.hasData = true;
}

void noteReadback(Uint64 bytes, const char *source = nullptr) {
	if (!telemetryEnabled())
		return;
	++telemetry.readbacks;
	telemetry.readbackBytes += bytes;
	auto &transfer = transferTelemetryForSource(currentTelemetrySource(source));
	++transfer.readbacks;
	transfer.readbackBytes += bytes;
	telemetry.hasData = true;
}

size_t shaderTelemetryIndex(SDL3GPUShaderKind kind) {
	const size_t index = static_cast<size_t>(kind);
	return index < telemetry.shaders.size() ? index : static_cast<size_t>(SDL3GPUShaderKind::Unknown);
}

void noteShaderCompilation(SDL3GPUShaderKind kind, bool native) {
	if (!telemetryEnabled())
		return;
	auto &shader = telemetry.shaders[shaderTelemetryIndex(kind)];
	if (native) {
		++shader.nativeCompiles;
		++telemetry.nativeShaderCompiles;
	} else {
		++shader.compatibilityCompiles;
		++telemetry.compatibilityShaderCompiles;
	}
	telemetry.hasData = true;
}

void noteNativeFixedDrawForSource(const std::string &source, Uint64 vertices) {
	if (!telemetryEnabled())
		return;
	++telemetry.nativeFixedDraws;
	telemetry.nativeFixedVertices += vertices;
	auto &fixedDraw = fixedDrawTelemetryForSource(source.empty() ? currentTelemetrySource("native_fixed_draw") : source);
	++fixedDraw.nativeFixedDraws;
	fixedDraw.nativeFixedVertices += vertices;
	telemetry.hasData = true;
}

void noteNativeFixedDraw(Uint64 vertices) {
	noteNativeFixedDrawForSource(std::string{}, vertices);
}

void noteNativeBlitCull() {
	if (!telemetryEnabled())
		return;
	++telemetry.nativeBlitCulls;
	telemetry.hasData = true;
}

void noteNativeBlitClip() {
	if (!telemetryEnabled())
		return;
	++telemetry.nativeBlitClips;
	telemetry.hasData = true;
}

void noteNativeShaderDraw(SDL3GPUShaderKind kind, Uint64 vertices) {
	if (!telemetryEnabled())
		return;
	++telemetry.nativeShaderDraws;
	telemetry.nativeShaderVertices += vertices;
	++telemetry.shaders[shaderTelemetryIndex(kind)].nativeDraws;
	telemetry.hasData = true;
}

void noteCpuBlit(Uint64 pixels) {
	if (!telemetryEnabled())
		return;
	++telemetry.cpuBlitDraws;
	telemetry.cpuBlitPixels += pixels;
	telemetry.hasData = true;
}

void noteCpuShaderFallback(SDL3GPUShaderKind kind, Uint64 pixels) {
	if (!telemetryEnabled())
		return;
	++telemetry.cpuShaderDraws;
	telemetry.cpuShaderPixels += pixels;
	auto &shader = telemetry.shaders[shaderTelemetryIndex(kind)];
	++shader.cpuFallbackDraws;
	shader.cpuFallbackPixels += pixels;
	telemetry.hasData = true;
}

int textureBytesPerPixel(GPU_FormatEnum format) {
	switch (format) {
		case GPU_FORMAT_LUMINANCE:
			return 1;
		case GPU_FORMAT_LUMINANCE_ALPHA:
			return 2;
		case GPU_FORMAT_RGB:
		case GPU_FORMAT_RGBA:
		default:
			return 4;
	}
}

size_t imagePixelBytes(const GPU_Image *image) {
	return image ? static_cast<size_t>(image->pitch) * image->h : 0;
}

Uint32 alignUp(Uint32 value, Uint32 alignment) {
	if (alignment == 0)
		return value;
	const Uint32 remainder = value % alignment;
	return remainder == 0 ? value : value + (alignment - remainder);
}

bool ensureImagePixelStorage(GPU_Image *image) {
	if (!image)
		return false;
	const size_t bytes = imagePixelBytes(image);
	if (bytes == 0)
		return false;
	if (image->pixels.size() == bytes)
		return true;
	try {
		image->pixels.assign(bytes, 0);
	} catch (...) {
		image->pixels.clear();
		return false;
	}
	return true;
}

void discardCleanImagePixels(GPU_Image *image) {
	if (!image || image->pixels.empty())
		return;
	if (image->pixels_dirty && !image->texture_initialized && !image->pixels_solid)
		return;
	std::vector<Uint8>().swap(image->pixels);
}

void liveImageMemoryTotals(size_t &textureBytes, size_t &pixelBytes) {
	textureBytes = 0;
	pixelBytes   = 0;
	for (auto *image : liveTextureImages) {
		textureBytes += imagePixelBytes(image);
		pixelBytes += image ? image->pixels.size() : 0;
	}
}

SDL_GPUTextureFormat textureFormat(GPU_FormatEnum format) {
	switch (format) {
		case GPU_FORMAT_LUMINANCE:
			return SDL_GPU_TEXTUREFORMAT_R8_UNORM;
		case GPU_FORMAT_LUMINANCE_ALPHA:
			return SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
		case GPU_FORMAT_RGB:
		case GPU_FORMAT_RGBA:
		default:
			return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	}
}

bool isNativeTextureFormat(GPU_FormatEnum format) {
	return format == GPU_FORMAT_RGB || format == GPU_FORMAT_RGBA;
}

bool isNativeShaderSamplerFormat(GPU_FormatEnum format) {
	return format == GPU_FORMAT_LUMINANCE ||
	       format == GPU_FORMAT_LUMINANCE_ALPHA ||
	       isNativeTextureFormat(format);
}

bool sameBlendMode(const GPU_BlendMode &a, const GPU_BlendMode &b) {
	return a.source_color == b.source_color &&
	       a.dest_color == b.dest_color &&
	       a.source_alpha == b.source_alpha &&
	       a.dest_alpha == b.dest_alpha &&
	       a.color_equation == b.color_equation &&
	       a.alpha_equation == b.alpha_equation;
}

SDL_GPUBlendFactor toSDLBlendFactor(GPU_BlendFuncEnum factor) {
	switch (factor) {
		case GPU_FUNC_ZERO:
			return SDL_GPU_BLENDFACTOR_ZERO;
		case GPU_FUNC_ONE:
			return SDL_GPU_BLENDFACTOR_ONE;
		case GPU_FUNC_SRC_COLOR:
			return SDL_GPU_BLENDFACTOR_SRC_COLOR;
		case GPU_FUNC_DST_COLOR:
			return SDL_GPU_BLENDFACTOR_DST_COLOR;
		case GPU_FUNC_ONE_MINUS_SRC:
			return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
		case GPU_FUNC_ONE_MINUS_DST:
			return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
		case GPU_FUNC_SRC_ALPHA:
			return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		case GPU_FUNC_DST_ALPHA:
			return SDL_GPU_BLENDFACTOR_DST_ALPHA;
		case GPU_FUNC_ONE_MINUS_SRC_ALPHA:
			return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		case GPU_FUNC_ONE_MINUS_DST_ALPHA:
			return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
		default:
			return SDL_GPU_BLENDFACTOR_ONE;
	}
}

SDL_GPUBlendOp toSDLBlendOp(GPU_BlendEqEnum equation) {
	switch (equation) {
		case GPU_EQ_SUBTRACT:
			return SDL_GPU_BLENDOP_SUBTRACT;
		case GPU_EQ_REVERSE_SUBTRACT:
			return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
		case GPU_EQ_ADD:
		default:
			return SDL_GPU_BLENDOP_ADD;
	}
}

SDL3GPUShaderBytecode selectShaderBytecode(SDL_GPUShaderFormat supported, GPU_ShaderEnum stage) {
	if (supported & SDL_GPU_SHADERFORMAT_DXIL) {
		if (stage == GPU_VERTEX_SHADER)
			return SDL3GPUShaderBytecode{tri_texture_vert_dxil, sizeof(tri_texture_vert_dxil), SDL_GPU_SHADERFORMAT_DXIL, "main"};
		return SDL3GPUShaderBytecode{texture_rgba_frag_dxil, sizeof(texture_rgba_frag_dxil), SDL_GPU_SHADERFORMAT_DXIL, "main"};
	}
	if (supported & SDL_GPU_SHADERFORMAT_SPIRV) {
		if (stage == GPU_VERTEX_SHADER)
			return SDL3GPUShaderBytecode{tri_texture_vert_spv, sizeof(tri_texture_vert_spv), SDL_GPU_SHADERFORMAT_SPIRV, "main"};
		return SDL3GPUShaderBytecode{texture_rgba_frag_spv, sizeof(texture_rgba_frag_spv), SDL_GPU_SHADERFORMAT_SPIRV, "main"};
	}
	if (supported & SDL_GPU_SHADERFORMAT_MSL) {
		if (stage == GPU_VERTEX_SHADER)
			return SDL3GPUShaderBytecode{tri_texture_vert_msl, sizeof(tri_texture_vert_msl), SDL_GPU_SHADERFORMAT_MSL, "main0"};
		return SDL3GPUShaderBytecode{texture_rgba_frag_msl, sizeof(texture_rgba_frag_msl), SDL_GPU_SHADERFORMAT_MSL, "main0"};
	}
	return SDL3GPUShaderBytecode{};
}

SDL_GPUShader *createNativeShader(GPU_ShaderEnum shaderType) {
	if (!rendererState.device)
		return nullptr;

	const SDL_GPUShaderFormat supported = SDL_GetGPUShaderFormats(rendererState.device);
	const SDL3GPUShaderBytecode bytecode = selectShaderBytecode(supported, shaderType);
	if (!bytecode.code || bytecode.format == SDL_GPU_SHADERFORMAT_INVALID) {
		setShaderMessage("SDL3_GPU backend does not expose a supported fixed shader format");
		return nullptr;
	}

	SDL_GPUShaderCreateInfo shaderInfo{};
	shaderInfo.code_size = bytecode.size;
	shaderInfo.code      = bytecode.code;
	shaderInfo.entrypoint = bytecode.entrypoint;
	shaderInfo.format    = bytecode.format;
	shaderInfo.stage     = shaderType == GPU_VERTEX_SHADER ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
	shaderInfo.num_uniform_buffers = 1;
	shaderInfo.num_samplers = shaderType == GPU_VERTEX_SHADER ? 0 : 1;

	SDL_GPUShader *shader = SDL_CreateGPUShader(rendererState.device, &shaderInfo);
	if (!shader)
		setShaderMessage(SDL_GetError());
	return shader;
}

bool ensureNativeShaders() {
	if (texturedVertexShader && texturedFragmentShader)
		return true;

	texturedVertexShader = createNativeShader(GPU_VERTEX_SHADER);
	if (!texturedVertexShader)
		return false;

	texturedFragmentShader = createNativeShader(GPU_FRAGMENT_SHADER);
	if (!texturedFragmentShader)
		return false;

	return true;
}

SDL_GPUShader *createPrecompiledFragmentShader(const Uint8 *code,
                                               size_t codeSize,
                                               SDL_GPUShaderFormat format,
                                               const SDL3GPUNativeResourceInfo &resources,
                                               const char *entrypoint = "main") {
	if (!rendererState.device || !code || codeSize == 0)
		return nullptr;

	SDL_GPUShaderCreateInfo shaderInfo{};
	shaderInfo.code_size = codeSize;
	shaderInfo.code      = code;
	shaderInfo.entrypoint = entrypoint;
	shaderInfo.format    = format;
	shaderInfo.stage     = SDL_GPU_SHADERSTAGE_FRAGMENT;
	shaderInfo.num_samplers = resources.numSamplers;
	shaderInfo.num_storage_textures = resources.numStorageTextures;
	shaderInfo.num_storage_buffers = resources.numStorageBuffers;
	shaderInfo.num_uniform_buffers = resources.numUniformBuffers;

	SDL_GPUShader *shader = SDL_CreateGPUShader(rendererState.device, &shaderInfo);
	if (!shader)
		setShaderMessage(SDL_GetError());
	return shader;
}

SDL3GPUNativeUniform nativeUniform(const char *name,
                                   SDL3GPUUniformType type,
                                   int components,
                                   Uint32 registerIndex,
                                   int arraySize = 1) {
	SDL3GPUNativeUniform uniform{};
	uniform.name          = name;
	uniform.type          = type;
	uniform.components    = components;
	uniform.arraySize     = std::max(1, arraySize);
	uniform.registerIndex = registerIndex;
	return uniform;
}

bool compilePrecompiledBuiltInShader(GPU_ShaderEnum shaderType,
                                     SDL3GPUShaderKind kind,
                                     SDL3GPUShaderObject &object) {
	if (!rendererState.device || shaderType == GPU_VERTEX_SHADER)
		return false;

	const SDL_GPUShaderFormat supported = SDL_GetGPUShaderFormats(rendererState.device);
	if (kind == SDL3GPUShaderKind::AlphaOutsideTextures && (supported & SDL_GPU_SHADERFORMAT_SPIRV)) {
		SDL3GPUNativeResourceInfo resources{};
		resources.numSamplers = 1;

		object.nativeShader = createPrecompiledFragmentShader(alpha_outside_textures_frag_spv,
		                                                      sizeof(alpha_outside_textures_frag_spv),
		                                                      SDL_GPU_SHADERFORMAT_SPIRV,
		                                                      resources);
		if (!object.nativeShader)
			return false;

		object.nativeResources = resources;
		return true;
	}

	if (kind == SDL3GPUShaderKind::BlendByMask && (supported & SDL_GPU_SHADERFORMAT_SPIRV)) {
		SDL3GPUNativeResourceInfo resources{};
		resources.numSamplers = 3;
		resources.numUniformBuffers = 1;

		object.nativeShader = createPrecompiledFragmentShader(blend_by_mask_frag_spv,
		                                                      sizeof(blend_by_mask_frag_spv),
		                                                      SDL_GPU_SHADERFORMAT_SPIRV,
		                                                      resources);
		if (!object.nativeShader)
			return false;

		object.nativeResources = resources;
		object.nativeUniforms.push_back(nativeUniform("mask_value", SDL3GPUUniformType::Int, 1, 0));
		object.nativeUniforms.push_back(nativeUniform("constant_mask", SDL3GPUUniformType::Bool, 1, 1));
		object.nativeUniforms.push_back(nativeUniform("crossfade", SDL3GPUUniformType::Bool, 1, 2));
		return true;
	}

	if ((kind == SDL3GPUShaderKind::BlurH || kind == SDL3GPUShaderKind::BlurV) &&
	    (supported & SDL_GPU_SHADERFORMAT_SPIRV)) {
		SDL3GPUNativeResourceInfo resources{};
		resources.numSamplers = 1;
		resources.numUniformBuffers = 1;

		const bool horizontal = kind == SDL3GPUShaderKind::BlurH;
		object.nativeShader = createPrecompiledFragmentShader(horizontal ? blur_h_frag_spv : blur_v_frag_spv,
		                                                      horizontal ? sizeof(blur_h_frag_spv) : sizeof(blur_v_frag_spv),
		                                                      SDL_GPU_SHADERFORMAT_SPIRV,
		                                                      resources);
		if (!object.nativeShader)
			return false;

		object.nativeResources = resources;
		object.nativeUniforms.push_back(nativeUniform("sigma", SDL3GPUUniformType::Float, 1, 0));
		object.nativeUniforms.push_back(nativeUniform("blurSize", SDL3GPUUniformType::Float, 1, 1));
		return true;
	}

	if (kind == SDL3GPUShaderKind::Breakup && (supported & SDL_GPU_SHADERFORMAT_SPIRV)) {
		SDL3GPUNativeResourceInfo resources{};
		resources.numSamplers = 3;
		resources.numUniformBuffers = 1;

		object.nativeShader = createPrecompiledFragmentShader(breakup_frag_spv,
		                                                      sizeof(breakup_frag_spv),
		                                                      SDL_GPU_SHADERFORMAT_SPIRV,
		                                                      resources);
		if (!object.nativeShader)
			return false;

		object.nativeResources = resources;
		object.nativeUniforms.push_back(nativeUniform("tilesX", SDL3GPUUniformType::Float, 1, 0));
		object.nativeUniforms.push_back(nativeUniform("tilesY", SDL3GPUUniformType::Float, 1, 1));
		object.nativeUniforms.push_back(nativeUniform("breakupCellforms", SDL3GPUUniformType::Int, 1, 2));
		return true;
	}

	if (kind == SDL3GPUShaderKind::ColorModification && (supported & SDL_GPU_SHADERFORMAT_SPIRV)) {
		SDL3GPUNativeResourceInfo resources{};
		resources.numSamplers = 1;
		resources.numUniformBuffers = 1;

		object.nativeShader = createPrecompiledFragmentShader(color_modification_frag_spv,
		                                                      sizeof(color_modification_frag_spv),
		                                                      SDL_GPU_SHADERFORMAT_SPIRV,
		                                                      resources);
		if (!object.nativeShader)
			return false;

		object.nativeResources = resources;
		object.nativeUniforms.push_back(nativeUniform("modificationType", SDL3GPUUniformType::Int, 1, 0));
		object.nativeUniforms.push_back(nativeUniform("multiplyAlpha", SDL3GPUUniformType::Int, 1, 1));
		object.nativeUniforms.push_back(nativeUniform("blurFactor", SDL3GPUUniformType::Int, 1, 2));
		object.nativeUniforms.push_back(nativeUniform("dimension", SDL3GPUUniformType::Int, 1, 3));
		object.nativeUniforms.push_back(nativeUniform("darkenHue", SDL3GPUUniformType::FloatVec, 4, 4));
		object.nativeUniforms.push_back(nativeUniform("greyscaleHue", SDL3GPUUniformType::FloatVec, 4, 5));
		object.nativeUniforms.push_back(nativeUniform("replaceSrcColor", SDL3GPUUniformType::FloatVec, 4, 6));
		object.nativeUniforms.push_back(nativeUniform("replaceDstColor", SDL3GPUUniformType::FloatVec, 4, 7));
		return true;
	}

	if (kind == SDL3GPUShaderKind::ColourConversion && (supported & SDL_GPU_SHADERFORMAT_SPIRV)) {
		SDL3GPUNativeResourceInfo resources{};
		resources.numSamplers = 3;
		resources.numUniformBuffers = 1;

		object.nativeShader = createPrecompiledFragmentShader(colour_conversion_frag_spv,
		                                                      sizeof(colour_conversion_frag_spv),
		                                                      SDL_GPU_SHADERFORMAT_SPIRV,
		                                                      resources);
		if (!object.nativeShader)
			return false;

		object.nativeResources = resources;
		object.nativeUniforms.push_back(nativeUniform("conversionType", SDL3GPUUniformType::Int, 1, 0));
		object.nativeUniforms.push_back(nativeUniform("maskHeight", SDL3GPUUniformType::Int, 1, 1));
		return true;
	}

	if (kind == SDL3GPUShaderKind::CropByMask && (supported & SDL_GPU_SHADERFORMAT_SPIRV)) {
		SDL3GPUNativeResourceInfo resources{};
		resources.numSamplers = 2;

		object.nativeShader = createPrecompiledFragmentShader(crop_by_mask_frag_spv,
		                                                      sizeof(crop_by_mask_frag_spv),
		                                                      SDL_GPU_SHADERFORMAT_SPIRV,
		                                                      resources);
		if (!object.nativeShader)
			return false;

		object.nativeResources = resources;
		return true;
	}

	if (kind == SDL3GPUShaderKind::EffectTrvswave && (supported & SDL_GPU_SHADERFORMAT_SPIRV)) {
		SDL3GPUNativeResourceInfo resources{};
		resources.numSamplers = 1;
		resources.numUniformBuffers = 1;

		object.nativeShader = createPrecompiledFragmentShader(effect_trvswave_frag_spv,
		                                                      sizeof(effect_trvswave_frag_spv),
		                                                      SDL_GPU_SHADERFORMAT_SPIRV,
		                                                      resources);
		if (!object.nativeShader)
			return false;

		object.nativeResources = resources;
		object.nativeUniforms.push_back(nativeUniform("script_width", SDL3GPUUniformType::Int, 1, 0));
		object.nativeUniforms.push_back(nativeUniform("script_height", SDL3GPUUniformType::Int, 1, 1));
		object.nativeUniforms.push_back(nativeUniform("effect_counter", SDL3GPUUniformType::Int, 1, 2));
		object.nativeUniforms.push_back(nativeUniform("duration", SDL3GPUUniformType::Int, 1, 3));
		return true;
	}

	if (kind == SDL3GPUShaderKind::EffectWarp && (supported & SDL_GPU_SHADERFORMAT_SPIRV)) {
		SDL3GPUNativeResourceInfo resources{};
		resources.numSamplers = 1;
		resources.numUniformBuffers = 1;

		object.nativeShader = createPrecompiledFragmentShader(effect_warp_frag_spv,
		                                                      sizeof(effect_warp_frag_spv),
		                                                      SDL_GPU_SHADERFORMAT_SPIRV,
		                                                      resources);
		if (!object.nativeShader)
			return false;

		object.nativeResources = resources;
		object.nativeUniforms.push_back(nativeUniform("animationClock", SDL3GPUUniformType::Float, 1, 0));
		object.nativeUniforms.push_back(nativeUniform("amplitude", SDL3GPUUniformType::Float, 1, 1));
		object.nativeUniforms.push_back(nativeUniform("wavelength", SDL3GPUUniformType::Float, 1, 2));
		object.nativeUniforms.push_back(nativeUniform("speed", SDL3GPUUniformType::Float, 1, 3));
		object.nativeUniforms.push_back(nativeUniform("cx", SDL3GPUUniformType::Float, 1, 4));
		object.nativeUniforms.push_back(nativeUniform("cy", SDL3GPUUniformType::Float, 1, 5));
		return true;
	}

	if (kind == SDL3GPUShaderKind::EffectWhirl && (supported & SDL_GPU_SHADERFORMAT_SPIRV)) {
		SDL3GPUNativeResourceInfo resources{};
		resources.numSamplers = 1;
		resources.numUniformBuffers = 1;

		object.nativeShader = createPrecompiledFragmentShader(effect_whirl_frag_spv,
		                                                      sizeof(effect_whirl_frag_spv),
		                                                      SDL_GPU_SHADERFORMAT_SPIRV,
		                                                      resources);
		if (!object.nativeShader)
			return false;

		object.nativeResources = resources;
		object.nativeUniforms.push_back(nativeUniform("effect_counter", SDL3GPUUniformType::Int, 1, 0));
		object.nativeUniforms.push_back(nativeUniform("duration", SDL3GPUUniformType::Int, 1, 1));
		object.nativeUniforms.push_back(nativeUniform("direction", SDL3GPUUniformType::Int, 1, 2));
		object.nativeUniforms.push_back(nativeUniform("render_width", SDL3GPUUniformType::Float, 1, 3));
		object.nativeUniforms.push_back(nativeUniform("render_height", SDL3GPUUniformType::Float, 1, 4));
		object.nativeUniforms.push_back(nativeUniform("texture_width", SDL3GPUUniformType::Float, 1, 5));
		object.nativeUniforms.push_back(nativeUniform("texture_height", SDL3GPUUniformType::Float, 1, 6));
		return true;
	}

	if (kind == SDL3GPUShaderKind::GlassSmash && (supported & SDL_GPU_SHADERFORMAT_SPIRV)) {
		SDL3GPUNativeResourceInfo resources{};
		resources.numSamplers = 1;
		resources.numUniformBuffers = 1;

		object.nativeShader = createPrecompiledFragmentShader(glass_smash_frag_spv,
		                                                      sizeof(glass_smash_frag_spv),
		                                                      SDL_GPU_SHADERFORMAT_SPIRV,
		                                                      resources);
		if (!object.nativeShader)
			return false;

		object.nativeResources = resources;
		object.nativeUniforms.push_back(nativeUniform("alpha", SDL3GPUUniformType::Float, 1, 0));
		return true;
	}

	if (kind == SDL3GPUShaderKind::GlyphGradient && (supported & SDL_GPU_SHADERFORMAT_SPIRV)) {
		SDL3GPUNativeResourceInfo resources{};
		resources.numSamplers = 1;
		resources.numUniformBuffers = 1;

		object.nativeShader = createPrecompiledFragmentShader(glyph_gradient_frag_spv,
		                                                      sizeof(glyph_gradient_frag_spv),
		                                                      SDL_GPU_SHADERFORMAT_SPIRV,
		                                                      resources);
		if (!object.nativeShader)
			return false;

		object.nativeResources = resources;
		object.nativeUniforms.push_back(nativeUniform("color", SDL3GPUUniformType::FloatVec, 4, 0));
		object.nativeUniforms.push_back(nativeUniform("maxy", SDL3GPUUniformType::Int, 1, 1));
		object.nativeUniforms.push_back(nativeUniform("faceAscender", SDL3GPUUniformType::Int, 1, 2));
		object.nativeUniforms.push_back(nativeUniform("height", SDL3GPUUniformType::Int, 1, 3));
		return true;
	}

	if (kind == SDL3GPUShaderKind::MergeAlpha && (supported & SDL_GPU_SHADERFORMAT_SPIRV)) {
		SDL3GPUNativeResourceInfo resources{};
		resources.numSamplers = 2;

		object.nativeShader = createPrecompiledFragmentShader(merge_alpha_frag_spv,
		                                                      sizeof(merge_alpha_frag_spv),
		                                                      SDL_GPU_SHADERFORMAT_SPIRV,
		                                                      resources);
		if (!object.nativeShader)
			return false;

		object.nativeResources = resources;
		return true;
	}

	if (kind == SDL3GPUShaderKind::MultiplyAlpha && (supported & SDL_GPU_SHADERFORMAT_SPIRV)) {
		SDL3GPUNativeResourceInfo resources{};
		resources.numSamplers = 1;

		object.nativeShader = createPrecompiledFragmentShader(multiply_alpha_frag_spv,
		                                                      sizeof(multiply_alpha_frag_spv),
		                                                      SDL_GPU_SHADERFORMAT_SPIRV,
		                                                      resources);
		if (!object.nativeShader)
			return false;

		object.nativeResources = resources;
		return true;
	}

	if (kind == SDL3GPUShaderKind::Pixelate && (supported & SDL_GPU_SHADERFORMAT_SPIRV)) {
		SDL3GPUNativeResourceInfo resources{};
		resources.numSamplers = 1;
		resources.numUniformBuffers = 1;

		object.nativeShader = createPrecompiledFragmentShader(pixelate_frag_spv,
		                                                      sizeof(pixelate_frag_spv),
		                                                      SDL_GPU_SHADERFORMAT_SPIRV,
		                                                      resources);
		if (!object.nativeShader)
			return false;

		object.nativeResources = resources;
		object.nativeUniforms.push_back(nativeUniform("width", SDL3GPUUniformType::Int, 1, 0));
		object.nativeUniforms.push_back(nativeUniform("height", SDL3GPUUniformType::Int, 1, 1));
		object.nativeUniforms.push_back(nativeUniform("factor", SDL3GPUUniformType::Int, 1, 2));
		return true;
	}

	if (kind == SDL3GPUShaderKind::RenderSubtitles && (supported & SDL_GPU_SHADERFORMAT_SPIRV)) {
		SDL3GPUNativeResourceInfo resources{};
		resources.numSamplers = 1;
		resources.numUniformBuffers = 1;

		object.nativeShader = createPrecompiledFragmentShader(render_subtitles_frag_spv,
		                                                      sizeof(render_subtitles_frag_spv),
		                                                      SDL_GPU_SHADERFORMAT_SPIRV,
		                                                      resources);
		if (!object.nativeShader)
			return false;

		object.nativeResources = resources;
		object.nativeSamplerImageUnits[0] = 1;
		object.nativeUniforms.push_back(nativeUniform("ntextures", SDL3GPUUniformType::Int, 1, 0));
		object.nativeUniforms.push_back(nativeUniform("subDims", SDL3GPUUniformType::FloatVec, 2, 1, 8));
		object.nativeUniforms.push_back(nativeUniform("subCoords", SDL3GPUUniformType::FloatVec, 2, 9, 8));
		object.nativeUniforms.push_back(nativeUniform("subColors", SDL3GPUUniformType::FloatVec, 4, 17, 8));
		object.nativeUniforms.push_back(nativeUniform("dstDims", SDL3GPUUniformType::FloatVec, 2, 25));
		return true;
	}

	if (kind == SDL3GPUShaderKind::TextFade && (supported & SDL_GPU_SHADERFORMAT_SPIRV)) {
		SDL3GPUNativeResourceInfo resources{};
		resources.numSamplers = 1;
		resources.numUniformBuffers = 1;

		object.nativeShader = createPrecompiledFragmentShader(text_fade_frag_spv,
		                                                      sizeof(text_fade_frag_spv),
		                                                      SDL_GPU_SHADERFORMAT_SPIRV,
		                                                      resources);
		if (!object.nativeShader)
			return false;

		object.nativeResources = resources;
		object.nativeUniforms.push_back(nativeUniform("partial", SDL3GPUUniformType::Int, 1, 0));
		object.nativeUniforms.push_back(nativeUniform("full", SDL3GPUUniformType::Int, 1, 1));
		object.nativeUniforms.push_back(nativeUniform("width", SDL3GPUUniformType::Int, 1, 2));
		return true;
	}

	return false;
}

SDL_GPUSampler *getSampler(GPU_FilterEnum filter) {
	SDL_GPUSampler **slot = filter == GPU_FILTER_NEAREST ? &nearestSampler : &linearSampler;
	if (*slot)
		return *slot;

	SDL_GPUSamplerCreateInfo samplerInfo{};
	const bool nearest = filter == GPU_FILTER_NEAREST;
	samplerInfo.min_filter = nearest ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
	samplerInfo.mag_filter = nearest ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
	samplerInfo.mipmap_mode = nearest ? SDL_GPU_SAMPLERMIPMAPMODE_NEAREST : SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
	samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	*slot = SDL_CreateGPUSampler(rendererState.device, &samplerInfo);
	if (!*slot)
		setShaderMessage(SDL_GetError());
	return *slot;
}

SDL_GPUGraphicsPipeline *getPipeline(SDL_GPUTextureFormat targetFormat, GPU_bool useBlending, const GPU_BlendMode &blendMode,
                                     SDL_GPUShader *vertexShader = nullptr, SDL_GPUShader *fragmentShader = nullptr) {
	if (!vertexShader || !fragmentShader) {
		if (!ensureNativeShaders())
			return nullptr;
		vertexShader   = texturedVertexShader;
		fragmentShader = texturedFragmentShader;
	}

	for (const auto &entry : pipelineCache) {
		if (entry.targetFormat == targetFormat &&
		    entry.useBlending == useBlending &&
		    entry.vertexShader == vertexShader &&
		    entry.fragmentShader == fragmentShader &&
		    (!useBlending || sameBlendMode(entry.blendMode, blendMode))) {
			return entry.pipeline;
		}
	}

	SDL_GPUColorTargetDescription colorTarget{};
	colorTarget.format = targetFormat;
	colorTarget.blend_state.enable_blend = useBlending;
	colorTarget.blend_state.enable_color_write_mask = true;
	colorTarget.blend_state.color_write_mask = SDL_GPU_COLORCOMPONENT_R |
	                                           SDL_GPU_COLORCOMPONENT_G |
	                                           SDL_GPU_COLORCOMPONENT_B |
	                                           SDL_GPU_COLORCOMPONENT_A;
	if (useBlending) {
		colorTarget.blend_state.src_color_blendfactor = toSDLBlendFactor(blendMode.source_color);
		colorTarget.blend_state.dst_color_blendfactor = toSDLBlendFactor(blendMode.dest_color);
		colorTarget.blend_state.src_alpha_blendfactor = toSDLBlendFactor(blendMode.source_alpha);
		colorTarget.blend_state.dst_alpha_blendfactor = toSDLBlendFactor(blendMode.dest_alpha);
		colorTarget.blend_state.color_blend_op = toSDLBlendOp(blendMode.color_equation);
		colorTarget.blend_state.alpha_blend_op = toSDLBlendOp(blendMode.alpha_equation);
	}

	SDL_GPUVertexBufferDescription vertexBuffer{};
	vertexBuffer.slot = 0;
	vertexBuffer.pitch = sizeof(SDL3GPUVertex);
	vertexBuffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

	std::array<SDL_GPUVertexAttribute, 3> attributes{};
	attributes[0].location = 0;
	attributes[0].buffer_slot = 0;
	attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
	attributes[0].offset = offsetof(SDL3GPUVertex, x);
	attributes[1].location = 1;
	attributes[1].buffer_slot = 0;
	attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
	attributes[1].offset = offsetof(SDL3GPUVertex, r);
	attributes[2].location = 2;
	attributes[2].buffer_slot = 0;
	attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
	attributes[2].offset = offsetof(SDL3GPUVertex, s);

	SDL_GPUGraphicsPipelineCreateInfo pipelineInfo{};
	pipelineInfo.vertex_shader = vertexShader;
	pipelineInfo.fragment_shader = fragmentShader;
	pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	pipelineInfo.vertex_input_state.vertex_buffer_descriptions = &vertexBuffer;
	pipelineInfo.vertex_input_state.num_vertex_buffers = 1;
	pipelineInfo.vertex_input_state.vertex_attributes = attributes.data();
	pipelineInfo.vertex_input_state.num_vertex_attributes = static_cast<Uint32>(attributes.size());
	pipelineInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
	pipelineInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
	pipelineInfo.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
	pipelineInfo.rasterizer_state.enable_depth_clip = true;
	pipelineInfo.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
	pipelineInfo.target_info.color_target_descriptions = &colorTarget;
	pipelineInfo.target_info.num_color_targets = 1;

	SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(rendererState.device, &pipelineInfo);
	if (!pipeline) {
		setShaderMessage(SDL_GetError());
		return nullptr;
	}

	pipelineCache.push_back(SDL3GPUPipelineEntry{targetFormat, useBlending, blendMode, vertexShader, fragmentShader, pipeline});
	return pipeline;
}

GPU_BlendMode normalBlendMode() {
	GPU_BlendMode mode{};
	mode.source_color   = GPU_FUNC_ONE;
	mode.dest_color     = GPU_FUNC_ONE_MINUS_SRC_ALPHA;
	mode.source_alpha   = GPU_FUNC_ONE;
	mode.dest_alpha     = GPU_FUNC_ONE_MINUS_SRC_ALPHA;
	mode.color_equation = GPU_EQ_ADD;
	mode.alpha_equation = GPU_EQ_ADD;
	return mode;
}

void setShaderMessage(const char *message) {
	std::snprintf(shaderMessage, sizeof(shaderMessage), "%s", message ? message : "");
}

bool containsText(const std::string &text, const char *needle) {
	return text.find(needle) != std::string::npos;
}

SDL3GPUShaderKind identifyShaderSource(GPU_ShaderEnum shaderType, const std::string &source) {
	if (shaderType == GPU_VERTEX_SHADER) {
		if (containsText(source, "gpu_ModelViewProjectionMatrix"))
			return SDL3GPUShaderKind::DefaultVertex;
		return SDL3GPUShaderKind::Unknown;
	}

	if (containsText(source, "constant_mask") && containsText(source, "crossfade"))
		return SDL3GPUShaderKind::BlendByMask;
	if (containsText(source, "NTEXTURES") && containsText(source, "subColors"))
		return SDL3GPUShaderKind::RenderSubtitles;
	if (containsText(source, "color.r *= color.a") && containsText(source, "color.g *= color.a") &&
	    !containsText(source, "modificationType"))
		return SDL3GPUShaderKind::MultiplyAlpha;
	if (containsText(source, "colorSrc") && containsText(source, "color.a = colorSrc.r"))
		return SDL3GPUShaderKind::MergeAlpha;
	if (containsText(source, "conversionType") && containsText(source, "YUVToRGB"))
		return SDL3GPUShaderKind::ColourConversion;
	if (containsText(source, "modificationType") && containsText(source, "replaceSrcColor"))
		return SDL3GPUShaderKind::ColorModification;
	if (containsText(source, "HORIZONTAL_BLUR_9"))
		return SDL3GPUShaderKind::BlurH;
	if (containsText(source, "VERTICAL_BLUR_9"))
		return SDL3GPUShaderKind::BlurV;
	if (containsText(source, "breakupCellforms"))
		return SDL3GPUShaderKind::Breakup;
	if (containsText(source, "uniform float alpha"))
		return SDL3GPUShaderKind::GlassSmash;
	if (containsText(source, "factor+1") && containsText(source, "cell_w"))
		return SDL3GPUShaderKind::Pixelate;
	if (containsText(source, "uniform int partial") && containsText(source, "uniform int full"))
		return SDL3GPUShaderKind::TextFade;
	if (containsText(source, "faceAscender") && containsText(source, "lightening_factor"))
		return SDL3GPUShaderKind::GlyphGradient;
	if (containsText(source, "script_width") && containsText(source, "TRVSWAVE_AMPLITUDE"))
		return SDL3GPUShaderKind::EffectTrvswave;
	if (containsText(source, "animationClock") && containsText(source, "wavelength"))
		return SDL3GPUShaderKind::EffectWarp;
	if (containsText(source, "effect_counter") && containsText(source, "OMEGA"))
		return SDL3GPUShaderKind::EffectWhirl;
	if (containsText(source, "fix crappy masks"))
		return SDL3GPUShaderKind::CropByMask;
	if (containsText(source, "insideBox") && !containsText(source, "uniform float alpha"))
		return SDL3GPUShaderKind::AlphaOutsideTextures;
	return SDL3GPUShaderKind::Unknown;
}

#if defined(ONS_USE_SDL3_SHADERCROSS)
std::string stripGLSLComments(const std::string &source) {
	std::string result;
	result.reserve(source.size());
	bool lineComment = false;
	bool blockComment = false;
	for (size_t i = 0; i < source.size(); ++i) {
		const char ch   = source[i];
		const char next = i + 1 < source.size() ? source[i + 1] : '\0';
		if (lineComment) {
			if (ch == '\n') {
				lineComment = false;
				result.push_back(ch);
			}
			continue;
		}
		if (blockComment) {
			if (ch == '*' && next == '/') {
				blockComment = false;
				++i;
			} else if (ch == '\n') {
				result.push_back(ch);
			}
			continue;
		}
		if (ch == '/' && next == '/') {
			lineComment = true;
			++i;
			continue;
		}
		if (ch == '/' && next == '*') {
			blockComment = true;
			++i;
			continue;
		}
		result.push_back(ch);
	}
	return result;
}

std::string regexReplace(const std::string &source, const char *pattern, const std::string &replacement) {
	return std::regex_replace(source, std::regex(pattern), replacement);
}

std::string trimString(const std::string &value) {
	const size_t begin = value.find_first_not_of(" \t\r\n");
	if (begin == std::string::npos)
		return "";
	const size_t end = value.find_last_not_of(" \t\r\n");
	return value.substr(begin, end - begin + 1);
}

int parsePositiveInt(const std::string &value, const std::unordered_map<std::string, int> &defines, int fallback = 1) {
	if (value.empty())
		return fallback;
	char *end = nullptr;
	const long parsed = std::strtol(value.c_str(), &end, 10);
	if (end && *end == '\0' && parsed > 0)
		return static_cast<int>(parsed);
	auto it = defines.find(value);
	return it == defines.end() ? fallback : std::max(1, it->second);
}

int samplerSlotForName(const std::string &name, int fallback) {
	if (name == "tex" || name == "u_texture")
		return 0;
	if (name == "subTex")
		return 1;
	if (name.size() > 3 && name.compare(0, 3, "tex") == 0) {
		char *end = nullptr;
		const long parsed = std::strtol(name.c_str() + 3, &end, 10);
		if (end && *end == '\0' && parsed >= 0 && parsed < 8)
			return static_cast<int>(parsed);
	}
	return fallback;
}

const char *hlslTypeName(const SDL3GPUNativeUniform &uniform) {
	if (uniform.type == SDL3GPUUniformType::Int)
		return "int";
	if (uniform.type == SDL3GPUUniformType::Bool)
		return "int";
	if (uniform.components <= 1)
		return "float";
	if (uniform.components == 2)
		return "float2";
	if (uniform.components == 3)
		return "float3";
	return "float4";
}

std::string hlslUniformMacroSuffix(const SDL3GPUNativeUniform &uniform) {
	if (uniform.type == SDL3GPUUniformType::Int || uniform.type == SDL3GPUUniformType::Bool)
		return ".x";
	if (uniform.components <= 1)
		return ".x";
	if (uniform.components == 2)
		return ".xy";
	if (uniform.components == 3)
		return ".xyz";
	return "";
}

bool parseLegacyGLSLUniforms(const std::string &source,
                             std::vector<SDL3GPUNativeUniform> &uniforms,
                             std::vector<std::pair<std::string, int>> &samplers,
                             Uint32 &samplerCount,
                             std::string &error) {
	std::unordered_map<std::string, int> defines;
	const std::regex defineRegex(R"(^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s+([0-9]+)\s*$)");
	std::istringstream lines(source);
	std::string line;
	while (std::getline(lines, line)) {
		std::smatch match;
		if (std::regex_match(line, match, defineRegex))
			defines[match[1].str()] = parsePositiveInt(match[2].str(), defines);
	}

	const std::regex uniformRegex(
	    R"(\buniform\s+(?:(?:lowp|mediump|highp)\s+)?([A-Za-z_][A-Za-z0-9_]*)\s+([^;]+);)");
	const std::regex declarationRegex(
	    R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[\s*([A-Za-z_][A-Za-z0-9_]*|[0-9]+)\s*\])?\s*$)");
	int nextSamplerSlot = 0;
	Uint32 nextRegister = 0;
	for (auto it = std::sregex_iterator(source.begin(), source.end(), uniformRegex); it != std::sregex_iterator(); ++it) {
		const std::string type = (*it)[1].str();
		const std::string declarations = (*it)[2].str();
		std::istringstream declarationStream(declarations);
		std::string declaration;
		while (std::getline(declarationStream, declaration, ',')) {
			const std::string trimmedDeclaration = trimString(declaration);
			std::smatch declarationMatch;
			if (!std::regex_match(trimmedDeclaration, declarationMatch, declarationRegex)) {
				error = "Unsupported GLSL uniform declaration in SDL3 shadercross translator: " + trimmedDeclaration;
				return false;
			}

			const std::string name = declarationMatch[1].str();
			const int arraySize    = parsePositiveInt(declarationMatch[2].str(), defines);
			if (type == "sampler2D") {
				const int slot = samplerSlotForName(name, nextSamplerSlot);
				nextSamplerSlot = std::max(nextSamplerSlot, slot + 1);
				samplers.push_back(std::make_pair(name, slot));
				samplerCount = std::max<Uint32>(samplerCount, static_cast<Uint32>(slot + 1));
				continue;
			}

			SDL3GPUNativeUniform uniform{};
			uniform.name = name;
			uniform.arraySize = arraySize;
			uniform.registerIndex = nextRegister;
			if (type == "int") {
				uniform.type = SDL3GPUUniformType::Int;
				uniform.components = 1;
			} else if (type == "bool") {
				uniform.type = SDL3GPUUniformType::Bool;
				uniform.components = 1;
			} else if (type == "float") {
				uniform.type = SDL3GPUUniformType::Float;
				uniform.components = 1;
			} else if (type == "vec2") {
				uniform.type = SDL3GPUUniformType::FloatVec;
				uniform.components = 2;
			} else if (type == "vec3") {
				uniform.type = SDL3GPUUniformType::FloatVec;
				uniform.components = 3;
			} else if (type == "vec4") {
				uniform.type = SDL3GPUUniformType::FloatVec;
				uniform.components = 4;
			} else {
				error = "Unsupported GLSL uniform type in SDL3 shadercross translator: " + type;
				return false;
			}
			nextRegister += static_cast<Uint32>(uniform.arraySize);
			uniforms.push_back(uniform);
		}
	}
	return true;
}

bool sourceUsesVarying(const std::string &source, const char *name) {
	const std::regex varyingRegex(std::string(R"(\bvarying\s+(?:(?:lowp|mediump|highp)\s+)?[A-Za-z_][A-Za-z0-9_]*\s+)") +
	                             name + R"(\s*;)");
	return std::regex_search(source, varyingRegex);
}

bool translateLegacyGLSLFragmentToHLSL(const std::string &source,
                                       std::string &hlsl,
                                       std::vector<SDL3GPUNativeUniform> &uniforms,
                                       SDL3GPUNativeResourceInfo &resources,
                                       std::string &error) {
	const std::string cleaned = stripGLSLComments(source);
	std::vector<std::pair<std::string, int>> samplers;
	Uint32 samplerCount = 0;
	if (!parseLegacyGLSLUniforms(cleaned, uniforms, samplers, samplerCount, error))
		return false;

	std::ostringstream out;
	for (const auto &sampler : samplers) {
		out << "Texture2D " << sampler.first << "Texture : register(t" << sampler.second << ", space2);\n";
		out << "SamplerState " << sampler.first << "Sampler : register(s" << sampler.second << ", space2);\n";
	}

	if (!uniforms.empty()) {
		out << "cbuffer SDL3GPUCompatUniforms : register(b0, space3) {\n";
		for (const auto &uniform : uniforms) {
			out << "\t" << hlslTypeName(uniform) << " _ons_" << uniform.name;
			if (uniform.arraySize > 1)
				out << "[" << uniform.arraySize << "]";
			out << " : packoffset(c" << uniform.registerIndex << ");\n";
		}
		out << "};\n";
		for (const auto &uniform : uniforms) {
			out << "#define " << uniform.name << " ";
			if (uniform.type == SDL3GPUUniformType::Bool && uniform.arraySize <= 1) {
				out << "(_ons_" << uniform.name << hlslUniformMacroSuffix(uniform) << " != 0)";
			} else {
				out << "_ons_" << uniform.name;
				if (uniform.arraySize <= 1)
					out << hlslUniformMacroSuffix(uniform);
			}
			out << "\n";
		}
	}

	out << "struct PSInput { float4 color : TEXCOORD0; float2 texCoord : TEXCOORD1; };\n";
	out << "struct PSOutput { float4 o_color : SV_Target; };\n";

	std::string body = cleaned;
	body = regexReplace(body, R"(^\s*#\s*version[^\n]*(?:\n|$))", "");
	body = regexReplace(body, R"(^\s*precision\s+[A-Za-z_][A-Za-z0-9_]*\s+[A-Za-z_][A-Za-z0-9_]*\s*;\s*(?:\n|$))", "");
	body = regexReplace(body, R"(\buniform\s+(?:(?:lowp|mediump|highp)\s+)?[A-Za-z_][A-Za-z0-9_]*\s+[^;]+;)", "");
	body = regexReplace(body, R"(\bvarying\s+(?:(?:lowp|mediump|highp)\s+)?[A-Za-z_][A-Za-z0-9_]*\s+[A-Za-z_][A-Za-z0-9_]*\s*;)", "");
	body = regexReplace(body, R"(\btexture2D\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,)", "$1Texture.Sample($1Sampler,");
	body = regexReplace(body, R"(\bgl_FragColor\b)", "output.o_color");
	const bool usesVaryingColor = sourceUsesVarying(cleaned, "color");
	const bool usesVaryingTexCoord = sourceUsesVarying(cleaned, "texCoord");
	if (usesVaryingColor)
		body = regexReplace(body, R"(\bcolor\b)", "input.color");
	if (usesVaryingTexCoord)
		body = regexReplace(body, R"(\btexCoord\b)", "input.texCoord");
	body = regexReplace(body, R"(\bvec2\b)", "float2");
	body = regexReplace(body, R"(\bvec3\b)", "float3");
	body = regexReplace(body, R"(\bvec4\b)", "float4");
	body = regexReplace(body, R"(\bmat4\b)", "float4x4");
	body = regexReplace(body, R"(\bmix\s*\()", "lerp(");
	body = regexReplace(body, R"(\bfract\s*\()", "frac(");
	body = regexReplace(body, R"(\bmod\s*\()", "fmod(");
	body = regexReplace(body, R"(\.stpq\b)", ".xyzw");
	body = regexReplace(body, R"(\.stp\b)", ".xyz");
	body = regexReplace(body, R"(\.st\b)", ".xy");
	body = regexReplace(body, R"(\.s\b)", ".x");
	body = regexReplace(body, R"(\.t\b)", ".y");
	body = regexReplace(body, R"(\.p\b)", ".z");
	body = regexReplace(body, R"(\.q\b)", ".w");
	body = regexReplace(body, R"(\breturn\s*;)", "return output;");

	const std::regex mainRegex(R"(\bvoid\s+main\s*\(\s*(?:void)?\s*\)\s*\{)");
	if (!std::regex_search(body, mainRegex)) {
		error = "Legacy GLSL fragment shader has no void main() entrypoint";
		return false;
	}
	body = std::regex_replace(body, mainRegex,
	                          std::string("PSOutput main(PSInput input) {") +
	                              "\n\tPSOutput output;\n\toutput.o_color = float4(0.0, 0.0, 0.0, 0.0);");
	const size_t lastBrace = body.find_last_of('}');
	if (lastBrace == std::string::npos) {
		error = "Legacy GLSL fragment shader has no closing main() brace";
		return false;
	}
	body.insert(lastBrace, "\n\treturn output;\n");

	out << body << "\n";
	hlsl = out.str();
	resources.numSamplers = samplerCount;
	resources.numUniformBuffers = uniforms.empty() ? 0 : 1;
	return true;
}

bool looksLikeHLSL(const std::string &source) {
	return containsText(source, "SV_Target") || containsText(source, "SV_POSITION") ||
	       containsText(source, "Texture2D") || containsText(source, "SamplerState") ||
	       containsText(source, "cbuffer") || containsText(source, ": register(");
}

bool looksLikeLegacyGLSL(const std::string &source) {
	return containsText(source, "gl_FragColor") || containsText(source, "texture2D") ||
	       containsText(source, "varying");
}

bool looksLikeGLSL(const std::string &source) {
	return containsText(source, "#version") || containsText(source, "layout(") ||
	       containsText(source, "gl_Position") || containsText(source, "sampler2D") ||
	       containsText(source, "vec2") || containsText(source, "vec3") ||
	       containsText(source, "vec4");
}

bool looksLikeSPIRV(const std::string &source) {
	if (source.size() < 4)
		return false;
	const auto *bytes = reinterpret_cast<const Uint8 *>(source.data());
	const Uint32 magic = static_cast<Uint32>(bytes[0]) |
	                     (static_cast<Uint32>(bytes[1]) << 8) |
	                     (static_cast<Uint32>(bytes[2]) << 16) |
	                     (static_cast<Uint32>(bytes[3]) << 24);
	return magic == 0x07230203;
}

SDL3GPUNativeResourceInfo inferHLSLResourceInfo(const std::string &source) {
	SDL3GPUNativeResourceInfo resources{};
	const std::regex samplerRegisterRegex(R"(\bregister\s*\(\s*[ts]([0-9]+))");
	for (auto it = std::sregex_iterator(source.begin(), source.end(), samplerRegisterRegex); it != std::sregex_iterator(); ++it)
		resources.numSamplers = std::max<Uint32>(resources.numSamplers, static_cast<Uint32>(parsePositiveInt((*it)[1].str(), {}, 0) + 1));
	if (resources.numSamplers == 0) {
		const std::regex textureDeclRegex(R"(\bTexture2D(?:<[^>]+>)?\s+[A-Za-z_][A-Za-z0-9_]*)");
		resources.numSamplers = static_cast<Uint32>(std::distance(std::sregex_iterator(source.begin(), source.end(), textureDeclRegex),
		                                                           std::sregex_iterator()));
	}

	const std::regex uniformRegisterRegex(R"(\bregister\s*\(\s*b([0-9]+))");
	for (auto it = std::sregex_iterator(source.begin(), source.end(), uniformRegisterRegex); it != std::sregex_iterator(); ++it)
		resources.numUniformBuffers = std::max<Uint32>(resources.numUniformBuffers, static_cast<Uint32>(parsePositiveInt((*it)[1].str(), {}, 0) + 1));
	if (resources.numUniformBuffers == 0) {
		const std::regex cbufferRegex(R"(\bcbuffer\s+[A-Za-z_][A-Za-z0-9_]*)");
		resources.numUniformBuffers = static_cast<Uint32>(std::distance(std::sregex_iterator(source.begin(), source.end(), cbufferRegex),
		                                                                 std::sregex_iterator()));
	}
	return resources;
}

SDL_GPUShaderStage toSDLShaderStage(GPU_ShaderEnum shaderType) {
	return shaderType == GPU_VERTEX_SHADER ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
}

SDL_ShaderCross_ShaderStage toShaderCrossStage(GPU_ShaderEnum shaderType) {
	return shaderType == GPU_VERTEX_SHADER ? SDL_SHADERCROSS_SHADERSTAGE_VERTEX : SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT;
}

bool ensureShaderCross() {
	if (shaderCrossInitialized)
		return true;
	if (!SDL_ShaderCross_Init()) {
		setShaderMessage(SDL_GetError());
		return false;
	}
	shaderCrossInitialized = true;
	return true;
}

SDL_GPUShader *createNativeShaderFromBytecode(GPU_ShaderEnum shaderType,
                                              const void *bytecode,
                                              size_t bytecodeSize,
                                              SDL_GPUShaderFormat format,
                                              const SDL3GPUNativeResourceInfo &resources,
                                              const char *entrypoint = "main") {
	if (!rendererState.device || !bytecode || bytecodeSize == 0)
		return nullptr;

	SDL_GPUShaderCreateInfo shaderInfo{};
	shaderInfo.code_size = bytecodeSize;
	shaderInfo.code      = static_cast<const Uint8 *>(bytecode);
	shaderInfo.entrypoint = entrypoint;
	shaderInfo.format    = format;
	shaderInfo.stage     = toSDLShaderStage(shaderType);
	shaderInfo.num_samplers = resources.numSamplers;
	shaderInfo.num_storage_textures = resources.numStorageTextures;
	shaderInfo.num_storage_buffers = resources.numStorageBuffers;
	shaderInfo.num_uniform_buffers = resources.numUniformBuffers;
	return SDL_CreateGPUShader(rendererState.device, &shaderInfo);
}

SDL3GPUNativeResourceInfo toNativeResourceInfo(const SDL_ShaderCross_GraphicsShaderResourceInfo &info) {
	SDL3GPUNativeResourceInfo resources{};
	resources.numSamplers       = info.num_samplers;
	resources.numStorageTextures = info.num_storage_textures;
	resources.numStorageBuffers  = info.num_storage_buffers;
	resources.numUniformBuffers  = info.num_uniform_buffers;
	return resources;
}

#if defined(ONS_USE_SDL3_SHADERC)
shaderc_shader_kind toShadercKind(GPU_ShaderEnum shaderType) {
	return shaderType == GPU_VERTEX_SHADER ? shaderc_glsl_vertex_shader : shaderc_glsl_fragment_shader;
}

bool compileGLSLToSPIRV(GPU_ShaderEnum shaderType, const std::string &source, std::vector<Uint8> &spirv) {
	shaderc_compiler_t compiler = shaderc_compiler_initialize();
	if (!compiler) {
		setShaderMessage("shaderc compiler initialization failed");
		return false;
	}

	shaderc_compile_options_t options = shaderc_compile_options_initialize();
	if (!options) {
		shaderc_compiler_release(compiler);
		setShaderMessage("shaderc compile options initialization failed");
		return false;
	}

	shaderc_compile_options_set_source_language(options, shaderc_source_language_glsl);
	shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_0);
	shaderc_compile_options_set_target_spirv(options, shaderc_spirv_version_1_0);
	shaderc_compile_options_set_auto_bind_uniforms(options, true);
	shaderc_compile_options_set_auto_combined_image_sampler(options, true);
	shaderc_compile_options_set_auto_map_locations(options, true);
	shaderc_compile_options_set_vulkan_rules_relaxed(options, true);

	shaderc_compilation_result_t result = shaderc_compile_into_spv(compiler,
	                                                               source.data(),
	                                                               source.size(),
	                                                               toShadercKind(shaderType),
	                                                               shaderType == GPU_VERTEX_SHADER ? "ons.vert" : "ons.frag",
	                                                               "main",
	                                                               options);
	shaderc_compile_options_release(options);
	shaderc_compiler_release(compiler);

	if (!result) {
		setShaderMessage("shaderc returned no compilation result");
		return false;
	}

	const shaderc_compilation_status status = shaderc_result_get_compilation_status(result);
	if (status != shaderc_compilation_status_success) {
		const char *message = shaderc_result_get_error_message(result);
		setShaderMessage(message && *message ? message : "shaderc GLSL compilation failed");
		shaderc_result_release(result);
		return false;
	}

	const char *bytes = shaderc_result_get_bytes(result);
	const size_t length = shaderc_result_get_length(result);
	spirv.assign(reinterpret_cast<const Uint8 *>(bytes), reinterpret_cast<const Uint8 *>(bytes) + length);
	shaderc_result_release(result);
	return !spirv.empty();
}
#endif

SDL_GPUShader *compileNativeSPIRVShader(GPU_ShaderEnum shaderType,
                                        const Uint8 *bytecode,
                                        size_t bytecodeSize,
                                        SDL3GPUNativeResourceInfo &resources) {
	if (!ensureShaderCross())
		return nullptr;

	SDL_ShaderCross_GraphicsShaderMetadata *metadata = SDL_ShaderCross_ReflectGraphicsSPIRV(bytecode, bytecodeSize, 0);
	if (!metadata) {
		setShaderMessage(SDL_GetError());
		return nullptr;
	}

	SDL_ShaderCross_SPIRV_Info spirvInfo{};
	spirvInfo.bytecode = bytecode;
	spirvInfo.bytecode_size = bytecodeSize;
	spirvInfo.entrypoint = "main";
	spirvInfo.shader_stage = toShaderCrossStage(shaderType);
	spirvInfo.props = 0;

	SDL_GPUShader *shader = SDL_ShaderCross_CompileGraphicsShaderFromSPIRV(rendererState.device, &spirvInfo, &metadata->resource_info, 0);
	resources = toNativeResourceInfo(metadata->resource_info);
	SDL_free(metadata);
	if (!shader)
		setShaderMessage(SDL_GetError());
	return shader;
}

SDL_GPUShader *compileNativeGLSLShader(GPU_ShaderEnum shaderType,
                                       const std::string &glsl,
                                       SDL3GPUNativeResourceInfo &resources) {
#if defined(ONS_USE_SDL3_SHADERC)
	std::vector<Uint8> spirv;
	if (!compileGLSLToSPIRV(shaderType, glsl, spirv))
		return nullptr;
	return compileNativeSPIRVShader(shaderType, spirv.data(), spirv.size(), resources);
#else
	(void)shaderType;
	(void)glsl;
	(void)resources;
	setShaderMessage("External SDL3 GLSL shaders require shaderc or precompiled SPIR-V");
	return nullptr;
#endif
}

SDL_GPUShader *compileNativeHLSLShader(GPU_ShaderEnum shaderType,
                                       const std::string &hlsl,
                                       SDL3GPUNativeResourceInfo &resources) {
	if (!ensureShaderCross())
		return nullptr;

	SDL_ShaderCross_HLSL_Info hlslInfo{};
	hlslInfo.source = hlsl.c_str();
	hlslInfo.entrypoint = "main";
	hlslInfo.shader_stage = toShaderCrossStage(shaderType);
	hlslInfo.props = 0;

	size_t spirvSize = 0;
	void *spirv = SDL_ShaderCross_CompileSPIRVFromHLSL(&hlslInfo, &spirvSize);
	if (spirv) {
		SDL_GPUShader *shader = compileNativeSPIRVShader(shaderType, static_cast<const Uint8 *>(spirv), spirvSize, resources);
		SDL_free(spirv);
		if (shader)
			return shader;
	}

	const SDL_GPUShaderFormat supported = rendererState.device ? SDL_GetGPUShaderFormats(rendererState.device) : SDL_GPU_SHADERFORMAT_INVALID;
	SDL_PropertiesID props = SDL_CreateProperties();
	if (props)
		SDL_SetBooleanProperty(props, SDL_SHADERCROSS_PROP_HLSL_SKIP_SPIRV_ROUNDTRIP_BOOLEAN, true);
	hlslInfo.props = props;

	SDL_GPUShader *shader = nullptr;
	if (supported & SDL_GPU_SHADERFORMAT_DXIL) {
		size_t dxilSize = 0;
		void *dxil = SDL_ShaderCross_CompileDXILFromHLSL(&hlslInfo, &dxilSize);
		if (dxil) {
			shader = createNativeShaderFromBytecode(shaderType, dxil, dxilSize, SDL_GPU_SHADERFORMAT_DXIL, resources);
			SDL_free(dxil);
		}
	}
	if (!shader && (supported & SDL_GPU_SHADERFORMAT_DXBC)) {
		size_t dxbcSize = 0;
		void *dxbc = SDL_ShaderCross_CompileDXBCFromHLSL(&hlslInfo, &dxbcSize);
		if (dxbc) {
			shader = createNativeShaderFromBytecode(shaderType, dxbc, dxbcSize, SDL_GPU_SHADERFORMAT_DXBC, resources);
			SDL_free(dxbc);
		}
	}
	if (props)
		SDL_DestroyProperties(props);

	if (!shader)
		setShaderMessage(SDL_GetError());
	return shader;
}

bool compileNativeExternalShader(GPU_ShaderEnum shaderType, const std::string &source, SDL3GPUShaderObject &object) {
	if (!rendererState.device) {
		setShaderMessage("SDL3 shadercross compilation requires an initialized GPU device");
		return false;
	}

	if (looksLikeSPIRV(source)) {
		object.nativeShader = compileNativeSPIRVShader(shaderType,
		                                              reinterpret_cast<const Uint8 *>(source.data()),
		                                              source.size(),
		                                              object.nativeResources);
		return object.nativeShader != nullptr;
	}

	std::string hlsl;
	SDL3GPUNativeResourceInfo resources{};
	if (shaderType != GPU_VERTEX_SHADER && looksLikeLegacyGLSL(source)) {
		std::string error;
		if (!translateLegacyGLSLFragmentToHLSL(source, hlsl, object.nativeUniforms, resources, error)) {
			setShaderMessage(error.c_str());
			return false;
		}
		object.translatedLegacyGLSL = true;
	} else if (looksLikeHLSL(source)) {
		hlsl = source;
		resources = inferHLSLResourceInfo(hlsl);
	} else if (looksLikeGLSL(source)) {
		object.nativeShader = compileNativeGLSLShader(shaderType, source, object.nativeResources);
		return object.nativeShader != nullptr;
	} else {
		setShaderMessage("External SDL3 shader is neither SPIR-V, HLSL, GLSL, nor a supported legacy GLSL fragment");
		return false;
	}

	object.nativeResources = resources;
	object.nativeShader = compileNativeHLSLShader(shaderType, hlsl, object.nativeResources);
	return object.nativeShader != nullptr;
}
#else
bool compileNativeExternalShader(GPU_ShaderEnum, const std::string &, SDL3GPUShaderObject &) {
	setShaderMessage("SDL3_GPU backend only supports external shaders when built with SDL_shadercross");
	return false;
}
#endif

const char *shaderKindName(SDL3GPUShaderKind kind) {
	switch (kind) {
		case SDL3GPUShaderKind::DefaultVertex: return "default vertex";
		case SDL3GPUShaderKind::AlphaOutsideTextures: return "alphaOutsideTextures.frag";
		case SDL3GPUShaderKind::BlendByMask: return "blendByMask.frag";
		case SDL3GPUShaderKind::BlurH: return "blurH.frag";
		case SDL3GPUShaderKind::BlurV: return "blurV.frag";
		case SDL3GPUShaderKind::Breakup: return "breakup.frag";
		case SDL3GPUShaderKind::ColorModification: return "colorModification.frag";
		case SDL3GPUShaderKind::ColourConversion: return "colourConversion.frag";
		case SDL3GPUShaderKind::CropByMask: return "cropByMask.frag";
		case SDL3GPUShaderKind::EffectTrvswave: return "effectTrvswave.frag";
		case SDL3GPUShaderKind::EffectWarp: return "effectWarp.frag";
		case SDL3GPUShaderKind::EffectWhirl: return "effectWhirl.frag";
		case SDL3GPUShaderKind::GlassSmash: return "glassSmash.frag";
		case SDL3GPUShaderKind::GlyphGradient: return "glyphGradient.frag";
		case SDL3GPUShaderKind::MergeAlpha: return "mergeAlpha.frag";
		case SDL3GPUShaderKind::MultiplyAlpha: return "multiplyAlpha.frag";
		case SDL3GPUShaderKind::Pixelate: return "pixelate.frag";
		case SDL3GPUShaderKind::RenderSubtitles: return "renderSubtitles.frag";
		case SDL3GPUShaderKind::TextFade: return "textFade.frag";
		case SDL3GPUShaderKind::Unknown:
		default: return "unknown shader";
	}
}

void resetTelemetryCounters() {
	const bool initialized = telemetry.initialized;
	const bool enabled     = telemetry.enabled;
	telemetry = SDL3GPUTelemetry{};
	telemetry.initialized = initialized;
	telemetry.enabled     = enabled;
	nextLiveTextureTelemetryBytes = 256ull * 1024ull * 1024ull;
}

void printAndResetTelemetry() {
	if (!telemetryEnabled() || !telemetry.hasData)
		return;

	size_t liveTextureBytes = 0;
	size_t livePixelBytes   = 0;
	liveImageMemoryTotals(liveTextureBytes, livePixelBytes);

	sendToLog(LogLevel::Info,
	          "SDL3_GPU telemetry: command_buffers=%llu texture_uploads=%llu texture_upload_bytes=%llu "
	          "readbacks=%llu readback_bytes=%llu native_fixed_draws=%llu native_fixed_vertices=%llu "
	          "native_shader_compiles=%llu compatibility_shader_compiles=%llu native_shader_draws=%llu "
	          "native_shader_vertices=%llu cpu_blit_draws=%llu cpu_blit_pixels=%llu "
	          "cpu_shader_draws=%llu cpu_shader_pixels=%llu blocking_gpu_waits=%llu "
	          "submission_fences=%llu submission_fence_waits=%llu submission_backlog_peak=%llu "
	          "native_blit_culls=%llu native_blit_clips=%llu live_images=%llu "
	          "live_texture_bytes=%llu live_cpu_pixel_bytes=%llu\n",
	          static_cast<unsigned long long>(telemetry.commandBuffersSubmitted),
	          static_cast<unsigned long long>(telemetry.textureUploads),
	          static_cast<unsigned long long>(telemetry.textureUploadBytes),
	          static_cast<unsigned long long>(telemetry.readbacks),
	          static_cast<unsigned long long>(telemetry.readbackBytes),
	          static_cast<unsigned long long>(telemetry.nativeFixedDraws),
	          static_cast<unsigned long long>(telemetry.nativeFixedVertices),
	          static_cast<unsigned long long>(telemetry.nativeShaderCompiles),
	          static_cast<unsigned long long>(telemetry.compatibilityShaderCompiles),
	          static_cast<unsigned long long>(telemetry.nativeShaderDraws),
	          static_cast<unsigned long long>(telemetry.nativeShaderVertices),
	          static_cast<unsigned long long>(telemetry.cpuBlitDraws),
	          static_cast<unsigned long long>(telemetry.cpuBlitPixels),
	          static_cast<unsigned long long>(telemetry.cpuShaderDraws),
	          static_cast<unsigned long long>(telemetry.cpuShaderPixels),
	          static_cast<unsigned long long>(telemetry.blockingGPUWaits),
	          static_cast<unsigned long long>(telemetry.submissionFences),
	          static_cast<unsigned long long>(telemetry.submissionFenceWaits),
	          static_cast<unsigned long long>(telemetry.submissionBacklogPeak),
	          static_cast<unsigned long long>(telemetry.nativeBlitCulls),
	          static_cast<unsigned long long>(telemetry.nativeBlitClips),
	          static_cast<unsigned long long>(liveTextureImages.size()),
	          static_cast<unsigned long long>(liveTextureBytes),
	          static_cast<unsigned long long>(livePixelBytes));

	for (const auto &stats : telemetry.transfers) {
		if (stats.textureUploads == 0 && stats.readbacks == 0)
			continue;

		sendToLog(LogLevel::Info,
		          "SDL3_GPU transfer telemetry: source=%s texture_uploads=%llu texture_upload_bytes=%llu "
		          "readbacks=%llu readback_bytes=%llu\n",
		          stats.source.c_str(),
		          static_cast<unsigned long long>(stats.textureUploads),
		          static_cast<unsigned long long>(stats.textureUploadBytes),
		          static_cast<unsigned long long>(stats.readbacks),
		          static_cast<unsigned long long>(stats.readbackBytes));
	}

	for (const auto &stats : telemetry.fixedDraws) {
		if (stats.nativeFixedDraws == 0)
			continue;

		sendToLog(LogLevel::Info,
		          "SDL3_GPU fixed draw telemetry: source=%s native_fixed_draws=%llu "
		          "native_fixed_vertices=%llu\n",
		          stats.source.c_str(),
		          static_cast<unsigned long long>(stats.nativeFixedDraws),
		          static_cast<unsigned long long>(stats.nativeFixedVertices));
	}

	for (size_t i = 0; i < static_cast<size_t>(SDL3GPUShaderKind::Count); ++i) {
		const auto &stats = telemetry.shaders[i];
		if (stats.nativeCompiles == 0 && stats.compatibilityCompiles == 0 &&
		    stats.nativeDraws == 0 && stats.cpuFallbackDraws == 0)
			continue;

		sendToLog(LogLevel::Info,
		          "SDL3_GPU shader telemetry: shader=%s native_compiles=%llu compatibility_compiles=%llu "
		          "native_draws=%llu cpu_fallback_draws=%llu cpu_fallback_pixels=%llu\n",
		          shaderKindName(static_cast<SDL3GPUShaderKind>(i)),
		          static_cast<unsigned long long>(stats.nativeCompiles),
		          static_cast<unsigned long long>(stats.compatibilityCompiles),
		          static_cast<unsigned long long>(stats.nativeDraws),
		          static_cast<unsigned long long>(stats.cpuFallbackDraws),
		          static_cast<unsigned long long>(stats.cpuFallbackPixels));
	}

	resetTelemetryCounters();
}

SDL3GPUProgramObject *activeProgramObject() {
	if (!rendererState.current_context_target || !rendererState.current_context_target->context)
		return nullptr;
	const Uint32 program = rendererState.current_context_target->context->current_shader_program;
	if (program == 0)
		return nullptr;
	auto it = programObjects.find(program);
	return it == programObjects.end() ? nullptr : &it->second;
}

int uniformInt(const SDL3GPUProgramObject &program, const char *name, int fallback = 0) {
	auto it = program.uniforms.find(name);
	if (it == program.uniforms.end())
		return fallback;
	if (it->second.type == SDL3GPUUniformType::Int)
		return it->second.intValue;
	return static_cast<int>(it->second.values[0]);
}

float uniformFloat(const SDL3GPUProgramObject &program, const char *name, float fallback = 0.0f) {
	auto it = program.uniforms.find(name);
	if (it == program.uniforms.end())
		return fallback;
	if (it->second.type == SDL3GPUUniformType::Int)
		return static_cast<float>(it->second.intValue);
	return it->second.values[0];
}

SDL3GPUColorF uniformVec4(const SDL3GPUProgramObject &program, const char *name, SDL3GPUColorF fallback = {}) {
	auto it = program.uniforms.find(name);
	if (it == program.uniforms.end())
		return fallback;
	if (it->second.type == SDL3GPUUniformType::Int) {
		const float v = static_cast<float>(it->second.intValue);
		return SDL3GPUColorF{v, v, v, v};
	}
	return SDL3GPUColorF{it->second.values[0], it->second.values[1], it->second.values[2], it->second.values[3]};
}

Uint32 packFloatWord(float value) {
	Uint32 word = 0;
	static_assert(sizeof(word) == sizeof(value), "Unexpected float packing size");
	std::memcpy(&word, &value, sizeof(word));
	return word;
}

Uint32 packIntWord(int value) {
	Uint32 word = 0;
	static_assert(sizeof(word) == sizeof(value), "Unexpected int packing size");
	std::memcpy(&word, &value, sizeof(word));
	return word;
}

bool parseUniformArrayElement(const std::string &name, std::string &baseName, int &element) {
	const size_t open = name.find('[');
	const size_t close = name.find(']', open == std::string::npos ? 0 : open);
	if (open == std::string::npos || close == std::string::npos || close <= open + 1)
		return false;
	baseName = name.substr(0, open);
	const std::string index = name.substr(open + 1, close - open - 1);
	char *end = nullptr;
	const long parsed = std::strtol(index.c_str(), &end, 10);
	if (!end || *end != '\0' || parsed < 0)
		return false;
	element = static_cast<int>(parsed);
	return true;
}

const SDL3GPUNativeUniform *nativeUniformForName(const SDL3GPUProgramObject &program,
                                                const std::string &name,
                                                Uint32 &registerIndex) {
	auto it = program.nativeUniformLookup.find(name);
	std::string baseName;
	int element = 0;
	if (it == program.nativeUniformLookup.end() && parseUniformArrayElement(name, baseName, element))
		it = program.nativeUniformLookup.find(baseName);
	if (it == program.nativeUniformLookup.end())
		return nullptr;

	const SDL3GPUNativeUniform &uniform = program.nativeUniforms[it->second];
	if (parseUniformArrayElement(name, baseName, element)) {
		if (baseName != uniform.name || element >= uniform.arraySize)
			return nullptr;
	} else {
		element = 0;
	}
	registerIndex = uniform.registerIndex + static_cast<Uint32>(element);
	if (registerIndex >= program.nativeUniformRegisters.size())
		return nullptr;
	return &uniform;
}

void updateNativeUniformRegister(SDL3GPUProgramObject &program,
                                 const std::string &name,
                                 const SDL3GPUUniformValue &value) {
	Uint32 registerIndex = 0;
	const SDL3GPUNativeUniform *uniform = nativeUniformForName(program, name, registerIndex);
	if (!uniform)
		return;

	SDL3GPUNativeUniformRegister &reg = program.nativeUniformRegisters[registerIndex];
	for (auto &word : reg.words)
		word = 0;

	const int components = std::clamp(value.components, 1, std::max(1, uniform->components));
	for (int i = 0; i < components; ++i) {
		if (uniform->type == SDL3GPUUniformType::Int || uniform->type == SDL3GPUUniformType::Bool) {
			const int intValue = value.type == SDL3GPUUniformType::Int ? value.intValue : static_cast<int>(value.values[i]);
			reg.words[i] = packIntWord(intValue);
		} else {
			const float floatValue = value.type == SDL3GPUUniformType::Int ? static_cast<float>(value.intValue) : value.values[i];
			reg.words[i] = packFloatWord(floatValue);
		}
	}
}

std::string indexedUniformName(const char *base, int index) {
	return std::string(base) + "[" + std::to_string(index) + "]";
}

float clampFloat(float value, float low = 0.0f, float high = 1.0f) {
	return std::clamp(value, low, high);
}

SDL3GPUColorF clampColor(SDL3GPUColorF color) {
	color.r = clampFloat(color.r);
	color.g = clampFloat(color.g);
	color.b = clampFloat(color.b);
	color.a = clampFloat(color.a);
	return color;
}

SDL3GPUColorF addColor(SDL3GPUColorF a, SDL3GPUColorF b) {
	return SDL3GPUColorF{a.r + b.r, a.g + b.g, a.b + b.b, a.a + b.a};
}

SDL3GPUColorF mulColor(SDL3GPUColorF color, float scalar) {
	return SDL3GPUColorF{color.r * scalar, color.g * scalar, color.b * scalar, color.a * scalar};
}

SDL3GPUColorF yuvToRgb(SDL3GPUColorF yuv) {
	const float y  = (yuv.r - 16.0f / 255.0f) * 1.16438f;
	const float cb = yuv.g - 128.0f / 255.0f;
	const float cr = yuv.b - 128.0f / 255.0f;
	return SDL3GPUColorF{
	    y + cr * 1.79274f,
	    y - 0.532910f * cr - 0.213250f * cb,
	    y + cb * 2.11240f,
	    1.0f};
}

void setUnsupported(const char *functionName) {
	SDL_SetError("%s is not implemented in the SDL3_GPU transition backend", functionName);
	setShaderMessage(SDL_GetError());
}

Uint32 mipLevelCountForSize(Uint16 w, Uint16 h) {
	Uint32 size = std::max<Uint32>(w, h);
	Uint32 levels = 1;
	while (size > 1) {
		size /= 2;
		++levels;
	}
	return levels;
}

void initialiseImageDefaults(GPU_Image *image, Uint16 w, Uint16 h, GPU_FormatEnum format) {
	image->w                   = w;
	image->h                   = h;
	image->base_w              = w;
	image->base_h              = h;
	image->texture_w           = w;
	image->texture_h           = h;
	image->format              = format;
	image->bytes_per_pixel     = textureBytesPerPixel(format);
	image->pitch               = image->bytes_per_pixel * w;
	image->color               = SDL_Color{255, 255, 255, 255};
	image->use_blending        = true;
	image->blend_mode          = normalBlendMode();
	image->filter_mode         = GPU_FILTER_LINEAR;
	image->snap_mode           = GPU_SNAP_POSITION_AND_DIMENSIONS;
	image->mip_level_count     = 1;
	image->anchor_x            = 0.5f;
	image->anchor_y            = 0.5f;
	image->refcount            = 1;
	image->renderer            = rendererState.device ? &rendererState : nullptr;
	image->context_target      = rendererState.current_context_target;
	image->pixels_dirty        = false;
	image->pixels_solid        = false;
	image->solid_color         = SDL_Color{0, 0, 0, 0};
	image->texture_initialized = false;
}

void registerImageTexture(GPU_Image *image) {
	if (image && image->texture) {
		liveTextureImages.insert(image);
		if (telemetryEnabled()) {
			size_t liveTextureBytes = 0;
			size_t livePixelBytes   = 0;
			liveImageMemoryTotals(liveTextureBytes, livePixelBytes);
			if (liveTextureBytes >= nextLiveTextureTelemetryBytes) {
				while (liveTextureBytes >= nextLiveTextureTelemetryBytes)
					nextLiveTextureTelemetryBytes += 256ull * 1024ull * 1024ull;
				sendToLog(LogLevel::Info,
				          "SDL3_GPU live texture telemetry: live_images=%llu live_texture_bytes=%llu live_cpu_pixel_bytes=%llu latest_image=%ux%u format=%d\n",
				          static_cast<unsigned long long>(liveTextureImages.size()),
				          static_cast<unsigned long long>(liveTextureBytes),
				          static_cast<unsigned long long>(livePixelBytes),
				          static_cast<unsigned>(image->w),
				          static_cast<unsigned>(image->h),
				          static_cast<int>(image->format));
			}
		}
	}
}

void unregisterImageTexture(GPU_Image *image) {
	if (image)
		liveTextureImages.erase(image);
}

SDL_GPUTexture *createTextureObject(const GPU_Image *image, Uint32 mipLevels) {
	SDL_GPUTextureCreateInfo textureInfo{};
	textureInfo.type                 = SDL_GPU_TEXTURETYPE_2D;
	textureInfo.format               = textureFormat(image->format);
	textureInfo.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
	textureInfo.width                = image->w;
	textureInfo.height               = image->h;
	textureInfo.layer_count_or_depth = 1;
	textureInfo.num_levels           = std::max<Uint32>(1, mipLevels);
	textureInfo.sample_count         = SDL_GPU_SAMPLECOUNT_1;

	return SDL_CreateGPUTexture(rendererState.device, &textureInfo);
}

bool createTexture(GPU_Image *image) {
	if (!image || !rendererState.device)
		return false;

	image->texture = createTextureObject(image, image->mip_level_count);
	registerImageTexture(image);
	return image->texture != nullptr;
}

bool clearImageTexture(GPU_Image *image, SDL_Color color = SDL_Color{0, 0, 0, 0}) {
	if (!image || !image->texture || !rendererState.device)
		return false;

	SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(rendererState.device);
	if (!commands)
		return false;

	SDL_GPUColorTargetInfo colorTarget{};
	colorTarget.texture     = image->texture;
	colorTarget.clear_color = SDL_FColor{
	    color.r / 255.0f,
	    color.g / 255.0f,
	    color.b / 255.0f,
	    color.a / 255.0f};
	colorTarget.load_op  = SDL_GPU_LOADOP_CLEAR;
	colorTarget.store_op = SDL_GPU_STOREOP_STORE;

	SDL_GPURenderPass *renderPass = SDL_BeginGPURenderPass(commands, &colorTarget, 1, nullptr);
	if (!renderPass) {
		SDL_CancelGPUCommandBuffer(commands);
		return false;
	}

	SDL_EndGPURenderPass(renderPass);
	const bool submitted = submitGPUCommandBuffer(commands);
	if (submitted) {
		image->texture_initialized = true;
		image->pixels_dirty        = false;
		image->pixels_solid        = true;
		image->solid_color         = color;
		image->has_mipmaps         = false;
	}
	return submitted;
}

bool ensureImageTextureInitialized(GPU_Image *image) {
	if (!image || image->texture_initialized)
		return true;
	return clearImageTexture(image);
}

Uint32 reusableBufferCapacity(Uint32 size) {
	constexpr Uint32 MinimumCapacity = 4096;
	constexpr Uint32 Alignment       = 256 * 1024;
	if (size <= MinimumCapacity)
		return MinimumCapacity;
	if (size > UINT32_MAX - (Alignment - 1))
		return size;
	return alignUp(size, Alignment);
}

Uint32 textureUploadPitchBytes(const GPU_Image *image, Uint32 rowBytes) {
	const Uint32 texelSize = std::max<Uint32>(1, image ? static_cast<Uint32>(image->bytes_per_pixel) : 1);
	const Uint32 d3d12AlignedPitch = alignUp(rowBytes, 256);
	return alignUp(d3d12AlignedPitch, texelSize);
}

void releaseReusableTransferBuffer(SDL3GPUReusableTransferBuffer &buffer) {
	if (buffer.transfer && rendererState.device)
		SDL_ReleaseGPUTransferBuffer(rendererState.device, buffer.transfer);
	buffer = SDL3GPUReusableTransferBuffer{};
}

void releaseReusableUploadedBuffer(SDL3GPUReusableUploadedBuffer &buffer) {
	if (buffer.buffer && rendererState.device)
		SDL_ReleaseGPUBuffer(rendererState.device, buffer.buffer);
	if (buffer.transfer && rendererState.device)
		SDL_ReleaseGPUTransferBuffer(rendererState.device, buffer.transfer);
	buffer = SDL3GPUReusableUploadedBuffer{};
}

bool ensureReusableTransferBuffer(SDL3GPUReusableTransferBuffer &buffer, SDL_GPUTransferBufferUsage usage, Uint32 size) {
	if (!rendererState.device || size == 0)
		return false;
	if (buffer.transfer && buffer.usage == usage && buffer.capacity >= size)
		return true;

	releaseReusableTransferBuffer(buffer);

	SDL_GPUTransferBufferCreateInfo transferInfo{};
	transferInfo.usage = usage;
	transferInfo.size  = reusableBufferCapacity(size);
	buffer.transfer    = SDL_CreateGPUTransferBuffer(rendererState.device, &transferInfo);
	if (!buffer.transfer)
		return false;
	buffer.capacity = transferInfo.size;
	buffer.usage    = usage;
	return true;
}

bool ensureReusableUploadedBuffer(SDL3GPUReusableUploadedBuffer &buffer, SDL_GPUBufferUsageFlags usage, Uint32 size) {
	if (!rendererState.device || size == 0)
		return false;
	if (buffer.buffer && buffer.transfer && buffer.usage == usage && buffer.capacity >= size) {
		buffer.size = size;
		return true;
	}

	releaseReusableUploadedBuffer(buffer);

	const Uint32 capacity = reusableBufferCapacity(size);
	SDL_GPUBufferCreateInfo bufferInfo{};
	bufferInfo.usage = usage;
	bufferInfo.size  = capacity;
	buffer.buffer    = SDL_CreateGPUBuffer(rendererState.device, &bufferInfo);
	if (!buffer.buffer)
		return false;

	SDL_GPUTransferBufferCreateInfo transferInfo{};
	transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transferInfo.size  = capacity;
	buffer.transfer    = SDL_CreateGPUTransferBuffer(rendererState.device, &transferInfo);
	if (!buffer.transfer) {
		releaseReusableUploadedBuffer(buffer);
		return false;
	}

	buffer.capacity = capacity;
	buffer.size     = size;
	buffer.usage    = usage;
	return true;
}

bool uploadImageRows(GPU_Image *image,
                     SDL_Rect bounds,
                     const Uint8 *rows,
                     Uint32 rowBytes,
                     Uint32 sourcePitch,
                     const char *telemetrySource,
                     bool cycleTexture) {
	if (!image || !image->texture || !rendererState.device || !rows || rowBytes == 0 || sourcePitch == 0)
		return false;

	bounds = clampImageUploadRect(image, bounds);
	if (bounds.w <= 0 || bounds.h <= 0)
		return true;
	if (!imageRectCoversImage(image, bounds) && !ensureImageTextureInitialized(image))
		return false;

	const Uint32 uploadPitch = textureUploadPitchBytes(image, rowBytes);
	const Uint32 uploadSize  = uploadPitch * static_cast<Uint32>(bounds.h);
	if (!ensureReusableTransferBuffer(textureUploadBuffer, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, uploadSize))
		return false;

	void *mapped = SDL_MapGPUTransferBuffer(rendererState.device, textureUploadBuffer.transfer, true);
	if (!mapped)
		return false;

	auto *dst = static_cast<Uint8 *>(mapped);
	if (sourcePitch == rowBytes && uploadPitch == rowBytes) {
		std::memcpy(dst, rows, static_cast<size_t>(rowBytes) * bounds.h);
	} else {
		for (int y = 0; y < bounds.h; ++y) {
			Uint8 *dstRow = dst + static_cast<size_t>(y) * uploadPitch;
			std::memcpy(dstRow, rows + static_cast<size_t>(y) * sourcePitch, rowBytes);
			if (uploadPitch > rowBytes)
				std::memset(dstRow + rowBytes, 0, uploadPitch - rowBytes);
		}
	}
	SDL_UnmapGPUTransferBuffer(rendererState.device, textureUploadBuffer.transfer);

	SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(rendererState.device);
	if (!commands)
		return false;

	SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(commands);
	SDL_GPUTextureTransferInfo source{};
	source.transfer_buffer = textureUploadBuffer.transfer;
	source.pixels_per_row  = uploadPitch / static_cast<Uint32>(image->bytes_per_pixel);
	source.rows_per_layer  = static_cast<Uint32>(bounds.h);

	SDL_GPUTextureRegion destination{};
	destination.texture = image->texture;
	destination.x       = static_cast<Uint32>(bounds.x);
	destination.y       = static_cast<Uint32>(bounds.y);
	destination.w       = static_cast<Uint32>(bounds.w);
	destination.h       = static_cast<Uint32>(bounds.h);
	destination.d       = 1;

	SDL_UploadToGPUTexture(copyPass, &source, &destination, cycleTexture);
	SDL_EndGPUCopyPass(copyPass);
	const bool submitted = submitGPUCommandBuffer(commands);
	if (submitted) {
		noteTextureUpload(uploadSize, telemetrySource);
		image->pixels_dirty        = false;
		image->pixels_solid        = false;
		image->texture_initialized = true;
		image->has_mipmaps         = false;
	}
	return submitted;
}

bool uploadImage(GPU_Image *image, const char *telemetrySource = "upload_image") {
	if (!image || !image->texture || !rendererState.device)
		return false;
	materializeSolidPixels(image);
	if (image->pixels.empty())
		return false;

	const SDL_Rect bounds{0, 0, image->w, image->h};
	return uploadImageRows(image,
	                       bounds,
	                       image->pixels.data(),
	                       static_cast<Uint32>(image->pitch),
	                       static_cast<Uint32>(image->pitch),
	                       telemetrySource,
	                       true);
}

SDL_Rect clampImageUploadRect(const GPU_Image *image, SDL_Rect rect) {
	SDL_Rect bounds{0, 0, 0, 0};
	if (!image)
		return bounds;

	const int x0 = std::max(0, rect.x);
	const int y0 = std::max(0, rect.y);
	const int x1 = std::min<int>(image->w, rect.x + rect.w);
	const int y1 = std::min<int>(image->h, rect.y + rect.h);
	bounds.x = x0;
	bounds.y = y0;
	bounds.w = std::max(0, x1 - x0);
	bounds.h = std::max(0, y1 - y0);
	return bounds;
}

bool imageRectCoversImage(const GPU_Image *image, const SDL_Rect &rect) {
	return image && rect.x == 0 && rect.y == 0 && rect.w == image->w && rect.h == image->h;
}

std::string clearFullTelemetrySource(const GPU_Image *image, const SDL_Rect &bounds, bool coversImage) {
	std::ostringstream out;
	out << (coversImage ? "clear_full_native_fallback_" : "clear_full_clipped_upload_");
	if (image)
		out << image->w << "x" << image->h;
	else
		out << "unknown";
	if (!coversImage)
		out << "_rect_" << bounds.w << "x" << bounds.h;
	return out.str();
}

bool uploadImageRegion(GPU_Image *image, SDL_Rect bounds, const char *telemetrySource = "upload_image_region") {
	if (!image || !image->texture || !rendererState.device)
		return false;
	materializeSolidPixels(image);
	if (image->pixels.empty())
		return false;

	bounds = clampImageUploadRect(image, bounds);
	if (bounds.w <= 0 || bounds.h <= 0)
		return true;
	if (imageRectCoversImage(image, bounds))
		return uploadImage(image, telemetrySource);
	if (!ensureImageTextureInitialized(image))
		return false;

	const Uint32 rowBytes = static_cast<Uint32>(bounds.w * image->bytes_per_pixel);
	const auto *src       = image->pixels.data() + bounds.y * image->pitch + bounds.x * image->bytes_per_pixel;
	return uploadImageRows(image,
	                       bounds,
	                       src,
	                       rowBytes,
	                       static_cast<Uint32>(image->pitch),
	                       telemetrySource,
	                       false);
}

bool downloadImage(GPU_Image *image, const char *telemetrySource = "ensure_pixels_current", bool force = false) {
	if (!image)
		return false;
	if (!ensureImagePixelStorage(image))
		return false;
	if (!image->texture_initialized)
		return true;
	if (!image->texture || !rendererState.device)
		return false;
	if (!force && !image->pixels_dirty)
		return true;

	flushNativeBlitBatch();

	const Uint32 downloadSize = static_cast<Uint32>(image->pixels.size());
	if (!ensureReusableTransferBuffer(textureDownloadBuffer, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, downloadSize))
		return false;

	SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(rendererState.device);
	if (!commands)
		return false;

	SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(commands);
	SDL_GPUTextureRegion source{};
	source.texture = image->texture;
	source.w       = image->w;
	source.h       = image->h;
	source.d       = 1;

	SDL_GPUTextureTransferInfo destination{};
	destination.transfer_buffer = textureDownloadBuffer.transfer;
	destination.pixels_per_row  = image->w;
	destination.rows_per_layer  = image->h;
	SDL_DownloadFromGPUTexture(copyPass, &source, &destination);
	SDL_EndGPUCopyPass(copyPass);

	SDL_GPUFence *fence = submitGPUCommandBufferAndAcquireFence(commands);
	if (!fence)
		return false;

	const bool waited = SDL_WaitForGPUFences(rendererState.device, true, &fence, 1);
	noteBlockingGPUWait();
	if (waited)
		retireSubmissionBacklogAfterCompletedFence();
	SDL_ReleaseGPUFence(rendererState.device, fence);
	if (!waited)
		return false;

	void *mapped = SDL_MapGPUTransferBuffer(rendererState.device, textureDownloadBuffer.transfer, false);
	if (!mapped) {
		releaseReusableTransferBuffer(textureDownloadBuffer);
		return false;
	}
	std::memcpy(image->pixels.data(), mapped, image->pixels.size());
	SDL_UnmapGPUTransferBuffer(rendererState.device, textureDownloadBuffer.transfer);
	releaseReusableTransferBuffer(textureDownloadBuffer);
	noteReadback(downloadSize, telemetrySource);
	image->pixels_dirty = false;
	image->pixels_solid = false;
	return true;
}

void ensureImagePixelsCurrent(GPU_Image *image) {
	if (image)
		flushNativeBlitBatch();
	if (image && image->pixels_dirty)
		downloadImage(image);
	if (image && image->pixels_solid)
		materializeSolidPixels(image);
	if (image && image->pixels.empty() && image->texture_initialized)
		downloadImage(image, "ensure_pixels_current", true);
}

SDL_PixelFormat canonicalSurfaceFormatForUpload(const GPU_Image *image) {
	if (!image)
		return SDL_PIXELFORMAT_UNKNOWN;
	switch (image->bytes_per_pixel) {
		case 4:
			return SDL_PIXELFORMAT_RGBA32;
		case 3:
			return SDL_PIXELFORMAT_RGB24;
		default:
			return SDL_PIXELFORMAT_UNKNOWN;
	}
}

SDL_Surface *convertSurfaceForUpload(SDL_Surface *surface, const GPU_Image *image, bool &freeSurface) {
	freeSurface = false;
	const SDL_PixelFormat format = canonicalSurfaceFormatForUpload(image);
	if (!surface || format == SDL_PIXELFORMAT_UNKNOWN)
		return surface;
	if (onsSurfacePixelFormatEnum(surface) == static_cast<Uint32>(format))
		return surface;

	SDL_Surface *converted = onsConvertSurfaceFormat(surface, static_cast<Uint32>(format), SDL_SWSURFACE);
	if (!converted)
		return nullptr;
	freeSurface = true;
	return converted;
}

void copyPixelRow(Uint8 *dst, int dstBpp, const Uint8 *src, int srcBpp, int width) {
	if (dstBpp == srcBpp) {
		std::memcpy(dst, src, static_cast<size_t>(width) * dstBpp);
		return;
	}

	for (int x = 0; x < width; ++x) {
		const Uint8 *srcPixel = src + x * srcBpp;
		Uint8 *dstPixel       = dst + x * dstBpp;

		if (dstBpp == 4) {
			dstPixel[0] = srcBpp > 0 ? srcPixel[0] : 0;
			dstPixel[1] = srcBpp > 1 ? srcPixel[1] : dstPixel[0];
			dstPixel[2] = srcBpp > 2 ? srcPixel[2] : dstPixel[0];
			dstPixel[3] = srcBpp > 3 ? srcPixel[3] : 255;
		} else if (dstBpp == 2) {
			dstPixel[0] = srcBpp > 0 ? srcPixel[0] : 0;
			dstPixel[1] = srcBpp > 3 ? srcPixel[3] : 255;
		} else if (dstBpp == 1) {
			dstPixel[0] = srcBpp > 0 ? srcPixel[0] : 0;
		}
	}
}

void materializeSolidPixels(GPU_Image *image) {
	if (!image || !image->pixels_solid)
		return;
	if (!ensureImagePixelStorage(image))
		return;

	const Uint8 source[4]{image->solid_color.r, image->solid_color.g, image->solid_color.b, image->solid_color.a};
	for (int y = 0; y < image->h; ++y) {
		auto *row = image->pixels.data() + y * image->pitch;
		if (image->bytes_per_pixel == 4) {
			std::memcpy(row, source, 4);
			int filled = 1;
			while (filled < image->w) {
				const int copyPixels = std::min(filled, static_cast<int>(image->w) - filled);
				std::memcpy(row + filled * 4, row, static_cast<size_t>(copyPixels) * 4);
				filled += copyPixels;
			}
			continue;
		}
		for (int x = 0; x < image->w; ++x)
			copyPixelRow(row + x * image->bytes_per_pixel, image->bytes_per_pixel, source, 4, 1);
	}
	image->pixels_solid = false;
}

void fillImagePixels(GPU_Image *image, SDL_Rect bounds, SDL_Color color) {
	if (!image)
		return;
	if (!ensureImagePixelStorage(image))
		return;

	bounds = clampImageUploadRect(image, bounds);
	if (bounds.w <= 0 || bounds.h <= 0)
		return;

	materializeSolidPixels(image);
	image->pixels_solid = false;

	const Uint8 source[4]{color.r, color.g, color.b, color.a};
	if (image->bytes_per_pixel == 4) {
		for (int y = bounds.y; y < bounds.y + bounds.h; ++y) {
			auto *row = image->pixels.data() + y * image->pitch + bounds.x * image->bytes_per_pixel;
			std::memcpy(row, source, 4);
			int filled = 1;
			while (filled < bounds.w) {
				const int copyPixels = std::min(filled, bounds.w - filled);
				std::memcpy(row + filled * 4, row, static_cast<size_t>(copyPixels) * 4);
				filled += copyPixels;
			}
		}
		return;
	}

	for (int y = bounds.y; y < bounds.y + bounds.h; ++y) {
		auto *row = image->pixels.data() + y * image->pitch + bounds.x * image->bytes_per_pixel;
		for (int x = 0; x < bounds.w; ++x)
			copyPixelRow(row + x * image->bytes_per_pixel, image->bytes_per_pixel, source, 4, 1);
	}
}

bool extensionIsBmp(const char *filename) {
	if (!filename)
		return false;
	const char *extension = std::strrchr(filename, '.');
	if (!extension)
		return false;
	return std::tolower(static_cast<unsigned char>(extension[1])) == 'b' &&
	       std::tolower(static_cast<unsigned char>(extension[2])) == 'm' &&
	       std::tolower(static_cast<unsigned char>(extension[3])) == 'p' &&
	       extension[4] == '\0';
}

bool extensionIsPng(const char *filename) {
	if (!filename)
		return false;
	const char *extension = std::strrchr(filename, '.');
	if (!extension)
		return false;
	return std::tolower(static_cast<unsigned char>(extension[1])) == 'p' &&
	       std::tolower(static_cast<unsigned char>(extension[2])) == 'n' &&
	       std::tolower(static_cast<unsigned char>(extension[3])) == 'g' &&
	       extension[4] == '\0';
}

void pngWriteData(png_structp png_ptr, png_bytep data, png_size_t length) {
	SDL_RWops *rwops = static_cast<SDL_RWops *>(png_get_io_ptr(png_ptr));
	if (!rwops || onsRWwrite(rwops, data, 1, length) != length)
		png_error(png_ptr, "Failed to write PNG data");
}

void pngFlushData(png_structp) {}

void finishPNGWrite(png_structp png_ptr, png_infop info_ptr, SDL_Surface *rgba, bool free_surface,
                    bool surface_locked, SDL_RWops *rwops, bool free_rwops) {
	if (png_ptr || info_ptr)
		png_destroy_write_struct(png_ptr ? &png_ptr : nullptr, info_ptr ? &info_ptr : nullptr);
	if (surface_locked)
		SDL_UnlockSurface(rgba);
	if (free_surface)
		SDL_FreeSurface(rgba);
	if (free_rwops)
		SDL_RWclose(rwops);
}

struct PNGSurfaceWrite {
	SDL_Surface *surface{nullptr};
	SDL_RWops *rwops{nullptr};
	png_bytep *rows{nullptr};
};

bool writePNGSurfaceData(png_structp png_ptr, png_infop info_ptr, const PNGSurfaceWrite *write) {
	if (setjmp(png_jmpbuf(png_ptr)))
		return false;

	png_set_write_fn(png_ptr, write->rwops, pngWriteData, pngFlushData);
	png_set_IHDR(png_ptr, info_ptr, write->surface->w, write->surface->h, 8, PNG_COLOR_TYPE_RGBA,
	             PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png_ptr, info_ptr);
	png_write_image(png_ptr, write->rows);
	png_write_end(png_ptr, info_ptr);
	return true;
}

bool saveSurfacePNG_RW(SDL_Surface *surface, SDL_RWops *rwops, bool free_rwops) {
	if (!surface || !rwops)
		return false;

	SDL_Surface *const inputSurface = surface;
	SDL_RWops *const output         = rwops;
	const bool closeOutput         = free_rwops;
	SDL_Surface *convertedSurface  = nullptr;
	const bool needsConversion     = onsSurfacePixelFormatEnum(inputSurface) != static_cast<Uint32>(SDL_PIXELFORMAT_RGBA32);
	if (needsConversion)
		convertedSurface = onsConvertSurfaceFormat(inputSurface, SDL_PIXELFORMAT_RGBA32, SDL_SWSURFACE);
	if (needsConversion && !convertedSurface) {
		if (closeOutput)
			SDL_RWclose(output);
		return false;
	}

	SDL_Surface *const rgba = convertedSurface ? convertedSurface : inputSurface;
	const bool freeSurface = convertedSurface != nullptr;
	const bool shouldLockSurface = SDL_MUSTLOCK(rgba);
	if (shouldLockSurface && !SDL_LockSurface(rgba)) {
		finishPNGWrite(nullptr, nullptr, rgba, freeSurface, false, output, closeOutput);
		return false;
	}
	const bool surfaceLocked = shouldLockSurface;

	std::vector<png_bytep> rows(static_cast<size_t>(rgba->h));
	for (int y = 0; y < rgba->h; ++y)
		rows[y] = static_cast<png_bytep>(rgba->pixels) + y * rgba->pitch;

	png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	png_infop info_ptr = png_ptr ? png_create_info_struct(png_ptr) : nullptr;
	if (!png_ptr || !info_ptr) {
		finishPNGWrite(png_ptr, info_ptr, rgba, freeSurface, surfaceLocked, output, closeOutput);
		return false;
	}

	PNGSurfaceWrite write{rgba, output, rows.data()};
	const bool saved = writePNGSurfaceData(png_ptr, info_ptr, &write);
	finishPNGWrite(png_ptr, info_ptr, rgba, freeSurface, surfaceLocked, output, closeOutput);
	return saved;
}

void releaseImageTexture(GPU_Image *image) {
	if (!image)
		return;
	if (image->texture && rendererState.device)
		SDL_ReleaseGPUTexture(rendererState.device, image->texture);
	unregisterImageTexture(image);
	image->texture             = nullptr;
	image->texture_initialized = false;
	image->has_mipmaps         = false;
	if (image->target)
		image->target->texture = nullptr;
}

void releaseAllImageTextures() {
	if (!rendererState.device) {
		liveTextureImages.clear();
		return;
	}

	std::vector<GPU_Image *> images(liveTextureImages.begin(), liveTextureImages.end());
	for (auto *image : images)
		releaseImageTexture(image);
	liveTextureImages.clear();
}

bool copyTextureBaseLevel(SDL_GPUTexture *sourceTexture, SDL_GPUTexture *destinationTexture, Uint16 width, Uint16 height) {
	if (!sourceTexture || !destinationTexture || !rendererState.device)
		return false;

	SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(rendererState.device);
	if (!commands)
		return false;

	SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(commands);
	SDL_GPUTextureLocation source{};
	source.texture = sourceTexture;
	SDL_GPUTextureLocation destination{};
	destination.texture = destinationTexture;
	SDL_CopyGPUTextureToTexture(copyPass, &source, &destination, width, height, 1, true);
	SDL_EndGPUCopyPass(copyPass);
	return submitGPUCommandBuffer(commands);
}

bool recreateImageTextureForMipmaps(GPU_Image *image) {
	if (!image || !rendererState.device)
		return false;

	const Uint32 levels = mipLevelCountForSize(image->w, image->h);
	if (levels <= 1)
		return false;
	if (image->texture && image->mip_level_count >= levels)
		return true;

	if (image->texture && image->texture_initialized) {
		SDL_GPUTexture *oldTexture = image->texture;
		SDL_GPUTexture *newTexture = createTextureObject(image, levels);
		if (newTexture && copyTextureBaseLevel(oldTexture, newTexture, image->w, image->h)) {
			SDL_ReleaseGPUTexture(rendererState.device, oldTexture);
			image->texture = newTexture;
			image->mip_level_count = levels;
			image->texture_initialized = true;
			image->has_mipmaps = false;
			if (image->target)
				image->target->texture = newTexture;
			registerImageTexture(image);
			return true;
		}
		if (newTexture)
			SDL_ReleaseGPUTexture(rendererState.device, newTexture);
	}

	ensureImagePixelsCurrent(image);
	releaseImageTexture(image);
	if (image->target)
		image->target->texture = nullptr;

	image->mip_level_count = levels;
	if (!createTexture(image))
		return false;
	if (image->target)
		image->target->texture = image->texture;

	return uploadImage(image, "generate_mipmaps");
}

void releaseNativeRendererObjects() {
	if (!rendererState.device)
		return;
	flushNativeBlitBatch();
	releaseReusableTransferBuffer(textureUploadBuffer);
	releaseReusableTransferBuffer(textureDownloadBuffer);
	releaseReusableUploadedBuffer(vertexUploadBuffer);
	releaseReusableUploadedBuffer(indexUploadBuffer);
	for (auto &entry : pipelineCache) {
		if (entry.pipeline)
			SDL_ReleaseGPUGraphicsPipeline(rendererState.device, entry.pipeline);
	}
	pipelineCache.clear();
	if (nearestSampler) {
		SDL_ReleaseGPUSampler(rendererState.device, nearestSampler);
		nearestSampler = nullptr;
	}
	if (linearSampler) {
		SDL_ReleaseGPUSampler(rendererState.device, linearSampler);
		linearSampler = nullptr;
	}
	if (solidWhiteTexture) {
		SDL_ReleaseGPUTexture(rendererState.device, solidWhiteTexture);
		solidWhiteTexture = nullptr;
	}
	if (texturedVertexShader) {
		SDL_ReleaseGPUShader(rendererState.device, texturedVertexShader);
		texturedVertexShader = nullptr;
	}
	if (texturedFragmentShader) {
		SDL_ReleaseGPUShader(rendererState.device, texturedFragmentShader);
		texturedFragmentShader = nullptr;
	}
	for (auto &entry : shaderObjects) {
		if (entry.second.nativeShader)
			SDL_ReleaseGPUShader(rendererState.device, entry.second.nativeShader);
	}
	shaderObjects.clear();
	programObjects.clear();
	uniformLocationOwners.clear();
	nextUniformLocation = 1;
	nextShaderObject = 1;
#if defined(ONS_USE_SDL3_SHADERCROSS)
	if (shaderCrossInitialized) {
		SDL_ShaderCross_Quit();
		shaderCrossInitialized = false;
	}
#endif
}

bool resizeTargetBacking(GPU_Target *target, Uint16 w, Uint16 h) {
	if (!target || !target->is_window)
		return false;

	if (!target->image) {
		target->image          = new GPU_Image{};
		target->image->target  = nullptr;
		target->image->context_target = target;
	}

	releaseImageTexture(target->image);
	target->texture = nullptr;
	initialiseImageDefaults(target->image, w, h, GPU_FORMAT_RGBA);
	target->image->renderer       = &rendererState;
	target->image->context_target = target;
	target->image->target         = nullptr;
	if (!createTexture(target->image))
		return false;

	target->texture = target->image->texture;
	return clearImageTexture(target->image);
}

bool ensureTargetBacking(GPU_Target *target) {
	if (!target)
		return false;
	if (target->image)
		return true;
	if (!target->is_window)
		return false;
	return resizeTargetBacking(target, target->w, target->h);
}

Uint8 clampByte(int value) {
	return static_cast<Uint8>(std::clamp(value, 0, 255));
}

SDL_Color readImagePixel(const GPU_Image *image, int x, int y) {
	SDL_Color color{0, 0, 0, 0};
	if (!image || x < 0 || y < 0 || x >= image->w || y >= image->h)
		return color;

	const Uint8 *pixel = image->pixels.data() + y * image->pitch + x * image->bytes_per_pixel;
	if (image->bytes_per_pixel == 1) {
		color = SDL_Color{pixel[0], pixel[0], pixel[0], 255};
	} else if (image->bytes_per_pixel == 2) {
		color = SDL_Color{pixel[0], pixel[0], pixel[0], pixel[1]};
	} else {
		color = SDL_Color{pixel[0], pixel[1], pixel[2], static_cast<Uint8>(image->format == GPU_FORMAT_RGB ? 255 : pixel[3])};
	}
	return color;
}

SDL_Color readImagePixelFromData(const GPU_Image *image, const Uint8 *pixels, int x, int y) {
	SDL_Color color{0, 0, 0, 0};
	if (!image || !pixels || x < 0 || y < 0 || x >= image->w || y >= image->h)
		return color;

	const Uint8 *pixel = pixels + y * image->pitch + x * image->bytes_per_pixel;
	if (image->bytes_per_pixel == 1) {
		color = SDL_Color{pixel[0], pixel[0], pixel[0], 255};
	} else if (image->bytes_per_pixel == 2) {
		color = SDL_Color{pixel[0], pixel[0], pixel[0], pixel[1]};
	} else {
		color = SDL_Color{pixel[0], pixel[1], pixel[2], static_cast<Uint8>(image->format == GPU_FORMAT_RGB ? 255 : pixel[3])};
	}
	return color;
}

SDL3GPUColorF toColorF(SDL_Color color) {
	return SDL3GPUColorF{
	    color.r / 255.0f,
	    color.g / 255.0f,
	    color.b / 255.0f,
	    color.a / 255.0f};
}

SDL_Color toSDLColor(SDL3GPUColorF color) {
	color = clampColor(color);
	return SDL_Color{
	    static_cast<Uint8>(std::lround(color.r * 255.0f)),
	    static_cast<Uint8>(std::lround(color.g * 255.0f)),
	    static_cast<Uint8>(std::lround(color.b * 255.0f)),
	    static_cast<Uint8>(std::lround(color.a * 255.0f))};
}

struct SDL3GPUTextureView {
	GPU_Image *image{nullptr};
	const Uint8 *pixels{nullptr};
};

SDL3GPUColorF sampleTexture(const SDL3GPUTextureView &view, float u, float v) {
	if (!view.image || !view.pixels || view.image->w <= 0 || view.image->h <= 0)
		return SDL3GPUColorF{};

	u = clampFloat(u);
	v = clampFloat(v);
	int x = static_cast<int>(u * view.image->w);
	int y = static_cast<int>(v * view.image->h);
	if (x >= view.image->w)
		x = view.image->w - 1;
	if (y >= view.image->h)
		y = view.image->h - 1;
	return toColorF(readImagePixelFromData(view.image, view.pixels, x, y));
}

SDL3GPUColorF sampleSlot(const std::array<SDL3GPUTextureView, 8> &textures, int slot, float u, float v) {
	if (slot < 0 || slot >= static_cast<int>(textures.size()))
		return SDL3GPUColorF{};
	return sampleTexture(textures[static_cast<size_t>(slot)], u, v);
}

bool insideUnitBox(float u, float v) {
	return u >= 0.0f && v >= 0.0f && u < 1.0f && v < 1.0f;
}

void writeImagePixel(GPU_Image *image, int x, int y, SDL_Color color) {
	if (!image || x < 0 || y < 0 || x >= image->w || y >= image->h)
		return;

	image->pixels_solid = false;
	Uint8 *pixel = image->pixels.data() + y * image->pitch + x * image->bytes_per_pixel;
	if (image->bytes_per_pixel == 1) {
		pixel[0] = static_cast<Uint8>((static_cast<int>(color.r) + color.g + color.b) / 3);
	} else if (image->bytes_per_pixel == 2) {
		pixel[0] = static_cast<Uint8>((static_cast<int>(color.r) + color.g + color.b) / 3);
		pixel[1] = color.a;
	} else {
		pixel[0] = color.r;
		pixel[1] = color.g;
		pixel[2] = color.b;
		pixel[3] = color.a;
	}
}

SDL_Color modulatePixel(SDL_Color color, const GPU_Image *image) {
	if (!image)
		return color;
	color.r = static_cast<Uint8>((static_cast<int>(color.r) * image->color.r + 127) / 255);
	color.g = static_cast<Uint8>((static_cast<int>(color.g) * image->color.g + 127) / 255);
	color.b = static_cast<Uint8>((static_cast<int>(color.b) * image->color.b + 127) / 255);
	color.a = static_cast<Uint8>((static_cast<int>(color.a) * image->color.a + 127) / 255);
	return color;
}

bool usesStraightAlphaBlend(const GPU_Image *image) {
	return image && image->blend_mode.source_color == GPU_FUNC_SRC_ALPHA;
}

SDL_Color blendPixel(const GPU_Image *sourceImage, SDL_Color source, SDL_Color destination) {
	if (!sourceImage || !sourceImage->use_blending)
		return source;

	if (sourceImage->blend_mode.color_equation == GPU_EQ_SUBTRACT) {
		return SDL_Color{
		    clampByte(static_cast<int>(destination.r) - source.r),
		    clampByte(static_cast<int>(destination.g) - source.g),
		    clampByte(static_cast<int>(destination.b) - source.b),
		    destination.a};
	}

	if (sourceImage->blend_mode.source_color == GPU_FUNC_DST_COLOR &&
	    sourceImage->blend_mode.dest_color == GPU_FUNC_ZERO) {
		return SDL_Color{
		    static_cast<Uint8>((static_cast<int>(source.r) * destination.r + 127) / 255),
		    static_cast<Uint8>((static_cast<int>(source.g) * destination.g + 127) / 255),
		    static_cast<Uint8>((static_cast<int>(source.b) * destination.b + 127) / 255),
		    source.a};
	}

	if (sourceImage->blend_mode.dest_color == GPU_FUNC_ONE) {
		return SDL_Color{
		    clampByte(static_cast<int>(destination.r) + source.r),
		    clampByte(static_cast<int>(destination.g) + source.g),
		    clampByte(static_cast<int>(destination.b) + source.b),
		    clampByte(static_cast<int>(destination.a) + source.a)};
	}

	const int inverseAlpha = 255 - source.a;
	const bool straightAlpha = usesStraightAlphaBlend(sourceImage);
	const int sourceR = straightAlpha ? (static_cast<int>(source.r) * source.a + 127) / 255 : source.r;
	const int sourceG = straightAlpha ? (static_cast<int>(source.g) * source.a + 127) / 255 : source.g;
	const int sourceB = straightAlpha ? (static_cast<int>(source.b) * source.a + 127) / 255 : source.b;

	return SDL_Color{
	    clampByte(sourceR + (static_cast<int>(destination.r) * inverseAlpha + 127) / 255),
	    clampByte(sourceG + (static_cast<int>(destination.g) * inverseAlpha + 127) / 255),
	    clampByte(sourceB + (static_cast<int>(destination.b) * inverseAlpha + 127) / 255),
	    clampByte(static_cast<int>(source.a) + (static_cast<int>(destination.a) * inverseAlpha + 127) / 255)};
}

bool targetAllowsPixel(const GPU_Target *target, int x, int y) {
	if (!target || !target->image || x < 0 || y < 0 || x >= target->image->w || y >= target->image->h)
		return false;
	if (!target->use_clip_rect)
		return true;
	return x >= static_cast<int>(target->clip_rect.x) &&
	       y >= static_cast<int>(target->clip_rect.y) &&
	       x < static_cast<int>(target->clip_rect.x + target->clip_rect.w) &&
	       y < static_cast<int>(target->clip_rect.y + target->clip_rect.h);
}

SDL_Rect targetPixelBounds(const GPU_Target *target, GPU_Rect rect) {
	SDL_Rect bounds{0, 0, 0, 0};
	if (!target || !target->image)
		return bounds;

	int x0 = std::max<int>(0, static_cast<int>(std::floor(rect.x)));
	int y0 = std::max<int>(0, static_cast<int>(std::floor(rect.y)));
	int x1 = std::min<int>(target->image->w, static_cast<int>(std::ceil(rect.x + rect.w)));
	int y1 = std::min<int>(target->image->h, static_cast<int>(std::ceil(rect.y + rect.h)));

	if (target->use_clip_rect) {
		x0 = std::max<int>(x0, static_cast<int>(std::floor(target->clip_rect.x)));
		y0 = std::max<int>(y0, static_cast<int>(std::floor(target->clip_rect.y)));
		x1 = std::min<int>(x1, static_cast<int>(std::ceil(target->clip_rect.x + target->clip_rect.w)));
		y1 = std::min<int>(y1, static_cast<int>(std::ceil(target->clip_rect.y + target->clip_rect.h)));
	}

	bounds.x = x0;
	bounds.y = y0;
	bounds.w = std::max(0, x1 - x0);
	bounds.h = std::max(0, y1 - y0);
	return bounds;
}

SDL_Rect imagePixelBounds(const GPU_Image *image, GPU_Rect rect) {
	SDL_Rect bounds{0, 0, 0, 0};
	if (!image)
		return bounds;

	const int x0 = std::max<int>(0, static_cast<int>(std::floor(rect.x)));
	const int y0 = std::max<int>(0, static_cast<int>(std::floor(rect.y)));
	const int x1 = std::min<int>(image->w, static_cast<int>(std::ceil(rect.x + rect.w)));
	const int y1 = std::min<int>(image->h, static_cast<int>(std::ceil(rect.y + rect.h)));

	bounds.x = x0;
	bounds.y = y0;
	bounds.w = std::max(0, x1 - x0);
	bounds.h = std::max(0, y1 - y0);
	return bounds;
}

SDL3GPUColorF evaluateColorModification(const SDL3GPUProgramObject &program,
                                        const std::array<SDL3GPUTextureView, 8> &textures,
                                        float u, float v) {
	SDL3GPUColorF color{};
	float grey = 0.0f;
	const int modificationType = uniformInt(program, "modificationType");
	const int dimension = std::max(1, uniformInt(program, "dimension", textures[0].image ? textures[0].image->w : 1));
	const float blurSize = 1.0f / static_cast<float>(dimension);

	if (modificationType == 0) {
		color = sampleSlot(textures, 0, u, v);
	} else if (modificationType == 1) {
		color = sampleSlot(textures, 0, u, v);
		grey = color.r * 0.299f + color.g * 0.587f + color.b * 0.114f;
		if (color.a != 0.0f)
			color = SDL3GPUColorF{grey * 1.2f, grey, grey * 0.8f, color.a};
	} else if (modificationType == 2 || modificationType == 3) {
		static const float weights[9]{0.05f, 0.09f, 0.12f, 0.15f, 0.16f, 0.15f, 0.12f, 0.09f, 0.05f};
		for (int i = -4; i <= 4; ++i) {
			const float du = modificationType == 2 ? i * blurSize : 0.0f;
			const float dv = modificationType == 3 ? i * blurSize : 0.0f;
			color = addColor(color, mulColor(sampleSlot(textures, 0, u + du, v + dv), weights[i + 4]));
		}
	} else if (modificationType == 4) {
		color = sampleSlot(textures, 0, u, v);
		grey = color.r * 0.299f + color.g * 0.587f + color.b * 0.114f;
		if (color.a != 0.0f)
			color = SDL3GPUColorF{grey, grey, grey, color.a};
	} else if (modificationType == 5) {
		color = sampleSlot(textures, 0, u, v);
		if (color.a != 0.0f)
			color = SDL3GPUColorF{1.0f - color.r, 1.0f - color.g, 1.0f - color.b, color.a};
	} else if (modificationType == 6) {
		color = sampleSlot(textures, 0, u, v);
		const SDL3GPUColorF darkenHue = uniformVec4(program, "darkenHue", SDL3GPUColorF{1.0f, 1.0f, 1.0f, 1.0f});
		if (color.a != 0.0f)
			color = SDL3GPUColorF{color.r * darkenHue.r, color.g * darkenHue.g, color.b * darkenHue.b, color.a};
	} else if (modificationType == 7) {
		color = sampleSlot(textures, 0, u, v);
		const SDL3GPUColorF src = uniformVec4(program, "replaceSrcColor");
		const SDL3GPUColorF dst = uniformVec4(program, "replaceDstColor");
		const float epsilon = 0.5f / 255.0f;
		if (std::fabs(color.r - src.r) <= epsilon &&
		    std::fabs(color.g - src.g) <= epsilon &&
		    std::fabs(color.b - src.b) <= epsilon) {
			color.r = dst.r;
			color.g = dst.g;
			color.b = dst.b;
		}
	}

	if (modificationType == 0 || uniformInt(program, "multiplyAlpha") == 1) {
		color.r *= color.a;
		color.g *= color.a;
		color.b *= color.a;
	}
	return color;
}

SDL3GPUColorF evaluateGaussianBlur(const SDL3GPUProgramObject &program,
                                   const std::array<SDL3GPUTextureView, 8> &textures,
                                   float u, float v, bool vertical) {
	const float sigma = std::max(uniformFloat(program, "sigma", 1.0f), 0.0001f);
	const float blurSize = uniformFloat(program, "blurSize", 0.0f);
	constexpr float pi = 3.14159265358979323846f;

	float incremental[3]{
	    1.0f / (std::sqrt(2.0f * pi) * sigma),
	    std::exp(-0.5f / (sigma * sigma)),
	    0.0f};
	incremental[2] = incremental[1] * incremental[1];

	SDL3GPUColorF avg = mulColor(sampleSlot(textures, 0, u, v), incremental[0]);
	float coefficientSum = incremental[0];
	incremental[0] *= incremental[1];
	incremental[1] *= incremental[2];

	for (float i = 1.0f; i <= 4.0f; i += 1.0f) {
		const float du = vertical ? 0.0f : i * blurSize;
		const float dv = vertical ? i * blurSize : 0.0f;
		avg = addColor(avg, mulColor(sampleSlot(textures, 0, u - du, v - dv), incremental[0]));
		avg = addColor(avg, mulColor(sampleSlot(textures, 0, u + du, v + dv), incremental[0]));
		coefficientSum += 2.0f * incremental[0];
		incremental[0] *= incremental[1];
		incremental[1] *= incremental[2];
	}

	return coefficientSum > 0.0f ? mulColor(avg, 1.0f / coefficientSum) : avg;
}

SDL3GPUColorF evaluateColourConversion(const SDL3GPUProgramObject &program,
                                       const std::array<SDL3GPUTextureView, 8> &textures,
                                       float u, float v) {
	const int conversionType = uniformInt(program, "conversionType");
	const int maskHeight = uniformInt(program, "maskHeight");
	if (maskHeight > 0 && v > 0.5f)
		return SDL3GPUColorF{};

	auto grabYUV = [&](float sampleU, float sampleV) {
		if (conversionType == 1) {
			return SDL3GPUColorF{
			    sampleSlot(textures, 0, sampleU, sampleV).r,
			    sampleSlot(textures, 1, sampleU, sampleV).r,
			    sampleSlot(textures, 2, sampleU, sampleV).r,
			    1.0f};
		}
		const SDL3GPUColorF uv = sampleSlot(textures, 1, sampleU, sampleV);
		return SDL3GPUColorF{sampleSlot(textures, 0, sampleU, sampleV).r, uv.r, uv.a, 1.0f};
	};

	SDL3GPUColorF rgba = yuvToRgb(grabYUV(u, v));
	rgba.a = 1.0f;
	if (maskHeight > 0) {
		rgba.a = yuvToRgb(grabYUV(u, v + 0.5f)).r;
		rgba.r *= rgba.a;
		rgba.g *= rgba.a;
		rgba.b *= rgba.a;
	}
	return rgba;
}

SDL3GPUColorF evaluateShaderPixel(const SDL3GPUProgramObject &program,
                                  const std::array<SDL3GPUTextureView, 8> &textures,
                                  float u, float v) {
	switch (program.kind) {
		case SDL3GPUShaderKind::AlphaOutsideTextures:
			return insideUnitBox(u, v) ? sampleSlot(textures, 0, u, v) : SDL3GPUColorF{};

		case SDL3GPUShaderKind::BlendByMask: {
			const SDL3GPUColorF img1 = sampleSlot(textures, 0, u, v);
			const SDL3GPUColorF img2 = sampleSlot(textures, 1, u, v);
			const SDL3GPUColorF mask = uniformInt(program, "constant_mask") ? SDL3GPUColorF{} : sampleSlot(textures, 2, u, v);
			const int maskValue = uniformInt(program, "mask_value");
			if (uniformInt(program, "crossfade")) {
				const float left = clampFloat((256.0f + mask.r * 256.0f - static_cast<float>(maskValue)) / 256.0f);
				return clampColor(addColor(mulColor(img1, left), mulColor(img2, 1.0f - left)));
			}
			return mask.r * 256.0f >= static_cast<float>(maskValue) ? img1 : img2;
		}

		case SDL3GPUShaderKind::BlurH:
			return evaluateGaussianBlur(program, textures, u, v, false);

		case SDL3GPUShaderKind::BlurV:
			return evaluateGaussianBlur(program, textures, u, v, true);

		case SDL3GPUShaderKind::Breakup: {
			const float tilesX = std::max(uniformFloat(program, "tilesX", 1.0f), 1.0f);
			const float tilesY = std::max(uniformFloat(program, "tilesY", 1.0f), 1.0f);
			const int breakupCellforms = std::max(1, uniformInt(program, "breakupCellforms", 1));
			const float belongsToTileX = std::floor(u * tilesX) / tilesX;
			const float belongsToTileY = std::floor(v * tilesY) / tilesY;
			const float gridRadius = sampleSlot(textures, 2, belongsToTileX, belongsToTileY).r;
			const SDL3GPUColorF source = sampleSlot(textures, 0, u, v);
			if (gridRadius >= 1.0f)
				return source;

			const int thisRadius = static_cast<int>(std::floor(gridRadius * breakupCellforms));
			const float xPercent = std::fmod(u, 1.0f / tilesX) * tilesX;
			const float yPercent = std::fmod(v, 1.0f / tilesY) * tilesY;
			const float interval = 1.0f / static_cast<float>(breakupCellforms);
			const float maskU = (thisRadius - 1) * interval + interval * xPercent;
			const float maskV = yPercent;
			return mulColor(source, sampleSlot(textures, 1, maskU, maskV).r);
		}

		case SDL3GPUShaderKind::ColorModification:
			return evaluateColorModification(program, textures, u, v);

		case SDL3GPUShaderKind::ColourConversion:
			return evaluateColourConversion(program, textures, u, v);

		case SDL3GPUShaderKind::CropByMask: {
			const SDL3GPUColorF img = sampleSlot(textures, 0, u, v);
			const float d = 1.0f / 512.0f;
			const float n = 1.0f / 9.0f;
			float mask = 0.0f;
			for (int dy = -1; dy <= 1; ++dy) {
				for (int dx = -1; dx <= 1; ++dx)
					mask += sampleSlot(textures, 1, u + dx * d, v + dy * d).r * n;
			}
			return mulColor(img, mask);
		}

		case SDL3GPUShaderKind::EffectTrvswave: {
			const int w = std::max(1, uniformInt(program, "script_width", textures[0].image ? textures[0].image->w : 1));
			const int h = std::max(1, uniformInt(program, "script_height", textures[0].image ? textures[0].image->h : 1));
			const int effectCounter = uniformInt(program, "effect_counter");
			const int duration = std::max(1, uniformInt(program, "duration", 1));
			constexpr float pi = 3.14159265358979323846f;
			constexpr float amplitudeMax = 18.0f;
			constexpr float waveEnd = 64.0f;
			constexpr float waveStart = 512.0f;
			float amplitude = 0.0f;
			float wavelength = waveStart;
			if (effectCounter * 2 < duration) {
				amplitude = amplitudeMax * static_cast<float>(2 * effectCounter) / duration;
				wavelength = 1.0f / (((1.0f / waveEnd - 1.0f / waveStart) * static_cast<float>(2 * effectCounter) / duration) + (1.0f / waveStart));
			} else {
				amplitude = amplitudeMax * static_cast<float>(2 * (duration - effectCounter)) / duration;
				wavelength = 1.0f / (((1.0f / waveEnd - 1.0f / waveStart) * static_cast<float>(2 * (duration - effectCounter)) / duration) + (1.0f / waveStart));
			}
			const int i = static_cast<int>(u * w);
			const int j = static_cast<int>(v * h);
			int ii = i + static_cast<int>(amplitude * std::sin(pi * 2.0f * static_cast<float>(j) / wavelength));
			ii = std::clamp(ii, 0, w - 1);
			return sampleSlot(textures, 0, static_cast<float>(ii) / w, static_cast<float>(j) / h);
		}

		case SDL3GPUShaderKind::EffectWarp: {
			constexpr float pi = 3.14159265358f;
			float uvx = u * uniformFloat(program, "cx", 1.0f);
			float uvy = v * uniformFloat(program, "cy", 1.0f);
			const float x = uvx * 2.0f - 1.0f;
			const float y = uvy * 2.0f - 1.0f;
			const float radius = std::sqrt(x * x + y * y);
			float phi = std::atan2(y, x);
			const float cyclePerSec = uniformFloat(program, "animationClock") * pi * 2.0f;
			const float amplitude = uniformFloat(program, "amplitude");
			const float wavelength = std::max(uniformFloat(program, "wavelength", 1.0f), 0.0001f);
			const float speed = uniformFloat(program, "speed");
			phi += (amplitude * pi / 1000.0f) * std::sin((cyclePerSec * speed * 0.06f) + (1000.0f * 1920.0f * pi * radius) / (1080.0f * wavelength));
			uvx = (radius * std::cos(phi) + 1.0f) / 2.0f;
			uvy = (radius * std::sin(phi) + 1.0f) / 2.0f;
			const float cx = std::max(uniformFloat(program, "cx", 1.0f), 0.0001f);
			const float cy = std::max(uniformFloat(program, "cy", 1.0f), 0.0001f);
			return sampleSlot(textures, 0, clampFloat(uvx) / cx, clampFloat(uvy) / cy);
		}

		case SDL3GPUShaderKind::EffectWhirl: {
			const int effectCounter = uniformInt(program, "effect_counter");
			const int duration = std::max(1, uniformInt(program, "duration", 1));
			const int direction = uniformInt(program, "direction");
			const float renderW = std::max(uniformFloat(program, "render_width", textures[0].image ? textures[0].image->w : 1), 1.0f);
			const float renderH = std::max(uniformFloat(program, "render_height", textures[0].image ? textures[0].image->h : 1), 1.0f);
			const float textureW = std::max(uniformFloat(program, "texture_width", textures[0].image ? textures[0].image->w : 1), 1.0f);
			const float textureH = std::max(uniformFloat(program, "texture_height", textures[0].image ? textures[0].image->h : 1), 1.0f);
			constexpr float pi = 3.14159265358979323846f;
			constexpr float omega = pi / 64.0f;
			const float t = static_cast<float>(effectCounter) * pi / static_cast<float>(duration * 2);
			float radAmp = std::sin(2.0f * t);
			float radBase = 0.0f;
			int d = -1;
			if (direction == -1 || direction == 1) {
				radBase = 4.0f * t;
			} else if (direction == -2 || direction == 2) {
				const float oneMinusCos = 1.0f - std::cos(t);
				radAmp = pi * (std::sin(t) - oneMinusCos);
				radBase = pi * 2.0f * oneMinusCos + radAmp;
			}
			const float centerX = renderW / 2.0f;
			const float centerY = renderH / 2.0f;
			const float x = u * textureW - centerX;
			const float y = v * textureH - centerY;
			const float theta = static_cast<float>(d) * (radBase + radAmp * std::sin(std::sqrt(x * x + y * y) * omega));
			const float i = clampFloat(x * std::cos(theta) - y * std::sin(theta) + centerX, 0.0f, renderW - 1.0f);
			const float j = clampFloat(x * std::sin(theta) + y * std::cos(theta) + centerY, 0.0f, renderH - 1.0f);
			return sampleSlot(textures, 0, i / textureW, j / textureH);
		}

		case SDL3GPUShaderKind::GlassSmash:
			return insideUnitBox(u, v) ? mulColor(sampleSlot(textures, 0, u, v), uniformFloat(program, "alpha", 1.0f)) : SDL3GPUColorF{};

		case SDL3GPUShaderKind::GlyphGradient: {
			SDL3GPUColorF source = sampleSlot(textures, 0, u, v);
			const SDL3GPUColorF color = uniformVec4(program, "color");
			const int height = std::max(1, uniformInt(program, "height", textures[0].image ? textures[0].image->h : 1));
			const int maxy = uniformInt(program, "maxy");
			const int faceAscender = std::max(1, uniformInt(program, "faceAscender", 1));
			const float currentY = static_cast<float>(height) * (1.0f - v);
			const float currentAboveBaseline = currentY - static_cast<float>(height - maxy);
			const float percentAboveBaseline = std::max(0.0f, currentAboveBaseline / static_cast<float>(faceAscender));
			const bool isWhite = std::fabs(color.r - 1.0f) <= 0.5f / 255.0f &&
			                     std::fabs(color.g - 1.0f) <= 0.5f / 255.0f &&
			                     std::fabs(color.b - 1.0f) <= 0.5f / 255.0f;
			const float lightening = 0.5f * (isWhite ? (percentAboveBaseline - 0.6f) : (0.65f - percentAboveBaseline));
			source.r = (color.r + lightening) * source.a;
			source.g = (color.g + lightening) * source.a;
			source.b = (color.b + lightening) * source.a;
			return source;
		}

		case SDL3GPUShaderKind::MergeAlpha: {
			SDL3GPUColorF color = sampleSlot(textures, 0, u, v);
			const float alpha = sampleSlot(textures, 1, u, v).r;
			color.a = alpha;
			color.r *= alpha;
			color.g *= alpha;
			color.b *= alpha;
			return color;
		}

		case SDL3GPUShaderKind::MultiplyAlpha: {
			SDL3GPUColorF color = sampleSlot(textures, 0, u, v);
			color.r *= color.a;
			color.g *= color.a;
			color.b *= color.a;
			return color;
		}

		case SDL3GPUShaderKind::Pixelate: {
			const float width = std::max(static_cast<float>(uniformInt(program, "width", textures[0].image ? textures[0].image->w : 1)), 1.0f);
			const float height = std::max(static_cast<float>(uniformInt(program, "height", textures[0].image ? textures[0].image->h : 1)), 1.0f);
			const float factor = static_cast<float>(uniformInt(program, "factor"));
			const float cellW = (factor + 1.0f) / width;
			const float cellH = (factor + 1.0f) / height;
			const float sampleU = cellW * std::floor(u / cellW + 0.5f);
			const float sampleV = cellH * std::floor(v / cellH + 0.5f);
			SDL3GPUColorF color = sampleSlot(textures, 0, sampleU, sampleV);
			color.a = 1.0f;
			return color;
		}

		case SDL3GPUShaderKind::RenderSubtitles: {
			SDL3GPUColorF result{};
			const int ntextures = std::clamp(uniformInt(program, "ntextures"), 0, 8);
			const SDL3GPUColorF dstDims = uniformVec4(program, "dstDims", SDL3GPUColorF{textures[0].image ? static_cast<float>(textures[0].image->w) : 1.0f, textures[0].image ? static_cast<float>(textures[0].image->h) : 1.0f, 0.0f, 0.0f});
			const float absX = dstDims.r * u;
			const float absY = dstDims.g * v;
			for (int i = 0; i < ntextures; ++i) {
				const SDL3GPUColorF dims = uniformVec4(program, indexedUniformName("subDims", i).c_str());
				const SDL3GPUColorF coords = uniformVec4(program, indexedUniformName("subCoords", i).c_str());
				if (absX >= coords.r && absY >= coords.g && absX <= coords.r + dims.r && absY <= coords.g + dims.g) {
					const float sampleU = (absX - coords.r) / 2048.0f;
					const float sampleV = (absY - coords.g + static_cast<float>(i) * 256.0f) / (256.0f * 8.0f);
					const SDL3GPUColorF subColor = uniformVec4(program, indexedUniformName("subColors", i).c_str());
					const float alpha = sampleSlot(textures, 1, sampleU, sampleV).r * subColor.a;
					const SDL3GPUColorF col{alpha * subColor.r, alpha * subColor.g, alpha * subColor.b, alpha};
					result = addColor(col, mulColor(result, 1.0f - col.a));
				}
			}
			return result;
		}

		case SDL3GPUShaderKind::TextFade: {
			SDL3GPUColorF color = sampleSlot(textures, 0, u, v);
			const int current = static_cast<int>(u * static_cast<float>(uniformInt(program, "width", textures[0].image ? textures[0].image->w : 1)));
			const int full = uniformInt(program, "full");
			const int partial = uniformInt(program, "partial");
			if (current > full) {
				if (current >= full + partial)
					return SDL3GPUColorF{};
				if (partial > 0)
					color = mulColor(color, 1.0f - (static_cast<float>(current - full) / partial));
			}
			return color;
		}

		case SDL3GPUShaderKind::DefaultVertex:
		case SDL3GPUShaderKind::Unknown:
		default:
			return sampleSlot(textures, 0, u, v);
	}
}

void cpuBlit(GPU_Image *image, GPU_Rect *src_rect, GPU_Target *target, float x, float y,
             float degrees, float scaleX, float scaleY) {
	if (!image || !ensureTargetBacking(target) || scaleX == 0.0f || scaleY == 0.0f)
		return;
	GPU_TelemetryScope telemetryScope("cpu_blit_fallback");
	ensureImagePixelsCurrent(image);
	ensureImagePixelsCurrent(target->image);

	const float srcX = src_rect ? src_rect->x : 0.0f;
	const float srcY = src_rect ? src_rect->y : 0.0f;
	const float srcW = src_rect ? src_rect->w : static_cast<float>(image->w);
	const float srcH = src_rect ? src_rect->h : static_cast<float>(image->h);
	if (srcW <= 0.0f || srcH <= 0.0f)
		return;

	const float destW = std::abs(srcW * scaleX);
	const float destH = std::abs(srcH * scaleY);
	if (destW < 1.0f || destH < 1.0f)
		return;

	const int x0 = static_cast<int>(std::floor(x - destW * 0.5f));
	const int y0 = static_cast<int>(std::floor(y - destH * 0.5f));
	const int x1 = static_cast<int>(std::ceil(x + destW * 0.5f));
	const int y1 = static_cast<int>(std::ceil(y + destH * 0.5f));
	constexpr float pi = 3.14159265358979323846f;
	const float radians = -degrees * pi / 180.0f;
	const float cosTheta = std::cos(radians);
	const float sinTheta = std::sin(radians);
	const bool rotated = std::abs(degrees) > 0.0001f;
	Uint64 blendedPixels = 0;

	for (int dstY = y0; dstY < y1; ++dstY) {
		for (int dstX = x0; dstX < x1; ++dstX) {
			if (!targetAllowsPixel(target, dstX, dstY))
				continue;

			float localX = dstX + 0.5f - x;
			float localY = dstY + 0.5f - y;
			if (rotated) {
				const float unrotatedX = cosTheta * localX - sinTheta * localY;
				const float unrotatedY = sinTheta * localX + cosTheta * localY;
				localX = unrotatedX;
				localY = unrotatedY;
			}

			const float sourceLocalX = localX / scaleX + srcW * 0.5f;
			const float sourceLocalY = localY / scaleY + srcH * 0.5f;
			if (sourceLocalX < 0.0f || sourceLocalY < 0.0f || sourceLocalX >= srcW || sourceLocalY >= srcH)
				continue;

			const int sampleX = static_cast<int>(srcX + sourceLocalX);
			const int sampleY = static_cast<int>(srcY + sourceLocalY);
			SDL_Color source = modulatePixel(readImagePixel(image, sampleX, sampleY), image);
			if (source.a == 0 && image->use_blending)
				continue;

			const SDL_Color destination = readImagePixel(target->image, dstX, dstY);
			writeImagePixel(target->image, dstX, dstY, blendPixel(image, source, destination));
			++blendedPixels;
		}
	}

	if (blendedPixels > 0)
		noteCpuBlit(blendedPixels);
	uploadImage(target->image, "cpu_blit_fallback");
}

bool cpuShaderBlit(const SDL3GPUProgramObject &program, GPU_Image *image, GPU_Rect *src_rect, GPU_Target *target,
                   float x, float y, float degrees, float scaleX, float scaleY) {
	if (program.kind == SDL3GPUShaderKind::Unknown || program.kind == SDL3GPUShaderKind::DefaultVertex)
		return false;
	if (!image || !ensureTargetBacking(target) || scaleX == 0.0f || scaleY == 0.0f)
		return false;
	GPU_TelemetryScope telemetryScope("cpu_shader_fallback");

	std::array<SDL3GPUTextureView, 8> textures{};
	std::array<std::vector<Uint8>, 8> snapshots{};
	textures[0].image = image;
	for (size_t i = 1; i < textures.size(); ++i)
		textures[i].image = program.images[i];

	for (size_t i = 0; i < textures.size(); ++i) {
		if (!textures[i].image)
			continue;
		ensureImagePixelsCurrent(textures[i].image);
		if (textures[i].image == target->image) {
			snapshots[i] = textures[i].image->pixels;
			textures[i].pixels = snapshots[i].data();
		} else {
			textures[i].pixels = textures[i].image->pixels.data();
		}
	}
	ensureImagePixelsCurrent(target->image);

	const float srcX = src_rect ? src_rect->x : 0.0f;
	const float srcY = src_rect ? src_rect->y : 0.0f;
	const float srcW = src_rect ? src_rect->w : static_cast<float>(image->w);
	const float srcH = src_rect ? src_rect->h : static_cast<float>(image->h);
	if (srcW <= 0.0f || srcH <= 0.0f)
		return false;

	const float destW = std::abs(srcW * scaleX);
	const float destH = std::abs(srcH * scaleY);
	if (destW < 1.0f || destH < 1.0f)
		return false;

	const int x0 = static_cast<int>(std::floor(x - destW * 0.5f));
	const int y0 = static_cast<int>(std::floor(y - destH * 0.5f));
	const int x1 = static_cast<int>(std::ceil(x + destW * 0.5f));
	const int y1 = static_cast<int>(std::ceil(y + destH * 0.5f));
	constexpr float pi = 3.14159265358979323846f;
	const float radians = -degrees * pi / 180.0f;
	const float cosTheta = std::cos(radians);
	const float sinTheta = std::sin(radians);
	const bool rotated = std::abs(degrees) > 0.0001f;
	Uint64 evaluatedPixels = 0;

	for (int dstY = y0; dstY < y1; ++dstY) {
		for (int dstX = x0; dstX < x1; ++dstX) {
			if (!targetAllowsPixel(target, dstX, dstY))
				continue;

			float localX = dstX + 0.5f - x;
			float localY = dstY + 0.5f - y;
			if (rotated) {
				const float unrotatedX = cosTheta * localX - sinTheta * localY;
				const float unrotatedY = sinTheta * localX + cosTheta * localY;
				localX = unrotatedX;
				localY = unrotatedY;
			}

			const float sourceLocalX = localX / scaleX + srcW * 0.5f;
			const float sourceLocalY = localY / scaleY + srcH * 0.5f;
			if (sourceLocalX < 0.0f || sourceLocalY < 0.0f || sourceLocalX >= srcW || sourceLocalY >= srcH)
				continue;

			const float u = (srcX + sourceLocalX) / static_cast<float>(image->w);
			const float v = (srcY + sourceLocalY) / static_cast<float>(image->h);
			const SDL_Color source = toSDLColor(evaluateShaderPixel(program, textures, u, v));
			++evaluatedPixels;
			if (source.a == 0 && image->use_blending)
				continue;

			const SDL_Color destination = readImagePixel(target->image, dstX, dstY);
			writeImagePixel(target->image, dstX, dstY, blendPixel(image, source, destination));
		}
	}

	if (evaluatedPixels > 0)
		noteCpuShaderFallback(program.kind, evaluatedPixels);
	uploadImage(target->image, "cpu_shader_fallback");
	return true;
}

float edgeFunction(const SDL3GPUVertex &a, const SDL3GPUVertex &b, float x, float y) {
	return (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
}

bool cpuShaderTriangles(const SDL3GPUProgramObject &program, GPU_Image *image, GPU_Target *target,
                        const SDL3GPUVertex *vertices, Uint32 numVertices,
                        const Uint16 *indices, Uint32 numIndices) {
	if (program.kind == SDL3GPUShaderKind::Unknown || program.kind == SDL3GPUShaderKind::DefaultVertex)
		return false;
	if (!image || !target || !vertices || !indices || numVertices == 0 || numIndices < 3 || !ensureTargetBacking(target))
		return false;
	GPU_TelemetryScope telemetryScope("cpu_shader_fallback");

	std::array<SDL3GPUTextureView, 8> textures{};
	std::array<std::vector<Uint8>, 8> snapshots{};
	textures[0].image = image;
	for (size_t i = 1; i < textures.size(); ++i)
		textures[i].image = program.images[i];

	for (size_t i = 0; i < textures.size(); ++i) {
		if (!textures[i].image)
			continue;
		ensureImagePixelsCurrent(textures[i].image);
		if (textures[i].image == target->image) {
			snapshots[i] = textures[i].image->pixels;
			textures[i].pixels = snapshots[i].data();
		} else {
			textures[i].pixels = textures[i].image->pixels.data();
		}
	}
	ensureImagePixelsCurrent(target->image);
	Uint64 evaluatedPixels = 0;

	for (Uint32 i = 0; i + 2 < numIndices; i += 3) {
		if (indices[i] >= numVertices || indices[i + 1] >= numVertices || indices[i + 2] >= numVertices)
			continue;

		const SDL3GPUVertex &a = vertices[indices[i]];
		const SDL3GPUVertex &b = vertices[indices[i + 1]];
		const SDL3GPUVertex &c = vertices[indices[i + 2]];
		const float area = edgeFunction(a, b, c.x, c.y);
		if (std::fabs(area) <= 0.00001f)
			continue;

		const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
		const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
		const int x1 = std::min<int>(target->image->w, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
		const int y1 = std::min<int>(target->image->h, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));

		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				if (!targetAllowsPixel(target, x, y))
					continue;

				const float px = x + 0.5f;
				const float py = y + 0.5f;
				const float w0 = edgeFunction(b, c, px, py) / area;
				const float w1 = edgeFunction(c, a, px, py) / area;
				const float w2 = edgeFunction(a, b, px, py) / area;
				constexpr float epsilon = -0.0001f;
				if (w0 < epsilon || w1 < epsilon || w2 < epsilon)
					continue;

				const float u = w0 * a.s + w1 * b.s + w2 * c.s;
				const float v = w0 * a.t + w1 * b.t + w2 * c.t;
				SDL3GPUColorF fragment = evaluateShaderPixel(program, textures, u, v);
				++evaluatedPixels;
				fragment.r *= w0 * a.r + w1 * b.r + w2 * c.r;
				fragment.g *= w0 * a.g + w1 * b.g + w2 * c.g;
				fragment.b *= w0 * a.b + w1 * b.b + w2 * c.b;
				fragment.a *= w0 * a.a + w1 * b.a + w2 * c.a;

				const SDL_Color source = toSDLColor(fragment);
				if (source.a == 0 && image->use_blending)
					continue;
				const SDL_Color destination = readImagePixel(target->image, x, y);
				writeImagePixel(target->image, x, y, blendPixel(image, source, destination));
			}
		}
	}

	if (evaluatedPixels > 0)
		noteCpuShaderFallback(program.kind, evaluatedPixels);
	uploadImage(target->image, "cpu_shader_fallback");
	return true;
}

bool prepareUploadedBuffer(SDL3GPUReusableUploadedBuffer &uploaded, SDL_GPUBufferUsageFlags usage, const void *data, Uint32 size) {
	if (!rendererState.device || !data || size == 0)
		return false;

	if (!ensureReusableUploadedBuffer(uploaded, usage, size))
		return false;

	void *mapped = SDL_MapGPUTransferBuffer(rendererState.device, uploaded.transfer, true);
	if (!mapped)
		return false;
	std::memcpy(mapped, data, size);
	SDL_UnmapGPUTransferBuffer(rendererState.device, uploaded.transfer);
	return true;
}

void encodeBufferUpload(SDL_GPUCopyPass *copyPass, const SDL3GPUReusableUploadedBuffer &uploaded) {
	SDL_GPUTransferBufferLocation source{};
	source.transfer_buffer = uploaded.transfer;
	SDL_GPUBufferRegion destination{};
	destination.buffer = uploaded.buffer;
	destination.size   = uploaded.size;
	SDL_UploadToGPUBuffer(copyPass, &source, &destination, true);
}

bool ensureSolidWhiteTexture() {
	if (solidWhiteTexture)
		return true;
	if (!rendererState.device)
		return false;

	SDL_GPUTextureCreateInfo textureInfo{};
	textureInfo.type                 = SDL_GPU_TEXTURETYPE_2D;
	textureInfo.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	textureInfo.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
	textureInfo.width                = 1;
	textureInfo.height               = 1;
	textureInfo.layer_count_or_depth = 1;
	textureInfo.num_levels           = 1;
	textureInfo.sample_count         = SDL_GPU_SAMPLECOUNT_1;

	SDL_GPUTexture *texture = SDL_CreateGPUTexture(rendererState.device, &textureInfo);
	if (!texture)
		return false;

	const Uint8 white[4]{255, 255, 255, 255};
	if (!ensureReusableTransferBuffer(textureUploadBuffer, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, sizeof(white))) {
		SDL_ReleaseGPUTexture(rendererState.device, texture);
		return false;
	}

	void *mapped = SDL_MapGPUTransferBuffer(rendererState.device, textureUploadBuffer.transfer, true);
	if (!mapped) {
		SDL_ReleaseGPUTexture(rendererState.device, texture);
		return false;
	}
	std::memcpy(mapped, white, sizeof(white));
	SDL_UnmapGPUTransferBuffer(rendererState.device, textureUploadBuffer.transfer);

	SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(rendererState.device);
	if (!commands) {
		SDL_ReleaseGPUTexture(rendererState.device, texture);
		return false;
	}

	SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(commands);
	SDL_GPUTextureTransferInfo source{};
	source.transfer_buffer = textureUploadBuffer.transfer;
	source.pixels_per_row  = 1;
	source.rows_per_layer  = 1;

	SDL_GPUTextureRegion destination{};
	destination.texture = texture;
	destination.w       = 1;
	destination.h       = 1;
	destination.d       = 1;
	SDL_UploadToGPUTexture(copyPass, &source, &destination, true);
	SDL_EndGPUCopyPass(copyPass);

	if (!submitGPUCommandBuffer(commands)) {
		SDL_ReleaseGPUTexture(rendererState.device, texture);
		return false;
	}

	solidWhiteTexture = texture;
	return true;
}

bool nativeSolidRect(GPU_Target *target, const SDL_Rect &bounds, SDL_Color color) {
	if (!rendererState.device || !target || !target->image || bounds.w <= 0 || bounds.h <= 0)
		return false;
	if (!ensureTargetBacking(target) || !target->texture || !target->image->texture)
		return false;
	if (!isNativeTextureFormat(target->image->format))
		return false;
	if (!ensureImageTextureInitialized(target->image) || !ensureNativeShaders() || !ensureSolidWhiteTexture())
		return false;

	SDL_GPUGraphicsPipeline *pipeline = getPipeline(textureFormat(target->image->format), false, normalBlendMode());
	SDL_GPUSampler *sampler = getSampler(GPU_FILTER_NEAREST);
	if (!pipeline || !sampler)
		return false;

	const float r = color.r / 255.0f;
	const float g = color.g / 255.0f;
	const float b = color.b / 255.0f;
	const float a = color.a / 255.0f;
	const float x0 = static_cast<float>(bounds.x);
	const float y0 = static_cast<float>(bounds.y);
	const float x1 = static_cast<float>(bounds.x + bounds.w);
	const float y1 = static_cast<float>(bounds.y + bounds.h);

	const std::array<SDL3GPUVertex, 4> vertices{{
	    SDL3GPUVertex{x0, y0, r, g, b, a, 0.0f, 0.0f},
	    SDL3GPUVertex{x1, y0, r, g, b, a, 0.0f, 0.0f},
	    SDL3GPUVertex{x0, y1, r, g, b, a, 0.0f, 0.0f},
	    SDL3GPUVertex{x1, y1, r, g, b, a, 0.0f, 0.0f},
	}};
	const Uint16 indices[6]{0, 1, 2, 2, 1, 3};
	const Uint32 vertexBytes = static_cast<Uint32>(vertices.size() * sizeof(SDL3GPUVertex));
	const Uint32 indexBytes  = static_cast<Uint32>(sizeof(indices));
	if (!prepareUploadedBuffer(vertexUploadBuffer, SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(), vertexBytes) ||
	    !prepareUploadedBuffer(indexUploadBuffer, SDL_GPU_BUFFERUSAGE_INDEX, indices, indexBytes)) {
		return false;
	}

	SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(rendererState.device);
	if (!commands)
		return false;

	SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(commands);
	encodeBufferUpload(copyPass, vertexUploadBuffer);
	encodeBufferUpload(copyPass, indexUploadBuffer);
	SDL_EndGPUCopyPass(copyPass);

	SDL_GPUColorTargetInfo colorTarget{};
	colorTarget.texture  = target->texture;
	colorTarget.load_op  = SDL_GPU_LOADOP_LOAD;
	colorTarget.store_op = SDL_GPU_STOREOP_STORE;

	SDL_GPURenderPass *renderPass = SDL_BeginGPURenderPass(commands, &colorTarget, 1, nullptr);
	if (!renderPass) {
		SDL_CancelGPUCommandBuffer(commands);
		return false;
	}

	SDL_BindGPUGraphicsPipeline(renderPass, pipeline);

	SDL_GPUBufferBinding vertexBinding{};
	vertexBinding.buffer = vertexUploadBuffer.buffer;
	SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);

	SDL_GPUBufferBinding indexBinding{};
	indexBinding.buffer = indexUploadBuffer.buffer;
	SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);

	SDL_GPUTextureSamplerBinding samplerBinding{};
	samplerBinding.texture = solidWhiteTexture;
	samplerBinding.sampler = sampler;
	SDL_BindGPUFragmentSamplers(renderPass, 0, &samplerBinding, 1);

	struct VertexUniforms {
		float mvp[4][4];
	} vertexUniforms{};
	vertexUniforms.mvp[0][0] = 2.0f / static_cast<float>(target->image->w);
	vertexUniforms.mvp[1][1] = -2.0f / static_cast<float>(target->image->h);
	vertexUniforms.mvp[2][2] = 1.0f;
	vertexUniforms.mvp[3][0] = -1.0f;
	vertexUniforms.mvp[3][1] = 1.0f;
	vertexUniforms.mvp[3][3] = 1.0f;
	SDL_PushGPUVertexUniformData(commands, 0, &vertexUniforms, sizeof(vertexUniforms));

	const float colorScale = 1.0f;
	SDL_PushGPUFragmentUniformData(commands, 0, &colorScale, sizeof(colorScale));

	SDL_GPUViewport viewport{};
	viewport.w = static_cast<float>(target->image->w);
	viewport.h = static_cast<float>(target->image->h);
	viewport.min_depth = 0.0f;
	viewport.max_depth = 1.0f;
	SDL_SetGPUViewport(renderPass, &viewport);
	SDL_SetGPUScissor(renderPass, &bounds);
	SDL_DrawGPUIndexedPrimitives(renderPass, static_cast<Uint32>(sizeof(indices) / sizeof(indices[0])), 1, 0, 0, 0);
	SDL_EndGPURenderPass(renderPass);

	const bool submitted = submitGPUCommandBuffer(commands);
	if (submitted) {
		noteNativeFixedDraw(vertices.size());
		target->image->pixels_dirty        = true;
		target->image->pixels_solid        = false;
		target->image->texture_initialized = true;
		target->image->has_mipmaps         = false;
	}
	return submitted;
}

SDL_Rect targetScissor(const GPU_Target *target) {
	SDL_Rect scissor{0, 0, target ? target->w : 0, target ? target->h : 0};
	if (!target || !target->use_clip_rect)
		return scissor;

	const int x0 = std::max<int>(0, static_cast<int>(std::floor(target->clip_rect.x)));
	const int y0 = std::max<int>(0, static_cast<int>(std::floor(target->clip_rect.y)));
	const int x1 = std::min<int>(target->w, static_cast<int>(std::ceil(target->clip_rect.x + target->clip_rect.w)));
	const int y1 = std::min<int>(target->h, static_cast<int>(std::ceil(target->clip_rect.y + target->clip_rect.h)));
	scissor.x = x0;
	scissor.y = y0;
	scissor.w = std::max(0, x1 - x0);
	scissor.h = std::max(0, y1 - y0);
	return scissor;
}

bool blitBoundsOutsideScissor(float x0, float y0, float x1, float y1, const SDL_Rect &scissor) {
	if (scissor.w <= 0 || scissor.h <= 0)
		return true;

	const float left   = std::min(x0, x1);
	const float right  = std::max(x0, x1);
	const float top    = std::min(y0, y1);
	const float bottom = std::max(y0, y1);
	return right <= static_cast<float>(scissor.x) ||
	       left >= static_cast<float>(scissor.x + scissor.w) ||
	       bottom <= static_cast<float>(scissor.y) ||
	       top >= static_cast<float>(scissor.y + scissor.h);
}

bool clipAxisAlignedBlitToScissor(float &x0, float &y0, float &x1, float &y1,
                                  float &u0, float &v0, float &u1, float &v1,
                                  const SDL_Rect &scissor) {
	if (blitBoundsOutsideScissor(x0, y0, x1, y1, scissor)) {
		noteNativeBlitCull();
		return false;
	}

	if (x0 >= x1 || y0 >= y1)
		return true;

	const float originalX0 = x0;
	const float originalY0 = y0;
	const float originalX1 = x1;
	const float originalY1 = y1;
	const float scissorX0  = static_cast<float>(scissor.x);
	const float scissorY0  = static_cast<float>(scissor.y);
	const float scissorX1  = static_cast<float>(scissor.x + scissor.w);
	const float scissorY1  = static_cast<float>(scissor.y + scissor.h);
	const float clippedX0  = std::max(x0, scissorX0);
	const float clippedY0  = std::max(y0, scissorY0);
	const float clippedX1  = std::min(x1, scissorX1);
	const float clippedY1  = std::min(y1, scissorY1);

	if (clippedX0 == x0 && clippedY0 == y0 && clippedX1 == x1 && clippedY1 == y1)
		return true;

	const float invW = 1.0f / (x1 - x0);
	const float invH = 1.0f / (y1 - y0);
	const float oldU0 = u0;
	const float oldV0 = v0;
	const float oldU1 = u1;
	const float oldV1 = v1;
	u0 = oldU0 + (oldU1 - oldU0) * ((clippedX0 - x0) * invW);
	u1 = oldU0 + (oldU1 - oldU0) * ((clippedX1 - x0) * invW);
	v0 = oldV0 + (oldV1 - oldV0) * ((clippedY0 - y0) * invH);
	v1 = oldV0 + (oldV1 - oldV0) * ((clippedY1 - y0) * invH);
	x0 = clippedX0;
	y0 = clippedY0;
	x1 = clippedX1;
	y1 = clippedY1;

	if (x0 != originalX0 || y0 != originalY0 || x1 != originalX1 || y1 != originalY1)
		noteNativeBlitClip();
	return true;
}

bool renderNativeProgramIndexedTriangles(const SDL3GPUProgramObject &program,
                                         GPU_Image *image, GPU_Target *target,
                                         const SDL3GPUVertex *vertices, Uint32 numVertices,
                                         const Uint16 *indices, Uint32 numIndices) {
	if (!program.nativeFragmentShader) {
		setShaderMessage("SDL3 native shader program has no fragment shader");
		return false;
	}
	if (!rendererState.device || !image || !target || !vertices || !indices || numVertices == 0 || numIndices == 0)
		return false;
	if (!image->texture || !ensureTargetBacking(target) || !target->texture || !target->image)
		return false;
	if (!isNativeShaderSamplerFormat(image->format) || !isNativeTextureFormat(target->image->format))
		return false;
	if (!ensureImageTextureInitialized(image) || !ensureImageTextureInitialized(target->image))
		return false;
	if (!ensureNativeShaders())
		return false;

	SDL_GPUShader *vertexShader = program.nativeVertexShader ? program.nativeVertexShader : texturedVertexShader;
	SDL_GPUGraphicsPipeline *pipeline = getPipeline(textureFormat(target->image->format),
	                                                image->use_blending,
	                                                image->blend_mode,
	                                                vertexShader,
	                                                program.nativeFragmentShader);
	if (!pipeline)
		return false;

	const Uint32 samplerCount = std::min<Uint32>(program.nativeFragmentResources.numSamplers,
	                                             static_cast<Uint32>(program.images.size()));
	SDL3GPUSamplerSet samplerSet;
	for (Uint32 i = 0; i < samplerCount; ++i) {
		const int mappedImageUnit = program.nativeSamplerImageUnits[i] >= 0 ?
		                                program.nativeSamplerImageUnits[i] :
		                                static_cast<int>(i);
		if (mappedImageUnit < 0 || mappedImageUnit >= static_cast<int>(program.images.size())) {
			setShaderMessage("SDL3 native shader program references an invalid sampler image unit");
			return false;
		}

		GPU_Image *boundImage = program.images[static_cast<size_t>(mappedImageUnit)];
		if (mappedImageUnit == 0 && !boundImage)
			boundImage = image;
		if (!boundImage || !boundImage->texture || !isNativeShaderSamplerFormat(boundImage->format) ||
		    !ensureImageTextureInitialized(boundImage)) {
			setShaderMessage("SDL3 native shader program is missing a bound sampler texture");
			return false;
		}
		if (boundImage == target->image) {
			setShaderMessage("SDL3 native shader program cannot sample from its render target");
			return false;
		}
		SDL_GPUSampler *sampler = getSampler(boundImage->filter_mode);
		if (!sampler)
			return false;
		if (!samplerSet.push(boundImage->texture, sampler))
			return false;
	}

	const float viewportW = target->viewport.w > 0.0f ? target->viewport.w : static_cast<float>(target->w);
	const float viewportH = target->viewport.h > 0.0f ? target->viewport.h : static_cast<float>(target->h);
	SDL_GPUViewport viewport{};
	viewport.x = target->viewport.x;
	viewport.y = target->viewport.y;
	viewport.w = viewportW;
	viewport.h = viewportH;
	viewport.min_depth = 0.0f;
	viewport.max_depth = 1.0f;

	const SDL_Rect scissor = targetScissor(target);
	return queueNativeTriangleDraw(image, target, pipeline, viewport, scissor, samplerSet,
	                               true, program.kind, false, program.nativeUniformRegisters,
	                               vertices, numVertices, indices, numIndices);
}

bool renderNativeIndexedTriangles(GPU_Image *image, GPU_Target *target, const SDL3GPUVertex *vertices,
                                  Uint32 numVertices, const Uint16 *indices, Uint32 numIndices) {
	if (!rendererState.device || !image || !target || !vertices || !indices || numVertices == 0 || numIndices == 0)
		return false;

	if (auto *program = activeProgramObject()) {
		if (program->nativeFragmentShader) {
			if (renderNativeProgramIndexedTriangles(*program, image, target, vertices, numVertices, indices, numIndices))
				return true;
			flushNativeBlitBatch();
			if (cpuShaderTriangles(*program, image, target, vertices, numVertices, indices, numIndices))
				return true;
			if (program->kind == SDL3GPUShaderKind::Unknown || program->kind == SDL3GPUShaderKind::DefaultVertex)
				setShaderMessage("SDL3_GPU backend could not execute the active native shader program");
			return true;
		}
		flushNativeBlitBatch();
		if (cpuShaderTriangles(*program, image, target, vertices, numVertices, indices, numIndices))
			return true;
		setShaderMessage("SDL3_GPU backend cannot execute the active external triangle shader program");
		return true;
	}

	if (!image->texture || !ensureTargetBacking(target) || !target->texture || !target->image)
		return false;
	if (target->image == image)
		return false;
	if (!isNativeTextureFormat(image->format) || !isNativeTextureFormat(target->image->format))
		return false;
	if (!ensureImageTextureInitialized(image) || !ensureImageTextureInitialized(target->image))
		return false;

	SDL_GPUGraphicsPipeline *pipeline = getPipeline(textureFormat(target->image->format), image->use_blending, image->blend_mode);
	SDL_GPUSampler *sampler = getSampler(image->filter_mode);
	if (!pipeline || !sampler)
		return false;

	const float viewportW = target->viewport.w > 0.0f ? target->viewport.w : static_cast<float>(target->w);
	const float viewportH = target->viewport.h > 0.0f ? target->viewport.h : static_cast<float>(target->h);
	SDL_GPUViewport viewport{};
	viewport.x = target->viewport.x;
	viewport.y = target->viewport.y;
	viewport.w = viewportW;
	viewport.h = viewportH;
	viewport.min_depth = 0.0f;
	viewport.max_depth = 1.0f;

	const SDL_Rect scissor = targetScissor(target);
	SDL3GPUSamplerSet samplerSet;
	if (!samplerSet.push(image->texture, sampler))
		return false;
	const std::vector<SDL3GPUNativeUniformRegister> noFragmentUniforms;
	return queueNativeTriangleDraw(image, target, pipeline, viewport, scissor, samplerSet,
	                               false, SDL3GPUShaderKind::Unknown, true, noFragmentUniforms,
	                               vertices, numVertices, indices, numIndices);
}

bool nativeBlitBatchActive() {
	return !nativeBlitBatch.vertices.empty();
}

bool blendModesEqual(const GPU_BlendMode &a, const GPU_BlendMode &b) {
	return a.source_color == b.source_color &&
	       a.dest_color == b.dest_color &&
	       a.source_alpha == b.source_alpha &&
	       a.dest_alpha == b.dest_alpha &&
	       a.color_equation == b.color_equation &&
	       a.alpha_equation == b.alpha_equation;
}

bool viewportsEqual(const SDL_GPUViewport &a, const SDL_GPUViewport &b) {
	return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h &&
	       a.min_depth == b.min_depth && a.max_depth == b.max_depth;
}

bool scissorsEqual(const SDL_Rect &a, const SDL_Rect &b) {
	return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

bool nativeUniformRegistersEqual(const std::vector<SDL3GPUNativeUniformRegister> &a,
                                 const std::vector<SDL3GPUNativeUniformRegister> &b) {
	if (a.size() != b.size())
		return false;
	if (a.empty())
		return true;
	return std::memcmp(a.data(), b.data(), a.size() * sizeof(SDL3GPUNativeUniformRegister)) == 0;
}

bool nativeTriangleBatchActive() {
	return !nativeTriangleBatch.vertices.empty();
}

void resetNativeTriangleBatch() {
	nativeTriangleBatch.target          = nullptr;
	nativeTriangleBatch.targetImage     = nullptr;
	nativeTriangleBatch.targetTexture   = nullptr;
	nativeTriangleBatch.pipeline        = nullptr;
	nativeTriangleBatch.viewport        = SDL_GPUViewport{};
	nativeTriangleBatch.scissor         = SDL_Rect{};
	nativeTriangleBatch.useBlending     = false;
	nativeTriangleBatch.blendMode       = GPU_BlendMode{};
	nativeTriangleBatch.nativeShaderProgram = false;
	nativeTriangleBatch.shaderKind      = SDL3GPUShaderKind::Unknown;
	nativeTriangleBatch.pushColorScale  = false;
	nativeTriangleBatch.samplerSet.clear();
	nativeTriangleBatch.fragmentUniformRegisters.clear();
	nativeTriangleBatch.vertices.clear();
	nativeTriangleBatch.indices.clear();
}

bool flushNativeTriangleBatch() {
	if (!nativeTriangleBatchActive())
		return true;

	if (!rendererState.device || !nativeTriangleBatch.targetImage || !nativeTriangleBatch.targetTexture ||
	    !nativeTriangleBatch.pipeline) {
		resetNativeTriangleBatch();
		return false;
	}

	const Uint32 vertexBytes = static_cast<Uint32>(nativeTriangleBatch.vertices.size() * sizeof(SDL3GPUVertex));
	const Uint32 indexBytes  = static_cast<Uint32>(nativeTriangleBatch.indices.size() * sizeof(Uint16));
	if (!prepareUploadedBuffer(vertexUploadBuffer, SDL_GPU_BUFFERUSAGE_VERTEX, nativeTriangleBatch.vertices.data(), vertexBytes) ||
	    !prepareUploadedBuffer(indexUploadBuffer, SDL_GPU_BUFFERUSAGE_INDEX, nativeTriangleBatch.indices.data(), indexBytes)) {
		resetNativeTriangleBatch();
		return false;
	}

	SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(rendererState.device);
	if (!commands) {
		resetNativeTriangleBatch();
		return false;
	}

	SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(commands);
	encodeBufferUpload(copyPass, vertexUploadBuffer);
	encodeBufferUpload(copyPass, indexUploadBuffer);
	SDL_EndGPUCopyPass(copyPass);

	SDL_GPUColorTargetInfo colorTarget{};
	colorTarget.texture  = nativeTriangleBatch.targetTexture;
	colorTarget.load_op  = SDL_GPU_LOADOP_LOAD;
	colorTarget.store_op = SDL_GPU_STOREOP_STORE;

	SDL_GPURenderPass *renderPass = SDL_BeginGPURenderPass(commands, &colorTarget, 1, nullptr);
	if (!renderPass) {
		SDL_CancelGPUCommandBuffer(commands);
		resetNativeTriangleBatch();
		return false;
	}

	SDL_BindGPUGraphicsPipeline(renderPass, nativeTriangleBatch.pipeline);

	SDL_GPUBufferBinding vertexBinding{};
	vertexBinding.buffer = vertexUploadBuffer.buffer;
	SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);

	SDL_GPUBufferBinding indexBinding{};
	indexBinding.buffer = indexUploadBuffer.buffer;
	SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);

	std::array<SDL_GPUTextureSamplerBinding, 8> samplerBindings{};
	for (size_t i = 0; i < nativeTriangleBatch.samplerSet.count; ++i) {
		samplerBindings[i].texture = nativeTriangleBatch.samplerSet.textures[i];
		samplerBindings[i].sampler = nativeTriangleBatch.samplerSet.samplers[i];
	}
	if (nativeTriangleBatch.samplerSet.count > 0)
		SDL_BindGPUFragmentSamplers(renderPass, 0, samplerBindings.data(), static_cast<Uint32>(nativeTriangleBatch.samplerSet.count));

	struct VertexUniforms {
		float mvp[4][4];
	} vertexUniforms{};
	vertexUniforms.mvp[0][0] = 2.0f / nativeTriangleBatch.viewport.w;
	vertexUniforms.mvp[1][1] = -2.0f / nativeTriangleBatch.viewport.h;
	vertexUniforms.mvp[2][2] = 1.0f;
	vertexUniforms.mvp[3][0] = -1.0f;
	vertexUniforms.mvp[3][1] = 1.0f;
	vertexUniforms.mvp[3][3] = 1.0f;
	SDL_PushGPUVertexUniformData(commands, 0, &vertexUniforms, sizeof(vertexUniforms));

	if (nativeTriangleBatch.pushColorScale) {
		const float colorScale = 1.0f;
		SDL_PushGPUFragmentUniformData(commands, 0, &colorScale, sizeof(colorScale));
	} else if (!nativeTriangleBatch.fragmentUniformRegisters.empty()) {
		SDL_PushGPUFragmentUniformData(commands, 0,
		                               nativeTriangleBatch.fragmentUniformRegisters.data(),
		                               static_cast<Uint32>(nativeTriangleBatch.fragmentUniformRegisters.size() * sizeof(SDL3GPUNativeUniformRegister)));
	}

	SDL_SetGPUViewport(renderPass, &nativeTriangleBatch.viewport);
	SDL_SetGPUScissor(renderPass, &nativeTriangleBatch.scissor);
	SDL_DrawGPUIndexedPrimitives(renderPass, static_cast<Uint32>(nativeTriangleBatch.indices.size()), 1, 0, 0, 0);
	SDL_EndGPURenderPass(renderPass);
	const bool submitted = submitGPUCommandBuffer(commands);

	if (submitted) {
		if (nativeTriangleBatch.nativeShaderProgram) {
			noteNativeShaderDraw(nativeTriangleBatch.shaderKind, nativeTriangleBatch.vertices.size());
		} else {
			noteNativeFixedDraw(nativeTriangleBatch.vertices.size());
		}
		nativeTriangleBatch.targetImage->pixels_dirty = true;
		nativeTriangleBatch.targetImage->has_mipmaps  = false;
	}
	resetNativeTriangleBatch();
	return submitted;
}

bool nativeTriangleBatchMatches(GPU_Image *image,
                                GPU_Target *target,
                                SDL_GPUGraphicsPipeline *pipeline,
                                const SDL_GPUViewport &viewport,
                                const SDL_Rect &scissor,
                                const SDL3GPUSamplerSet &samplerSet,
                                bool nativeShaderProgram,
                                SDL3GPUShaderKind shaderKind,
                                bool pushColorScale,
                                const std::vector<SDL3GPUNativeUniformRegister> &fragmentUniformRegisters) {
	if (!nativeTriangleBatchActive())
		return false;
	return nativeTriangleBatch.target == target &&
	       nativeTriangleBatch.targetImage == target->image &&
	       nativeTriangleBatch.targetTexture == target->texture &&
	       nativeTriangleBatch.pipeline == pipeline &&
	       nativeTriangleBatch.useBlending == image->use_blending &&
	       blendModesEqual(nativeTriangleBatch.blendMode, image->blend_mode) &&
	       viewportsEqual(nativeTriangleBatch.viewport, viewport) &&
	       scissorsEqual(nativeTriangleBatch.scissor, scissor) &&
	       nativeTriangleBatch.nativeShaderProgram == nativeShaderProgram &&
	       nativeTriangleBatch.shaderKind == shaderKind &&
	       nativeTriangleBatch.pushColorScale == pushColorScale &&
	       samplerSetsEqual(nativeTriangleBatch.samplerSet, samplerSet) &&
	       nativeUniformRegistersEqual(nativeTriangleBatch.fragmentUniformRegisters, fragmentUniformRegisters);
}

bool queueNativeTriangleDraw(GPU_Image *image,
                             GPU_Target *target,
                             SDL_GPUGraphicsPipeline *pipeline,
                             const SDL_GPUViewport &viewport,
                             const SDL_Rect &scissor,
                             const SDL3GPUSamplerSet &samplerSet,
                             bool nativeShaderProgram,
                             SDL3GPUShaderKind shaderKind,
                             bool pushColorScale,
                             const std::vector<SDL3GPUNativeUniformRegister> &fragmentUniformRegisters,
                             const SDL3GPUVertex *vertices,
                             Uint32 numVertices,
                             const Uint16 *indices,
                             Uint32 numIndices) {
	if (!image || !target || !target->image || !pipeline || !vertices || !indices ||
	    numVertices == 0 || numIndices == 0 || samplerSet.count > samplerSet.textures.size())
		return false;
	if (viewport.w <= 0.0f || viewport.h <= 0.0f)
		return false;

	constexpr size_t maxVertices = UINT16_MAX;
	if (numVertices > maxVertices)
		return false;
	for (Uint32 i = 0; i < numIndices; ++i) {
		if (indices[i] >= numVertices)
			return false;
	}

	if (nativeBlitBatchActive()) {
		if (!flushNativeBlitBatchOnly())
			return false;
	}

	if (nativeTriangleBatchActive() &&
	    (!nativeTriangleBatchMatches(image, target, pipeline, viewport, scissor, samplerSet,
	                                 nativeShaderProgram, shaderKind, pushColorScale, fragmentUniformRegisters) ||
	     nativeTriangleBatch.vertices.size() + numVertices > maxVertices)) {
		if (!flushNativeTriangleBatch())
			return false;
	}

	if (!nativeTriangleBatchActive()) {
		nativeTriangleBatch.target          = target;
		nativeTriangleBatch.targetImage     = target->image;
		nativeTriangleBatch.targetTexture   = target->texture;
		nativeTriangleBatch.pipeline        = pipeline;
		nativeTriangleBatch.viewport        = viewport;
		nativeTriangleBatch.scissor         = scissor;
		nativeTriangleBatch.useBlending     = image->use_blending;
		nativeTriangleBatch.blendMode       = image->blend_mode;
		nativeTriangleBatch.nativeShaderProgram = nativeShaderProgram;
		nativeTriangleBatch.shaderKind      = shaderKind;
		nativeTriangleBatch.pushColorScale  = pushColorScale;
		nativeTriangleBatch.samplerSet      = samplerSet;
		nativeTriangleBatch.fragmentUniformRegisters = fragmentUniformRegisters;
		nativeTriangleBatch.vertices.reserve(4096);
		nativeTriangleBatch.indices.reserve(6144);
	}

	const Uint16 base = static_cast<Uint16>(nativeTriangleBatch.vertices.size());
	nativeTriangleBatch.vertices.insert(nativeTriangleBatch.vertices.end(), vertices, vertices + numVertices);
	for (Uint32 i = 0; i < numIndices; ++i)
		nativeTriangleBatch.indices.push_back(static_cast<Uint16>(base + indices[i]));
	return true;
}

void resetNativeBlitBatch() {
	nativeBlitBatch.target        = nullptr;
	nativeBlitBatch.targetImage   = nullptr;
	nativeBlitBatch.targetTexture = nullptr;
	nativeBlitBatch.pipeline      = nullptr;
	nativeBlitBatch.viewport      = SDL_GPUViewport{};
	nativeBlitBatch.scissor       = SDL_Rect{};
	nativeBlitBatch.useBlending   = false;
	nativeBlitBatch.blendMode     = GPU_BlendMode{};
	nativeBlitBatch.vertices.clear();
	nativeBlitBatch.indices.clear();
	nativeBlitBatch.drawGroups.clear();
}

bool flushNativeBlitBatchOnly() {
	if (!nativeBlitBatchActive())
		return true;

	if (!rendererState.device || !nativeBlitBatch.targetImage || !nativeBlitBatch.targetTexture ||
	    !nativeBlitBatch.pipeline || nativeBlitBatch.drawGroups.empty()) {
		resetNativeBlitBatch();
		return false;
	}
	for (const auto &group : nativeBlitBatch.drawGroups) {
		if (!group.sourceTexture || !group.sampler || group.indexCount == 0 ||
		    group.firstIndex + group.indexCount > nativeBlitBatch.indices.size()) {
			resetNativeBlitBatch();
			return false;
		}
	}

	const Uint32 vertexBytes = static_cast<Uint32>(nativeBlitBatch.vertices.size() * sizeof(SDL3GPUVertex));
	const Uint32 indexBytes  = static_cast<Uint32>(nativeBlitBatch.indices.size() * sizeof(Uint16));
	if (!prepareUploadedBuffer(vertexUploadBuffer, SDL_GPU_BUFFERUSAGE_VERTEX, nativeBlitBatch.vertices.data(), vertexBytes) ||
	    !prepareUploadedBuffer(indexUploadBuffer, SDL_GPU_BUFFERUSAGE_INDEX, nativeBlitBatch.indices.data(), indexBytes)) {
		resetNativeBlitBatch();
		return false;
	}

	SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(rendererState.device);
	if (!commands) {
		resetNativeBlitBatch();
		return false;
	}

	SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(commands);
	encodeBufferUpload(copyPass, vertexUploadBuffer);
	encodeBufferUpload(copyPass, indexUploadBuffer);
	SDL_EndGPUCopyPass(copyPass);

	SDL_GPUColorTargetInfo colorTarget{};
	colorTarget.texture  = nativeBlitBatch.targetTexture;
	colorTarget.load_op  = SDL_GPU_LOADOP_LOAD;
	colorTarget.store_op = SDL_GPU_STOREOP_STORE;

	SDL_GPURenderPass *renderPass = SDL_BeginGPURenderPass(commands, &colorTarget, 1, nullptr);
	if (!renderPass) {
		SDL_CancelGPUCommandBuffer(commands);
		resetNativeBlitBatch();
		return false;
	}

	SDL_BindGPUGraphicsPipeline(renderPass, nativeBlitBatch.pipeline);

	SDL_GPUBufferBinding vertexBinding{};
	vertexBinding.buffer = vertexUploadBuffer.buffer;
	SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);

	SDL_GPUBufferBinding indexBinding{};
	indexBinding.buffer = indexUploadBuffer.buffer;
	SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);

	struct VertexUniforms {
		float mvp[4][4];
	} vertexUniforms{};
	vertexUniforms.mvp[0][0] = 2.0f / nativeBlitBatch.viewport.w;
	vertexUniforms.mvp[1][1] = -2.0f / nativeBlitBatch.viewport.h;
	vertexUniforms.mvp[2][2] = 1.0f;
	vertexUniforms.mvp[3][0] = -1.0f;
	vertexUniforms.mvp[3][1] = 1.0f;
	vertexUniforms.mvp[3][3] = 1.0f;
	SDL_PushGPUVertexUniformData(commands, 0, &vertexUniforms, sizeof(vertexUniforms));

	const float colorScale = 1.0f;
	SDL_PushGPUFragmentUniformData(commands, 0, &colorScale, sizeof(colorScale));

	SDL_SetGPUViewport(renderPass, &nativeBlitBatch.viewport);
	SDL_SetGPUScissor(renderPass, &nativeBlitBatch.scissor);
	for (const auto &group : nativeBlitBatch.drawGroups) {
		SDL_GPUTextureSamplerBinding samplerBinding{};
		samplerBinding.texture = group.sourceTexture;
		samplerBinding.sampler = group.sampler;
		SDL_BindGPUFragmentSamplers(renderPass, 0, &samplerBinding, 1);
		SDL_DrawGPUIndexedPrimitives(renderPass, group.indexCount, 1, group.firstIndex, 0, 0);
	}
	SDL_EndGPURenderPass(renderPass);
	const bool submitted = submitGPUCommandBuffer(commands);

	if (submitted) {
		for (const auto &group : nativeBlitBatch.drawGroups)
			noteNativeFixedDrawForSource(group.telemetrySource, group.vertexCount);
		nativeBlitBatch.targetImage->pixels_dirty = true;
		nativeBlitBatch.targetImage->has_mipmaps  = false;
	}
	resetNativeBlitBatch();
	return submitted;
}

bool flushNativeBlitBatch() {
	const bool trianglesFlushed = flushNativeTriangleBatch();
	const bool blitsFlushed     = flushNativeBlitBatchOnly();
	return trianglesFlushed && blitsFlushed;
}

bool nativeBlitBatchMatchesState(GPU_Target *target, SDL_GPUGraphicsPipeline *pipeline,
                                 const SDL_GPUViewport &viewport, const SDL_Rect &scissor) {
	if (!nativeBlitBatchActive())
		return false;
	return nativeBlitBatch.target == target &&
	       nativeBlitBatch.targetImage == target->image &&
	       nativeBlitBatch.targetTexture == target->texture &&
	       nativeBlitBatch.pipeline == pipeline &&
	       viewportsEqual(nativeBlitBatch.viewport, viewport) &&
	       scissorsEqual(nativeBlitBatch.scissor, scissor);
}

bool prepareNativeBlitBatch(GPU_Image *image, GPU_Target *target, SDL_GPUGraphicsPipeline *pipeline,
                            SDL_GPUSampler *sampler, const SDL_GPUViewport &viewport,
                            const SDL_Rect &scissor, size_t additionalVertices) {
	if (!image || !target || !target->image || !pipeline || !sampler)
		return false;

	if (nativeTriangleBatchActive()) {
		if (!flushNativeTriangleBatch())
			return false;
	}

	constexpr size_t maxVertices = UINT16_MAX;
	if (nativeBlitBatchActive() &&
	    (!nativeBlitBatchMatchesState(target, pipeline, viewport, scissor) ||
	     nativeBlitBatch.useBlending != image->use_blending ||
	     !blendModesEqual(nativeBlitBatch.blendMode, image->blend_mode) ||
	     nativeBlitBatch.vertices.size() + additionalVertices > maxVertices)) {
		if (!flushNativeBlitBatchOnly())
			return false;
	}

	if (!nativeBlitBatchActive()) {
		nativeBlitBatch.target        = target;
		nativeBlitBatch.targetImage   = target->image;
		nativeBlitBatch.targetTexture = target->texture;
		nativeBlitBatch.pipeline      = pipeline;
		nativeBlitBatch.viewport      = viewport;
		nativeBlitBatch.scissor       = scissor;
		nativeBlitBatch.useBlending   = image->use_blending;
		nativeBlitBatch.blendMode     = image->blend_mode;
		nativeBlitBatch.vertices.reserve(4096);
		nativeBlitBatch.indices.reserve(6144);
		nativeBlitBatch.drawGroups.reserve(64);
	}
	return true;
}

void appendNativeBlitDrawGroup(SDL_GPUTexture *sourceTexture, SDL_GPUSampler *sampler, Uint32 firstIndex, Uint32 indexCount, Uint32 vertexCount) {
	const std::string telemetrySource = telemetryEnabled() ? currentTelemetrySource("native_fixed_draw") : std::string{};
	if (!nativeBlitBatch.drawGroups.empty()) {
		auto &last = nativeBlitBatch.drawGroups.back();
		if (last.sourceTexture == sourceTexture && last.sampler == sampler &&
		    last.firstIndex + last.indexCount == firstIndex &&
		    last.telemetrySource == telemetrySource) {
			last.indexCount += indexCount;
			last.vertexCount += vertexCount;
			return;
		}
	}
	nativeBlitBatch.drawGroups.push_back(SDL3GPUNativeBlitBatch::DrawGroup{sourceTexture, sampler, firstIndex, indexCount, vertexCount, telemetrySource});
}

void appendNativeBlitQuadIndices(Uint16 base) {
	const Uint16 indices[6]{base, static_cast<Uint16>(base + 1), static_cast<Uint16>(base + 2),
	                        static_cast<Uint16>(base + 2), static_cast<Uint16>(base + 1), static_cast<Uint16>(base + 3)};
	nativeBlitBatch.indices.insert(nativeBlitBatch.indices.end(), indices, indices + 6);
}

bool queueNativeBlit(GPU_Image *image, GPU_Target *target, SDL_GPUGraphicsPipeline *pipeline,
                     SDL_GPUSampler *sampler, const SDL_GPUViewport &viewport, const SDL_Rect &scissor,
                     const std::array<SDL3GPUVertex, 4> &vertices) {
	if (!prepareNativeBlitBatch(image, target, pipeline, sampler, viewport, scissor, vertices.size()))
		return false;

	const Uint16 base = static_cast<Uint16>(nativeBlitBatch.vertices.size());
	nativeBlitBatch.vertices.insert(nativeBlitBatch.vertices.end(), vertices.begin(), vertices.end());
	const Uint32 firstIndex = static_cast<Uint32>(nativeBlitBatch.indices.size());
	appendNativeBlitQuadIndices(base);
	appendNativeBlitDrawGroup(image->texture, sampler, firstIndex, 6, 4);
	return true;
}

bool queueNativeBlitRect(GPU_Image *image, GPU_Target *target, SDL_GPUGraphicsPipeline *pipeline,
                         SDL_GPUSampler *sampler, const SDL_GPUViewport &viewport, const SDL_Rect &scissor,
                         float x0, float y0, float x1, float y1,
                         float u0, float v0, float u1, float v1,
                         float r, float g, float b, float a) {
	if (!prepareNativeBlitBatch(image, target, pipeline, sampler, viewport, scissor, 4))
		return false;

	const Uint16 base = static_cast<Uint16>(nativeBlitBatch.vertices.size());
	nativeBlitBatch.vertices.push_back(SDL3GPUVertex{x0, y0, r, g, b, a, u0, v0});
	nativeBlitBatch.vertices.push_back(SDL3GPUVertex{x1, y0, r, g, b, a, u1, v0});
	nativeBlitBatch.vertices.push_back(SDL3GPUVertex{x0, y1, r, g, b, a, u0, v1});
	nativeBlitBatch.vertices.push_back(SDL3GPUVertex{x1, y1, r, g, b, a, u1, v1});
	const Uint32 firstIndex = static_cast<Uint32>(nativeBlitBatch.indices.size());
	appendNativeBlitQuadIndices(base);
	appendNativeBlitDrawGroup(image->texture, sampler, firstIndex, 6, 4);
	return true;
}

bool nativeBlit(GPU_Image *image, GPU_Rect *src_rect, GPU_Target *target, float x, float y,
                float degrees, float scaleX, float scaleY) {
	if (!image || !target || scaleX == 0.0f || scaleY == 0.0f)
		return false;

	SDL3GPUProgramObject *program = activeProgramObject();
	const bool activeNativeProgram = program && program->nativeFragmentShader;
	if (program && !program->nativeFragmentShader) {
		flushNativeBlitBatch();
		if (cpuShaderBlit(*program, image, src_rect, target, x, y, degrees, scaleX, scaleY))
			return true;
		setShaderMessage("SDL3_GPU backend cannot execute the active external shader program");
		return true;
	}

	const float srcX = src_rect ? src_rect->x : 0.0f;
	const float srcY = src_rect ? src_rect->y : 0.0f;
	const float srcW = src_rect ? src_rect->w : static_cast<float>(image->w);
	const float srcH = src_rect ? src_rect->h : static_cast<float>(image->h);
	if (srcW <= 0.0f || srcH <= 0.0f)
		return false;

	const SDL_Color color = image->color;
	const float r = color.r / 255.0f;
	const float g = color.g / 255.0f;
	const float b = color.b / 255.0f;
	const float a = color.a / 255.0f;
	const float u0 = srcX / image->w;
	const float v0 = srcY / image->h;
	const float u1 = (srcX + srcW) / image->w;
	const float v1 = (srcY + srcH) / image->h;

	const float halfW = srcW * scaleX * 0.5f;
	const float halfH = srcH * scaleY * 0.5f;
	const float rawX0 = x - halfW;
	const float rawY0 = y - halfH;
	const float rawX1 = x + halfW;
	const float rawY1 = y + halfH;
	const SDL_Rect scissor = targetScissor(target);

	if (activeNativeProgram) {
		auto vertexAt = [&](float px, float py, float u, float v) {
			return SDL3GPUVertex{px, py, r, g, b, a, u, v};
		};

		std::array<SDL3GPUVertex, 4> vertices{};
		if (degrees == 0.0f) {
			if (blitBoundsOutsideScissor(rawX0, rawY0, rawX1, rawY1, scissor)) {
				noteNativeBlitCull();
				return true;
			}
			vertices = {
			    vertexAt(rawX0, rawY0, u0, v0),
			    vertexAt(rawX1, rawY0, u1, v0),
			    vertexAt(rawX0, rawY1, u0, v1),
			    vertexAt(rawX1, rawY1, u1, v1)};
		} else {
			constexpr float pi = 3.14159265358979323846f;
			const float radians = degrees * pi / 180.0f;
			const float cosTheta = std::cos(radians);
			const float sinTheta = std::sin(radians);

			auto transform = [&](float localX, float localY, float u, float v) {
				return vertexAt(x + localX * cosTheta - localY * sinTheta,
				                y + localX * sinTheta + localY * cosTheta, u, v);
			};

			vertices = {
			    transform(-halfW, -halfH, u0, v0),
			    transform(halfW, -halfH, u1, v0),
			    transform(-halfW, halfH, u0, v1),
			    transform(halfW, halfH, u1, v1)};
			float boundsX0 = vertices[0].x;
			float boundsY0 = vertices[0].y;
			float boundsX1 = vertices[0].x;
			float boundsY1 = vertices[0].y;
			for (const auto &vertex : vertices) {
				boundsX0 = std::min(boundsX0, vertex.x);
				boundsY0 = std::min(boundsY0, vertex.y);
				boundsX1 = std::max(boundsX1, vertex.x);
				boundsY1 = std::max(boundsY1, vertex.y);
			}
			if (blitBoundsOutsideScissor(boundsX0, boundsY0, boundsX1, boundsY1, scissor)) {
				noteNativeBlitCull();
				return true;
			}
		}
		std::array<Uint16, 6> indices{{0, 1, 2, 2, 1, 3}};
		return renderNativeIndexedTriangles(image, target, vertices.data(), static_cast<Uint32>(vertices.size()),
		                                    indices.data(), static_cast<Uint32>(indices.size()));
	}

	if (!image->texture || !ensureTargetBacking(target) || !target->texture || !target->image)
		return false;
	if (target->image == image)
		return false;
	if (!isNativeTextureFormat(image->format) || !isNativeTextureFormat(target->image->format))
		return false;
	if (!ensureImageTextureInitialized(image) || !ensureImageTextureInitialized(target->image))
		return false;

	SDL_GPUGraphicsPipeline *pipeline = getPipeline(textureFormat(target->image->format), image->use_blending, image->blend_mode);
	SDL_GPUSampler *sampler = getSampler(image->filter_mode);
	if (!pipeline || !sampler)
		return false;

	SDL_GPUViewport viewport{};
	viewport.x = target->viewport.x;
	viewport.y = target->viewport.y;
	viewport.w = target->viewport.w > 0.0f ? target->viewport.w : static_cast<float>(target->w);
	viewport.h = target->viewport.h > 0.0f ? target->viewport.h : static_cast<float>(target->h);
	viewport.min_depth = 0.0f;
	viewport.max_depth = 1.0f;

	if (degrees == 0.0f) {
		float clippedX0 = rawX0;
		float clippedY0 = rawY0;
		float clippedX1 = rawX1;
		float clippedY1 = rawY1;
		float clippedU0 = u0;
		float clippedV0 = v0;
		float clippedU1 = u1;
		float clippedV1 = v1;
		if (scaleX > 0.0f && scaleY > 0.0f) {
			if (!clipAxisAlignedBlitToScissor(clippedX0, clippedY0, clippedX1, clippedY1,
			                                  clippedU0, clippedV0, clippedU1, clippedV1, scissor))
				return true;
		} else if (blitBoundsOutsideScissor(clippedX0, clippedY0, clippedX1, clippedY1, scissor)) {
			noteNativeBlitCull();
			return true;
		}
		return queueNativeBlitRect(image, target, pipeline, sampler, viewport, scissor,
		                           clippedX0, clippedY0, clippedX1, clippedY1,
		                           clippedU0, clippedV0, clippedU1, clippedV1, r, g, b, a);
	}

	constexpr float pi = 3.14159265358979323846f;
	const float radians = degrees * pi / 180.0f;
	const float cosTheta = std::cos(radians);
	const float sinTheta = std::sin(radians);
	auto vertexAt = [&](float px, float py, float u, float v) {
		return SDL3GPUVertex{px, py, r, g, b, a, u, v};
	};
	auto transform = [&](float localX, float localY, float u, float v) {
		return vertexAt(x + localX * cosTheta - localY * sinTheta,
		                y + localX * sinTheta + localY * cosTheta, u, v);
	};
	const std::array<SDL3GPUVertex, 4> vertices{
	    transform(-halfW, -halfH, u0, v0),
	    transform(halfW, -halfH, u1, v0),
	    transform(-halfW, halfH, u0, v1),
	    transform(halfW, halfH, u1, v1)};
	float boundsX0 = vertices[0].x;
	float boundsY0 = vertices[0].y;
	float boundsX1 = vertices[0].x;
	float boundsY1 = vertices[0].y;
	for (const auto &vertex : vertices) {
		boundsX0 = std::min(boundsX0, vertex.x);
		boundsY0 = std::min(boundsY0, vertex.y);
		boundsX1 = std::max(boundsX1, vertex.x);
		boundsY1 = std::max(boundsY1, vertex.y);
	}
	if (blitBoundsOutsideScissor(boundsX0, boundsY0, boundsX1, boundsY1, scissor)) {
		noteNativeBlitCull();
		return true;
	}
	return queueNativeBlit(image, target, pipeline, sampler, viewport, scissor, vertices);
}

SDL_GPUFilter toSDLFilter(GPU_FilterEnum filter) {
	return filter == GPU_FILTER_NEAREST ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
}

bool presentTarget(GPU_Target *target, SDL_GPUCommandBuffer *commands, SDL_GPUTexture *swapchainTexture, Uint32 width, Uint32 height) {
	if (!target || !target->image || !target->texture || !commands || !swapchainTexture)
		return false;
	if (!ensureImageTextureInitialized(target->image))
		return false;

	SDL_GPUBlitInfo blit{};
	blit.source.texture      = target->texture;
	blit.source.w            = target->image->w;
	blit.source.h            = target->image->h;
	blit.destination.texture = swapchainTexture;
	blit.destination.w       = width;
	blit.destination.h       = height;
	blit.load_op             = SDL_GPU_LOADOP_DONT_CARE;
	blit.flip_mode           = SDL_FLIP_NONE;
	blit.filter              = toSDLFilter(target->image->filter_mode);
	blit.cycle               = false;
	SDL_BlitGPUTexture(commands, &blit);
	return true;
}

template <typename Fn>
double benchmarkMs(Fn &&fn) {
	const Uint64 start = SDL_GetPerformanceCounter();
	fn();
	const Uint64 end = SDL_GetPerformanceCounter();
	return static_cast<double>(end - start) * 1000.0 / static_cast<double>(SDL_GetPerformanceFrequency());
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
void printBenchmarkLine(FILE *output, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	std::vprintf(fmt, args);
	va_end(args);
	std::fflush(stdout);

	if (!output)
		return;

	va_start(args, fmt);
	std::vfprintf(output, fmt, args);
	va_end(args);
	std::fflush(output);
}

void printBenchmarkResult(FILE *output, const char *name, int iterations, double elapsedMs) {
	const double averageUs = iterations > 0 ? (elapsedMs * 1000.0 / static_cast<double>(iterations)) : 0.0;
	printBenchmarkLine(output, "%s,%d,%.3f,%.3f\n", name, iterations, elapsedMs, averageUs);
}

struct BenchmarkOutputFile {
	FILE *file{nullptr};

	explicit BenchmarkOutputFile(const char *path) {
		if (path && path[0])
			file = std::fopen(path, "wb");
	}

	~BenchmarkOutputFile() {
		if (file)
			std::fclose(file);
	}

	BenchmarkOutputFile(const BenchmarkOutputFile &)            = delete;
	BenchmarkOutputFile &operator=(const BenchmarkOutputFile &) = delete;
};

SDL_Surface *createBenchmarkSurface(int width, int height) {
	SDL_Surface *surface = onsCreateRGBSurface(SDL_SWSURFACE, width, height, 32,
	                                           0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
	if (!surface)
		return nullptr;

	if (SDL_MUSTLOCK(surface) && !SDL_LockSurface(surface)) {
		SDL_FreeSurface(surface);
		return nullptr;
	}

	for (int y = 0; y < surface->h; ++y) {
		auto *row = static_cast<Uint8 *>(surface->pixels) + y * surface->pitch;
		for (int x = 0; x < surface->w; ++x) {
			Uint8 *pixel = row + x * 4;
			pixel[0]     = static_cast<Uint8>((x * 3 + y) & 0xff);
			pixel[1]     = static_cast<Uint8>((x + y * 5) & 0xff);
			pixel[2]     = static_cast<Uint8>((x * 7 + y * 11) & 0xff);
			pixel[3]     = 255;
		}
	}

	if (SDL_MUSTLOCK(surface))
		SDL_UnlockSurface(surface);
	return surface;
}

void finishBenchmarkWork(GPU_Image *image) {
	SDL_Surface *surface = GPU_CopySurfaceFromImage(image);
	if (surface)
		SDL_FreeSurface(surface);
}

bool copyImageRowsToSurface(const GPU_Image *image, SDL_Surface *surface, const Uint8 *pixels) {
	if (!image || !surface || !pixels)
		return false;

	if (SDL_MUSTLOCK(surface) && !SDL_LockSurface(surface))
		return false;

	const int dstBpp = onsSurfaceBytesPerPixel(surface);
	for (int y = 0; y < image->h; ++y) {
		auto *dst       = static_cast<Uint8 *>(surface->pixels) + y * surface->pitch;
		const auto *src = pixels + y * image->pitch;
		copyPixelRow(dst, dstBpp, src, image->bytes_per_pixel, image->w);
	}

	if (SDL_MUSTLOCK(surface))
		SDL_UnlockSurface(surface);
	return true;
}

bool fillSurfaceWithSolidImageColor(const GPU_Image *image, SDL_Surface *surface) {
	if (!image || !surface)
		return false;

	if (SDL_MUSTLOCK(surface) && !SDL_LockSurface(surface))
		return false;

	const int dstBpp = onsSurfaceBytesPerPixel(surface);
	const Uint8 source[4]{image->solid_color.r, image->solid_color.g, image->solid_color.b, image->solid_color.a};
	for (int y = 0; y < image->h; ++y) {
		auto *row = static_cast<Uint8 *>(surface->pixels) + y * surface->pitch;
		copyPixelRow(row, dstBpp, source, 4, 1);
		int filled = 1;
		while (filled < image->w) {
			const int copyPixels = std::min(filled, static_cast<int>(image->w) - filled);
			std::memcpy(row + filled * dstBpp, row, static_cast<size_t>(copyPixels) * dstBpp);
			filled += copyPixels;
		}
	}

	if (SDL_MUSTLOCK(surface))
		SDL_UnlockSurface(surface);
	return true;
}

bool downloadImageToSurface(GPU_Image *image, SDL_Surface *surface) {
	if (!image || !surface || !image->texture || !rendererState.device || !image->texture_initialized)
		return false;

	flushNativeBlitBatch();

	const Uint32 downloadSize = static_cast<Uint32>(static_cast<size_t>(image->pitch) * image->h);
	if (!ensureReusableTransferBuffer(textureDownloadBuffer, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, downloadSize))
		return false;

	SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(rendererState.device);
	if (!commands)
		return false;

	SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(commands);
	SDL_GPUTextureRegion source{};
	source.texture = image->texture;
	source.w       = image->w;
	source.h       = image->h;
	source.d       = 1;

	SDL_GPUTextureTransferInfo destination{};
	destination.transfer_buffer = textureDownloadBuffer.transfer;
	destination.pixels_per_row  = image->w;
	destination.rows_per_layer  = image->h;
	SDL_DownloadFromGPUTexture(copyPass, &source, &destination);
	SDL_EndGPUCopyPass(copyPass);

	SDL_GPUFence *fence = submitGPUCommandBufferAndAcquireFence(commands);
	if (!fence)
		return false;

	const bool waited = SDL_WaitForGPUFences(rendererState.device, true, &fence, 1);
	noteBlockingGPUWait();
	if (waited)
		retireSubmissionBacklogAfterCompletedFence();
	SDL_ReleaseGPUFence(rendererState.device, fence);
	if (!waited)
		return false;

	void *mapped = SDL_MapGPUTransferBuffer(rendererState.device, textureDownloadBuffer.transfer, false);
	if (!mapped) {
		releaseReusableTransferBuffer(textureDownloadBuffer);
		return false;
	}
	const bool copied = copyImageRowsToSurface(image, surface, static_cast<const Uint8 *>(mapped));
	SDL_UnmapGPUTransferBuffer(rendererState.device, textureDownloadBuffer.transfer);
	releaseReusableTransferBuffer(textureDownloadBuffer);
	if (copied)
		noteReadback(downloadSize, "copy_surface_from_image");
	return copied;
}

SDL_GPUDevice *createGPUDevice(bool debugDevice) {
#if defined(DROID)
	SDL_PropertiesID props = SDL_CreateProperties();
	if (!props)
		return nullptr;

	// SDL's simple device constructor enables every optional Vulkan feature.
	// This renderer does not use any of them, and requiring them rejects some
	// otherwise capable Android GPUs. Match SDL's own compatibility renderer by
	// requesting only the SPIR-V shader format and baseline Vulkan features.
	const bool configured =
	    SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, "vulkan") &&
	    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, debugDevice) &&
	    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true) &&
	    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_CLIP_DISTANCE_BOOLEAN, false) &&
	    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_DEPTH_CLAMPING_BOOLEAN, false) &&
	    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_INDIRECT_DRAW_FIRST_INSTANCE_BOOLEAN, false) &&
	    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_ANISOTROPY_BOOLEAN, false);

	SDL_GPUDevice *device = configured ? SDL_CreateGPUDeviceWithProperties(props) : nullptr;
	SDL_DestroyProperties(props);
	return device;
#else
	const SDL_GPUShaderFormat shaderFormats = SDL_GPU_SHADERFORMAT_SPIRV |
	                                          SDL_GPU_SHADERFORMAT_DXBC |
	                                          SDL_GPU_SHADERFORMAT_DXIL |
	                                          SDL_GPU_SHADERFORMAT_MSL |
	                                          SDL_GPU_SHADERFORMAT_METALLIB;
	return SDL_CreateGPUDevice(shaderFormats, debugDevice, "vulkan");
#endif
}
} // namespace

void GPU_GetLiveImageMemory(size_t &images, size_t &textureBytes, size_t &pixelBytes) {
	images = liveTextureImages.size();
	liveImageMemoryTotals(textureBytes, pixelBytes);
}

void GPU_LogLargestLiveImages(size_t count) {
	std::vector<GPU_Image *> sorted(liveTextureImages.begin(), liveTextureImages.end());
	std::sort(sorted.begin(), sorted.end(), [](GPU_Image *a, GPU_Image *b) {
		return imagePixelBytes(a) > imagePixelBytes(b);
	});
	if (sorted.size() > count)
		sorted.resize(count);
	for (auto *image : sorted) {
		if (!image)
			continue;
		sendToLog(LogLevel::Info, "    %5ux%-5u %2d bpp %6llu KB%s\n", image->w, image->h,
		          image->bytes_per_pixel,
		          static_cast<unsigned long long>(imagePixelBytes(image) / 1024),
		          image->target ? " (render target)" : "");
	}
}


void SDLCALL GPU_PushTelemetryScope(const char *source) {
	if (!telemetryEnabled())
		return;
	telemetrySourceStack.push_back(normalizedTelemetrySource(source));
}

void SDLCALL GPU_PopTelemetryScope(void) {
	if (!telemetryEnabled())
		return;
	if (!telemetrySourceStack.empty())
		telemetrySourceStack.pop_back();
}

GPU_RendererID SDLCALL GPU_MakeRendererID(const char *name, GPU_RendererEnum renderer, int major_version, int minor_version) {
	GPU_RendererID id{};
	id.name          = name;
	id.renderer      = renderer;
	id.major_version = major_version;
	id.minor_version = minor_version;
	return id;
}

void SDLCALL GPU_SetPreInitFlags(GPU_InitFlagEnum GPU_flags) {
	pendingPreinitFlags = GPU_flags;
}

const char *presentModeName(SDL_GPUPresentMode mode) {
	switch (mode) {
		case SDL_GPU_PRESENTMODE_IMMEDIATE:
			return "immediate";
		case SDL_GPU_PRESENTMODE_MAILBOX:
			return "mailbox";
		case SDL_GPU_PRESENTMODE_VSYNC:
		default:
			return "vsync";
	}
}

SDL_GPUPresentMode choosePresentMode(SDL_GPUDevice *device, SDL_Window *window) {
	if ((pendingPreinitFlags & GPU_INIT_DISABLE_VSYNC) &&
	    SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_IMMEDIATE))
		return SDL_GPU_PRESENTMODE_IMMEDIATE;

	if (pendingPreinitFlags & GPU_INIT_ENABLE_VSYNC)
		return SDL_GPU_PRESENTMODE_VSYNC;

	if (SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_MAILBOX))
		return SDL_GPU_PRESENTMODE_MAILBOX;

	if ((pendingPreinitFlags & GPU_INIT_DISABLE_VSYNC) &&
	    SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_MAILBOX))
		return SDL_GPU_PRESENTMODE_MAILBOX;

	return SDL_GPU_PRESENTMODE_VSYNC;
}

GPU_Target *SDLCALL GPU_InitRendererByID(GPU_RendererID renderer_request, Uint16 w, Uint16 h, GPU_WindowFlagEnum SDL_flags) {
	GPU_Quit();

	rendererState.device = createGPUDevice(rendererState.debug_level == GPU_DEBUG_LEVEL_MAX);
	if (!rendererState.device)
		return nullptr;

	rendererState.window = SDL_CreateWindow("onscripter-new", w, h, SDL_flags);
	if (!rendererState.window) {
		GPU_Quit();
		return nullptr;
	}

	if (!SDL_ClaimWindowForGPUDevice(rendererState.device, rendererState.window)) {
		GPU_Quit();
		return nullptr;
	}

	SDL_GPUPresentMode presentMode = choosePresentMode(rendererState.device, rendererState.window);
	if (!SDL_SetGPUSwapchainParameters(rendererState.device, rendererState.window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, presentMode)) {
		presentMode = SDL_GPU_PRESENTMODE_VSYNC;
		SDL_SetGPUSwapchainParameters(rendererState.device, rendererState.window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, presentMode);
	}
	sendToLog(LogLevel::Info, "SDL3_GPU present mode: %s\n", presentModeName(presentMode));

	auto *target         = new GPU_Target{};
	auto *context        = new GPU_Context{};
	context->windowID    = SDL_GetWindowID(rendererState.window);
	context->window_w    = w;
	context->window_h    = h;
	context->drawable_w  = w;
	context->drawable_h  = h;
	context->refcount    = 1;
	context->shapes_use_blending = true;

	target->renderer       = &rendererState;
	target->context_target = target;
	target->w              = w;
	target->h              = h;
	target->base_w         = w;
	target->base_h         = h;
	target->viewport       = GPU_Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
	target->context        = context;
	target->refcount       = 1;
	target->is_window      = true;

	rendererState.id                     = renderer_request;
	rendererState.current_context_target = target;
	rendererState.preinit_flags          = pendingPreinitFlags;
	rendererState.swapchain_format       = SDL_GetGPUSwapchainTextureFormat(rendererState.device, rendererState.window);
	rendererState.present_mode           = presentMode;

	if (!resizeTargetBacking(target, w, h)) {
		GPU_Quit();
		return nullptr;
	}

	return target;
}

void SDLCALL GPU_Quit(void) {
	if (rendererState.device)
		flushNativeBlitBatch();
	printAndResetTelemetry();
	if (rendererState.device)
		releaseSubmissionFences();

	if (rendererState.current_context_target) {
		auto *target = rendererState.current_context_target;
		if (target->image && !target->image->target) {
			releaseImageTexture(target->image);
			delete target->image;
			target->image = nullptr;
			target->texture = nullptr;
		}
		delete target->context;
		delete target;
		rendererState.current_context_target = nullptr;
	}

	if (rendererState.device && rendererState.window) {
		SDL_ReleaseWindowFromGPUDevice(rendererState.device, rendererState.window);
	}

	releaseAllImageTextures();
	releaseNativeRendererObjects();

	if (rendererState.window) {
		SDL_DestroyWindow(rendererState.window);
		rendererState.window = nullptr;
	}

	if (rendererState.device) {
		SDL_DestroyGPUDevice(rendererState.device);
		rendererState.device = nullptr;
	}
}

void SDLCALL GPU_PrintTelemetry(void) {
	if (rendererState.device)
		flushNativeBlitBatch();
	printAndResetTelemetry();
}

void SDLCALL GPU_SetDebugLevel(GPU_DebugLevelEnum level) {
	rendererState.debug_level = level;
}

GPU_Renderer *SDLCALL GPU_GetCurrentRenderer(void) {
	return rendererState.device ? &rendererState : nullptr;
}

GPU_Target *SDLCALL GPU_GetContextTarget(void) {
	return rendererState.current_context_target;
}

GPU_bool SDLCALL GPU_SetWindowResolution(Uint16 w, Uint16 h) {
	if (!rendererState.window || !rendererState.current_context_target)
		return false;

	flushNativeBlitBatch();
	if (!SDL_SetWindowSize(rendererState.window, w, h))
		return false;

	auto *target = rendererState.current_context_target;
	target->base_w = w;
	target->base_h = h;
	if (!target->using_virtual_resolution) {
		target->w = w;
		target->h = h;
		target->viewport = GPU_Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
		resizeTargetBacking(target, w, h);
	}
	if (target->context) {
		target->context->window_w   = w;
		target->context->window_h   = h;
		target->context->drawable_w = w;
		target->context->drawable_h = h;
	}
	return true;
}

void SDLCALL GPU_SetShapeBlending(GPU_bool enable) {
	if (rendererState.current_context_target && rendererState.current_context_target->context)
		rendererState.current_context_target->context->shapes_use_blending = enable;
}

GPU_Target *SDLCALL GPU_GetTarget(GPU_Image *image) {
	if (!image)
		return nullptr;

	if (image->target)
		return image->target;

	auto *target         = new GPU_Target{};
	target->renderer     = image->renderer;
	target->context_target = image->context_target;
	target->image        = image;
	target->w            = image->w;
	target->h            = image->h;
	target->base_w       = image->w;
	target->base_h       = image->h;
	target->viewport     = GPU_Rect{0, 0, static_cast<float>(image->w), static_cast<float>(image->h)};
	target->refcount     = 1;
	target->texture      = image->texture;

	image->target = target;
	ensureImageTextureInitialized(image);
	return target;
}

void SDLCALL GPU_SetVirtualResolution(GPU_Target *target, Uint16 w, Uint16 h) {
	if (!target)
		return;
	flushNativeBlitBatch();
	target->w = w;
	target->h = h;
	target->viewport = GPU_Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
	target->using_virtual_resolution = true;
	if (target->is_window)
		resizeTargetBacking(target, w, h);
}

GPU_Rect SDLCALL GPU_SetClipRect(GPU_Target *target, GPU_Rect rect) {
	GPU_Rect previous{};
	if (!target)
		return previous;
	previous              = target->clip_rect;
	target->clip_rect      = rect;
	target->use_clip_rect  = true;
	return previous;
}

void SDLCALL GPU_UnsetClip(GPU_Target *target) {
	if (target)
		target->use_clip_rect = false;
}

GPU_Image *SDLCALL GPU_CreateImage(Uint16 w, Uint16 h, GPU_FormatEnum format) {
	auto *image = new GPU_Image{};
	initialiseImageDefaults(image, w, h, format);
	createTexture(image);
	return image;
}

GPU_Image *SDLCALL GPU_CopyImage(GPU_Image *image) {
	if (!image)
		return nullptr;
	GPU_TelemetryScope telemetryScope("copy_image");
	flushNativeBlitBatch();
	ensureImagePixelsCurrent(image);
	GPU_Image *copy = GPU_CreateImage(image->w, image->h, image->format);
	copy->pixels    = image->pixels;
	copy->pitch     = image->pitch;
	copy->pixels_solid = image->pixels_solid;
	copy->solid_color  = image->solid_color;
	copy->color     = image->color;
	copy->use_blending = image->use_blending;
	copy->blend_mode = image->blend_mode;
	copy->snap_mode  = image->snap_mode;
	copy->filter_mode = image->filter_mode;
	uploadImage(copy, "copy_image");
	return copy;
}

void SDLCALL GPU_FreeImage(GPU_Image *image) {
	if (!image)
		return;

	flushNativeBlitBatch();
	if (image->refcount > 1) {
		--image->refcount;
		return;
	}

	releaseImageTexture(image);
	delete image->target;
	delete image;
}

void SDLCALL GPU_UpdateImage(GPU_Image *image, const GPU_Rect *image_rect, SDL_Surface *surface, const GPU_Rect *surface_rect) {
	if (!image || !surface)
		return;

	flushNativeBlitBatch();
	SDL_Surface *working = surface;
	bool freeWorking     = false;
	working = convertSurfaceForUpload(surface, image, freeWorking);

	if (!working)
		return;

	const int srcBpp = onsSurfaceBytesPerPixel(working);
	const int srcX   = surface_rect ? static_cast<int>(surface_rect->x) : 0;
	const int srcY   = surface_rect ? static_cast<int>(surface_rect->y) : 0;
	const int dstX   = image_rect ? static_cast<int>(image_rect->x) : 0;
	const int dstY   = image_rect ? static_cast<int>(image_rect->y) : 0;
	const int width  = image_rect ? static_cast<int>(image_rect->w) : (surface_rect ? static_cast<int>(surface_rect->w) : working->w);
	const int height = image_rect ? static_cast<int>(image_rect->h) : (surface_rect ? static_cast<int>(surface_rect->h) : working->h);

	int copySrcX = srcX;
	int copySrcY = srcY;
	int copyDstX = dstX;
	int copyDstY = dstY;
	int copyW    = width;
	int copyH    = height;

	if (copyDstX < 0) {
		const int delta = -copyDstX;
		copyDstX = 0;
		copySrcX += delta;
		copyW -= delta;
	}
	if (copyDstY < 0) {
		const int delta = -copyDstY;
		copyDstY = 0;
		copySrcY += delta;
		copyH -= delta;
	}
	if (copySrcX < 0) {
		const int delta = -copySrcX;
		copySrcX = 0;
		copyDstX += delta;
		copyW -= delta;
	}
	if (copySrcY < 0) {
		const int delta = -copySrcY;
		copySrcY = 0;
		copyDstY += delta;
		copyH -= delta;
	}
	copyW = std::min<int>(copyW, image->w - copyDstX);
	copyH = std::min<int>(copyH, image->h - copyDstY);
	copyW = std::min<int>(copyW, working->w - copySrcX);
	copyH = std::min<int>(copyH, working->h - copySrcY);
	if (copyW <= 0 || copyH <= 0) {
		if (freeWorking)
			SDL_FreeSurface(working);
		return;
	}

	const SDL_Rect bounds{copyDstX, copyDstY, copyW, copyH};
	const bool coversImage = imageRectCoversImage(image, bounds);
	if (!coversImage) {
		ensureImagePixelsCurrent(image);
		if (image->pixels_dirty) {
			if (freeWorking)
				SDL_FreeSurface(working);
			return;
		}
	}

	if (SDL_MUSTLOCK(working) && !SDL_LockSurface(working)) {
		if (freeWorking)
			SDL_FreeSurface(working);
		return;
	}

	if (coversImage && srcBpp == image->bytes_per_pixel) {
		const int rowBytes = copyW * image->bytes_per_pixel;
		const auto *src = static_cast<const Uint8 *>(working->pixels) + copySrcY * working->pitch + copySrcX * srcBpp;
		const bool uploaded = uploadImageRows(image,
		                                      bounds,
		                                      src,
		                                      static_cast<Uint32>(rowBytes),
		                                      static_cast<Uint32>(working->pitch),
		                                      "update_image",
		                                      true);
		if (SDL_MUSTLOCK(working))
			SDL_UnlockSurface(working);
		if (freeWorking)
			SDL_FreeSurface(working);
		if (uploaded) {
			std::vector<Uint8>().swap(image->pixels);
			image->pixels_dirty = false;
			image->pixels_solid = false;
		}
		return;
	}

	if (!ensureImagePixelStorage(image)) {
		if (SDL_MUSTLOCK(working))
			SDL_UnlockSurface(working);
		if (freeWorking)
			SDL_FreeSurface(working);
		return;
	}

	for (int y = 0; y < copyH; ++y) {
		const auto *src = static_cast<const Uint8 *>(working->pixels) + (copySrcY + y) * working->pitch + copySrcX * srcBpp;
		auto *dst       = image->pixels.data() + (copyDstY + y) * image->pitch + copyDstX * image->bytes_per_pixel;
		copyPixelRow(dst, image->bytes_per_pixel, src, srcBpp, copyW);
	}
	image->pixels_solid = false;

	if (SDL_MUSTLOCK(working))
		SDL_UnlockSurface(working);
	if (freeWorking)
		SDL_FreeSurface(working);

	uploadImageRegion(image, bounds, "update_image");
}

void SDLCALL GPU_UpdateImageBytes(GPU_Image *image, const GPU_Rect *image_rect, const unsigned char *bytes, int bytes_per_row) {
	if (!image || !bytes)
		return;

	flushNativeBlitBatch();
	const int dstX   = image_rect ? static_cast<int>(image_rect->x) : 0;
	const int dstY   = image_rect ? static_cast<int>(image_rect->y) : 0;
	const int width  = image_rect ? static_cast<int>(image_rect->w) : image->w;
	const int height = image_rect ? static_cast<int>(image_rect->h) : image->h;
	int copyDstX     = dstX;
	int copyDstY     = dstY;
	int copyW        = width;
	int copyH        = height;
	int srcOffsetX   = 0;
	int srcOffsetY   = 0;

	if (copyDstX < 0) {
		const int delta = -copyDstX;
		copyDstX = 0;
		srcOffsetX += delta;
		copyW -= delta;
	}
	if (copyDstY < 0) {
		const int delta = -copyDstY;
		copyDstY = 0;
		srcOffsetY += delta;
		copyH -= delta;
	}
	copyW = std::min<int>(copyW, image->w - copyDstX);
	copyH = std::min<int>(copyH, image->h - copyDstY);
	if (copyW <= 0 || copyH <= 0)
		return;

	const int srcByteOffset = srcOffsetX * image->bytes_per_pixel;
	const int rowBytes = std::min(copyW * image->bytes_per_pixel, bytes_per_row - srcByteOffset);
	if (rowBytes <= 0)
		return;
	copyW = rowBytes / image->bytes_per_pixel;
	if (copyW <= 0)
		return;

	const SDL_Rect bounds{copyDstX, copyDstY, copyW, copyH};
	const auto *src = bytes + srcOffsetY * bytes_per_row + srcByteOffset;
	if (uploadImageRows(image,
	                    bounds,
	                    src,
	                    static_cast<Uint32>(rowBytes),
	                    static_cast<Uint32>(bytes_per_row),
	                    "update_image_bytes",
	                    imageRectCoversImage(image, bounds))) {
		std::vector<Uint8>().swap(image->pixels);
		image->pixels_dirty = false;
		image->pixels_solid = false;
	}
}

GPU_bool SDLCALL GPU_SaveImage(GPU_Image *image, const char *filename, GPU_FileFormatEnum format) {
	if (!image || !filename)
		return false;

	SDL_Surface *surface = GPU_CopySurfaceFromImage(image);
	if (!surface)
		return false;
	bool saved = false;
	if (format == GPU_FILE_PNG || (format == GPU_FILE_AUTO && extensionIsPng(filename))) {
		SDL_RWops *rwops = SDL_RWFromFile(filename, "wb");
		saved = saveSurfacePNG_RW(surface, rwops, true);
	} else if (format == GPU_FILE_BMP || (format == GPU_FILE_AUTO && extensionIsBmp(filename))) {
		saved = SDL_SaveBMP(surface, filename);
	} else {
		SDL_SetError("Unsupported SDL3_GPU image save format");
	}
	SDL_FreeSurface(surface);
	return saved;
}

GPU_bool SDLCALL GPU_SaveImage_RW(GPU_Image *image, SDL_RWops *rwops, GPU_bool free_rwops, GPU_FileFormatEnum format) {
	if (!image || !rwops)
		return false;

	SDL_Surface *surface = GPU_CopySurfaceFromImage(image);
	if (!surface) {
		if (free_rwops)
			SDL_RWclose(rwops);
		return false;
	}
	bool saved = false;
	if (format == GPU_FILE_PNG) {
		saved = saveSurfacePNG_RW(surface, rwops, free_rwops);
	} else if (format == GPU_FILE_BMP) {
		saved = SDL_SaveBMP_RW(surface, rwops, free_rwops);
	} else {
		SDL_SetError("Unsupported SDL3_GPU image save format");
		if (free_rwops)
			SDL_RWclose(rwops);
	}
	SDL_FreeSurface(surface);
	return saved;
}

void SDLCALL GPU_GenerateMipmaps(GPU_Image *image) {
	if (!image || !image->texture || !rendererState.device)
		return;
	flushNativeBlitBatch();
	if (mipLevelCountForSize(image->w, image->h) <= 1)
		return;
	if (!recreateImageTextureForMipmaps(image))
		return;
	if (!ensureImageTextureInitialized(image))
		return;

	SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(rendererState.device);
	if (!commands)
		return;
	SDL_GenerateMipmapsForGPUTexture(commands, image->texture);
	if (submitGPUCommandBuffer(commands)) {
		image->has_mipmaps = true;
	}
}

void SDLCALL GPU_SetRGBA(GPU_Image *image, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
	if (!image)
		return;
	if (image->color.r == r && image->color.g == g && image->color.b == b && image->color.a == a)
		return;
	image->color = SDL_Color{r, g, b, a};
}

void SDLCALL GPU_SetBlending(GPU_Image *image, GPU_bool enable) {
	if (image)
		image->use_blending = enable;
}

void SDLCALL GPU_SetBlendMode(GPU_Image *image, GPU_BlendPresetEnum mode) {
	if (!image)
		return;
	if (mode == GPU_BLEND_ADD) {
		image->blend_mode.source_color = GPU_FUNC_SRC_ALPHA;
		image->blend_mode.dest_color   = GPU_FUNC_ONE;
	} else {
		image->blend_mode = normalBlendMode();
	}
}

void SDLCALL GPU_SetImageFilter(GPU_Image *image, GPU_FilterEnum filter) {
	if (image)
		image->filter_mode = filter;
}

void SDLCALL GPU_SetSnapMode(GPU_Image *image, GPU_SnapEnum mode) {
	if (image)
		image->snap_mode = mode;
}

GPU_Image *SDLCALL GPU_CopyImageFromSurface(SDL_Surface *surface) {
	if (!surface)
		return nullptr;
	GPU_TelemetryScope telemetryScope("copy_image_from_surface");
	GPU_Image *image = GPU_CreateImage(surface->w, surface->h, onsSurfaceBytesPerPixel(surface) == 4 ? GPU_FORMAT_RGBA : GPU_FORMAT_RGB);
	GPU_UpdateImage(image, nullptr, surface, nullptr);
	return image;
}

GPU_Image *SDLCALL GPU_CopyImageFromTarget(GPU_Target *target) {
	if (!target)
		return nullptr;
	GPU_TelemetryScope telemetryScope("copy_image_from_target");
	ensureTargetBacking(target);
	if (target->image)
		return GPU_CopyImage(target->image);

	GPU_Image *image = GPU_CreateImage(target->w, target->h, GPU_FORMAT_RGBA);
	return image;
}

SDL_Surface *SDLCALL GPU_CopySurfaceFromImage(GPU_Image *image) {
	if (!image)
		return nullptr;
	GPU_TelemetryScope telemetryScope("copy_surface_from_image");
	flushNativeBlitBatch();

	SDL_Surface *surface = onsCreateRGBSurface(SDL_SWSURFACE, image->w, image->h, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
	if (!surface)
		return nullptr;

	if (image->pixels_dirty && downloadImageToSurface(image, surface))
		return surface;

	if (image->pixels_solid && fillSurfaceWithSolidImageColor(image, surface))
		return surface;

	ensureImagePixelsCurrent(image);
	if (copyImageRowsToSurface(image, surface, image->pixels.data()))
		return surface;

	SDL_FreeSurface(surface);
	return nullptr;
}

void SDLCALL GPU_MatrixMode(int matrix_mode) {
	if (rendererState.current_context_target && rendererState.current_context_target->context)
		rendererState.current_context_target->context->matrix_mode = matrix_mode;
}

void SDLCALL GPU_PushMatrix(void) {}
void SDLCALL GPU_PopMatrix(void) {}
void SDLCALL GPU_LoadIdentity(void) {}
void SDLCALL GPU_Frustum(float, float, float, float, float, float) {}

void SDLCALL GPU_ClearRGBA(GPU_Target *target, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
	if (!target)
		return;
	flushNativeBlitBatch();
	target->color = SDL_Color{r, g, b, a};
	target->use_color = true;

	if (ensureTargetBacking(target)) {
		const GPU_Rect full{0.0f, 0.0f, static_cast<float>(target->image->w), static_cast<float>(target->image->h)};
		const SDL_Rect bounds = targetPixelBounds(target, full);
		if (bounds.w <= 0 || bounds.h <= 0)
			return;

		const bool coversImage = imageRectCoversImage(target->image, bounds);
		if (coversImage && clearImageTexture(target->image, target->color))
			return;

		const std::string telemetrySource = clearFullTelemetrySource(target->image, bounds, coversImage);
		if (!coversImage && nativeSolidRect(target, bounds, target->color))
			return;

		GPU_TelemetryScope telemetryScope(telemetrySource.c_str());
		if (!coversImage)
			ensureImagePixelsCurrent(target->image);

		fillImagePixels(target->image, bounds, target->color);
		uploadImageRegion(target->image, bounds, telemetrySource.c_str());
	}
}

void SDLCALL GPU_Blit(GPU_Image *image, GPU_Rect *src_rect, GPU_Target *target, float x, float y) {
	if (!nativeBlit(image, src_rect, target, x, y, 0.0f, 1.0f, 1.0f)) {
		flushNativeBlitBatch();
		cpuBlit(image, src_rect, target, x, y, 0.0f, 1.0f, 1.0f);
	}
}

void SDLCALL GPU_BlitRotate(GPU_Image *image, GPU_Rect *src_rect, GPU_Target *target, float x, float y, float degrees) {
	if (!nativeBlit(image, src_rect, target, x, y, degrees, 1.0f, 1.0f)) {
		flushNativeBlitBatch();
		cpuBlit(image, src_rect, target, x, y, degrees, 1.0f, 1.0f);
	}
}

void SDLCALL GPU_BlitScale(GPU_Image *image, GPU_Rect *src_rect, GPU_Target *target, float x, float y, float scaleX, float scaleY) {
	if (!nativeBlit(image, src_rect, target, x, y, 0.0f, scaleX, scaleY)) {
		flushNativeBlitBatch();
		cpuBlit(image, src_rect, target, x, y, 0.0f, scaleX, scaleY);
	}
}

void SDLCALL GPU_BlitTransform(GPU_Image *image, GPU_Rect *src_rect, GPU_Target *target, float x, float y, float degrees, float scaleX, float scaleY) {
	if (!nativeBlit(image, src_rect, target, x, y, degrees, scaleX, scaleY)) {
		flushNativeBlitBatch();
		cpuBlit(image, src_rect, target, x, y, degrees, scaleX, scaleY);
	}
}

void SDLCALL GPU_TriangleBatch(GPU_Image *image, GPU_Target *target, unsigned short num_vertices, float *values,
                               unsigned int num_indices, unsigned short *indices, GPU_BatchFlagEnum flags) {
	if (!image || !values || !indices || num_vertices == 0 || num_indices == 0)
		return;

	const bool xyz = (flags & GPU_BATCH_XYZ) == GPU_BATCH_XYZ;
	const bool st  = (flags & GPU_BATCH_ST) == GPU_BATCH_ST;
	const int stride = (xyz ? 3 : 2) + (st ? 2 : 0);
	if (!st || stride <= 0) {
		setUnsupported("GPU_TriangleBatch without texture coordinates");
		return;
	}

	thread_local std::vector<SDL3GPUVertex> triangleBatchVertices;
	triangleBatchVertices.resize(num_vertices);
	const SDL_Color color = image->color;
	const float r = color.r / 255.0f;
	const float g = color.g / 255.0f;
	const float b = color.b / 255.0f;
	const float a = color.a / 255.0f;
	for (unsigned short i = 0; i < num_vertices; ++i) {
		const float *src = values + i * stride;
		triangleBatchVertices[i].x = src[0];
		triangleBatchVertices[i].y = src[1];
		triangleBatchVertices[i].r = r;
		triangleBatchVertices[i].g = g;
		triangleBatchVertices[i].b = b;
		triangleBatchVertices[i].a = a;
		const int uvOffset = xyz ? 3 : 2;
		triangleBatchVertices[i].s = src[uvOffset];
		triangleBatchVertices[i].t = src[uvOffset + 1];
	}

	if (!renderNativeIndexedTriangles(image, target, triangleBatchVertices.data(), num_vertices, indices, num_indices))
		setUnsupported("GPU_TriangleBatch native draw");
}

void SDLCALL GPU_TriangleBatchRGBA(GPU_Image *image, GPU_Target *target, unsigned short num_vertices,
                                   const GPU_TriangleBatchVertex *vertices, unsigned int num_indices,
                                   const unsigned short *indices) {
	if (!image || !vertices || !indices || num_vertices == 0 || num_indices == 0)
		return;

	if (!renderNativeIndexedTriangles(image, target, vertices, num_vertices, indices, num_indices))
		setUnsupported("GPU_TriangleBatchRGBA native draw");
}

void SDLCALL GPU_FlushBlitBuffer(void) {
	flushNativeBlitBatch();
}

#if defined(DROID)
static std::atomic<bool> presentationSuspended{false};
static std::atomic<bool> swapchainNeedsRebuild{false};
// Set when the presented canvas no longer matches the swapchain's shape; the
// engine owns the geometry, so it clears this and recomputes.
static std::atomic<bool> surfaceGeometryStale{false};

bool GPU_TakeSurfaceGeometryStale() {
	return surfaceGeometryStale.exchange(false, std::memory_order_acq_rel);
}

void GPU_SetPresentationSuspended(bool suspended) {
	const bool wasSuspended = presentationSuspended.exchange(suspended, std::memory_order_acq_rel);
	if (wasSuspended && !suspended)
		swapchainNeedsRebuild.store(true, std::memory_order_release);
}

/**
 * Rebinds the swapchain to the surface Android just handed back.
 *
 * A background trip destroys the native window and creates a fresh one. The
 * swapchain still points at the old surface, so presenting succeeds while
 * drawing nowhere -- the symptom is a black screen with audio still playing.
 *
 * Runs from GPU_Flip because the render thread owns the device; the lifecycle
 * watch that sets the flag runs on whichever thread pumped the event.
 */
static bool rebuildSwapchainIfNeeded() {
	if (!swapchainNeedsRebuild.exchange(false, std::memory_order_acq_rel))
		return true;
	if (!rendererState.device || !rendererState.window) {
		swapchainNeedsRebuild.store(true, std::memory_order_release);
		return false;
	}

	SDL_ReleaseWindowFromGPUDevice(rendererState.device, rendererState.window);
	if (!SDL_ClaimWindowForGPUDevice(rendererState.device, rendererState.window)) {
		sendToLog(LogLevel::Error, "Failed to reclaim window after resume: %s\n", SDL_GetError());
		swapchainNeedsRebuild.store(true, std::memory_order_release);
		return false;
	}

	SDL_GPUPresentMode presentMode = choosePresentMode(rendererState.device, rendererState.window);
	if (!SDL_SetGPUSwapchainParameters(rendererState.device, rendererState.window,
	                                   SDL_GPU_SWAPCHAINCOMPOSITION_SDR, presentMode)) {
		presentMode = SDL_GPU_PRESENTMODE_VSYNC;
		if (!SDL_SetGPUSwapchainParameters(rendererState.device, rendererState.window,
		                                   SDL_GPU_SWAPCHAINCOMPOSITION_SDR, presentMode)) {
			sendToLog(LogLevel::Error, "Failed to restore swapchain parameters after resume: %s\n", SDL_GetError());
			swapchainNeedsRebuild.store(true, std::memory_order_release);
			return false;
		}
	}
	sendToLog(LogLevel::Info, "Swapchain rebuilt after resume (%s)\n", presentModeName(presentMode));
	return true;
}
#endif

void SDLCALL GPU_Flip(GPU_Target *target) {
#if defined(DROID)
	// The surface may already be gone; presenting into it crashes in libvulkan.
	if (presentationSuspended.load(std::memory_order_acquire))
		return;
	if (!rebuildSwapchainIfNeeded() || presentationSuspended.load(std::memory_order_acquire))
		return;
#endif
	if (!rendererState.device || !rendererState.window)
		return;
	flushNativeBlitBatch();
	if (!target)
		target = rendererState.current_context_target;
	if (!ensureTargetBacking(target))
		return;

	SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(rendererState.device);
	if (!commands)
		return;

	SDL_GPUTexture *swapchainTexture = nullptr;
	Uint32 width = 0;
	Uint32 height = 0;
	if (!SDL_AcquireGPUSwapchainTexture(commands, rendererState.window, &swapchainTexture, &width, &height) || !swapchainTexture) {
		SDL_CancelGPUCommandBuffer(commands);
		return;
	}

#if defined(DROID)
	// The surface can change size under us -- rotation, multi-window, or
	// starting up before the display has settled -- and everything downstream is
	// scaled from these numbers, so log them whenever they move rather than
	// guessing later.
	{
		// Both sizes are tracked because either can change alone -- the surface
		// on a rotation or window drag, the canvas when the engine recomputes it.
		// A mismatch is rechecked until repaired so a transient zero-size window
		// cannot consume the one repair request and leave stale geometry forever.
		static Uint32 lastSwapW = 0, lastSwapH = 0;
		static int lastTargetW = 0, lastTargetH = 0;
		static bool geometryMismatch = false;
		const bool dimensionsChanged = width != lastSwapW || height != lastSwapH ||
		                               target->w != lastTargetW || target->h != lastTargetH;
		if (dimensionsChanged) {
			lastSwapW   = width;
			lastSwapH   = height;
			lastTargetW = target->w;
			lastTargetH = target->h;

			sendToLog(LogLevel::Info,
			          "Swapchain %ux%u, target %dx%d (base %dx%d, virtual %d), image %dx%d\n",
			          width, height, target->w, target->h, target->base_w, target->base_h,
			          target->using_virtual_resolution ? 1 : 0,
			          target->image ? target->image->w : -1,
			          target->image ? target->image->h : -1);

		}

		// The canvas is blitted to fill the swapchain, so disagreeing aspects
		// are exactly what stretching looks like. applySurfaceGeometry() is meant
		// to make that unreachable; this check repairs it if the invariant is
		// ever broken.
		if (dimensionsChanged || geometryMismatch) {
			bool mismatch = false;
			if (width > 0 && height > 0 && target->w > 0 && target->h > 0) {
				const float swapAspect   = static_cast<float>(width) / static_cast<float>(height);
				const float targetAspect = static_cast<float>(target->w) / static_cast<float>(target->h);
				mismatch = std::fabs(swapAspect - targetAspect) > 0.01f * swapAspect;
			}
			if (mismatch) {
				if (!geometryMismatch) {
					sendToLog(LogLevel::Warn,
					          "Canvas %dx%d does not match surface %ux%u; recomputing\n",
					          target->w, target->h, width, height);
				}
				surfaceGeometryStale.store(true, std::memory_order_release);
			}
			geometryMismatch = mismatch;
		}
	}
#endif

	presentTarget(target, commands, swapchainTexture, width, height);
	submitGPUCommandBuffer(commands);
}

void SDLCALL GPU_RectangleFilled2(GPU_Target *target, GPU_Rect rect, SDL_Color color) {
	if (!target || !ensureTargetBacking(target))
		return;

	flushNativeBlitBatch();
	const SDL_Rect bounds = targetPixelBounds(target, rect);
	if (bounds.w <= 0 || bounds.h <= 0)
		return;

	const bool coversImage = imageRectCoversImage(target->image, bounds);
	if (coversImage && clearImageTexture(target->image, color))
		return;

	if (!coversImage && nativeSolidRect(target, bounds, color))
		return;

	if (!coversImage)
		ensureImagePixelsCurrent(target->image);

	fillImagePixels(target->image, bounds, color);
	uploadImageRegion(target->image, bounds, "clear_rect");
}

Uint32 SDLCALL GPU_CompileShader_RW(GPU_ShaderEnum shader_type, SDL_RWops *shader_source, GPU_bool free_rwops) {
	if (!shader_source) {
		setShaderMessage("No shader source was provided");
		return 0;
	}

	std::string source;
	char buffer[4096];
	while (true) {
		const size_t read = onsRWread(shader_source, buffer, 1, sizeof(buffer));
		if (read == 0)
			break;
		source.append(buffer, read);
	}
	if (free_rwops)
		SDL_RWclose(shader_source);

	const SDL3GPUShaderKind kind = identifyShaderSource(shader_type, source);
	SDL3GPUShaderObject shaderObject{};
	shaderObject.type = shader_type;
	shaderObject.kind = kind;
	shaderObject.source = std::move(source);
	if (kind != SDL3GPUShaderKind::Unknown)
		compilePrecompiledBuiltInShader(shader_type, kind, shaderObject);
	if (kind == SDL3GPUShaderKind::Unknown &&
	    !compileNativeExternalShader(shader_type, shaderObject.source, shaderObject))
		return 0;
#if defined(ONS_USE_SDL3_SHADERCROSS)
	if (kind != SDL3GPUShaderKind::Unknown && !shaderObject.nativeShader && shader_type != GPU_VERTEX_SHADER &&
	    !compileNativeExternalShader(shader_type, shaderObject.source, shaderObject)) {
		shaderObject.nativeResources = SDL3GPUNativeResourceInfo{};
		shaderObject.nativeUniforms.clear();
		shaderObject.translatedLegacyGLSL = false;
	}
#endif

	const Uint32 shader = nextShaderObject++;
	const bool native = shaderObject.nativeShader != nullptr;
	noteShaderCompilation(kind, native);
	shaderObjects[shader] = std::move(shaderObject);
	if (native) {
		std::snprintf(shaderMessage, sizeof(shaderMessage), "Compiled SDL3 native shader");
	} else {
		std::snprintf(shaderMessage, sizeof(shaderMessage), "Compiled SDL3 compatibility shader: %s", shaderKindName(kind));
	}
	return shader;
}

Uint32 SDLCALL GPU_LinkShaders(Uint32 shader_object1, Uint32 shader_object2) {
	Uint32 objects[2]{shader_object1, shader_object2};
	return GPU_LinkManyShaders(objects, 2);
}

Uint32 SDLCALL GPU_LinkManyShaders(Uint32 *shader_objects, int count) {
	if (!shader_objects || count <= 0) {
		setShaderMessage("No shader objects were provided for linking");
		return 0;
	}

	SDL3GPUProgramObject program{};
	bool hasVertex = false;
	for (int i = 0; i < count; ++i) {
		const Uint32 shader = shader_objects[i];
		auto it = shaderObjects.find(shader);
		if (it == shaderObjects.end()) {
			setShaderMessage("Unknown shader object passed to SDL3 compatibility linker");
			return 0;
		}
		program.shaders.push_back(shader);
		if (it->second.type == GPU_VERTEX_SHADER) {
			hasVertex = true;
			if (it->second.nativeShader)
				program.nativeVertexShader = it->second.nativeShader;
		} else if (it->second.type == GPU_FRAGMENT_SHADER || it->second.type == GPU_PIXEL_SHADER) {
			if (it->second.nativeShader) {
				program.nativeFragmentShader = it->second.nativeShader;
				program.nativeFragmentResources = it->second.nativeResources;
				program.nativeUniforms = it->second.nativeUniforms;
				program.nativeSamplerImageUnits = it->second.nativeSamplerImageUnits;
				program.kind = it->second.kind;
			} else {
				program.kind = it->second.kind;
			}
		}
	}

	if (program.nativeFragmentShader) {
		if (!hasVertex) {
			setShaderMessage("SDL3 native shader program must link a vertex shader");
			return 0;
		}
		Uint32 registerCount = 0;
		for (size_t i = 0; i < program.nativeUniforms.size(); ++i) {
			const auto &uniform = program.nativeUniforms[i];
			program.nativeUniformLookup[uniform.name] = i;
			for (int element = 0; element < uniform.arraySize; ++element)
				program.nativeUniformLookup[indexedUniformName(uniform.name.c_str(), element)] = i;
			registerCount = std::max<Uint32>(registerCount, uniform.registerIndex + static_cast<Uint32>(uniform.arraySize));
		}
		program.nativeUniformRegisters.resize(registerCount);
		const Uint32 object = nextShaderObject++;
		programObjects[object] = std::move(program);
		std::snprintf(shaderMessage, sizeof(shaderMessage), "Linked SDL3 native shadercross program");
		return object;
	}

	if (!hasVertex || program.kind == SDL3GPUShaderKind::Unknown || program.kind == SDL3GPUShaderKind::DefaultVertex) {
		setShaderMessage("SDL3 compatibility shader program must link a known fragment shader with a vertex shader");
		return 0;
	}

	const Uint32 object = nextShaderObject++;
	programObjects[object] = std::move(program);
	std::snprintf(shaderMessage, sizeof(shaderMessage), "Linked SDL3 compatibility shader program: %s", shaderKindName(programObjects[object].kind));
	return object;
}

GPU_bool SDLCALL GPU_LinkShaderProgram(Uint32 program_object) {
	return program_object != 0 && programObjects.contains(program_object);
}

void SDLCALL GPU_ActivateShaderProgram(Uint32 program_object, GPU_ShaderBlock *block) {
	if (program_object != 0 && !programObjects.contains(program_object)) {
		setShaderMessage("Cannot activate unknown SDL3 compatibility shader program");
		return;
	}
	flushNativeBlitBatch();
	if (rendererState.current_context_target && rendererState.current_context_target->context) {
		rendererState.current_context_target->context->current_shader_program = program_object;
		if (block)
			rendererState.current_context_target->context->current_shader_block = *block;
	}
}

void SDLCALL GPU_DeactivateShaderProgram(void) {
	flushNativeBlitBatch();
	if (rendererState.current_context_target && rendererState.current_context_target->context)
		rendererState.current_context_target->context->current_shader_program = 0;
}

const char *SDLCALL GPU_GetShaderMessage(void) {
	return shaderMessage;
}

int SDLCALL GPU_GetUniformLocation(Uint32 program_object, const char *uniform_name) {
	if (!uniform_name || program_object == 0)
		return -1;
	auto programIt = programObjects.find(program_object);
	if (programIt == programObjects.end())
		return -1;

	auto &program = programIt->second;
	auto existing = program.uniformLocations.find(uniform_name);
	if (existing != program.uniformLocations.end())
		return existing->second;

	const int location = nextUniformLocation++;
	program.uniformLocations[uniform_name] = location;
	program.locationNames[location] = uniform_name;
	uniformLocationOwners[location] = program_object;
	return location;
}

GPU_ShaderBlock SDLCALL GPU_LoadShaderBlock(Uint32, const char *, const char *, const char *, const char *) {
	GPU_ShaderBlock block{};
	block.position_loc              = 0;
	block.texcoord_loc              = 1;
	block.color_loc                 = 2;
	block.modelViewProjection_loc   = 3;
	return block;
}

void SDLCALL GPU_SetShaderImage(GPU_Image *image, int, int image_unit) {
	auto *program = activeProgramObject();
	if (!program || image_unit < 0 || image_unit >= static_cast<int>(program->images.size()))
		return;
	program->images[static_cast<size_t>(image_unit)] = image;
}

void SDLCALL GPU_SetUniformi(int location, int value) {
	auto owner = uniformLocationOwners.find(location);
	if (owner == uniformLocationOwners.end())
		return;
	auto programIt = programObjects.find(owner->second);
	if (programIt == programObjects.end())
		return;
	auto nameIt = programIt->second.locationNames.find(location);
	if (nameIt == programIt->second.locationNames.end())
		return;

	SDL3GPUUniformValue uniform{};
	uniform.type = SDL3GPUUniformType::Int;
	uniform.intValue = value;
	uniform.values[0] = static_cast<float>(value);
	programIt->second.uniforms[nameIt->second] = uniform;
	updateNativeUniformRegister(programIt->second, nameIt->second, uniform);
}

void SDLCALL GPU_SetUniformf(int location, float value) {
	auto owner = uniformLocationOwners.find(location);
	if (owner == uniformLocationOwners.end())
		return;
	auto programIt = programObjects.find(owner->second);
	if (programIt == programObjects.end())
		return;
	auto nameIt = programIt->second.locationNames.find(location);
	if (nameIt == programIt->second.locationNames.end())
		return;

	SDL3GPUUniformValue uniform{};
	uniform.type = SDL3GPUUniformType::Float;
	uniform.values[0] = value;
	programIt->second.uniforms[nameIt->second] = uniform;
	updateNativeUniformRegister(programIt->second, nameIt->second, uniform);
}

void SDLCALL GPU_SetUniformfv(int location, int num_elements_per_value, int, float *values) {
	if (!values)
		return;
	auto owner = uniformLocationOwners.find(location);
	if (owner == uniformLocationOwners.end())
		return;
	auto programIt = programObjects.find(owner->second);
	if (programIt == programObjects.end())
		return;
	auto nameIt = programIt->second.locationNames.find(location);
	if (nameIt == programIt->second.locationNames.end())
		return;

	SDL3GPUUniformValue uniform{};
	uniform.type = SDL3GPUUniformType::FloatVec;
	uniform.components = std::clamp(num_elements_per_value, 1, 4);
	for (int i = 0; i < uniform.components; ++i)
		uniform.values[i] = values[i];
	programIt->second.uniforms[nameIt->second] = uniform;
	updateNativeUniformRegister(programIt->second, nameIt->second, uniform);
}

GPU_bool SDLCALL GPU_MultiplyAlpha(GPU_Image *image, const GPU_Rect *dst_clip) {
	if (!image || image->format == GPU_FORMAT_RGB)
		return true;
	flushNativeBlitBatch();
	GPU_Target *target = GPU_GetTarget(image);
	if (!target)
		return false;

	const GPU_Rect full{0.0f, 0.0f, static_cast<float>(image->w), static_cast<float>(image->h)};
	const SDL_Rect bounds = imagePixelBounds(image, dst_clip ? *dst_clip : full);
	if (bounds.w <= 0 || bounds.h <= 0)
		return true;
	if (!dst_clip && rendererState.device && image->texture)
		return false;

	ensureImagePixelsCurrent(image);
	if (image->pixels.empty())
		return false;

	for (int y = bounds.y; y < bounds.y + bounds.h; ++y) {
		auto *row = image->pixels.data() + y * image->pitch;
		for (int x = bounds.x; x < bounds.x + bounds.w; ++x) {
			Uint8 *pixel = row + x * image->bytes_per_pixel;
			const int alpha = pixel[3];
			pixel[0] = static_cast<Uint8>((static_cast<int>(pixel[0]) * alpha + 127) / 255);
			pixel[1] = static_cast<Uint8>((static_cast<int>(pixel[1]) * alpha + 127) / 255);
			pixel[2] = static_cast<Uint8>((static_cast<int>(pixel[2]) * alpha + 127) / 255);
		}
	}

	const bool uploaded = uploadImageRegion(image, bounds, "multiply_alpha");
	if (uploaded)
		discardCleanImagePixels(image);
	return uploaded;
}

void SDLCALL GPU_DiscardImagePixels(GPU_Image *image) {
	discardCleanImagePixels(image);
}

int SDLCALL GPU_RunMusicBoxBenchmark(int iterations, int width, int height, const char *outputPath) {
	iterations = std::max(1, iterations);
	width      = std::max(320, width);
	height     = std::max(240, height);

	BenchmarkOutputFile benchmarkOutput(outputPath);
	if (outputPath && outputPath[0] && !benchmarkOutput.file) {
		std::fprintf(stderr, "Music Box benchmark output open failed: %s\n", outputPath);
		return 1;
	}

	if (!onsSDLInit(SDL_INIT_VIDEO)) {
		std::fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
		return 1;
	}

	GPU_SetDebugLevel(GPU_DEBUG_LEVEL_0);
	GPU_SetPreInitFlags(GPU_INIT_DISABLE_VSYNC);
	GPU_Target *screen = GPU_InitRendererByID(GPU_MakeRendererID("SDL3_GPU", GPU_RENDERER_SDL3_GPU, 3, 0),
	                                          static_cast<Uint16>(width), static_cast<Uint16>(height),
	                                          SDL_WINDOW_HIDDEN);
	if (!screen) {
		std::fprintf(stderr, "SDL3_GPU init failed: %s\n", SDL_GetError());
		SDL_Quit();
		return 1;
	}

	// Build a synthetic Music Box scrollable element tree matching the real
	// *bgm_mode_ps3 layout: 2 columns, 800px wide elements, 80px tall, 50px
	// column gap, 150px first/last margin, ~200 unlocked BGM entries. Each
	// element carries the same StringTree branches that drawSpecialScrollable
	// queries every frame (x, y, height, width, bg, text, textmarginwidth,
	// textmargintop).
	const int elementWidth   = 800;
	const int elementHeight  = 80;
	const int columnGap      = 50;
	const int firstMargin    = 150;
	const int columns        = 2;
	const int totalElements  = 200;
	const int scrollY        = 0;
	const int visibleHeight  = 1080;

	struct Element {
		std::unordered_map<std::string, std::string> branches;
		std::string insertionKey;
	};
	std::vector<Element> elements;
	elements.reserve(totalElements);
	int currentY       = firstMargin;
	int currentX       = 0;
	int currentColumn  = 0;
	for (int i = 0; i < totalElements; ++i) {
		Element e;
		e.insertionKey = std::to_string(i);
		auto &b = e.branches;
		b["height"]          = std::to_string(elementHeight);
		b["width"]           = std::to_string(elementWidth);
		b["bg"]              = "318";
		b["textmarginwidth"] = "55";
		b["textmargintop"]   = "13";
		b["text"]            = "♪suspicious_audio_id_" + std::to_string(i + 1);
		if (currentColumn > 0)
			b["x"] = std::to_string(currentX);
		b["y"] = std::to_string(currentY);
		b["col"] = std::to_string(currentColumn);
		elements.push_back(std::move(e));
		currentColumn = (currentColumn + 1) % columns;
		if (currentColumn == 0) {
			currentY += elementHeight;
			currentX  = 0;
		} else {
			currentX += elementWidth + columnGap;
		}
	}

	// Precomputed (cached) geometry per element + the sorted y-end array used
	// by the optimized lookup path. This mirrors the ScrollableInfo cache added
	// to drawSpecialScrollable.
	struct CachedElement {
		int x, y, width, height;
		int textMarginLeft, textMarginRight, textMarginTop;
		int bgIndex;
		const std::string *text;
		bool hasText;
	};
	std::vector<CachedElement> cached;
	cached.reserve(totalElements);
	std::vector<int> yEndCache;
	yEndCache.reserve(totalElements);
	for (const auto &e : elements) {
		CachedElement c{};
		const auto &b = e.branches;
		c.x               = b.count("x") ? std::stoi(b.at("x")) : 0;
		c.y               = std::stoi(b.at("y"));
		c.width           = b.count("width") ? std::stoi(b.at("width")) : elementWidth;
		c.height          = b.count("height") ? std::stoi(b.at("height")) : elementHeight;
		c.textMarginLeft  = std::stoi(b.at("textmarginwidth"));
		c.textMarginRight = std::stoi(b.at("textmarginwidth"));
		c.textMarginTop   = std::stoi(b.at("textmargintop"));
		c.bgIndex         = std::stoi(b.at("bg"));
		c.text            = &b.at("text");
		c.hasText         = b.count("text") != 0;
		cached.push_back(c);
		yEndCache.push_back(c.y + c.height);
	}

	SDL_Surface *bgSurface    = createBenchmarkSurface(elementWidth, elementHeight);
	SDL_Surface *textSurface  = createBenchmarkSurface(700, 50);
	if (!bgSurface || !textSurface) {
		std::fprintf(stderr, "Music Box benchmark surface creation failed: %s\n", SDL_GetError());
		if (bgSurface) SDL_FreeSurface(bgSurface);
		if (textSurface) SDL_FreeSurface(textSurface);
		GPU_Quit();
		SDL_Quit();
		return 1;
	}

	GPU_Image *bgImage   = GPU_CopyImageFromSurface(bgSurface);
	GPU_Image *textImage = GPU_CopyImageFromSurface(textSurface);
	GPU_Image *target    = GPU_CreateImage(static_cast<Uint16>(width), static_cast<Uint16>(height), GPU_FORMAT_RGBA);
	GPU_Target *targetSurface = target ? GPU_GetTarget(target) : nullptr;
	if (!bgImage || !textImage || !target || !targetSurface) {
		std::fprintf(stderr, "Music Box benchmark image creation failed: %s\n", SDL_GetError());
		if (bgImage) GPU_FreeImage(bgImage);
		if (textImage) GPU_FreeImage(textImage);
		if (target) GPU_FreeImage(target);
		SDL_FreeSurface(bgSurface);
		SDL_FreeSurface(textSurface);
		GPU_Quit();
		SDL_Quit();
		return 1;
	}

	// Determine the visible element range once (Music Box at scroll_y=0 shows
	// ~13 rows x 2 columns). The find-visible stages below measure the cost of
	// computing this range each frame.
	auto findVisibleUncached = [&]() -> std::pair<int, int> {
		auto res = std::lower_bound(
		    elements.begin(), elements.end(), Element{},
		    [&](const Element &a, const Element &b) {
			    const auto &ba = a.branches;
			    const auto &bb = b.branches;
			    int ya = a.insertionKey.empty() ? scrollY : (ba.count("y") ? std::stoi(ba.at("y")) : 0) + (ba.count("height") ? std::stoi(ba.at("height")) : elementHeight);
			    int yb = b.insertionKey.empty() ? scrollY : (bb.count("y") ? std::stoi(bb.at("y")) : 0) + (bb.count("height") ? std::stoi(bb.at("height")) : elementHeight);
			    return ya < yb;
		    });
		int first = static_cast<int>(res - elements.begin());
		int last  = first;
		for (int i = first; i < totalElements; ++i) {
			int y = std::stoi(elements[i].branches.at("y"));
			if (y - scrollY > visibleHeight)
				break;
			last = i + 1;
		}
		return {first, last};
	};
	auto findVisibleCached = [&]() -> std::pair<int, int> {
		auto res = std::lower_bound(yEndCache.begin(), yEndCache.end(), scrollY);
		int first = static_cast<int>(res - yEndCache.begin());
		int last  = first;
		for (int i = first; i < totalElements; ++i) {
			if (cached[i].y - scrollY > visibleHeight)
				break;
			last = i + 1;
		}
		return {first, last};
	};

	auto [visibleFirst, visibleLast] = findVisibleCached();
	const int visibleCount = visibleLast - visibleFirst;

	// Warm up the renderer pipelines so the timed runs do not include one-off
	// shader/texture setup.
	for (int i = 0; i < 8; ++i) {
		GPU_ClearRGBA(targetSurface, 0, 0, 0, 0);
		for (int j = visibleFirst; j < visibleLast; ++j) {
			GPU_Blit(bgImage, nullptr, targetSurface, 16.0f + cached[j].x, 16.0f + cached[j].y);
			GPU_Blit(textImage, nullptr, targetSurface, 16.0f + cached[j].x + 55.0f, 16.0f + cached[j].y + 13.0f);
		}
		GPU_FlushBlitBuffer();
	}
	finishBenchmarkWork(target);

	printBenchmarkLine(benchmarkOutput.file, "onscripter-new Music Box benchmark\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "config: %dx%d, %d iterations, %d elements (%d visible/frame), scroll_y=%d\n",
	                   width, height, iterations, totalElements, visibleCount, scrollY);
	printBenchmarkLine(benchmarkOutput.file,
	                   "frame budget @144Hz: 6944 us | @120Hz: 8333 us | @60Hz: 16667 us\n\n");

	printBenchmarkLine(benchmarkOutput.file, "case,iterations,total_ms,avg_us\n");

	// --- find first visible element (getScrollableElementsVisibleAt) ---
	printBenchmarkResult(benchmarkOutput.file, "musicbox_find_visible_uncached", iterations, benchmarkMs([&]() {
		for (int i = 0; i < iterations; ++i) {
			volatile auto r = findVisibleUncached();
			(void)r;
		}
	}));
	printBenchmarkResult(benchmarkOutput.file, "musicbox_find_visible_cached", iterations, benchmarkMs([&]() {
		for (int i = 0; i < iterations; ++i) {
			volatile auto r = findVisibleCached();
			(void)r;
		}
	}));

	// --- per-element StringTree has()/stoi()/[] lookups (drawSpecialScrollable body) ---
	printBenchmarkResult(benchmarkOutput.file, "musicbox_per_element_lookups_uncached", iterations, benchmarkMs([&]() {
		for (int i = 0; i < iterations; ++i) {
			for (int j = visibleFirst; j < visibleLast; ++j) {
				auto &b = elements[j].branches;
				volatile bool hasLog = b.count("log");
				volatile int bg = b.count("bg") ? std::stoi(b["bg"]) : -1;
				volatile bool hasText = b.count("text");
				volatile int ml = b.count("textmarginwidth") ? std::stoi(b["textmarginwidth"]) : (b.count("textmarginleft") ? std::stoi(b["textmarginleft"]) : 0);
				volatile int mr = b.count("textmarginwidth") ? std::stoi(b["textmarginwidth"]) : (b.count("textmarginright") ? std::stoi(b["textmarginright"]) : 0);
				volatile int mt = b.count("textmargintop") ? std::stoi(b["textmargintop"]) : 0;
				volatile const char *txt = b.count("text") ? b["text"].c_str() : "";
				(void)hasLog; (void)bg; (void)hasText; (void)ml; (void)mr; (void)mt; (void)txt;
			}
		}
	}));
	printBenchmarkResult(benchmarkOutput.file, "musicbox_per_element_lookups_cached", iterations, benchmarkMs([&]() {
		for (int i = 0; i < iterations; ++i) {
			for (int j = visibleFirst; j < visibleLast; ++j) {
				auto &c = cached[j];
				volatile int bg = c.bgIndex;
				volatile int ml = c.textMarginLeft;
				volatile int mr = c.textMarginRight;
				volatile int mt = c.textMarginTop;
				volatile const char *txt = c.text->c_str();
				(void)bg; (void)ml; (void)mr; (void)mt; (void)txt;
			}
		}
	}));

	// --- GPU blits for element background plates (batched, one flush) ---
	printBenchmarkResult(benchmarkOutput.file, "musicbox_bg_blit_batched", iterations, benchmarkMs([&]() {
		for (int i = 0; i < iterations; ++i) {
			for (int j = visibleFirst; j < visibleLast; ++j)
				GPU_Blit(bgImage, nullptr, targetSurface, 16.0f + cached[j].x, 16.0f + cached[j].y);
			GPU_FlushBlitBuffer();
		}
	}));
	finishBenchmarkWork(target);

	// --- GPU blits for rendered text (proxy for the cached text-draw path) ---
	printBenchmarkResult(benchmarkOutput.file, "musicbox_text_draw_blit", iterations, benchmarkMs([&]() {
		for (int i = 0; i < iterations; ++i) {
			for (int j = visibleFirst; j < visibleLast; ++j)
				GPU_Blit(textImage, nullptr, targetSurface, 16.0f + cached[j].x + 55.0f, 16.0f + cached[j].y + 13.0f);
			GPU_FlushBlitBuffer();
		}
	}));
	finishBenchmarkWork(target);

	// --- full frame: current code path (uncached lookups + find) ---
	const double fullUncachedMs = benchmarkMs([&]() {
		for (int i = 0; i < iterations; ++i) {
			auto [f, l] = findVisibleUncached();
			for (int j = f; j < l; ++j) {
				auto &b = elements[j].branches;
				int bg = b.count("bg") ? std::stoi(b["bg"]) : -1;
				int ml = b.count("textmarginwidth") ? std::stoi(b["textmarginwidth"]) : 0;
				int mt = b.count("textmargintop") ? std::stoi(b["textmargintop"]) : 0;
				int y  = std::stoi(b["y"]);
				int x  = b.count("x") ? std::stoi(b["x"]) : 0;
				(void)bg;
				GPU_Blit(bgImage, nullptr, targetSurface, 16.0f + x, 16.0f + y);
				GPU_Blit(textImage, nullptr, targetSurface, 16.0f + x + ml, 16.0f + y + mt);
			}
			GPU_FlushBlitBuffer();
		}
	});
	printBenchmarkResult(benchmarkOutput.file, "musicbox_full_frame_uncached", iterations, fullUncachedMs);
	finishBenchmarkWork(target);

	// --- full frame: optimized code path (cached geometry + precomputed yEnd) ---
	const double fullCachedMs = benchmarkMs([&]() {
		for (int i = 0; i < iterations; ++i) {
			auto [f, l] = findVisibleCached();
			for (int j = f; j < l; ++j) {
				auto &c = cached[j];
				GPU_Blit(bgImage, nullptr, targetSurface, 16.0f + c.x, 16.0f + c.y);
				GPU_Blit(textImage, nullptr, targetSurface, 16.0f + c.x + c.textMarginLeft, 16.0f + c.y + c.textMarginTop);
			}
			GPU_FlushBlitBuffer();
		}
	});
	printBenchmarkResult(benchmarkOutput.file, "musicbox_full_frame_cached", iterations, fullCachedMs);
	finishBenchmarkWork(target);

	// --- full frame: optimized code path (cached geometry + two-pass draw) ---
	// Two-pass draw: all background plates first, then all text draws. The
	// native blit batch can now keep compatible source-texture switches inside
	// one command buffer, but two-pass ordering still reduces draw groups and
	// sampler rebinds from roughly two per element to two per frame.
	const double fullReorderedMs = benchmarkMs([&]() {
		for (int i = 0; i < iterations; ++i) {
			auto [f, l] = findVisibleCached();
			for (int j = f; j < l; ++j) {
				auto &c = cached[j];
				GPU_Blit(bgImage, nullptr, targetSurface, 16.0f + c.x, 16.0f + c.y);
			}
			for (int j = f; j < l; ++j) {
				auto &c = cached[j];
				GPU_Blit(textImage, nullptr, targetSurface, 16.0f + c.x + c.textMarginLeft, 16.0f + c.y + c.textMarginTop);
			}
			GPU_FlushBlitBuffer();
		}
	});
	printBenchmarkResult(benchmarkOutput.file, "musicbox_full_frame_reordered", iterations, fullReorderedMs);
	finishBenchmarkWork(target);

	const double fullUncachedUs = fullUncachedMs * 1000.0 / iterations;
	const double fullCachedUs   = fullCachedMs * 1000.0 / iterations;
	const double fullReorderedUs = fullReorderedMs * 1000.0 / iterations;
	const double uncachedFps    = fullUncachedUs > 0.0 ? 1000000.0 / fullUncachedUs : 0.0;
	const double cachedFps      = fullCachedUs > 0.0 ? 1000000.0 / fullCachedUs : 0.0;
	const double reorderedFps   = fullReorderedUs > 0.0 ? 1000000.0 / fullReorderedUs : 0.0;
	const double speedup        = fullUncachedUs > 0.0 ? fullUncachedUs / std::max(0.001, fullReorderedUs) : 0.0;

	printBenchmarkLine(benchmarkOutput.file, "\n");
	printBenchmarkLine(benchmarkOutput.file, "==== Music Box bottleneck analysis ====\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "visible elements/frame: %d (2 columns x ~%.1f rows in 1080px)\n",
	                   visibleCount, visibleCount / 2.0);
	printBenchmarkLine(benchmarkOutput.file,
	                   "synthetic full-frame (find + lookups + bg blit + text blit + flush):\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "  current single-pass (uncached): %.3f us/frame  ->  ~%.1f FPS\n",
	                   fullUncachedUs, uncachedFps);
	printBenchmarkLine(benchmarkOutput.file,
	                   "  cached geometry only:           %.3f us/frame  ->  ~%.1f FPS\n",
	                   fullCachedUs, cachedFps);
	printBenchmarkLine(benchmarkOutput.file,
	                   "  cached geometry + two-pass:     %.3f us/frame  ->  ~%.1f FPS  (%.2fx faster)\n",
	                   fullReorderedUs, reorderedFps, speedup);
	printBenchmarkLine(benchmarkOutput.file, "\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "hotspots identified in Engine/Core/Animation.cpp drawSpecialScrollable():\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "  1. NATIVE BLIT SOURCE GROUPING (dominant measurable cost). The current\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "     loop draws each element's background plate and then immediately draws\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "     its text. The background and text come from different source textures,\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "     so a single-pass frame creates ~2 draw groups per element. The renderer\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "     keeps compatible source switches in one command buffer, but each group\n");
	printBenchmarkLine(benchmarkOutput.file, "     still needs a sampler bind and indexed draw.\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "     Fix: split the loop into two passes - draw every background plate first,\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "     then draw every element's text. This reduces source groups from ~48 to\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "     ~2 per frame while preserving one command-buffer flush per frame.\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "     Scrollable elements never overlap, so text still composites on top of\n");
	printBenchmarkLine(benchmarkOutput.file, "     its own background identically.\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "  2. getScrollableElementsVisibleAt() runs std::lower_bound with a comparator\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "     that does unordered_map hash lookups + std::stoi on every comparison.\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "     Fix: precompute a sorted y-end int array and lower_bound over it.\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "  3. For every visible element, every frame, the body re-queries the\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "     StringTree branches with has()/operator[] and re-parses x/y/width/\n");
	printBenchmarkLine(benchmarkOutput.file, "     height/textmargins/bg with std::stoi.\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "     Fix: cache decoded geometry + bg sprite index per element; rebuild only\n");
	printBenchmarkLine(benchmarkOutput.file, "     when the layout generation changes.\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "  4. PER-FRAME TEXT RELAYOUT (not measurable without the font stack). Each\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "     visible element calls DialogueController::renderToTarget() every frame,\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "     which re-runs decodeUTF8String + layoutSegment (markup parse + wrap) +\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "     layoutLines + per-glyph blits. In the Music Box the element text and\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "     style never change between frames while idle, so the layout work is\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "     redundant. The two-pass fix above reduces draw grouping overhead but each\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "     element still re-lays-out its text every frame.\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "     Fix: cache the laid-out TextRenderingState per element (keyed by text +\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "     style + multiply color + gradient + wrap width) and replay render() with\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "     a per-frame offset, re-laying out only on a key change.\n");
	printBenchmarkLine(benchmarkOutput.file, "\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "note: the synthetic text_draw_blit case measures one batched blit per element.\n");
	printBenchmarkLine(benchmarkOutput.file,
	                   "      The real per-frame text cost adds layoutSegment + glyph blits on top;\n");
	printBenchmarkLine(benchmarkOutput.file, "      hotspot 4 removes the relayout portion of that cost.\n");
	if (reorderedFps >= 144.0)
		printBenchmarkLine(benchmarkOutput.file,
		                   "result: optimized scrollable draw path is within the 144Hz frame budget.\n");
	else
		printBenchmarkLine(benchmarkOutput.file,
		                   "result: optimized draw path=%.1f FPS; inspect hotspots above for the remainder.\n",
		                   reorderedFps);

	GPU_FreeImage(bgImage);
	GPU_FreeImage(textImage);
	GPU_FreeImage(target);
	SDL_FreeSurface(bgSurface);
	SDL_FreeSurface(textSurface);
	GPU_Quit();
	SDL_Quit();
	return 0;
}

int SDLCALL GPU_RunSDL3Benchmark(int iterations, int width, int height, const char *outputPath) {
	iterations = std::max(1, iterations);
	width      = std::max(320, width);
	height     = std::max(240, height);

	BenchmarkOutputFile benchmarkOutput(outputPath);
	if (outputPath && outputPath[0] && !benchmarkOutput.file) {
		std::fprintf(stderr, "Benchmark output open failed: %s\n", outputPath);
		return 1;
	}

	if (!onsSDLInit(SDL_INIT_VIDEO)) {
		std::fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
		return 1;
	}

	GPU_SetDebugLevel(GPU_DEBUG_LEVEL_0);
	GPU_SetPreInitFlags(GPU_INIT_DISABLE_VSYNC);
	GPU_Target *screen = GPU_InitRendererByID(GPU_MakeRendererID("SDL3_GPU", GPU_RENDERER_SDL3_GPU, 3, 0),
	                                          static_cast<Uint16>(width), static_cast<Uint16>(height),
	                                          SDL_WINDOW_HIDDEN);
	if (!screen) {
		std::fprintf(stderr, "SDL3_GPU init failed: %s\n", SDL_GetError());
		SDL_Quit();
		return 1;
	}

	SDL_Surface *fullSurface  = createBenchmarkSurface(width, height);
	SDL_Surface *smallSurface = createBenchmarkSurface(256, 256);
	if (!fullSurface || !smallSurface) {
		std::fprintf(stderr, "Benchmark surface creation failed: %s\n", SDL_GetError());
		if (fullSurface)
			SDL_FreeSurface(fullSurface);
		if (smallSurface)
			SDL_FreeSurface(smallSurface);
		GPU_Quit();
		SDL_Quit();
		return 1;
	}

	GPU_Image *uploadTarget   = GPU_CreateImage(static_cast<Uint16>(width), static_cast<Uint16>(height), GPU_FORMAT_RGBA);
	GPU_Image *source         = GPU_CopyImageFromSurface(smallSurface);
	GPU_Image *target         = GPU_CreateImage(static_cast<Uint16>(width), static_cast<Uint16>(height), GPU_FORMAT_RGBA);
	GPU_Target *targetSurface = GPU_GetTarget(target);
	if (!uploadTarget || !source || !target || !targetSurface) {
		std::fprintf(stderr, "Benchmark image creation failed: %s\n", SDL_GetError());
		SDL_FreeSurface(fullSurface);
		SDL_FreeSurface(smallSurface);
		if (uploadTarget)
			GPU_FreeImage(uploadTarget);
		if (source)
			GPU_FreeImage(source);
		if (target)
			GPU_FreeImage(target);
		GPU_Quit();
		SDL_Quit();
		return 1;
	}

	std::vector<float> triangleValues;
	std::vector<Uint16> triangleIndices;
	const int quadsX = 32;
	const int quadsY = 32;
	triangleValues.reserve(static_cast<size_t>(quadsX * quadsY * 4 * 4));
	triangleIndices.reserve(static_cast<size_t>(quadsX * quadsY * 6));
	for (int y = 0; y < quadsY; ++y) {
		for (int x = 0; x < quadsX; ++x) {
			const float dstX  = 8.0f + x * 8.0f;
			const float dstY  = 8.0f + y * 8.0f;
			const float dstW  = 7.0f;
			const float dstH  = 7.0f;
			const Uint16 base = static_cast<Uint16>(triangleValues.size() / 4);
			const float vertices[16]{
			    dstX, dstY, 0.0f, 0.0f,
			    dstX + dstW, dstY, 1.0f, 0.0f,
			    dstX, dstY + dstH, 0.0f, 1.0f,
			    dstX + dstW, dstY + dstH, 1.0f, 1.0f};
			triangleValues.insert(triangleValues.end(), vertices, vertices + 16);
			const Uint16 indices[6]{base, static_cast<Uint16>(base + 1), static_cast<Uint16>(base + 2),
			                        static_cast<Uint16>(base + 2), static_cast<Uint16>(base + 1), static_cast<Uint16>(base + 3)};
			triangleIndices.insert(triangleIndices.end(), indices, indices + 6);
		}
	}

	for (int i = 0; i < 8; ++i) {
		GPU_UpdateImage(uploadTarget, nullptr, fullSurface, nullptr);
		GPU_ClearRGBA(targetSurface, 0, 0, 0, 0);
		GPU_Blit(source, nullptr, targetSurface, 128.0f, 128.0f);
		GPU_TriangleBatch(source, targetSurface, static_cast<unsigned short>(triangleValues.size() / 4),
		                  triangleValues.data(), static_cast<unsigned int>(triangleIndices.size()),
		                  triangleIndices.data(), GPU_BATCH_XY_ST);
	}
	finishBenchmarkWork(target);

	printBenchmarkLine(benchmarkOutput.file, "case,iterations,total_ms,avg_us\n");
	printBenchmarkResult(benchmarkOutput.file, "texture_upload_full_submit", iterations, benchmarkMs([&]() {
		                     for (int i = 0; i < iterations; ++i)
			                     GPU_UpdateImage(uploadTarget, nullptr, fullSurface, nullptr);
	                     }));
	finishBenchmarkWork(uploadTarget);

	printBenchmarkResult(benchmarkOutput.file, "clear_full_submit", iterations, benchmarkMs([&]() {
		                     for (int i = 0; i < iterations; ++i)
			                     GPU_ClearRGBA(targetSurface, static_cast<Uint8>(i), static_cast<Uint8>(i * 3), static_cast<Uint8>(i * 7), 255);
	                     }));
	finishBenchmarkWork(target);

	printBenchmarkResult(benchmarkOutput.file, "clear_full_copy_surface_completed", iterations, benchmarkMs([&]() {
		                     for (int i = 0; i < iterations; ++i) {
			                     GPU_ClearRGBA(targetSurface, static_cast<Uint8>(i), static_cast<Uint8>(i * 3), static_cast<Uint8>(i * 7), 255);
			                     finishBenchmarkWork(target);
		                     }
	                     }));

	const GPU_Rect rect{17.0f, 19.0f, 256.0f, 256.0f};
	const SDL_Color rectColor{64, 128, 192, 255};
	printBenchmarkResult(benchmarkOutput.file, "clear_rect_256_submit", iterations, benchmarkMs([&]() {
		                     for (int i = 0; i < iterations; ++i)
			                     GPU_RectangleFilled2(targetSurface, rect, rectColor);
	                     }));
	finishBenchmarkWork(target);

	printBenchmarkResult(benchmarkOutput.file, "blit_256_immediate_flush", iterations, benchmarkMs([&]() {
		                     for (int i = 0; i < iterations; ++i) {
			                     const float x = 128.0f + static_cast<float>((i * 37) % std::max(1, width - 256));
			                     const float y = 128.0f + static_cast<float>((i * 53) % std::max(1, height - 256));
			                     GPU_Blit(source, nullptr, targetSurface, x, y);
			                     GPU_FlushBlitBuffer();
		                     }
	                     }));
	finishBenchmarkWork(target);

	printBenchmarkResult(benchmarkOutput.file, "blit_256_batched_submit", iterations, benchmarkMs([&]() {
		                     for (int i = 0; i < iterations; ++i) {
			                     const float x = 128.0f + static_cast<float>((i * 37) % std::max(1, width - 256));
			                     const float y = 128.0f + static_cast<float>((i * 53) % std::max(1, height - 256));
			                     GPU_Blit(source, nullptr, targetSurface, x, y);
		                     }
		                     GPU_FlushBlitBuffer();
	                     }));
	finishBenchmarkWork(target);

	printBenchmarkResult(benchmarkOutput.file, "triangle_batch_1024_quads_submit", iterations, benchmarkMs([&]() {
		                     for (int i = 0; i < iterations; ++i)
			                     GPU_TriangleBatch(source, targetSurface, static_cast<unsigned short>(triangleValues.size() / 4),
			                                       triangleValues.data(), static_cast<unsigned int>(triangleIndices.size()),
			                                       triangleIndices.data(), GPU_BATCH_XY_ST);
	                     }));
	finishBenchmarkWork(target);

	printBenchmarkResult(benchmarkOutput.file, "readback_full_completed", iterations, benchmarkMs([&]() {
		                     for (int i = 0; i < iterations; ++i)
			                     finishBenchmarkWork(target);
	                     }));

	GPU_FreeImage(uploadTarget);
	GPU_FreeImage(source);
	GPU_FreeImage(target);
	SDL_FreeSurface(fullSurface);
	SDL_FreeSurface(smallSurface);
	GPU_Quit();
	SDL_Quit();
	return 0;
}

RenderDriverId GPUController::makeRendererIdSDL3GPU() {
	return GPU_MakeRendererID("SDL3_GPU", GPU_RENDERER_SDL3_GPU, 3, 0);
}

void GPUController::initRendererFlagsSDL3GPU() {
	if (render_to_self < 0)
		render_to_self = 0;
}

int GPUController::getImageFormatSDL3GPU(RenderImage *) {
	return current_renderer ? current_renderer->formatRGBA : GL_RGBA;
}

void GPUController::printBlitBufferStateSDL3GPU() {
	sendToLog(LogLevel::Info, "SDL3_GPU transition backend does not use the SDL2_gpu blit buffer.\n");
}

void GPUController::syncRendererStateSDL3GPU() {
	// SDL3_GPU command buffers are submitted in order, and explicit readbacks
	// wait on their own fences. Avoid stalling every routine texture upload.
}

int GPUController::getMaxTextureSizeSDL3GPU() {
	return 8192;
}

#endif
