/**
 *  GPU.hpp
 *  ONScripter-RU
 *
 *  Contains higher level GPU abstraction.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#pragma once

#include "External/Compatibility.hpp"
#include "Engine/Graphics/Common.hpp"
#include "Engine/Graphics/RendererBackend.hpp"
#include "Engine/Components/Base.hpp"
#include "Engine/Entities/Breakup.hpp"
#include "Support/Cache.hpp"

#include "Support/SDLCompat.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <stack>
#include <vector>
#include <iostream>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Leave these constants as they are for now
#ifndef GL_RGBA
#define GL_RGBA 0x1908
#endif

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

void dbgSaveImg(void *ptr);
void dbgSaveTgt(void *ptr);
void dbgSaveImgR(RenderImage *ptr);
void dbgSaveTgtR(RenderTarget *ptr);

struct GPUImageDiff {
	int w{0}, h{0};
	RenderFormat format{GPU_FORMAT_RGBA};
	bool operator==(const GPUImageDiff &two) const;
};

namespace std {
template <>
struct hash<SDL_Point> {
	size_t operator()(const SDL_Point &x) const {
		return static_cast<size_t>(x.x) + (static_cast<size_t>(x.y) << 16);
	}
};
template <>
struct equal_to<SDL_Point> {
	bool operator()(const SDL_Point &one, const SDL_Point &two) const {
		return one.x == two.x && one.y == two.y;
	}
};
template <>
struct hash<GPUImageDiff> {
	size_t operator()(const GPUImageDiff &x) const {
		return static_cast<size_t>(x.w) + (static_cast<size_t>(x.h) << 16) + static_cast<size_t>(x.format);
	}
};
template <>
struct equal_to<GPUImageDiff> {
	bool operator()(const GPUImageDiff &one, const GPUImageDiff &two) const {
		return one == two;
	}
};
} // namespace std

class CombinedImagePool {
public:
	RenderImage *get(int w, int h, int channels, bool store);
	CombinedImagePool(int size)
	    : existent(size) {}
	LRUCachedSet<Wrapped_GPU_Image, GPUImageDiff> existent;
	void init() {}
	void clear() {
		auto it = requested.begin();
		while (it != requested.end()) {
			GPU_FreeImage(*it);
			it = requested.erase(it);
		}

		existent.clear();
		SDL_AtomicLock(&access);
		toDo.clear();
		hasPending.store(false, std::memory_order_release);
		SDL_AtomicUnlock(&access);
	}
	void push(RenderRect &&rect) {
		SDL_AtomicLock(&access);
		toDo.push_back(rect);
		hasPending.store(true, std::memory_order_release);
		SDL_AtomicUnlock(&access);
	}
	bool generate();

private:
	SDL_SpinLock access{0};
	std::atomic_bool hasPending{false};
	std::vector<RenderRect> toDo;
	std::vector<RenderImage *> requested;
};

const std::array<RenderBlendMode, static_cast<size_t>(BlendModeId::TOTAL)> BLEND_MODES{{//{GPU_FUNC_SRC_ALPHA, GPU_FUNC_ONE_MINUS_SRC_ALPHA, GPU_FUNC_SRC_ALPHA, GPU_FUNC_DST_ALPHA, GPU_EQ_ADD, GPU_EQ_ADD},
                                                                                      {GPU_FUNC_ONE, GPU_FUNC_ONE_MINUS_SRC_ALPHA, GPU_FUNC_ONE, GPU_FUNC_ONE_MINUS_SRC_ALPHA, GPU_EQ_ADD, GPU_EQ_ADD},
                                                                                      //Take care of alpha values, we need them for rain
                                                                                      {GPU_FUNC_SRC_ALPHA, GPU_FUNC_ONE, GPU_FUNC_SRC_ALPHA, GPU_FUNC_DST_ALPHA, GPU_EQ_ADD, GPU_EQ_ADD},
                                                                                      {GPU_FUNC_ONE, GPU_FUNC_ONE, GPU_FUNC_ONE, GPU_FUNC_ONE, GPU_EQ_SUBTRACT, GPU_EQ_SUBTRACT},
                                                                                      {GPU_FUNC_DST_COLOR, GPU_FUNC_ZERO, GPU_FUNC_SRC_ALPHA, GPU_FUNC_ONE_MINUS_SRC_ALPHA, GPU_EQ_ADD, GPU_EQ_ADD},
                                                                                      {GPU_FUNC_ZERO, GPU_FUNC_SRC_ALPHA, GPU_FUNC_ZERO, GPU_FUNC_SRC_ALPHA, GPU_EQ_ADD, GPU_EQ_ADD}}};

struct PooledGPUImage;
class TempGPUImagePool {
	std::unordered_map<RenderImage *, bool> pool; // boolean = is this RenderImage* "checked-out"?
public:
	SDL_Point size;
	RenderImage *getImage();         // get a fresh temporary image
	void giveImage(RenderImage *im); // return a temporary image to the pool for reuse
	void addImages(int n);         // pre-create some blank temporary images to avoid delays later
	size_t clearUnused(bool require_empty = false); // returns the bytes handed back to the GPU

	struct Census {
		size_t images{0};
		size_t checkedOut{0};
		size_t bytes{0};
	};
	Census census() const {
		Census c;
		for (const auto &entry : pool) {
			++c.images;
			if (entry.second)
				++c.checkedOut;
			c.bytes += static_cast<size_t>(entry.first->w) * entry.first->h * 4;
		}
		return c;
	}
};

struct PooledGPUImage {
	RenderImage *image{nullptr};
	TempGPUImagePool *pool{nullptr};
	PooledGPUImage() = default;
	PooledGPUImage(TempGPUImagePool *_pool)
	    : pool(_pool) {
		image = pool->getImage();
	}
	~PooledGPUImage() {
		if (image) {
			pool->giveImage(image);
		}
	}
	// Can't copy pooled gpu image containers
	PooledGPUImage(const PooledGPUImage &) = delete; // no copy
	PooledGPUImage &operator=(const PooledGPUImage &) = delete; // no assign
	// But you can move them
	PooledGPUImage(PooledGPUImage &&src) noexcept
	    : image(src.image), pool(src.pool) {
		src.image = nullptr;
		src.pool  = nullptr;
	}
	PooledGPUImage &operator=(PooledGPUImage &&src) noexcept {
		if (image && pool) {
			pool->giveImage(image);
		}
		image     = src.image;
		pool      = src.pool;
		src.image = nullptr;
		src.pool  = nullptr;
		return *this;
	}
};

class GPUTransformableCanvasImage {
	friend class GPUController;
	std::unordered_map<SDL_Point, PooledGPUImage> pooledDownscaledImages;

public:
	RenderImage *image{nullptr};
	void setImage(RenderImage *_canvas);
	void clearImage();
	GPUTransformableCanvasImage() {}
	GPUTransformableCanvasImage(RenderImage *canvas)
	    : image(canvas) {}
};

class GPUImageChunkLoader {
	friend class GPUController;
	int x{0}, y{0}; // next-to-load chunk position in units of chunk size dimensions
public:
	SDL_Surface *src{nullptr};
	RenderRect *src_area{nullptr};
	RenderImage *dst{nullptr};
	constexpr static uint32_t MinimumChunkDim{128};
	uint32_t chunkWidth{0};
	uint32_t chunkHeight{0};
	bool isLoaded{false};
	bool isActive{false};
	void loadChunk(bool finish);
};

class GPUBigImage {
	// Contains GPU_Images and their positions in an abstract image from top left to bottom right
	std::vector<Wrapped_GPU_Image> images;
	void create(SDL_Surface *surface = nullptr);

public:
	uint16_t w{0}, h{0};
	int channels{0};
	// Returns a vector of GPU_Images with their dst coordinates
	std::vector<std::pair<RenderImage *, RenderRect>> getImagesForArea(RenderRect &area);
	bool has() {
		return w > 0 && h > 0;
	}
	GPUBigImage(SDL_Surface *surface);
	GPUBigImage(uint16_t w_, uint16_t h_, int channels_);
	GPUBigImage() = default;
};

// Generalized struct for easier batch blitting of triangles.
// This is NOT breakup specific -- don't put any breakup methods or data in here.
// Glass smash may also use it later.
struct TriangleBlitter {
	std::vector<float> vertices;
	std::vector<uint16_t> indices;
	RenderImage *image{nullptr};
	RenderTarget *target{nullptr};
	int elementsPerVertex{4};
	RenderBatchFlag dataStructure{GPU_BATCH_XY_ST};
	static constexpr int maxVertices{60000};
	static constexpr int maxIndices{200000};
	uint16_t verticesInVertexBuffer{0};
	int verticesInIndexBuffer{0};
	bool fewerTriangles{false};

private:
	FORCE_INLINE void setTexturedVertex(float *vertices, uint16_t *indices, float s, float t, float x, float y, float z = 0) {
		float *ptr = vertices + verticesInVertexBuffer * elementsPerVertex;
		ptr[0]     = x;
		ptr[1]     = y;
		if (dataStructure == GPU_BATCH_XYZ_ST) {
			ptr[2] = z;
			ptr[3] = s;
			ptr[4] = t;
		} else {
			ptr[2] = s;
			ptr[3] = t;
		}
		if (indices) {
			indices[verticesInIndexBuffer++] = verticesInVertexBuffer;
		}
		verticesInVertexBuffer++;
	}

	FORCE_INLINE void setIndexedVertex(uint16_t *indices, uint16_t index) {
		indices[verticesInIndexBuffer] = index;
		verticesInIndexBuffer++;
	}

	// In these functions, s/t are the source texture coordinates and are always normalized to the size of the texture.
	void addEllipse(
	    float s, float t, float radius_s,
	    float radius_t, float x, float y, float radius_x,
	    float radius_y);

	void addTriangle(
	    float s1, float t1, float s2, float t2, float s3, float t3,
	    float x1, float y1, float z1, float x2, float y2, float z2,
	    float x3, float y3, float z3);

	void rotateCoordinates(
	    float coords[3][3],
	    float centerX, float centerY, float centerZ,
	    float yaw, float pitch, float roll);

public:
	// In these functions, xSrc/ySrc are normal pixel coordinates and are not normalized to the size of the texture.
	FORCE_INLINE void copyTriangle(
	    float xSrc1, float ySrc1, float xSrc2, float ySrc2, float xSrc3, float ySrc3,
	    float xDst, float yDst, float zDst = 0, float yaw = 0, float pitch = 0, float roll = 0) {
		float coords[3][3];

		coords[0][0] = xSrc1 + xDst;
		coords[0][1] = ySrc1 + yDst;
		coords[0][2] = zDst;

		coords[1][0] = xSrc2 + xDst;
		coords[1][1] = ySrc2 + yDst;
		coords[1][2] = zDst;

		coords[2][0] = xSrc3 + xDst;
		coords[2][1] = ySrc3 + yDst;
		coords[2][2] = zDst;

		if (yaw != 0 || pitch != 0 || roll != 0) {
			float centerX = (coords[0][0] + coords[1][0] + coords[2][0]) / 3.0;
			float centerY = (coords[0][1] + coords[1][1] + coords[2][1]) / 3.0;
			rotateCoordinates(coords, centerX, centerY, zDst, yaw, pitch, roll);
		}

		addTriangle(
		    xSrc1 / image->w, ySrc1 / image->h,
		    xSrc2 / image->w, ySrc2 / image->h,
		    xSrc3 / image->w, ySrc3 / image->h,
		    coords[0][0], coords[0][1], coords[0][2],
		    coords[1][0], coords[1][1], coords[1][2],
		    coords[2][0], coords[2][1], coords[2][2]);
	}

	FORCE_INLINE void copyCircle(
	    float xSrc, float ySrc, float radius, float xDst, float yDst, float resizeFactor = 1.0) {
		addEllipse(
		    xSrc / image->w, ySrc / image->h,
		    radius / image->w, radius / image->h,
		    xDst, yDst, radius * resizeFactor, radius * resizeFactor);
	}

	FORCE_INLINE void updateTargets(RenderImage *src, RenderTarget *dst) {
		//Note, that we do not check vector sizes here
		image  = src;
		target = dst;
	}

	FORCE_INLINE void useFewerTriangles(bool arg = true) {
		fewerTriangles = arg;
	}

	void finish();
};

class ONScripter;
class GPUController : public BaseController {
private:
	TempGPUImagePool canvasImagePool, scriptImagePool;
	std::unordered_map<SDL_Point, TempGPUImagePool> typedImagePools;
	CombinedImagePool globalImagePool;

	/* {program: {uniform name: location}} */
	std::unordered_map<uint32_t, std::unordered_map<std::string, int>> uniformLocations;
	const char *lastProgramAlias{nullptr};
	uint32_t lastProgramId{0};

public:
	int ownInit() override;
	int ownDeinit() override;

	std::unordered_map<std::string, uint32_t> shaders;
	std::unordered_map<std::string, uint32_t> programs;
	uint32_t currentProgram{0};
	std::stack<BlendModeId> blend_mode;
	bool texture_reuse{true};
	// Provides a way (if false) to reuse textures on some Intel machines
	bool use_glclear{true};
	// Provides a way to use texture atlas with VMware
	bool simulate_reads{false};
	// Provides a way to use new breakup / glass smash with added flushes on some Intel machines
	bool triangle_blit_flush{false};
	// Implements a speedhack by rendering to self at alpha multiplication and similar.
	// Violates GL/GLES standard but appears to work everywhere.
	// The only exception is ANGLE 33+: https://bugs.chromium.org/p/angleproject/issues/detail?id=496
	int render_to_self{-1};
	// Upper texture dimension limit (in pixels)
	int max_texture{0};
	// Upper chunk size limit (in bytes)
	int max_chunk{896 * 896 * 4};

	RenderImage *loadGPUImageByChunks(SDL_Surface *s, RenderRect *r = nullptr);

	void setVirtualResolution(unsigned int width, unsigned int height);
	void setBlendMode(RenderImage *image);
	void pushBlendMode(BlendModeId mode);
	void popBlendMode();

	void createShadersFromResources();
	bool isStandaloneShader(const char *text);
	std::vector<uint32_t> findAllLinkTargets(const char *text, size_t len);
	void createShader(const char *filename);
	void createProgramFromShaders(const char *programAlias, const char *frag, const char *vert);
	void createProgramFromShaders(const char *programAlias, std::vector<uint32_t> &targets);
	void linkProgram(const char *programAlias, uint32_t prog);

	//We are in need of a proper image loading that disables SDL_gpu blending...
	RenderImage *createImage(uint16_t w, uint16_t h, uint8_t channels, bool store = false) {
		RenderImage *image = globalImagePool.get(w, h, channels, store);
		//GPU_SetBlendMode(image, GPU_BLEND_OVERRIDE);
		if (image->snap_mode != GPU_SNAP_NONE)
			GPU_SetSnapMode(image, GPU_SNAP_NONE);
		return image;
	}

	RenderImage *copyImage(RenderImage *image) {
		GPU_SetSnapMode(image, GPU_SNAP_DIMENSIONS);
		RenderImage *newImage = GPU_CopyImage(image);
		GPU_SetSnapMode(newImage, GPU_SNAP_NONE);
		GPU_SetSnapMode(image, GPU_SNAP_NONE);
		return newImage;
	}

	RenderImage *copyImageFromTarget(RenderTarget *target) {
		RenderImage *image = GPU_CopyImageFromTarget(target);
		GPU_SetSnapMode(image, GPU_SNAP_NONE);
		return image;
	}

	RenderImage *copyImageFromSurface(SDL_Surface *surface) {
		RenderImage *image = createImage(surface->w, surface->h, onsSurfaceBytesPerPixel(surface) == 4 ? 4 : 3);
		updateImage(image, nullptr, surface, nullptr);
		//GPU_SetBlendMode(image, GPU_BLEND_OVERRIDE);
		if (image->snap_mode != GPU_SNAP_NONE)
			GPU_SetSnapMode(image, GPU_SNAP_NONE);
		return image;
	}

	void clear(RenderTarget *target, uint8_t r = 0, uint8_t g = 0, uint8_t b = 0, uint8_t a = 0) {
		if (use_glclear) {
			GPU_ClearRGBA(target, r, g, b, a);
		} else {
			// Dodges a strange bug on certain hardware
			GPU_SetShapeBlending(false);
			SDL_Color color{r, g, b, a};
			RenderRect full{0, 0, static_cast<float>(target->w), static_cast<float>(target->h)};
			GPU_RectangleFilled2(target, full, color);
		}
	}

	void freeImage(RenderImage *image) {
		if (!texture_reuse || !initialised() || image->refcount > 1 || (image->format != GPU_FORMAT_RGB && image->format != GPU_FORMAT_RGBA)) {
			GPU_FreeImage(image);
		} else {
			GPUImageDiff diff;
			diff.w      = image->w;
			diff.h      = image->h;
			diff.format = image->format;
			globalImagePool.existent.add(diff, std::make_shared<Wrapped_GPU_Image>(image));
		}
	}

	RenderShaderType getShaderTypeByExtension(const char *filename);
	void bindImageToSlot(RenderImage *image, int slot_number);
	void multiplyAlpha(RenderImage *image, RenderRect *dst_clip = nullptr);
	void mergeAlpha(RenderImage *image, RenderRect *imageRect, RenderImage *mask, RenderRect *maskRect, SDL_Surface *src);
	void enter3dMode();
	void exit3dMode();
	void setShaderProgram(const char *programAlias);
	void unsetShaderProgram();
	int32_t getUniformLoc(const char *name);
	void setShaderVar(const char *name, int value);
	void setShaderVar(const char *name, float value);
	void setShaderVar(const char *name, float value1, float value2);
	void setShaderVar(const char *name, const SDL_Color &color);
	void clearWholeTarget(RenderTarget *target, uint8_t r = 0, uint8_t g = 0, uint8_t b = 0, uint8_t a = 0);
	void copyGPUImage(RenderImage *img, RenderRect *src_rect, RenderRect *clip_rect, RenderTarget *target, float x = 0, float y = 0, float ratio_x = 1, float ratio_y = 1, float angle = 0, bool centre_coordinates = false);
	void copyGPUImage(RenderImage *img, RenderRect *src_rect, RenderRect *clip_rect, GPUBigImage *bigImage, float x = 0, float y = 0);
	void updateImage(RenderImage *image, const RenderRect *image_rect, SDL_Surface *surface, const RenderRect *surface_rect, bool finish = true);
	void convertNV12ToRGB(RenderImage *image, RenderImage **imgs, RenderRect &rect, uint8_t *planes[4], int *linesizes, bool masked);
	void convertYUVToRGB(RenderImage *image, RenderImage **imgs, RenderRect &rect, uint8_t *planes[4], int *linesizes, bool masked);
	void simulateRead(RenderImage *img);

	struct GPURendererInfo {
		const char *name{nullptr};
		RenderDriverId (GPUController::*makeRendererId)(){nullptr};
		void (GPUController::*initRendererFlags)(){nullptr};
		int (GPUController::*getImageFormat)(RenderImage *image){nullptr};
		void (GPUController::*printBlitBufferState)(){nullptr};
		void (GPUController::*syncRendererState)(){nullptr};
		int (GPUController::*getMaxTextureSize)(){nullptr};
		bool mobile{false};
		int formatRGBA{GL_RGBA};
		int formatBGRA{GL_BGRA};
	};

	RenderTarget *rendererInit(RenderWindowFlags SDL_flags);
	RenderTarget *rendererInitWithInfo(GPURendererInfo &info, uint16_t w, uint16_t h, RenderWindowFlags SDL_flags);

	RenderDriverId makeRendererIdSDL3GPU();
	void initRendererFlagsSDL3GPU();
	int getImageFormatSDL3GPU(RenderImage *image);
	void printBlitBufferStateSDL3GPU();
	void syncRendererStateSDL3GPU();
	int getMaxTextureSizeSDL3GPU();

#if defined(DROID) || defined(IOS)
	// This is generally too harsh on iOS
	static constexpr size_t GlobalImagePoolSize{10};
#else
	static constexpr size_t GlobalImagePoolSize{20};
#endif

	GPURendererInfo renderers[1]{
	    {"Vulkan",
	     &GPUController::makeRendererIdSDL3GPU,
	     &GPUController::initRendererFlagsSDL3GPU,
	     &GPUController::getImageFormatSDL3GPU,
	     &GPUController::printBlitBufferStateSDL3GPU,
	     &GPUController::syncRendererStateSDL3GPU,
	     &GPUController::getMaxTextureSizeSDL3GPU,
	     false}};

	GPURendererInfo *current_renderer{nullptr};

	PooledGPUImage getBlurredImage(GPUTransformableCanvasImage &im, int blurFactor);
	PooledGPUImage getMaskedImage(GPUTransformableCanvasImage &im, RenderImage *mask);

	void breakUpImage(BreakupID id, RenderImage *src, RenderRect *src_rect, RenderTarget *target, int breakupFactor,
	                  int breakupDirectionFlagset, const char *params, float dstX, float dstY);
	PooledGPUImage getBrokenUpImage(GPUTransformableCanvasImage &im, BreakupID id,
	                                int breakupFactor, int breakupDirectionFlagset,
	                                const char *params);
	void drawUnbrokenBreakupRegions(BreakupID id, float dstX, float dstY);

	void glassSmashImage(RenderImage *src, RenderTarget *dst, int smashFactor);
	PooledGPUImage getGlassSmashedImage(GPUTransformableCanvasImage &im, int smashFactor);

	PooledGPUImage getWarpedImage(GPUTransformableCanvasImage &im, float animationClock, float amplitude, float waveLength, float speed);
	PooledGPUImage getGreyscaleImage(GPUTransformableCanvasImage &im, const SDL_Color &color);
	PooledGPUImage getSepiaImage(GPUTransformableCanvasImage &im);
	PooledGPUImage getNegativeImage(GPUTransformableCanvasImage &im);
	PooledGPUImage getPixelatedImage(GPUTransformableCanvasImage &im, int factor);

	FORCE_INLINE TriangleBlitter createTriangleBlitter(RenderImage *image, RenderTarget *target) {
		TriangleBlitter res;
		res.image  = image;
		res.target = target;
		//res.elementsPerVertex = 4;
		//res.dataStructure = GPU_BATCH_XY_ST;
		res.elementsPerVertex = 5;
		res.dataStructure     = GPU_BATCH_XYZ_ST;
		res.vertices.resize(res.elementsPerVertex * res.maxVertices);
		res.indices.resize(res.maxIndices);
		return res;
	}

	void clearImage(GPUTransformableCanvasImage *im);

	PooledGPUImage getPooledImage(int w = -1, int h = -1);

	void scheduleLoadImage(int width, int height) {
		// Do not load large images, as they are to be loaded via GPUBigImage.
		if (width <= max_texture && height <= max_texture)
			globalImagePool.push({0, 0, static_cast<float>(width), static_cast<float>(height)});
	}

	bool handleScheduledJobs() {
		return globalImagePool.generate();
	}

	void clearImagePools(bool require_empty=false) {
		scriptImagePool.clearUnused(require_empty);
		canvasImagePool.clearUnused(require_empty);
		typedImagePools.clear();
		globalImagePool.clear();
	}

	// Where the pooled GPU memory actually sits, so a number in dumpsys can be
	// attributed rather than guessed at.
	void logPooledImageCensus();

	// The same totals as logPooledImageCensus, returned rather than logged, for
	// the performance counter. Walks every pooled image, so it is sampled on the
	// panel's redraw cadence and not per frame.
	TempGPUImagePool::Census pooledImageCensus() const {
		TempGPUImagePool::Census total = canvasImagePool.census();
		auto add = [&total](const TempGPUImagePool::Census &c) {
			total.images += c.images;
			total.checkedOut += c.checkedOut;
			total.bytes += c.bytes;
		};
		add(scriptImagePool.census());
		for (const auto &entry : typedImagePools)
			add(entry.second.census());
		return total;
	}

	// Hand back every pooled image nobody is holding, and report the bytes.
	//
	// Unlike clearImagePools this keeps the pool objects themselves, because
	// live PooledGPUImage handles store a pointer to the pool they came from
	// and return their image to it on destruction. It is therefore safe to call
	// at any point in the frame, which is what the Android trim path needs.
	size_t releaseUnusedPooledImages() {
		size_t freedBytes = scriptImagePool.clearUnused();
		freedBytes += canvasImagePool.clearUnused();
		for (auto &entry : typedImagePools)
			freedBytes += entry.second.clearUnused();
		globalImagePool.clear();
		return freedBytes;
	}

	RenderImage *getCanvasImage() { return canvasImagePool.getImage(); }
	void giveCanvasImage(RenderImage *im) { canvasImagePool.giveImage(im); }
	RenderImage *getScriptImage() { return scriptImagePool.getImage(); }
	void giveScriptImage(RenderImage *im) { scriptImagePool.giveImage(im); }

	GPUController()
	    : BaseController(this), globalImagePool(GlobalImagePoolSize) {}
};

extern GPUController gpu;
