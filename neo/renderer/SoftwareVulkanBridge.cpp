/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 2026 Justin Marshall

This file is part of the idTech 4 software renderer source code

===========================================================================
*/

#include "precompiled.h"
#pragma hdrstop

#define VK_USE_PLATFORM_WIN32_KHR
#include "../sys/win32/win_local.h"
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "tr_local.h"
#include "SoftwareVulkanBridge.h"
#include "Software2DComposite_spv.h"
#include "SoftwareHybridComposite_spv.h"
#include "SoftwareHybridCompositeVariants_spv.h"
#include "SoftwareOverlayRaster_spv.h"
#include "SoftwareOverlayTileComposite_spv.h"
#include "SoftwareRayQueryShadow_spv.h"

/*
===============================================================================

	Vulkan presentation bridge for the software rasterizer.

	The software renderer still owns rasterization.  This layer composites each
	software-rendered view into a CPU frame buffer and presents that frame through
	a Vulkan swapchain at the normal backend swap point.  The same device is
	created with acceleration-structure / ray-query support when the driver
	exposes it, which gives the shadow path a real Vulkan home without putting GL
	back into the software renderer's presentation loop.

===============================================================================
*/

struct swVkPresentPerfStats_t {
	void Clear() {
		memset( this, 0, sizeof( *this ) );
	}

	bool enabled;
	bool useHybridFrame;
	bool directHybridOverlay;
	int frameWidth;
	int frameHeight;
	int hybridWidth;
	int hybridHeight;
	int overlayTris;
	int sourcePixels;

	double totalMsec;
	double prepMsec;
	double waitMsec;
	double uploadMsec;
	double acquireMsec;
	double recordMsec;
	double submitMsec;
	double presentMsec;
};

enum swVkGpuTimestampQuery_t {
	SW_VK_GPU_TIMESTAMP_TOTAL_BEGIN,
	SW_VK_GPU_TIMESTAMP_HYBRID_BEGIN,
	SW_VK_GPU_TIMESTAMP_HYBRID_END,
	SW_VK_GPU_TIMESTAMP_OVERLAY_BEGIN,
	SW_VK_GPU_TIMESTAMP_OVERLAY_END,
	SW_VK_GPU_TIMESTAMP_COPY_BEGIN,
	SW_VK_GPU_TIMESTAMP_COPY_END,
	SW_VK_GPU_TIMESTAMP_TOTAL_END,
	SW_VK_GPU_TIMESTAMP_COUNT
};

struct swVkGpuTimestampFrame_t {
	void Clear() {
		memset( this, 0, sizeof( *this ) );
	}

	bool useHybridFrame;
	bool directHybridOverlay;
	int frameWidth;
	int frameHeight;
	int hybridWidth;
	int hybridHeight;
	int overlayTris;
	int sourcePixels;
};

static double SWVkTimestampDeltaMsec( uint64_t begin, uint64_t end, uint32_t validBits, float timestampPeriod ) {
	if ( validBits == 0 || timestampPeriod <= 0.0f ) {
		return 0.0;
	}

	uint64_t delta;
	if ( validBits >= 64 ) {
		delta = end - begin;
	} else {
		const uint64_t mask = ( 1ULL << validBits ) - 1ULL;
		delta = ( end - begin ) & mask;
	}
	return static_cast<double>( delta ) * static_cast<double>( timestampPeriod ) * 0.000001;
}

static const char *SWVkPresentModeName( VkPresentModeKHR mode ) {
	switch ( mode ) {
		case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
		case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
		case VK_PRESENT_MODE_FIFO_KHR: return "fifo";
		case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo_relaxed";
		default: return "unknown";
	}
}

static void SWVkPrintGpuTimestampStats( const swVkGpuTimestampFrame_t &frame, const uint64_t timestamps[SW_VK_GPU_TIMESTAMP_COUNT], float timestampPeriod, uint32_t validBits ) {
	if ( r_showSoftwarePerf.GetInteger() == 0 ) {
		return;
	}

	const double totalMsec = SWVkTimestampDeltaMsec( timestamps[SW_VK_GPU_TIMESTAMP_TOTAL_BEGIN], timestamps[SW_VK_GPU_TIMESTAMP_TOTAL_END], validBits, timestampPeriod );
	const double hybridMsec = frame.useHybridFrame ? SWVkTimestampDeltaMsec( timestamps[SW_VK_GPU_TIMESTAMP_HYBRID_BEGIN], timestamps[SW_VK_GPU_TIMESTAMP_HYBRID_END], validBits, timestampPeriod ) : 0.0;
	const double overlayMsec = frame.directHybridOverlay ? SWVkTimestampDeltaMsec( timestamps[SW_VK_GPU_TIMESTAMP_OVERLAY_BEGIN], timestamps[SW_VK_GPU_TIMESTAMP_OVERLAY_END], validBits, timestampPeriod ) : 0.0;
	const double copyMsec = SWVkTimestampDeltaMsec( timestamps[SW_VK_GPU_TIMESTAMP_COPY_BEGIN], timestamps[SW_VK_GPU_TIMESTAMP_COPY_END], validBits, timestampPeriod );

	common->Printf(
		"swperf gpu %s %dx%d hybrid:%dx%d total:%5.2f hybrid:%5.2f overlay:%5.2f copy:%5.2f overlay:%d source:%d%s\n",
		frame.useHybridFrame ? "hybrid" : "blit",
		frame.frameWidth,
		frame.frameHeight,
		frame.hybridWidth,
		frame.hybridHeight,
		totalMsec,
		hybridMsec,
		overlayMsec,
		copyMsec,
		frame.overlayTris,
		frame.sourcePixels,
		frame.directHybridOverlay ? " directOverlay" : "" );
}

static void SWVkPrintPresentPerfStats( const swVkPresentPerfStats_t &perf ) {
	if ( !perf.enabled ) {
		return;
	}

	common->Printf(
		"swperf present %s %dx%d hybrid:%dx%d total:%5.2f prep:%5.2f waitPrev:%5.2f upload:%5.2f acquire:%5.2f record:%5.2f submit:%5.2f present:%5.2f overlay:%d source:%d%s\n",
		perf.useHybridFrame ? "hybrid" : "blit",
		perf.frameWidth,
		perf.frameHeight,
		perf.hybridWidth,
		perf.hybridHeight,
		perf.totalMsec,
		perf.prepMsec,
		perf.waitMsec,
		perf.uploadMsec,
		perf.acquireMsec,
		perf.recordMsec,
		perf.submitMsec,
		perf.presentMsec,
		perf.overlayTris,
		perf.sourcePixels,
		perf.directHybridOverlay ? " directOverlay" : "" );
}

class swVkScopedPerfTimer {
public:
	swVkScopedPerfTimer( swVkPresentPerfStats_t &stats, double &accumulator ) :
		accumulator( accumulator ),
		enabled( stats.enabled ) {
		if ( enabled ) {
			timer.Start();
		}
	}

	~swVkScopedPerfTimer() {
		if ( enabled ) {
			timer.Stop();
			accumulator += timer.Milliseconds();
		}
	}

private:
	double &accumulator;
	bool enabled;
	idTimer timer;
};

class swVkScopedPresentPerfFrame {
public:
	swVkScopedPresentPerfFrame( swVkPresentPerfStats_t &stats ) :
		stats( stats ),
		enabled( stats.enabled ) {
		if ( enabled ) {
			timer.Start();
		}
	}

	~swVkScopedPresentPerfFrame() {
		if ( enabled ) {
			timer.Stop();
			stats.totalMsec += timer.Milliseconds();
			SWVkPrintPresentPerfStats( stats );
		}
	}

private:
	swVkPresentPerfStats_t &stats;
	bool enabled;
	idTimer timer;
};

struct swVkBuffer_t {
	swVkBuffer_t() {
		buffer = VK_NULL_HANDLE;
		memory = VK_NULL_HANDLE;
		size = 0;
	}

	VkBuffer buffer;
	VkDeviceMemory memory;
	VkDeviceSize size;
};

struct swVkShadowVertex_t {
	float xyz[3];
	float pad;
};

struct swVkSurfaceBlas_t {
	swVkSurfaceBlas_t() {
		owner = NULL;
		key = NULL;
		verts = NULL;
		indexes = NULL;
		vertCount = 0;
		indexCount = 0;
		primitiveCount = 0;
		geometryGeneration = 0;
		updatable = false;
		blas = VK_NULL_HANDLE;
		blasAddress = 0;
	}

	srfTriangles_t *owner;
	const srfTriangles_t *key;
	const idDrawVert *verts;
	const glIndex_t *indexes;
	int vertCount;
	int indexCount;
	uint32_t primitiveCount;
	unsigned int geometryGeneration;
	bool updatable;
	swVkBuffer_t vertexBuffer;
	swVkBuffer_t indexBuffer;
	swVkBuffer_t blasBuffer;
	VkAccelerationStructureKHR blas;
	VkDeviceAddress blasAddress;
};

struct swVkRayInstance_t {
	VkDeviceAddress blasAddress;
	float modelMatrix[16];
};

struct swVkEntityTlas_t {
	swVkEntityTlas_t() {
		owner = NULL;
		signature = 0;
		instanceCount = 0;
		frame = 0;
	}

	idRenderEntityLocal *owner;
	idList<swVkRayInstance_t> instances;
	uint64_t signature;
	int instanceCount;
	int frame;
};

struct swVkShadowPushConstants_t {
	float lightOrigin[4];
	uint32_t pixelCount;
	uint32_t sampleCount;
	float bias;
	float radius;
};

struct swVkHybridPushConstants_t {
	uint32_t dims[4];
	int32_t viewport[4];
	int32_t dispatch[4];
	uint32_t debugView;
	uint32_t textureCount;
	uint32_t overlayEnabled;
	uint32_t lightCount;
	uint32_t shadowEnabled;
	uint32_t overlayOnly;
	uint32_t shadowSamples;
	float shadowRadius;
	float viewOrigin[4];
};

struct swVk2DPushConstants_t {
	uint32_t src[4];		// offset, width, height, clear overlay
	uint32_t frame[4];		// width, height, unused, unused
	int32_t viewport[4];	// x, y, presentWidth, presentHeight in Doom's bottom-left frame space
	uint32_t targetFrame;
	uint32_t pad[3];
};

struct swVkOverlayPushConstants_t {
	uint32_t tri[4];		// index, clear, dispatch width, dispatch height
	uint32_t dims[4];		// source width, source height, frame width, frame height
	int32_t viewport[4];	// x, y, presentWidth, presentHeight
	int32_t dispatch[4];	// x, y, width, height
	uint32_t textureCount;
	uint32_t targetFrame;
	uint32_t pad[2];
};

struct swVkOverlayTilePushConstants_t {
	uint32_t frame[4];		// width, height, unused, unused
	int32_t dispatch[4];	// x, y, width, height in Doom's bottom-left frame space
	uint32_t tileGrid[4];	// tile width, tile height, tiles x, tiles y
	uint32_t textureCount;
	uint32_t targetFrame;
	uint32_t pad[2];
};

struct swVkOverlayTile_t {
	uint32_t offset;
	uint32_t count;
	uint32_t pad[2];
};

struct swVk2DJob_t {
	swVk2DJob_t() {
		sourceOffset = 0;
		sourceWidth = 0;
		sourceHeight = 0;
		viewportX = 0;
		viewportY = 0;
		presentWidth = 0;
		presentHeight = 0;
	}

	int sourceOffset;
	int sourceWidth;
	int sourceHeight;
	int viewportX;
	int viewportY;
	int presentWidth;
	int presentHeight;
};

enum swVkHybridCompositeVariant_t {
	SW_VK_HYBRID_VARIANT_GENERIC_NOSHADOW,
	SW_VK_HYBRID_VARIANT_GENERIC_SHADOW,
	SW_VK_HYBRID_VARIANT_NO_LIGHTS,
	SW_VK_HYBRID_VARIANT_AMBIENT_ONLY,
	SW_VK_HYBRID_VARIANT_PROJECTED_NOSHADOW,
	SW_VK_HYBRID_VARIANT_PROJECTED_SHADOW,
	SW_VK_HYBRID_VARIANT_POST_FOG_NOSHADOW,
	SW_VK_HYBRID_VARIANT_POST_FOG_SHADOW,
	SW_VK_HYBRID_VARIANT_POST_BLEND_NOSHADOW,
	SW_VK_HYBRID_VARIANT_POST_BLEND_SHADOW,
	SW_VK_HYBRID_VARIANT_DEBUG_DEPTH,
	SW_VK_HYBRID_VARIANT_DEBUG_NORMAL,
	SW_VK_HYBRID_VARIANT_DEBUG_UV,
	SW_VK_HYBRID_VARIANT_DEBUG_MATERIAL,
	SW_VK_HYBRID_VARIANT_DEBUG_ALBEDO,
	SW_VK_HYBRID_VARIANT_DEBUG_SURFACE,
	SW_VK_HYBRID_VARIANT_COUNT
};

struct swVkHybridCompositeShader_t {
	const unsigned int *code;
	size_t codeSize;
	const char *name;
	bool needsRayQuery;
};

#define SW_VK_HYBRID_SHADER( name, rayQuery ) { name, sizeof( name ), #name, rayQuery }

static const swVkHybridCompositeShader_t swVkHybridCompositeShaders[SW_VK_HYBRID_VARIANT_COUNT] = {
	SW_VK_HYBRID_SHADER( swHybridCompositeGenericNoShadowCompSpv, false ),
	SW_VK_HYBRID_SHADER( swHybridCompositeCompSpv, true ),
	SW_VK_HYBRID_SHADER( swHybridCompositeNoLightsCompSpv, false ),
	SW_VK_HYBRID_SHADER( swHybridCompositeAmbientOnlyCompSpv, false ),
	SW_VK_HYBRID_SHADER( swHybridCompositeProjectedNoShadowCompSpv, false ),
	SW_VK_HYBRID_SHADER( swHybridCompositeProjectedShadowCompSpv, true ),
	SW_VK_HYBRID_SHADER( swHybridCompositePostFogNoShadowCompSpv, false ),
	SW_VK_HYBRID_SHADER( swHybridCompositePostFogShadowCompSpv, true ),
	SW_VK_HYBRID_SHADER( swHybridCompositePostBlendNoShadowCompSpv, false ),
	SW_VK_HYBRID_SHADER( swHybridCompositePostBlendShadowCompSpv, true ),
	SW_VK_HYBRID_SHADER( swHybridCompositeDebugDepthCompSpv, false ),
	SW_VK_HYBRID_SHADER( swHybridCompositeDebugNormalCompSpv, false ),
	SW_VK_HYBRID_SHADER( swHybridCompositeDebugUvCompSpv, false ),
	SW_VK_HYBRID_SHADER( swHybridCompositeDebugMaterialCompSpv, false ),
	SW_VK_HYBRID_SHADER( swHybridCompositeDebugAlbedoCompSpv, false ),
	SW_VK_HYBRID_SHADER( swHybridCompositeDebugSurfaceCompSpv, false )
};

#undef SW_VK_HYBRID_SHADER

static const int SW_VK_MAX_SHADOW_JOBS = 64;
static const int SW_VK_FRAMES_IN_FLIGHT = 2;
static const uint32_t SW_VK_LINEAR_GROUP_SIZE_X = 32;
static const uint32_t SW_VK_LINEAR_GROUP_SIZE_Y = 4;
static const uint32_t SW_VK_OVERLAY_GROUP_SIZE_X = 16;
static const uint32_t SW_VK_OVERLAY_GROUP_SIZE_Y = 8;
static const uint32_t SW_VK_OVERLAY_TILE_WIDTH = 32;
static const uint32_t SW_VK_OVERLAY_TILE_HEIGHT = 16;
static const int SW_VK_OVERLAY_TILE_MAX_REFS = 8 * 1024 * 1024;
static const uint32_t SW_VK_SHADOW_GROUP_SIZE_X = 128;
static const unsigned int SW_VK_HYBRID_LIGHT_TYPE_FOG = 1u;
static const unsigned int SW_VK_HYBRID_LIGHT_TYPE_BLEND = 2u;
static const VkDeviceSize SW_VK_MIN_UPLOAD_BYTES = 8 * 1024 * 1024;
static const VkDeviceSize SW_VK_MIN_2D_SOURCE_BYTES = 4 * 1024 * 1024;
static const VkDeviceSize SW_VK_MIN_OVERLAY_TRI_BYTES = 1024 * 1024;
static const VkDeviceSize SW_VK_MIN_OVERLAY_TILE_BYTES = 64 * 1024;
static const VkDeviceSize SW_VK_MIN_OVERLAY_TILE_INDEX_BYTES = 1024 * 1024;
static const VkDeviceSize SW_VK_MIN_HYBRID_PIXEL_BYTES = 4 * 1024 * 1024;
static const VkDeviceSize SW_VK_MIN_HYBRID_FRAME_BYTES = 8 * 1024 * 1024;
static const VkDeviceSize SW_VK_MIN_HYBRID_WORLD_POSITION_BYTES = 4 * 1024 * 1024;
static const VkDeviceSize SW_VK_MIN_HYBRID_LIGHT_BYTES = 64 * 1024;
static const VkDeviceSize SW_VK_MIN_HYBRID_LIGHT_TILE_BYTES = 64 * 1024;
static const VkDeviceSize SW_VK_MIN_HYBRID_LIGHT_INDEX_BYTES = 256 * 1024;
static const VkDeviceSize SW_VK_MIN_TEXTURE_INFO_BYTES = 4096 * sizeof( swHybridTextureInfo_t );
static const VkDeviceSize SW_VK_MIN_TEXTURE_TEXEL_BYTES = 32 * 1024 * 1024 * sizeof( uint32_t );

static uint32_t SWVkDispatchGroups( uint32_t size, uint32_t groupSize ) {
	return ( size + groupSize - 1u ) / groupSize;
}

static void SWVkComputeMemoryBarrier( VkCommandBuffer commandBuffer, VkAccessFlags srcAccess, VkAccessFlags dstAccess ) {
	VkMemoryBarrier barrier;
	memset( &barrier, 0, sizeof( barrier ) );
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = srcAccess;
	barrier.dstAccessMask = dstAccess;
	vkCmdPipelineBarrier( commandBuffer,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 1, &barrier, 0, NULL, 0, NULL );
}

static bool SWVkRectsOverlap( int ax0, int ay0, int ax1, int ay1, int bx0, int by0, int bx1, int by1 ) {
	return ax0 < bx1 && bx0 < ax1 && ay0 < by1 && by0 < ay1;
}

static bool SWVkOverlayTriFrameBounds( const swHybridOverlayTri_t &tri, int frameWidth, int frameHeight, int &x0, int &y0, int &x1, int &y1 ) {
	const int sourceWidth = Max( 1, tri.source[0] );
	const int sourceHeight = Max( 1, tri.source[1] );
	const int viewportX = tri.viewport[0];
	const int viewportY = tri.viewport[1];
	const int presentW = Max( 1, tri.viewport[2] );
	const int presentH = Max( 1, tri.viewport[3] );
	const int minFrameX = viewportX + ( tri.bounds[0] * presentW ) / sourceWidth;
	const int minFrameY = viewportY + ( tri.bounds[1] * presentH ) / sourceHeight;
	const int maxFrameX = viewportX + ( ( tri.bounds[2] + 1 ) * presentW + sourceWidth - 1 ) / sourceWidth;
	const int maxFrameY = viewportY + ( ( tri.bounds[3] + 1 ) * presentH + sourceHeight - 1 ) / sourceHeight;
	x0 = Max( 0, Min( frameWidth - 1, minFrameX ) );
	y0 = Max( 0, Min( frameHeight - 1, minFrameY ) );
	x1 = Max( 0, Min( frameWidth, maxFrameX ) );
	y1 = Max( 0, Min( frameHeight, maxFrameY ) );
	return x1 > x0 && y1 > y0;
}

static VkDeviceSize SWVkGrowBufferSize( VkDeviceSize required, VkDeviceSize minimum ) {
	VkDeviceSize size = Max( required, minimum );
	VkDeviceSize grown = minimum;
	while ( grown < size && grown <= ( ~static_cast<VkDeviceSize>( 0 ) >> 1 ) ) {
		grown <<= 1;
	}
	return Max( size, grown );
}

static uint64_t SWVkHashU32( uint64_t hash, uint32_t value ) {
	hash ^= value;
	hash *= 1099511628211ULL;
	return hash;
}

static uint64_t SWVkHashU64( uint64_t hash, uint64_t value ) {
	hash = SWVkHashU32( hash, static_cast<uint32_t>( value ) );
	hash = SWVkHashU32( hash, static_cast<uint32_t>( value >> 32 ) );
	return hash;
}

struct swVkShadowJob_t {
	swVkShadowJob_t() {
		commandBuffer = VK_NULL_HANDLE;
		fence = VK_NULL_HANDLE;
		descriptorSet = VK_NULL_HANDLE;
		lightKey = 0;
		width = 0;
		height = 0;
		pendingSubmit = false;
		submitted = false;
	}

	swVkBuffer_t worldPositionBuffer;
	swVkBuffer_t shadowMaskBuffer;
	VkCommandBuffer commandBuffer;
	VkFence fence;
	VkDescriptorSet descriptorSet;
	uintptr_t lightKey;
	int width;
	int height;
	bool pendingSubmit;
	bool submitted;
};

struct swVkFrameContext_t {
	swVkFrameContext_t() {
		commandBuffer = VK_NULL_HANDLE;
		imageAvailableSemaphore = VK_NULL_HANDLE;
		renderFinishedSemaphore = VK_NULL_HANDLE;
		inFlightFence = VK_NULL_HANDLE;
		submitted = false;
		uploadBuffer = VK_NULL_HANDLE;
		uploadMemory = VK_NULL_HANDLE;
		uploadBufferSize = 0;
		hybridDescriptorSet = VK_NULL_HANDLE;
		twoDDescriptorSet = VK_NULL_HANDLE;
		hybridUpscaleDescriptorSet = VK_NULL_HANDLE;
		overlayDescriptorSet = VK_NULL_HANDLE;
		overlayTileDescriptorSet = VK_NULL_HANDLE;
		timestampQueryPending = false;
		timestampFrame.Clear();
	}

	VkCommandBuffer commandBuffer;
	VkSemaphore imageAvailableSemaphore;
	VkSemaphore renderFinishedSemaphore;
	VkFence inFlightFence;
	bool submitted;

	VkBuffer uploadBuffer;
	VkDeviceMemory uploadMemory;
	VkDeviceSize uploadBufferSize;

	swVkBuffer_t hybridDepthBuffer;
	swVkBuffer_t hybridNormalBuffer;
	swVkBuffer_t hybridTangentBuffer;
	swVkBuffer_t hybridBitangentBuffer;
	swVkBuffer_t hybridUVBuffer;
	swVkBuffer_t hybridMaterialBuffer;
	swVkBuffer_t hybridAlbedoBuffer;
	swVkBuffer_t hybridSpecularBuffer;
	swVkBuffer_t hybridSurfaceBuffer;
	swVkBuffer_t hybridLitBuffer;
	swVkBuffer_t hybridFrameBuffer;
	swVkBuffer_t hybridOverlayBuffer;
	swVkBuffer_t hybrid2DSourceBuffer;
	swVkBuffer_t hybridOverlayTriBuffer;
	swVkBuffer_t hybridOverlayTileBuffer;
	swVkBuffer_t hybridOverlayTileIndexBuffer;
	swVkBuffer_t hybridWorldPositionBuffer;
	swVkBuffer_t hybridLightBuffer;
	swVkBuffer_t hybridLightTileBuffer;
	swVkBuffer_t hybridLightIndexBuffer;

	VkDescriptorSet hybridDescriptorSet;
	VkDescriptorSet twoDDescriptorSet;
	VkDescriptorSet hybridUpscaleDescriptorSet;
	VkDescriptorSet overlayDescriptorSet;
	VkDescriptorSet overlayTileDescriptorSet;

	bool timestampQueryPending;
	swVkGpuTimestampFrame_t timestampFrame;
};

class idSoftwareVulkanBridge {
public:
	idSoftwareVulkanBridge();
	~idSoftwareVulkanBridge();

	bool BlitView( const viewDef_t *viewDef, const unsigned int *bgra, int width, int height, int presentWidth, int presentHeight );
	bool CompositeHybridGBuffer( const viewDef_t *viewDef, const swHybridGBufferUpload_t &gbuffer, int width, int height, int presentWidth, int presentHeight );
	bool UpdateHybridTextures( const swHybridTextureInfo_t *textureInfos, int textureInfoCount, const unsigned int *textureTexels, int textureTexelCount, unsigned int textureGeneration );
	bool QueueHybridOverlayTriangles( const viewDef_t *viewDef, const swHybridOverlayTri_t *tris, int triCount, int width, int height, int presentWidth, int presentHeight );
	bool QueueHybridOverlaySourceTriangles( const viewDef_t *viewDef, const swHybridOverlayTri_t *tris, int triCount, int width, int height, int presentWidth, int presentHeight, const unsigned int *sourcePixels, int sourceWidth, int sourceHeight );
	bool ReadView( const viewDef_t *viewDef, unsigned int *bgra, int width, int height );
	bool PresentFrame();
	bool RayQueryAvailable();
	bool PrepareRayQueryScene( const viewDef_t *viewDef );
	bool BeginLightShadowMask( const viewLight_t *vLight, const idVec4 *worldPositions, int width, int height );
	bool FinishLightShadowMask( const viewLight_t *vLight, int width, int height, unsigned char *shadowMask );
	bool TraceLightShadowMask( const viewLight_t *vLight, const idVec4 *worldPositions, int width, int height, unsigned char *shadowMask );
	void DestroyRayQueryBlasHandle( void *blas );
	void DestroyRayQueryEntityHandle( void *entityTlas );
	void ClearRayQueryScene();
	void Shutdown();

private:
	bool EnsureInitialized();
	bool CreateInstance();
	bool CreateSurface();
	bool PickPhysicalDevice();
	bool CreateDevice();
	bool CreateSwapchain();
	bool CreateCommandObjects();
	bool CreateTimestampQueries();
	bool Create2DPipeline();
	bool CreateOverlayPipeline();
	bool CreateOverlayTilePipeline();
	bool CreateHybridPipeline();
	bool CreateShadowPipeline();
	bool EnsureUploadBuffer( int requiredWidth, int requiredHeight );
	bool Ensure2DBuffers( int sourcePixelCount );
	bool EnsureOverlayBuffers( int triCount );
	bool EnsureOverlayTileBuffers( int tileCount, int tileIndexCount );
	bool EnsureHybridBuffers( int requiredWidth, int requiredHeight, int textureInfoCount, int textureTexelCount, int lightCount, int lightTileCount, int lightIndexCount );
	bool EnsureHybridTextureBuffers( int textureInfoCount, int textureTexelCount, bool &texelBufferPreserved );
	bool EnsureShadowBuffers( int width, int height );
	bool EnsureFrameBuffer();
	bool UploadBuffer( swVkBuffer_t &buffer, const void *data, VkDeviceSize size, const char *failureText );
	bool UploadBufferRange( swVkBuffer_t &buffer, VkDeviceSize offset, const void *data, VkDeviceSize size, const char *failureText );
	bool UploadHybridTextures( const swHybridTextureInfo_t *textureInfos, int textureInfoCount, const unsigned int *textureTexels, int textureTexelCount, unsigned int textureGeneration, bool texelBufferPreserved );
	bool CreateBuffer( VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, bool deviceAddress, swVkBuffer_t &buffer );
	void DestroyBuffer( swVkBuffer_t &buffer );
	VkDeviceAddress GetBufferAddress( VkBuffer buffer ) const;
	void LoadFrameContext( int index );
	void StoreFrameContext();
	void WaitForFrameContext( int index );
	void WaitForAllFrameContexts();
	void DestroyFrameContextBuffers( swVkFrameContext_t &ctx );
	bool ImmediateSubmit( const VkCommandBufferBeginInfo &beginInfo );
	void WaitForSubmittedWork();
	bool BuildSurfaceBlas( swVkSurfaceBlas_t &entry, srfTriangles_t *geo );
	swVkEntityTlas_t *EnsureEntityTlas( idRenderEntityLocal *entity );
	bool BuildRayQueryTlas( const idList<swVkRayInstance_t> &instances );
	void DestroySurfaceBlas( swVkSurfaceBlas_t &entry );
	void DestroySurfaceBlasList( idList<swVkSurfaceBlas_t *> &list );
	void DestroyRayQueryTlas();
	void DestroyRayQueryScene();
	void Destroy2DPipeline();
	void DestroyOverlayPipeline();
	void DestroyOverlayTilePipeline();
	void DestroyShadowPipeline();
	void DestroyHybridPipeline();
	void DestroyHybridBuffers();
	void DestroyTimestampQueries();
	uintptr_t ShadowLightKey( const viewLight_t *vLight ) const;
	bool HasPendingShadowJobs() const;
	bool EnsureShadowJobResources( swVkShadowJob_t &job, int width, int height );
	swVkShadowJob_t *AllocShadowJob();
	bool RecordShadowJob( swVkShadowJob_t &job, const viewLight_t *vLight, const idVec4 *worldPositions, int width, int height );
	void DestroyShadowJobs();
	void BeginFrame( bool clearFramePixels );
	void Clear2DJobs();
	bool Queue2DOverlayBlit( const viewDef_t *viewDef, const unsigned int *bgra, int width, int height, int presentWidth, int presentHeight );
	void Update2DDescriptorSet( bool targetFrame );
	void UpdateOverlayDescriptorSet( bool targetFrame );
	void UpdateOverlayTileDescriptorSet( bool targetFrame );
	void UpdateHybridUpscaleDescriptorSet();
	bool ResolveHybridFrameToCpu();
	bool Record2DCompositeCommands( bool targetFrame );
	bool BuildOverlayTileBins( bool targetFrame, int &dispatchX0, int &dispatchY0, int &dispatchX1, int &dispatchY1 );
	bool RecordOverlayTileCompositeCommands( bool targetFrame );
	bool RecordOverlayRasterCommands( bool targetFrame );
	void DestroySwapchain();
	void DestroyUploadBuffer();
	void LogFailure( const char *text );
	void UpdateHybridDescriptorSet();
	swVkHybridCompositeVariant_t SelectHybridCompositeVariant( const swHybridGBufferUpload_t &gbuffer, bool shadowsActive ) const;
	bool RecordHybridCompositeCommands( bool outputForCompute );
	void RecordHybridFrameTransferBarrier();
	void ReadTimestampQueries();
	void WriteTimestamp( VkPipelineStageFlagBits stage, swVkGpuTimestampQuery_t query );
	bool RayQuerySurfaceCanCastShadow( const srfTriangles_t *geo, const idMaterial *material, const viewEntity_t *space ) const;
	bool RayQuerySurfaceTouchesShadowLight( const viewDef_t *viewDef, const srfTriangles_t *geo, const idMaterial *material, const viewEntity_t *space ) const;
	bool AddRayQuerySurfaceInstance( idList<swVkRayInstance_t> &instances, srfTriangles_t *geo, const float modelMatrix[16], idRenderEntityLocal *entity );

	bool DeviceHasExtension( VkPhysicalDevice device, const char *name ) const;
	bool QueryRayQuerySupport( VkPhysicalDevice device ) const;
	bool FindQueueFamily( VkPhysicalDevice device, uint32_t &family ) const;
	uint32_t FindMemoryType( uint32_t memoryTypeBits, VkMemoryPropertyFlags properties ) const;
	VkSurfaceFormatKHR ChooseSurfaceFormat( const idList<VkSurfaceFormatKHR> &formats ) const;
	VkPresentModeKHR ChoosePresentMode( const idList<VkPresentModeKHR> &modes ) const;
	VkExtent2D ChooseExtent( const VkSurfaceCapabilitiesKHR &caps ) const;
	VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha( VkCompositeAlphaFlagsKHR supported ) const;

	bool initialized;
	bool failed;
	bool printedFailure;
	bool rayQuerySupported;
	int activeFrameContext;
	int nextFrameContext;
	bool frameBegun;
	bool frameDirty;
	bool hybridFrameDirty;
	bool hybridOverlayDirty;
	bool hybridOverlayOnly;
	bool timestampQuerySupported;
	bool timestampQueryPending;
	float timestampPeriod;
	uint32_t timestampValidBits;
	VkQueryPool timestampQueryPool;
	swVkGpuTimestampFrame_t timestampFrame;

	int frameWidth;
	int frameHeight;
	idList<unsigned int> framePixels;
	int hybridWidth;
	int hybridHeight;
	int hybridPresentWidth;
	int hybridPresentHeight;
	int hybridViewportX;
	int hybridViewportY;
	int hybridDebugView;
	int hybridTextureCount;
	int hybridTextureTexelCount;
	unsigned int hybridUploadedTextureGeneration;
	int hybridLightCount;
	int hybridLightTileCount;
	int hybridLightIndexCount;
	bool hybridShadowEnabled;
	swVkHybridCompositeVariant_t hybridCompositeVariant;
	float hybridViewOrigin[3];
	idList<unsigned int> hybrid2DSourcePixels;
	idList<swVk2DJob_t> hybrid2DJobs;
	idList<swHybridOverlayTri_t> hybridOverlayTris;
	idList<swVkOverlayTile_t> hybridOverlayTiles;
	idList<uint32_t> hybridOverlayTileIndices;
	int hybridOverlaySourceWidth;
	int hybridOverlaySourceHeight;
	int hybridOverlayPresentWidth;
	int hybridOverlayPresentHeight;
	int hybridOverlayViewportX;
	int hybridOverlayViewportY;

	VkInstance instance;
	VkSurfaceKHR surface;
	VkPhysicalDevice physicalDevice;
	VkDevice device;
	VkQueue queue;
	uint32_t queueFamily;

	VkSwapchainKHR swapchain;
	VkFormat swapchainFormat;
	VkPresentModeKHR swapchainPresentMode;
	VkExtent2D swapchainExtent;
	idList<VkImage> swapchainImages;
	idList<VkImageLayout> swapchainLayouts;

	VkCommandPool commandPool;
	swVkFrameContext_t frameContexts[SW_VK_FRAMES_IN_FLIGHT];
	VkCommandBuffer commandBuffer;
	VkSemaphore imageAvailableSemaphore;
	VkSemaphore renderFinishedSemaphore;
	VkFence inFlightFence;

	VkBuffer uploadBuffer;
	VkDeviceMemory uploadMemory;
	VkDeviceSize uploadBufferSize;

	swVkBuffer_t hybridDepthBuffer;
	swVkBuffer_t hybridNormalBuffer;
	swVkBuffer_t hybridTangentBuffer;
	swVkBuffer_t hybridBitangentBuffer;
	swVkBuffer_t hybridUVBuffer;
	swVkBuffer_t hybridMaterialBuffer;
	swVkBuffer_t hybridAlbedoBuffer;
	swVkBuffer_t hybridSpecularBuffer;
	swVkBuffer_t hybridSurfaceBuffer;
	swVkBuffer_t hybridLitBuffer;
	swVkBuffer_t hybridFrameBuffer;
	swVkBuffer_t hybridOverlayBuffer;
	swVkBuffer_t hybrid2DSourceBuffer;
	swVkBuffer_t hybridOverlayTriBuffer;
	swVkBuffer_t hybridOverlayTileBuffer;
	swVkBuffer_t hybridOverlayTileIndexBuffer;
	swVkBuffer_t hybridTextureInfoBuffer;
	swVkBuffer_t hybridTextureTexelBuffer;
	swVkBuffer_t hybridWorldPositionBuffer;
	swVkBuffer_t hybridLightBuffer;
	swVkBuffer_t hybridLightTileBuffer;
	swVkBuffer_t hybridLightIndexBuffer;

	VkDescriptorSetLayout hybridDescriptorSetLayout;
	VkDescriptorPool hybridDescriptorPool;
	VkDescriptorSet hybridDescriptorSet;
	VkPipelineLayout hybridPipelineLayout;
	VkPipeline hybridPipelines[SW_VK_HYBRID_VARIANT_COUNT];
	VkShaderModule hybridShaderModules[SW_VK_HYBRID_VARIANT_COUNT];

	swVkBuffer_t shadowScratchBuffer;
	swVkBuffer_t tlasBuffer;
	swVkBuffer_t instanceBuffer;
	swVkBuffer_t worldPositionBuffer;
	swVkBuffer_t shadowMaskBuffer;
	swVkShadowJob_t shadowJobs[SW_VK_MAX_SHADOW_JOBS];
	idList<swVkSurfaceBlas_t *> rayAttachedBlas;
	idList<swVkEntityTlas_t *> rayEntityTlasCache;

	VkDescriptorSetLayout twoDDescriptorSetLayout;
	VkDescriptorPool twoDDescriptorPool;
	VkDescriptorSet twoDDescriptorSet;
	VkDescriptorSet hybridUpscaleDescriptorSet;
	VkPipelineLayout twoDPipelineLayout;
	VkPipeline twoDPipeline;
	VkShaderModule twoDShaderModule;

	VkDescriptorSetLayout overlayDescriptorSetLayout;
	VkDescriptorPool overlayDescriptorPool;
	VkDescriptorSet overlayDescriptorSet;
	VkPipelineLayout overlayPipelineLayout;
	VkPipeline overlayPipeline;
	VkShaderModule overlayShaderModule;

	VkDescriptorSetLayout overlayTileDescriptorSetLayout;
	VkDescriptorPool overlayTileDescriptorPool;
	VkDescriptorSet overlayTileDescriptorSet;
	VkPipelineLayout overlayTilePipelineLayout;
	VkPipeline overlayTilePipeline;
	VkShaderModule overlayTileShaderModule;

	VkAccelerationStructureKHR tlas;
	bool rayQuerySceneReady;
	int rayQuerySceneFrame;
	uint64_t rayTlasSignature;
	int rayTlasInstanceCount;

	VkDescriptorSetLayout shadowDescriptorSetLayout;
	VkDescriptorPool shadowDescriptorPool;
	VkDescriptorSet shadowDescriptorSet;
	VkPipelineLayout shadowPipelineLayout;
	VkPipeline shadowPipeline;
	VkShaderModule shadowShaderModule;

	PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2Local;
	PFN_vkGetBufferDeviceAddressKHR vkGetBufferDeviceAddressKHRLocal;
	PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHRLocal;
	PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHRLocal;
	PFN_vkGetAccelerationStructureDeviceAddressKHR vkGetAccelerationStructureDeviceAddressKHRLocal;
	PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKHRLocal;
	PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAccelerationStructuresKHRLocal;
};

static idSoftwareVulkanBridge swVulkanBridge;

idSoftwareVulkanBridge::idSoftwareVulkanBridge() {
	initialized = false;
	failed = false;
	printedFailure = false;
	rayQuerySupported = false;
	activeFrameContext = 0;
	nextFrameContext = 0;
	frameBegun = false;
	frameDirty = false;
	hybridFrameDirty = false;
	hybridOverlayDirty = false;
	hybridOverlayOnly = false;
	timestampQuerySupported = false;
	timestampQueryPending = false;
	timestampPeriod = 0.0f;
	timestampValidBits = 0;
	timestampQueryPool = VK_NULL_HANDLE;
	timestampFrame.Clear();
	frameWidth = 0;
	frameHeight = 0;
	hybridWidth = 0;
	hybridHeight = 0;
	hybridPresentWidth = 0;
	hybridPresentHeight = 0;
	hybridViewportX = 0;
	hybridViewportY = 0;
	hybridDebugView = 0;
	hybridTextureCount = 0;
	hybridTextureTexelCount = 0;
	hybridUploadedTextureGeneration = 0;
	hybridLightCount = 0;
	hybridLightTileCount = 0;
	hybridLightIndexCount = 0;
	hybridShadowEnabled = false;
	hybridCompositeVariant = SW_VK_HYBRID_VARIANT_GENERIC_NOSHADOW;
	hybridViewOrigin[0] = hybridViewOrigin[1] = hybridViewOrigin[2] = 0.0f;
	hybridOverlaySourceWidth = 0;
	hybridOverlaySourceHeight = 0;
	hybridOverlayPresentWidth = 0;
	hybridOverlayPresentHeight = 0;
	hybridOverlayViewportX = 0;
	hybridOverlayViewportY = 0;

	instance = VK_NULL_HANDLE;
	surface = VK_NULL_HANDLE;
	physicalDevice = VK_NULL_HANDLE;
	device = VK_NULL_HANDLE;
	queue = VK_NULL_HANDLE;
	queueFamily = 0;

	swapchain = VK_NULL_HANDLE;
	swapchainFormat = VK_FORMAT_UNDEFINED;
	swapchainPresentMode = VK_PRESENT_MODE_FIFO_KHR;
	swapchainExtent.width = 0;
	swapchainExtent.height = 0;

	commandPool = VK_NULL_HANDLE;
	commandBuffer = VK_NULL_HANDLE;
	imageAvailableSemaphore = VK_NULL_HANDLE;
	renderFinishedSemaphore = VK_NULL_HANDLE;
	inFlightFence = VK_NULL_HANDLE;

	uploadBuffer = VK_NULL_HANDLE;
	uploadMemory = VK_NULL_HANDLE;
	uploadBufferSize = 0;

	hybridDescriptorSetLayout = VK_NULL_HANDLE;
	hybridDescriptorPool = VK_NULL_HANDLE;
	hybridDescriptorSet = VK_NULL_HANDLE;
	hybridPipelineLayout = VK_NULL_HANDLE;
	for ( int i = 0; i < SW_VK_HYBRID_VARIANT_COUNT; i++ ) {
		hybridPipelines[i] = VK_NULL_HANDLE;
		hybridShaderModules[i] = VK_NULL_HANDLE;
	}

	twoDDescriptorSetLayout = VK_NULL_HANDLE;
	twoDDescriptorPool = VK_NULL_HANDLE;
	twoDDescriptorSet = VK_NULL_HANDLE;
	hybridUpscaleDescriptorSet = VK_NULL_HANDLE;
	twoDPipelineLayout = VK_NULL_HANDLE;
	twoDPipeline = VK_NULL_HANDLE;
	twoDShaderModule = VK_NULL_HANDLE;

	overlayDescriptorSetLayout = VK_NULL_HANDLE;
	overlayDescriptorPool = VK_NULL_HANDLE;
	overlayDescriptorSet = VK_NULL_HANDLE;
	overlayPipelineLayout = VK_NULL_HANDLE;
	overlayPipeline = VK_NULL_HANDLE;
	overlayShaderModule = VK_NULL_HANDLE;

	overlayTileDescriptorSetLayout = VK_NULL_HANDLE;
	overlayTileDescriptorPool = VK_NULL_HANDLE;
	overlayTileDescriptorSet = VK_NULL_HANDLE;
	overlayTilePipelineLayout = VK_NULL_HANDLE;
	overlayTilePipeline = VK_NULL_HANDLE;
	overlayTileShaderModule = VK_NULL_HANDLE;

	tlas = VK_NULL_HANDLE;
	rayQuerySceneReady = false;
	rayQuerySceneFrame = 0;
	rayTlasSignature = 0;
	rayTlasInstanceCount = 0;

	shadowDescriptorSetLayout = VK_NULL_HANDLE;
	shadowDescriptorPool = VK_NULL_HANDLE;
	shadowDescriptorSet = VK_NULL_HANDLE;
	shadowPipelineLayout = VK_NULL_HANDLE;
	shadowPipeline = VK_NULL_HANDLE;
	shadowShaderModule = VK_NULL_HANDLE;

	vkGetPhysicalDeviceFeatures2Local = NULL;
	vkGetBufferDeviceAddressKHRLocal = NULL;
	vkCreateAccelerationStructureKHRLocal = NULL;
	vkDestroyAccelerationStructureKHRLocal = NULL;
	vkGetAccelerationStructureDeviceAddressKHRLocal = NULL;
	vkGetAccelerationStructureBuildSizesKHRLocal = NULL;
	vkCmdBuildAccelerationStructuresKHRLocal = NULL;
}

idSoftwareVulkanBridge::~idSoftwareVulkanBridge() {
	Shutdown();
}

void idSoftwareVulkanBridge::LogFailure( const char *text ) {
	if ( !printedFailure ) {
		common->Warning( "software vulkan: %s", text );
		printedFailure = true;
	}
}

bool idSoftwareVulkanBridge::EnsureInitialized() {
	if ( initialized ) {
		return true;
	}
	if ( failed ) {
		return false;
	}
	if ( !r_softwareVulkanPresent.GetBool() ) {
		return false;
	}
	if ( !win32.hWnd ) {
		LogFailure( "window handle is not ready" );
		failed = true;
		return false;
	}

	if ( !CreateInstance() || !CreateSurface() || !PickPhysicalDevice() || !CreateDevice() || !CreateSwapchain() || !CreateCommandObjects() || !Create2DPipeline() || !CreateOverlayPipeline() || !CreateHybridPipeline() || !CreateShadowPipeline() ) {
		Shutdown();
		failed = true;
		return false;
	}
	if ( !CreateOverlayTilePipeline() ) {
		common->Warning( "software vulkan: overlay tile compositor unavailable, using per-triangle overlay path" );
		DestroyOverlayTilePipeline();
	}

	initialized = true;
	common->Printf( "software vulkan: presentation enabled (%dx%d, %s present)%s\n",
		static_cast<int>( swapchainExtent.width ),
		static_cast<int>( swapchainExtent.height ),
		SWVkPresentModeName( swapchainPresentMode ),
		rayQuerySupported ? ", ray query shadows supported" : "" );
	return true;
}

bool idSoftwareVulkanBridge::CreateInstance() {
	VkApplicationInfo appInfo;
	memset( &appInfo, 0, sizeof( appInfo ) );
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = GAME_NAME;
	appInfo.applicationVersion = VK_MAKE_VERSION( 1, 0, 0 );
	appInfo.pEngineName = "Doom3Soft";
	appInfo.engineVersion = VK_MAKE_VERSION( 1, 0, 0 );
	appInfo.apiVersion = VK_API_VERSION_1_2;

	const char *extensions[] = {
		VK_KHR_SURFACE_EXTENSION_NAME,
		VK_KHR_WIN32_SURFACE_EXTENSION_NAME
	};

	VkInstanceCreateInfo createInfo;
	memset( &createInfo, 0, sizeof( createInfo ) );
	createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	createInfo.pApplicationInfo = &appInfo;
	createInfo.enabledExtensionCount = sizeof( extensions ) / sizeof( extensions[0] );
	createInfo.ppEnabledExtensionNames = extensions;

	if ( vkCreateInstance( &createInfo, NULL, &instance ) != VK_SUCCESS ) {
		LogFailure( "vkCreateInstance failed" );
		return false;
	}

	vkGetPhysicalDeviceFeatures2Local =
		reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>( vkGetInstanceProcAddr( instance, "vkGetPhysicalDeviceFeatures2" ) );
	if ( !vkGetPhysicalDeviceFeatures2Local ) {
		vkGetPhysicalDeviceFeatures2Local =
			reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>( vkGetInstanceProcAddr( instance, "vkGetPhysicalDeviceFeatures2KHR" ) );
	}
	return true;
}

bool idSoftwareVulkanBridge::CreateSurface() {
	VkWin32SurfaceCreateInfoKHR createInfo;
	memset( &createInfo, 0, sizeof( createInfo ) );
	createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	createInfo.hinstance = win32.hInstance ? win32.hInstance : GetModuleHandle( NULL );
	createInfo.hwnd = win32.hWnd;

	if ( vkCreateWin32SurfaceKHR( instance, &createInfo, NULL, &surface ) != VK_SUCCESS ) {
		LogFailure( "vkCreateWin32SurfaceKHR failed" );
		return false;
	}
	return true;
}

bool idSoftwareVulkanBridge::FindQueueFamily( VkPhysicalDevice testDevice, uint32_t &family ) const {
	uint32_t queueFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties( testDevice, &queueFamilyCount, NULL );
	if ( queueFamilyCount == 0 ) {
		return false;
	}

	idList<VkQueueFamilyProperties> queueFamilies;
	queueFamilies.SetNum( queueFamilyCount, false );
	vkGetPhysicalDeviceQueueFamilyProperties( testDevice, &queueFamilyCount, queueFamilies.Ptr() );

	for ( uint32_t i = 0; i < queueFamilyCount; i++ ) {
		VkBool32 presentSupport = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR( testDevice, i, surface, &presentSupport );
		if ( ( queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT ) && presentSupport ) {
			family = i;
			return true;
		}
	}
	return false;
}

bool idSoftwareVulkanBridge::DeviceHasExtension( VkPhysicalDevice testDevice, const char *name ) const {
	uint32_t extensionCount = 0;
	vkEnumerateDeviceExtensionProperties( testDevice, NULL, &extensionCount, NULL );
	if ( extensionCount == 0 ) {
		return false;
	}

	idList<VkExtensionProperties> extensions;
	extensions.SetNum( extensionCount, false );
	vkEnumerateDeviceExtensionProperties( testDevice, NULL, &extensionCount, extensions.Ptr() );

	for ( uint32_t i = 0; i < extensionCount; i++ ) {
		if ( strcmp( extensions[i].extensionName, name ) == 0 ) {
			return true;
		}
	}
	return false;
}

bool idSoftwareVulkanBridge::QueryRayQuerySupport( VkPhysicalDevice testDevice ) const {
	if ( !vkGetPhysicalDeviceFeatures2Local ) {
		return false;
	}
	if ( !DeviceHasExtension( testDevice, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME ) ||
		 !DeviceHasExtension( testDevice, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME ) ||
		 !DeviceHasExtension( testDevice, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME ) ||
		 !DeviceHasExtension( testDevice, VK_KHR_RAY_QUERY_EXTENSION_NAME ) ) {
		return false;
	}

	VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures;
	memset( &rayQueryFeatures, 0, sizeof( rayQueryFeatures ) );
	rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;

	VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationFeatures;
	memset( &accelerationFeatures, 0, sizeof( accelerationFeatures ) );
	accelerationFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
	accelerationFeatures.pNext = &rayQueryFeatures;

	VkPhysicalDeviceBufferDeviceAddressFeatures bufferAddressFeatures;
	memset( &bufferAddressFeatures, 0, sizeof( bufferAddressFeatures ) );
	bufferAddressFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
	bufferAddressFeatures.pNext = &accelerationFeatures;

	VkPhysicalDeviceFeatures2 features;
	memset( &features, 0, sizeof( features ) );
	features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features.pNext = &bufferAddressFeatures;

	vkGetPhysicalDeviceFeatures2Local( testDevice, &features );
	return bufferAddressFeatures.bufferDeviceAddress &&
		accelerationFeatures.accelerationStructure &&
		rayQueryFeatures.rayQuery;
}

bool idSoftwareVulkanBridge::PickPhysicalDevice() {
	uint32_t deviceCount = 0;
	vkEnumeratePhysicalDevices( instance, &deviceCount, NULL );
	if ( deviceCount == 0 ) {
		LogFailure( "no Vulkan physical devices found" );
		return false;
	}

	idList<VkPhysicalDevice> devices;
	devices.SetNum( deviceCount, false );
	vkEnumeratePhysicalDevices( instance, &deviceCount, devices.Ptr() );

	VkPhysicalDevice fallbackDevice = VK_NULL_HANDLE;
	uint32_t fallbackQueueFamily = 0;
	bool fallbackRayQuery = false;

	for ( uint32_t i = 0; i < deviceCount; i++ ) {
		uint32_t candidateQueueFamily = 0;
		if ( !FindQueueFamily( devices[i], candidateQueueFamily ) ) {
			continue;
		}
		if ( !DeviceHasExtension( devices[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME ) ) {
			continue;
		}

		uint32_t formatCount = 0;
		uint32_t presentModeCount = 0;
		vkGetPhysicalDeviceSurfaceFormatsKHR( devices[i], surface, &formatCount, NULL );
		vkGetPhysicalDeviceSurfacePresentModesKHR( devices[i], surface, &presentModeCount, NULL );
		if ( formatCount == 0 || presentModeCount == 0 ) {
			continue;
		}

		const bool candidateRayQuery = QueryRayQuerySupport( devices[i] );
		if ( fallbackDevice == VK_NULL_HANDLE || candidateRayQuery ) {
			fallbackDevice = devices[i];
			fallbackQueueFamily = candidateQueueFamily;
			fallbackRayQuery = candidateRayQuery;
			if ( candidateRayQuery ) {
				break;
			}
		}
	}

	if ( fallbackDevice == VK_NULL_HANDLE ) {
		LogFailure( "no Vulkan device supports graphics+present+swapchain" );
		return false;
	}

	physicalDevice = fallbackDevice;
	queueFamily = fallbackQueueFamily;
	rayQuerySupported = fallbackRayQuery;

	VkPhysicalDeviceProperties deviceProperties;
	memset( &deviceProperties, 0, sizeof( deviceProperties ) );
	vkGetPhysicalDeviceProperties( physicalDevice, &deviceProperties );
	timestampPeriod = deviceProperties.limits.timestampPeriod;

	uint32_t queueFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties( physicalDevice, &queueFamilyCount, NULL );
	if ( queueFamilyCount > queueFamily ) {
		idList<VkQueueFamilyProperties> queueFamilies;
		queueFamilies.SetNum( queueFamilyCount, false );
		vkGetPhysicalDeviceQueueFamilyProperties( physicalDevice, &queueFamilyCount, queueFamilies.Ptr() );
		timestampValidBits = queueFamilies[queueFamily].timestampValidBits;
	}
	timestampQuerySupported = timestampPeriod > 0.0f && timestampValidBits > 0;
	return true;
}

bool idSoftwareVulkanBridge::CreateDevice() {
	float queuePriority = 1.0f;

	VkDeviceQueueCreateInfo queueInfo;
	memset( &queueInfo, 0, sizeof( queueInfo ) );
	queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueInfo.queueFamilyIndex = queueFamily;
	queueInfo.queueCount = 1;
	queueInfo.pQueuePriorities = &queuePriority;

	const char *extensions[8];
	uint32_t extensionCount = 0;
	extensions[extensionCount++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
	if ( rayQuerySupported ) {
		extensions[extensionCount++] = VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME;
		extensions[extensionCount++] = VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME;
		extensions[extensionCount++] = VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME;
		extensions[extensionCount++] = VK_KHR_RAY_QUERY_EXTENSION_NAME;
	}

	VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures;
	memset( &rayQueryFeatures, 0, sizeof( rayQueryFeatures ) );
	rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
	rayQueryFeatures.rayQuery = rayQuerySupported ? VK_TRUE : VK_FALSE;

	VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationFeatures;
	memset( &accelerationFeatures, 0, sizeof( accelerationFeatures ) );
	accelerationFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
	accelerationFeatures.accelerationStructure = rayQuerySupported ? VK_TRUE : VK_FALSE;
	accelerationFeatures.pNext = rayQuerySupported ? &rayQueryFeatures : NULL;

	VkPhysicalDeviceBufferDeviceAddressFeatures bufferAddressFeatures;
	memset( &bufferAddressFeatures, 0, sizeof( bufferAddressFeatures ) );
	bufferAddressFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
	bufferAddressFeatures.bufferDeviceAddress = rayQuerySupported ? VK_TRUE : VK_FALSE;
	bufferAddressFeatures.pNext = rayQuerySupported ? &accelerationFeatures : NULL;

	VkPhysicalDeviceFeatures2 enabledFeatures;
	memset( &enabledFeatures, 0, sizeof( enabledFeatures ) );
	enabledFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	enabledFeatures.pNext = rayQuerySupported ? &bufferAddressFeatures : NULL;

	VkDeviceCreateInfo createInfo;
	memset( &createInfo, 0, sizeof( createInfo ) );
	createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	createInfo.pNext = rayQuerySupported ? &enabledFeatures : NULL;
	createInfo.queueCreateInfoCount = 1;
	createInfo.pQueueCreateInfos = &queueInfo;
	createInfo.enabledExtensionCount = extensionCount;
	createInfo.ppEnabledExtensionNames = extensions;

	if ( vkCreateDevice( physicalDevice, &createInfo, NULL, &device ) != VK_SUCCESS ) {
		LogFailure( "vkCreateDevice failed" );
		return false;
	}

	vkGetDeviceQueue( device, queueFamily, 0, &queue );

	if ( rayQuerySupported ) {
		vkGetBufferDeviceAddressKHRLocal =
			reinterpret_cast<PFN_vkGetBufferDeviceAddressKHR>( vkGetDeviceProcAddr( device, "vkGetBufferDeviceAddressKHR" ) );
		vkCreateAccelerationStructureKHRLocal =
			reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>( vkGetDeviceProcAddr( device, "vkCreateAccelerationStructureKHR" ) );
		vkDestroyAccelerationStructureKHRLocal =
			reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>( vkGetDeviceProcAddr( device, "vkDestroyAccelerationStructureKHR" ) );
		vkGetAccelerationStructureDeviceAddressKHRLocal =
			reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>( vkGetDeviceProcAddr( device, "vkGetAccelerationStructureDeviceAddressKHR" ) );
		vkGetAccelerationStructureBuildSizesKHRLocal =
			reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>( vkGetDeviceProcAddr( device, "vkGetAccelerationStructureBuildSizesKHR" ) );
		vkCmdBuildAccelerationStructuresKHRLocal =
			reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>( vkGetDeviceProcAddr( device, "vkCmdBuildAccelerationStructuresKHR" ) );

		if ( !vkGetBufferDeviceAddressKHRLocal || !vkCreateAccelerationStructureKHRLocal ||
			 !vkDestroyAccelerationStructureKHRLocal || !vkGetAccelerationStructureDeviceAddressKHRLocal ||
			 !vkGetAccelerationStructureBuildSizesKHRLocal ||
			 !vkCmdBuildAccelerationStructuresKHRLocal ) {
			rayQuerySupported = false;
			common->Warning( "software vulkan: ray-query device functions missing; shadows disabled" );
		}
	}
	return true;
}

VkSurfaceFormatKHR idSoftwareVulkanBridge::ChooseSurfaceFormat( const idList<VkSurfaceFormatKHR> &formats ) const {
	for ( int i = 0; i < formats.Num(); i++ ) {
		if ( formats[i].format == VK_FORMAT_B8G8R8A8_UNORM ) {
			return formats[i];
		}
	}
	for ( int i = 0; i < formats.Num(); i++ ) {
		if ( formats[i].format == VK_FORMAT_B8G8R8A8_SRGB ) {
			return formats[i];
		}
	}
	return formats[0];
}

VkPresentModeKHR idSoftwareVulkanBridge::ChoosePresentMode( const idList<VkPresentModeKHR> &modes ) const {
	bool hasImmediate = false;
	bool hasMailbox = false;
	for ( int i = 0; i < modes.Num(); i++ ) {
		hasImmediate = hasImmediate || modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR;
		hasMailbox = hasMailbox || modes[i] == VK_PRESENT_MODE_MAILBOX_KHR;
	}

	switch ( idMath::ClampInt( 0, 2, r_softwareVulkanPresentMode.GetInteger() ) ) {
		case 0:
			if ( hasImmediate ) {
				return VK_PRESENT_MODE_IMMEDIATE_KHR;
			}
			if ( hasMailbox ) {
				return VK_PRESENT_MODE_MAILBOX_KHR;
			}
			break;
		case 1:
			if ( hasMailbox ) {
				return VK_PRESENT_MODE_MAILBOX_KHR;
			}
			if ( hasImmediate ) {
				return VK_PRESENT_MODE_IMMEDIATE_KHR;
			}
			break;
		case 2:
		default:
			break;
	}
	return VK_PRESENT_MODE_FIFO_KHR;
}

VkCompositeAlphaFlagBitsKHR idSoftwareVulkanBridge::ChooseCompositeAlpha( VkCompositeAlphaFlagsKHR supported ) const {
	if ( supported & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR ) {
		return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	}
	if ( supported & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR ) {
		return VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
	}
	if ( supported & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR ) {
		return VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
	}
	return VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
}

VkExtent2D idSoftwareVulkanBridge::ChooseExtent( const VkSurfaceCapabilitiesKHR &caps ) const {
	if ( caps.currentExtent.width != UINT32_MAX ) {
		return caps.currentExtent;
	}

	RECT clientRect;
	GetClientRect( win32.hWnd, &clientRect );

	VkExtent2D extent;
	extent.width = Max( 1, static_cast<int>( clientRect.right - clientRect.left ) );
	extent.height = Max( 1, static_cast<int>( clientRect.bottom - clientRect.top ) );
	extent.width = Max( caps.minImageExtent.width, Min( caps.maxImageExtent.width, extent.width ) );
	extent.height = Max( caps.minImageExtent.height, Min( caps.maxImageExtent.height, extent.height ) );
	return extent;
}

bool idSoftwareVulkanBridge::CreateSwapchain() {
	VkSurfaceCapabilitiesKHR caps;
	if ( vkGetPhysicalDeviceSurfaceCapabilitiesKHR( physicalDevice, surface, &caps ) != VK_SUCCESS ) {
		LogFailure( "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed" );
		return false;
	}

	uint32_t formatCount = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR( physicalDevice, surface, &formatCount, NULL );
	idList<VkSurfaceFormatKHR> formats;
	formats.SetNum( formatCount, false );
	vkGetPhysicalDeviceSurfaceFormatsKHR( physicalDevice, surface, &formatCount, formats.Ptr() );

	uint32_t presentModeCount = 0;
	vkGetPhysicalDeviceSurfacePresentModesKHR( physicalDevice, surface, &presentModeCount, NULL );
	idList<VkPresentModeKHR> presentModes;
	presentModes.SetNum( presentModeCount, false );
	vkGetPhysicalDeviceSurfacePresentModesKHR( physicalDevice, surface, &presentModeCount, presentModes.Ptr() );

	const VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat( formats );
	const VkPresentModeKHR presentMode = ChoosePresentMode( presentModes );
	const VkExtent2D extent = ChooseExtent( caps );

	uint32_t imageCount = caps.minImageCount + 1;
	if ( caps.maxImageCount > 0 && imageCount > caps.maxImageCount ) {
		imageCount = caps.maxImageCount;
	}

	VkSwapchainCreateInfoKHR createInfo;
	memset( &createInfo, 0, sizeof( createInfo ) );
	createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	createInfo.surface = surface;
	createInfo.minImageCount = imageCount;
	createInfo.imageFormat = surfaceFormat.format;
	createInfo.imageColorSpace = surfaceFormat.colorSpace;
	createInfo.imageExtent = extent;
	createInfo.imageArrayLayers = 1;
	createInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	createInfo.preTransform = caps.currentTransform;
	createInfo.compositeAlpha = ChooseCompositeAlpha( caps.supportedCompositeAlpha );
	createInfo.presentMode = presentMode;
	createInfo.clipped = VK_TRUE;
	createInfo.oldSwapchain = VK_NULL_HANDLE;

	if ( vkCreateSwapchainKHR( device, &createInfo, NULL, &swapchain ) != VK_SUCCESS ) {
		LogFailure( "vkCreateSwapchainKHR failed" );
		return false;
	}

	uint32_t swapchainImageCount = 0;
	vkGetSwapchainImagesKHR( device, swapchain, &swapchainImageCount, NULL );
	swapchainImages.SetNum( swapchainImageCount, false );
	vkGetSwapchainImagesKHR( device, swapchain, &swapchainImageCount, swapchainImages.Ptr() );

	swapchainLayouts.SetNum( swapchainImageCount, false );
	for ( int i = 0; i < swapchainLayouts.Num(); i++ ) {
		swapchainLayouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;
	}

	swapchainFormat = surfaceFormat.format;
	swapchainPresentMode = presentMode;
	swapchainExtent = extent;
	frameWidth = static_cast<int>( swapchainExtent.width );
	frameHeight = static_cast<int>( swapchainExtent.height );
	return true;
}

bool idSoftwareVulkanBridge::CreateCommandObjects() {
	VkCommandPoolCreateInfo poolInfo;
	memset( &poolInfo, 0, sizeof( poolInfo ) );
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = queueFamily;

	if ( vkCreateCommandPool( device, &poolInfo, NULL, &commandPool ) != VK_SUCCESS ) {
		LogFailure( "vkCreateCommandPool failed" );
		return false;
	}

	VkCommandBufferAllocateInfo allocInfo;
	memset( &allocInfo, 0, sizeof( allocInfo ) );
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = commandPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = SW_VK_FRAMES_IN_FLIGHT;

	VkCommandBuffer commandBuffers[SW_VK_FRAMES_IN_FLIGHT];
	memset( commandBuffers, 0, sizeof( commandBuffers ) );
	if ( vkAllocateCommandBuffers( device, &allocInfo, commandBuffers ) != VK_SUCCESS ) {
		LogFailure( "vkAllocateCommandBuffers failed" );
		return false;
	}
	for ( int i = 0; i < SW_VK_FRAMES_IN_FLIGHT; i++ ) {
		frameContexts[i].commandBuffer = commandBuffers[i];
	}

	VkSemaphoreCreateInfo semaphoreInfo;
	memset( &semaphoreInfo, 0, sizeof( semaphoreInfo ) );
	semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	VkFenceCreateInfo fenceInfo;
	memset( &fenceInfo, 0, sizeof( fenceInfo ) );
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

	for ( int i = 0; i < SW_VK_FRAMES_IN_FLIGHT; i++ ) {
		if ( vkCreateSemaphore( device, &semaphoreInfo, NULL, &frameContexts[i].imageAvailableSemaphore ) != VK_SUCCESS ||
			 vkCreateSemaphore( device, &semaphoreInfo, NULL, &frameContexts[i].renderFinishedSemaphore ) != VK_SUCCESS ) {
			LogFailure( "vkCreateSemaphore failed" );
			return false;
		}
		if ( vkCreateFence( device, &fenceInfo, NULL, &frameContexts[i].inFlightFence ) != VK_SUCCESS ) {
			LogFailure( "vkCreateFence failed" );
			return false;
		}
	}
	LoadFrameContext( 0 );
	if ( timestampQuerySupported && !CreateTimestampQueries() ) {
		timestampQuerySupported = false;
		common->Warning( "software vulkan: timestamp queries unavailable" );
	}
	return true;
}

bool idSoftwareVulkanBridge::CreateTimestampQueries() {
	DestroyTimestampQueries();

	VkQueryPoolCreateInfo queryInfo;
	memset( &queryInfo, 0, sizeof( queryInfo ) );
	queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
	queryInfo.queryCount = SW_VK_GPU_TIMESTAMP_COUNT * SW_VK_FRAMES_IN_FLIGHT;
	if ( vkCreateQueryPool( device, &queryInfo, NULL, &timestampQueryPool ) != VK_SUCCESS ) {
		timestampQueryPool = VK_NULL_HANDLE;
		return false;
	}
	timestampQueryPending = false;
	return true;
}

bool idSoftwareVulkanBridge::CreateBuffer( VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, bool deviceAddress, swVkBuffer_t &buffer ) {
	DestroyBuffer( buffer );
	StoreFrameContext();

	VkBufferCreateInfo bufferInfo;
	memset( &bufferInfo, 0, sizeof( bufferInfo ) );
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = usage | ( deviceAddress ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0 );
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	if ( vkCreateBuffer( device, &bufferInfo, NULL, &buffer.buffer ) != VK_SUCCESS ) {
		LogFailure( "vkCreateBuffer failed" );
		return false;
	}

	VkMemoryRequirements memoryRequirements;
	vkGetBufferMemoryRequirements( device, buffer.buffer, &memoryRequirements );

	const uint32_t memoryType = FindMemoryType( memoryRequirements.memoryTypeBits, properties );
	if ( memoryType == UINT32_MAX ) {
		LogFailure( "no matching Vulkan memory type" );
		return false;
	}

	VkMemoryAllocateFlagsInfo flagsInfo;
	memset( &flagsInfo, 0, sizeof( flagsInfo ) );
	flagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
	flagsInfo.flags = deviceAddress ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0;

	VkMemoryAllocateInfo allocInfo;
	memset( &allocInfo, 0, sizeof( allocInfo ) );
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.pNext = deviceAddress ? &flagsInfo : NULL;
	allocInfo.allocationSize = memoryRequirements.size;
	allocInfo.memoryTypeIndex = memoryType;

	if ( vkAllocateMemory( device, &allocInfo, NULL, &buffer.memory ) != VK_SUCCESS ) {
		LogFailure( "vkAllocateMemory failed" );
		return false;
	}

	if ( vkBindBufferMemory( device, buffer.buffer, buffer.memory, 0 ) != VK_SUCCESS ) {
		LogFailure( "vkBindBufferMemory failed" );
		return false;
	}

	buffer.size = size;
	StoreFrameContext();
	return true;
}

void idSoftwareVulkanBridge::DestroyBuffer( swVkBuffer_t &buffer ) {
	if ( device == VK_NULL_HANDLE ) {
		buffer.buffer = VK_NULL_HANDLE;
		buffer.memory = VK_NULL_HANDLE;
		buffer.size = 0;
		return;
	}
	if ( buffer.buffer != VK_NULL_HANDLE ) {
		vkDestroyBuffer( device, buffer.buffer, NULL );
		buffer.buffer = VK_NULL_HANDLE;
	}
	if ( buffer.memory != VK_NULL_HANDLE ) {
		vkFreeMemory( device, buffer.memory, NULL );
		buffer.memory = VK_NULL_HANDLE;
	}
	buffer.size = 0;
}

VkDeviceAddress idSoftwareVulkanBridge::GetBufferAddress( VkBuffer buffer ) const {
	VkBufferDeviceAddressInfo addressInfo;
	memset( &addressInfo, 0, sizeof( addressInfo ) );
	addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
	addressInfo.buffer = buffer;
	return vkGetBufferDeviceAddressKHRLocal( device, &addressInfo );
}

void idSoftwareVulkanBridge::LoadFrameContext( int index ) {
	index = idMath::ClampInt( 0, SW_VK_FRAMES_IN_FLIGHT - 1, index );
	activeFrameContext = index;
	swVkFrameContext_t &ctx = frameContexts[index];

	commandBuffer = ctx.commandBuffer;
	imageAvailableSemaphore = ctx.imageAvailableSemaphore;
	renderFinishedSemaphore = ctx.renderFinishedSemaphore;
	inFlightFence = ctx.inFlightFence;
	uploadBuffer = ctx.uploadBuffer;
	uploadMemory = ctx.uploadMemory;
	uploadBufferSize = ctx.uploadBufferSize;

	hybridDepthBuffer = ctx.hybridDepthBuffer;
	hybridNormalBuffer = ctx.hybridNormalBuffer;
	hybridTangentBuffer = ctx.hybridTangentBuffer;
	hybridBitangentBuffer = ctx.hybridBitangentBuffer;
	hybridUVBuffer = ctx.hybridUVBuffer;
	hybridMaterialBuffer = ctx.hybridMaterialBuffer;
	hybridAlbedoBuffer = ctx.hybridAlbedoBuffer;
	hybridSpecularBuffer = ctx.hybridSpecularBuffer;
	hybridSurfaceBuffer = ctx.hybridSurfaceBuffer;
	hybridLitBuffer = ctx.hybridLitBuffer;
	hybridFrameBuffer = ctx.hybridFrameBuffer;
	hybridOverlayBuffer = ctx.hybridOverlayBuffer;
	hybrid2DSourceBuffer = ctx.hybrid2DSourceBuffer;
	hybridOverlayTriBuffer = ctx.hybridOverlayTriBuffer;
	hybridOverlayTileBuffer = ctx.hybridOverlayTileBuffer;
	hybridOverlayTileIndexBuffer = ctx.hybridOverlayTileIndexBuffer;
	hybridWorldPositionBuffer = ctx.hybridWorldPositionBuffer;
	hybridLightBuffer = ctx.hybridLightBuffer;
	hybridLightTileBuffer = ctx.hybridLightTileBuffer;
	hybridLightIndexBuffer = ctx.hybridLightIndexBuffer;

	hybridDescriptorSet = ctx.hybridDescriptorSet;
	twoDDescriptorSet = ctx.twoDDescriptorSet;
	hybridUpscaleDescriptorSet = ctx.hybridUpscaleDescriptorSet;
	overlayDescriptorSet = ctx.overlayDescriptorSet;
	overlayTileDescriptorSet = ctx.overlayTileDescriptorSet;
}

void idSoftwareVulkanBridge::StoreFrameContext() {
	swVkFrameContext_t &ctx = frameContexts[activeFrameContext];

	ctx.commandBuffer = commandBuffer;
	ctx.imageAvailableSemaphore = imageAvailableSemaphore;
	ctx.renderFinishedSemaphore = renderFinishedSemaphore;
	ctx.inFlightFence = inFlightFence;
	ctx.uploadBuffer = uploadBuffer;
	ctx.uploadMemory = uploadMemory;
	ctx.uploadBufferSize = uploadBufferSize;

	ctx.hybridDepthBuffer = hybridDepthBuffer;
	ctx.hybridNormalBuffer = hybridNormalBuffer;
	ctx.hybridTangentBuffer = hybridTangentBuffer;
	ctx.hybridBitangentBuffer = hybridBitangentBuffer;
	ctx.hybridUVBuffer = hybridUVBuffer;
	ctx.hybridMaterialBuffer = hybridMaterialBuffer;
	ctx.hybridAlbedoBuffer = hybridAlbedoBuffer;
	ctx.hybridSpecularBuffer = hybridSpecularBuffer;
	ctx.hybridSurfaceBuffer = hybridSurfaceBuffer;
	ctx.hybridLitBuffer = hybridLitBuffer;
	ctx.hybridFrameBuffer = hybridFrameBuffer;
	ctx.hybridOverlayBuffer = hybridOverlayBuffer;
	ctx.hybrid2DSourceBuffer = hybrid2DSourceBuffer;
	ctx.hybridOverlayTriBuffer = hybridOverlayTriBuffer;
	ctx.hybridOverlayTileBuffer = hybridOverlayTileBuffer;
	ctx.hybridOverlayTileIndexBuffer = hybridOverlayTileIndexBuffer;
	ctx.hybridWorldPositionBuffer = hybridWorldPositionBuffer;
	ctx.hybridLightBuffer = hybridLightBuffer;
	ctx.hybridLightTileBuffer = hybridLightTileBuffer;
	ctx.hybridLightIndexBuffer = hybridLightIndexBuffer;

	ctx.hybridDescriptorSet = hybridDescriptorSet;
	ctx.twoDDescriptorSet = twoDDescriptorSet;
	ctx.hybridUpscaleDescriptorSet = hybridUpscaleDescriptorSet;
	ctx.overlayDescriptorSet = overlayDescriptorSet;
	ctx.overlayTileDescriptorSet = overlayTileDescriptorSet;
}

void idSoftwareVulkanBridge::WaitForFrameContext( int index ) {
	index = idMath::ClampInt( 0, SW_VK_FRAMES_IN_FLIGHT - 1, index );
	swVkFrameContext_t &ctx = frameContexts[index];
	if ( device == VK_NULL_HANDLE || ctx.inFlightFence == VK_NULL_HANDLE ) {
		return;
	}
	if ( ctx.submitted ) {
		vkWaitForFences( device, 1, &ctx.inFlightFence, VK_TRUE, UINT64_MAX );
		ctx.submitted = false;
	}
	if ( ctx.timestampQueryPending ) {
		ReadTimestampQueries();
	}
}

void idSoftwareVulkanBridge::WaitForAllFrameContexts() {
	for ( int i = 0; i < SW_VK_FRAMES_IN_FLIGHT; i++ ) {
		WaitForFrameContext( i );
	}
}

void idSoftwareVulkanBridge::DestroyFrameContextBuffers( swVkFrameContext_t &ctx ) {
	DestroyBuffer( ctx.hybridLightBuffer );
	DestroyBuffer( ctx.hybridLightIndexBuffer );
	DestroyBuffer( ctx.hybridLightTileBuffer );
	DestroyBuffer( ctx.hybridWorldPositionBuffer );
	DestroyBuffer( ctx.hybridOverlayTileIndexBuffer );
	DestroyBuffer( ctx.hybridOverlayTileBuffer );
	DestroyBuffer( ctx.hybridOverlayTriBuffer );
	DestroyBuffer( ctx.hybrid2DSourceBuffer );
	DestroyBuffer( ctx.hybridOverlayBuffer );
	DestroyBuffer( ctx.hybridFrameBuffer );
	DestroyBuffer( ctx.hybridLitBuffer );
	DestroyBuffer( ctx.hybridSurfaceBuffer );
	DestroyBuffer( ctx.hybridSpecularBuffer );
	DestroyBuffer( ctx.hybridAlbedoBuffer );
	DestroyBuffer( ctx.hybridMaterialBuffer );
	DestroyBuffer( ctx.hybridUVBuffer );
	DestroyBuffer( ctx.hybridBitangentBuffer );
	DestroyBuffer( ctx.hybridTangentBuffer );
	DestroyBuffer( ctx.hybridNormalBuffer );
	DestroyBuffer( ctx.hybridDepthBuffer );
	if ( device != VK_NULL_HANDLE && ctx.uploadBuffer != VK_NULL_HANDLE ) {
		vkDestroyBuffer( device, ctx.uploadBuffer, NULL );
		ctx.uploadBuffer = VK_NULL_HANDLE;
	} else if ( device == VK_NULL_HANDLE ) {
		ctx.uploadBuffer = VK_NULL_HANDLE;
	}
	if ( device != VK_NULL_HANDLE && ctx.uploadMemory != VK_NULL_HANDLE ) {
		vkFreeMemory( device, ctx.uploadMemory, NULL );
		ctx.uploadMemory = VK_NULL_HANDLE;
	} else if ( device == VK_NULL_HANDLE ) {
		ctx.uploadMemory = VK_NULL_HANDLE;
	}
	ctx.uploadBufferSize = 0;
}

bool idSoftwareVulkanBridge::ImmediateSubmit( const VkCommandBufferBeginInfo &beginInfo ) {
	WaitForFrameContext( activeFrameContext );
	vkResetFences( device, 1, &inFlightFence );
	vkResetCommandBuffer( commandBuffer, 0 );

	if ( vkBeginCommandBuffer( commandBuffer, &beginInfo ) != VK_SUCCESS ) {
		LogFailure( "vkBeginCommandBuffer failed" );
		return false;
	}

	return true;
}

void idSoftwareVulkanBridge::WaitForSubmittedWork() {
	WaitForAllFrameContexts();
}

void idSoftwareVulkanBridge::DestroyTimestampQueries() {
	timestampQueryPending = false;
	timestampFrame.Clear();
	for ( int i = 0; i < SW_VK_FRAMES_IN_FLIGHT; i++ ) {
		frameContexts[i].timestampQueryPending = false;
		frameContexts[i].timestampFrame.Clear();
	}
	if ( device != VK_NULL_HANDLE && timestampQueryPool != VK_NULL_HANDLE ) {
		vkDestroyQueryPool( device, timestampQueryPool, NULL );
	}
	timestampQueryPool = VK_NULL_HANDLE;
}

void idSoftwareVulkanBridge::ReadTimestampQueries() {
	if ( !timestampQuerySupported || timestampQueryPool == VK_NULL_HANDLE || !timestampQueryPending ) {
		return;
	}

	bool anyPending = false;
	for ( int i = 0; i < SW_VK_FRAMES_IN_FLIGHT; i++ ) {
		swVkFrameContext_t &ctx = frameContexts[i];
		if ( !ctx.timestampQueryPending ) {
			continue;
		}
		if ( ctx.submitted && ctx.inFlightFence != VK_NULL_HANDLE && vkGetFenceStatus( device, ctx.inFlightFence ) == VK_NOT_READY ) {
			anyPending = true;
			continue;
		}

		uint64_t timestamps[SW_VK_GPU_TIMESTAMP_COUNT];
		memset( timestamps, 0, sizeof( timestamps ) );
		const uint32_t firstQuery = static_cast<uint32_t>( i * SW_VK_GPU_TIMESTAMP_COUNT );
		const VkResult result = vkGetQueryPoolResults( device, timestampQueryPool, firstQuery, SW_VK_GPU_TIMESTAMP_COUNT,
			sizeof( timestamps ), timestamps, sizeof( timestamps[0] ), VK_QUERY_RESULT_64_BIT );
		if ( result == VK_SUCCESS ) {
			SWVkPrintGpuTimestampStats( ctx.timestampFrame, timestamps, timestampPeriod, timestampValidBits );
			ctx.timestampQueryPending = false;
		} else {
			anyPending = true;
		}
	}
	timestampQueryPending = anyPending;
}

void idSoftwareVulkanBridge::WriteTimestamp( VkPipelineStageFlagBits stage, swVkGpuTimestampQuery_t query ) {
	if ( timestampQuerySupported && timestampQueryPool != VK_NULL_HANDLE && r_showSoftwarePerf.GetInteger() != 0 ) {
		const uint32_t queryIndex = static_cast<uint32_t>( activeFrameContext * SW_VK_GPU_TIMESTAMP_COUNT + query );
		vkCmdWriteTimestamp( commandBuffer, stage, timestampQueryPool, queryIndex );
	}
}

bool idSoftwareVulkanBridge::Create2DPipeline() {
	VkDescriptorSetLayoutBinding bindings[2];
	memset( bindings, 0, sizeof( bindings ) );
	for ( uint32_t i = 0; i < 2; i++ ) {
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}

	VkDescriptorSetLayoutCreateInfo layoutInfo;
	memset( &layoutInfo, 0, sizeof( layoutInfo ) );
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = 2;
	layoutInfo.pBindings = bindings;
	if ( vkCreateDescriptorSetLayout( device, &layoutInfo, NULL, &twoDDescriptorSetLayout ) != VK_SUCCESS ) {
		LogFailure( "vkCreateDescriptorSetLayout 2D failed" );
		return false;
	}

	VkPushConstantRange pushRange;
	memset( &pushRange, 0, sizeof( pushRange ) );
	pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof( swVk2DPushConstants_t );

	VkPipelineLayoutCreateInfo pipelineLayoutInfo;
	memset( &pipelineLayoutInfo, 0, sizeof( pipelineLayoutInfo ) );
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &twoDDescriptorSetLayout;
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &pushRange;
	if ( vkCreatePipelineLayout( device, &pipelineLayoutInfo, NULL, &twoDPipelineLayout ) != VK_SUCCESS ) {
		LogFailure( "vkCreatePipelineLayout 2D failed" );
		return false;
	}

	VkShaderModuleCreateInfo shaderInfo;
	memset( &shaderInfo, 0, sizeof( shaderInfo ) );
	shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shaderInfo.codeSize = sw2DCompositeCompSpvSize;
	shaderInfo.pCode = sw2DCompositeCompSpv;
	if ( vkCreateShaderModule( device, &shaderInfo, NULL, &twoDShaderModule ) != VK_SUCCESS ) {
		LogFailure( "vkCreateShaderModule 2D failed" );
		return false;
	}

	VkPipelineShaderStageCreateInfo stageInfo;
	memset( &stageInfo, 0, sizeof( stageInfo ) );
	stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stageInfo.module = twoDShaderModule;
	stageInfo.pName = "main";

	VkComputePipelineCreateInfo pipelineInfo;
	memset( &pipelineInfo, 0, sizeof( pipelineInfo ) );
	pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipelineInfo.stage = stageInfo;
	pipelineInfo.layout = twoDPipelineLayout;
	if ( vkCreateComputePipelines( device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &twoDPipeline ) != VK_SUCCESS ) {
		LogFailure( "vkCreateComputePipelines 2D failed" );
		return false;
	}

	VkDescriptorPoolSize poolSize;
	memset( &poolSize, 0, sizeof( poolSize ) );
	poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	poolSize.descriptorCount = 4 * SW_VK_FRAMES_IN_FLIGHT;

	VkDescriptorPoolCreateInfo poolInfo;
	memset( &poolInfo, 0, sizeof( poolInfo ) );
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = 2 * SW_VK_FRAMES_IN_FLIGHT;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes = &poolSize;
	if ( vkCreateDescriptorPool( device, &poolInfo, NULL, &twoDDescriptorPool ) != VK_SUCCESS ) {
		LogFailure( "vkCreateDescriptorPool 2D failed" );
		return false;
	}

	VkDescriptorSetAllocateInfo setInfo;
	memset( &setInfo, 0, sizeof( setInfo ) );
	setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	setInfo.descriptorPool = twoDDescriptorPool;
	VkDescriptorSetLayout setLayouts[2 * SW_VK_FRAMES_IN_FLIGHT];
	VkDescriptorSet descriptorSets[2 * SW_VK_FRAMES_IN_FLIGHT];
	for ( int i = 0; i < 2 * SW_VK_FRAMES_IN_FLIGHT; i++ ) {
		setLayouts[i] = twoDDescriptorSetLayout;
		descriptorSets[i] = VK_NULL_HANDLE;
	}
	setInfo.descriptorSetCount = 2 * SW_VK_FRAMES_IN_FLIGHT;
	setInfo.pSetLayouts = setLayouts;
	if ( vkAllocateDescriptorSets( device, &setInfo, descriptorSets ) != VK_SUCCESS ) {
		LogFailure( "vkAllocateDescriptorSets 2D failed" );
		return false;
	}
	for ( int i = 0; i < SW_VK_FRAMES_IN_FLIGHT; i++ ) {
		frameContexts[i].twoDDescriptorSet = descriptorSets[i * 2 + 0];
		frameContexts[i].hybridUpscaleDescriptorSet = descriptorSets[i * 2 + 1];
	}
	LoadFrameContext( activeFrameContext );
	return true;
}

bool idSoftwareVulkanBridge::CreateOverlayPipeline() {
	VkDescriptorSetLayoutBinding bindings[6];
	memset( bindings, 0, sizeof( bindings ) );
	for ( uint32_t i = 0; i < 6; i++ ) {
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}

	VkDescriptorSetLayoutCreateInfo layoutInfo;
	memset( &layoutInfo, 0, sizeof( layoutInfo ) );
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = 6;
	layoutInfo.pBindings = bindings;
	if ( vkCreateDescriptorSetLayout( device, &layoutInfo, NULL, &overlayDescriptorSetLayout ) != VK_SUCCESS ) {
		LogFailure( "vkCreateDescriptorSetLayout overlay failed" );
		return false;
	}

	VkPushConstantRange pushRange;
	memset( &pushRange, 0, sizeof( pushRange ) );
	pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof( swVkOverlayPushConstants_t );

	VkPipelineLayoutCreateInfo pipelineLayoutInfo;
	memset( &pipelineLayoutInfo, 0, sizeof( pipelineLayoutInfo ) );
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &overlayDescriptorSetLayout;
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &pushRange;
	if ( vkCreatePipelineLayout( device, &pipelineLayoutInfo, NULL, &overlayPipelineLayout ) != VK_SUCCESS ) {
		LogFailure( "vkCreatePipelineLayout overlay failed" );
		return false;
	}

	VkShaderModuleCreateInfo shaderInfo;
	memset( &shaderInfo, 0, sizeof( shaderInfo ) );
	shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shaderInfo.codeSize = swOverlayRasterCompSpvSize;
	shaderInfo.pCode = swOverlayRasterCompSpv;
	if ( vkCreateShaderModule( device, &shaderInfo, NULL, &overlayShaderModule ) != VK_SUCCESS ) {
		LogFailure( "vkCreateShaderModule overlay failed" );
		return false;
	}

	VkPipelineShaderStageCreateInfo stageInfo;
	memset( &stageInfo, 0, sizeof( stageInfo ) );
	stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stageInfo.module = overlayShaderModule;
	stageInfo.pName = "main";

	VkComputePipelineCreateInfo pipelineInfo;
	memset( &pipelineInfo, 0, sizeof( pipelineInfo ) );
	pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipelineInfo.stage = stageInfo;
	pipelineInfo.layout = overlayPipelineLayout;
	if ( vkCreateComputePipelines( device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &overlayPipeline ) != VK_SUCCESS ) {
		LogFailure( "vkCreateComputePipelines overlay failed" );
		return false;
	}

	VkDescriptorPoolSize poolSize;
	memset( &poolSize, 0, sizeof( poolSize ) );
	poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	poolSize.descriptorCount = 6 * SW_VK_FRAMES_IN_FLIGHT;

	VkDescriptorPoolCreateInfo poolInfo;
	memset( &poolInfo, 0, sizeof( poolInfo ) );
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = SW_VK_FRAMES_IN_FLIGHT;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes = &poolSize;
	if ( vkCreateDescriptorPool( device, &poolInfo, NULL, &overlayDescriptorPool ) != VK_SUCCESS ) {
		LogFailure( "vkCreateDescriptorPool overlay failed" );
		return false;
	}

	VkDescriptorSetAllocateInfo setInfo;
	memset( &setInfo, 0, sizeof( setInfo ) );
	setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	setInfo.descriptorPool = overlayDescriptorPool;
	VkDescriptorSetLayout setLayouts[SW_VK_FRAMES_IN_FLIGHT];
	VkDescriptorSet descriptorSets[SW_VK_FRAMES_IN_FLIGHT];
	for ( int i = 0; i < SW_VK_FRAMES_IN_FLIGHT; i++ ) {
		setLayouts[i] = overlayDescriptorSetLayout;
		descriptorSets[i] = VK_NULL_HANDLE;
	}
	setInfo.descriptorSetCount = SW_VK_FRAMES_IN_FLIGHT;
	setInfo.pSetLayouts = setLayouts;
	if ( vkAllocateDescriptorSets( device, &setInfo, descriptorSets ) != VK_SUCCESS ) {
		LogFailure( "vkAllocateDescriptorSets overlay failed" );
		return false;
	}
	for ( int i = 0; i < SW_VK_FRAMES_IN_FLIGHT; i++ ) {
		frameContexts[i].overlayDescriptorSet = descriptorSets[i];
	}
	LoadFrameContext( activeFrameContext );
	return true;
}

bool idSoftwareVulkanBridge::CreateOverlayTilePipeline() {
	VkDescriptorSetLayoutBinding bindings[8];
	memset( bindings, 0, sizeof( bindings ) );
	for ( uint32_t i = 0; i < 8; i++ ) {
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}

	VkDescriptorSetLayoutCreateInfo layoutInfo;
	memset( &layoutInfo, 0, sizeof( layoutInfo ) );
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = 8;
	layoutInfo.pBindings = bindings;
	if ( vkCreateDescriptorSetLayout( device, &layoutInfo, NULL, &overlayTileDescriptorSetLayout ) != VK_SUCCESS ) {
		return false;
	}

	VkPushConstantRange pushRange;
	memset( &pushRange, 0, sizeof( pushRange ) );
	pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof( swVkOverlayTilePushConstants_t );

	VkPipelineLayoutCreateInfo pipelineLayoutInfo;
	memset( &pipelineLayoutInfo, 0, sizeof( pipelineLayoutInfo ) );
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &overlayTileDescriptorSetLayout;
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &pushRange;
	if ( vkCreatePipelineLayout( device, &pipelineLayoutInfo, NULL, &overlayTilePipelineLayout ) != VK_SUCCESS ) {
		return false;
	}

	VkShaderModuleCreateInfo shaderInfo;
	memset( &shaderInfo, 0, sizeof( shaderInfo ) );
	shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shaderInfo.codeSize = sizeof( swOverlayTileCompositeCompSpv );
	shaderInfo.pCode = swOverlayTileCompositeCompSpv;
	if ( vkCreateShaderModule( device, &shaderInfo, NULL, &overlayTileShaderModule ) != VK_SUCCESS ) {
		return false;
	}

	VkPipelineShaderStageCreateInfo stageInfo;
	memset( &stageInfo, 0, sizeof( stageInfo ) );
	stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stageInfo.module = overlayTileShaderModule;
	stageInfo.pName = "main";

	VkComputePipelineCreateInfo pipelineInfo;
	memset( &pipelineInfo, 0, sizeof( pipelineInfo ) );
	pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipelineInfo.stage = stageInfo;
	pipelineInfo.layout = overlayTilePipelineLayout;
	if ( vkCreateComputePipelines( device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &overlayTilePipeline ) != VK_SUCCESS ) {
		return false;
	}

	VkDescriptorPoolSize poolSize;
	memset( &poolSize, 0, sizeof( poolSize ) );
	poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	poolSize.descriptorCount = 8 * SW_VK_FRAMES_IN_FLIGHT;

	VkDescriptorPoolCreateInfo poolInfo;
	memset( &poolInfo, 0, sizeof( poolInfo ) );
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = SW_VK_FRAMES_IN_FLIGHT;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes = &poolSize;
	if ( vkCreateDescriptorPool( device, &poolInfo, NULL, &overlayTileDescriptorPool ) != VK_SUCCESS ) {
		return false;
	}

	VkDescriptorSetAllocateInfo setInfo;
	memset( &setInfo, 0, sizeof( setInfo ) );
	setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	setInfo.descriptorPool = overlayTileDescriptorPool;
	VkDescriptorSetLayout setLayouts[SW_VK_FRAMES_IN_FLIGHT];
	VkDescriptorSet descriptorSets[SW_VK_FRAMES_IN_FLIGHT];
	for ( int i = 0; i < SW_VK_FRAMES_IN_FLIGHT; i++ ) {
		setLayouts[i] = overlayTileDescriptorSetLayout;
		descriptorSets[i] = VK_NULL_HANDLE;
	}
	setInfo.descriptorSetCount = SW_VK_FRAMES_IN_FLIGHT;
	setInfo.pSetLayouts = setLayouts;
	if ( vkAllocateDescriptorSets( device, &setInfo, descriptorSets ) != VK_SUCCESS ) {
		return false;
	}
	for ( int i = 0; i < SW_VK_FRAMES_IN_FLIGHT; i++ ) {
		frameContexts[i].overlayTileDescriptorSet = descriptorSets[i];
	}
	LoadFrameContext( activeFrameContext );
	return true;
}

bool idSoftwareVulkanBridge::CreateHybridPipeline() {
	VkDescriptorSetLayoutBinding bindings[18];
	memset( bindings, 0, sizeof( bindings ) );
	for ( uint32_t i = 0; i < 13; i++ ) {
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	bindings[13].binding = 13;
	bindings[13].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	bindings[13].descriptorCount = 1;
	bindings[13].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	for ( uint32_t i = 14; i < 18; i++ ) {
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}

	VkDescriptorSetLayoutCreateInfo layoutInfo;
	memset( &layoutInfo, 0, sizeof( layoutInfo ) );
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = 18;
	layoutInfo.pBindings = bindings;
	if ( vkCreateDescriptorSetLayout( device, &layoutInfo, NULL, &hybridDescriptorSetLayout ) != VK_SUCCESS ) {
		LogFailure( "vkCreateDescriptorSetLayout hybrid failed" );
		return false;
	}

	VkPushConstantRange pushRange;
	memset( &pushRange, 0, sizeof( pushRange ) );
	pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof( swVkHybridPushConstants_t );

	VkPipelineLayoutCreateInfo pipelineLayoutInfo;
	memset( &pipelineLayoutInfo, 0, sizeof( pipelineLayoutInfo ) );
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &hybridDescriptorSetLayout;
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &pushRange;
	if ( vkCreatePipelineLayout( device, &pipelineLayoutInfo, NULL, &hybridPipelineLayout ) != VK_SUCCESS ) {
		LogFailure( "vkCreatePipelineLayout hybrid failed" );
		return false;
	}

	for ( int i = 0; i < SW_VK_HYBRID_VARIANT_COUNT; i++ ) {
		const swVkHybridCompositeShader_t &shader = swVkHybridCompositeShaders[i];
		if ( shader.needsRayQuery && !rayQuerySupported ) {
			continue;
		}

		VkShaderModuleCreateInfo shaderInfo;
		memset( &shaderInfo, 0, sizeof( shaderInfo ) );
		shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		shaderInfo.codeSize = shader.codeSize;
		shaderInfo.pCode = shader.code;
		if ( vkCreateShaderModule( device, &shaderInfo, NULL, &hybridShaderModules[i] ) != VK_SUCCESS ) {
			LogFailure( "vkCreateShaderModule hybrid failed" );
			return false;
		}

		VkPipelineShaderStageCreateInfo stageInfo;
		memset( &stageInfo, 0, sizeof( stageInfo ) );
		stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		stageInfo.module = hybridShaderModules[i];
		stageInfo.pName = "main";

		VkComputePipelineCreateInfo pipelineInfo;
		memset( &pipelineInfo, 0, sizeof( pipelineInfo ) );
		pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
		pipelineInfo.stage = stageInfo;
		pipelineInfo.layout = hybridPipelineLayout;
		if ( vkCreateComputePipelines( device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &hybridPipelines[i] ) != VK_SUCCESS ) {
			LogFailure( "vkCreateComputePipelines hybrid failed" );
			return false;
		}
	}

	VkDescriptorPoolSize poolSizes[2];
	memset( poolSizes, 0, sizeof( poolSizes ) );
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	poolSizes[0].descriptorCount = 17 * SW_VK_FRAMES_IN_FLIGHT;
	poolSizes[1].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	poolSizes[1].descriptorCount = SW_VK_FRAMES_IN_FLIGHT;

	VkDescriptorPoolCreateInfo poolInfo;
	memset( &poolInfo, 0, sizeof( poolInfo ) );
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = SW_VK_FRAMES_IN_FLIGHT;
	poolInfo.poolSizeCount = 2;
	poolInfo.pPoolSizes = poolSizes;
	if ( vkCreateDescriptorPool( device, &poolInfo, NULL, &hybridDescriptorPool ) != VK_SUCCESS ) {
		LogFailure( "vkCreateDescriptorPool hybrid failed" );
		return false;
	}

	VkDescriptorSetAllocateInfo setInfo;
	memset( &setInfo, 0, sizeof( setInfo ) );
	setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	setInfo.descriptorPool = hybridDescriptorPool;
	VkDescriptorSetLayout setLayouts[SW_VK_FRAMES_IN_FLIGHT];
	VkDescriptorSet descriptorSets[SW_VK_FRAMES_IN_FLIGHT];
	for ( int i = 0; i < SW_VK_FRAMES_IN_FLIGHT; i++ ) {
		setLayouts[i] = hybridDescriptorSetLayout;
		descriptorSets[i] = VK_NULL_HANDLE;
	}
	setInfo.descriptorSetCount = SW_VK_FRAMES_IN_FLIGHT;
	setInfo.pSetLayouts = setLayouts;
	if ( vkAllocateDescriptorSets( device, &setInfo, descriptorSets ) != VK_SUCCESS ) {
		LogFailure( "vkAllocateDescriptorSets hybrid failed" );
		return false;
	}
	for ( int i = 0; i < SW_VK_FRAMES_IN_FLIGHT; i++ ) {
		frameContexts[i].hybridDescriptorSet = descriptorSets[i];
	}
	LoadFrameContext( activeFrameContext );
	return true;
}

bool idSoftwareVulkanBridge::CreateShadowPipeline() {
	if ( !rayQuerySupported ) {
		return true;
	}

	VkDescriptorSetLayoutBinding bindings[3];
	memset( bindings, 0, sizeof( bindings ) );
	bindings[0].binding = 0;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[1].binding = 1;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[2].binding = 2;
	bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[2].descriptorCount = 1;
	bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	VkDescriptorSetLayoutCreateInfo layoutInfo;
	memset( &layoutInfo, 0, sizeof( layoutInfo ) );
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = 3;
	layoutInfo.pBindings = bindings;
	if ( vkCreateDescriptorSetLayout( device, &layoutInfo, NULL, &shadowDescriptorSetLayout ) != VK_SUCCESS ) {
		LogFailure( "vkCreateDescriptorSetLayout failed" );
		return false;
	}

	VkPushConstantRange pushRange;
	memset( &pushRange, 0, sizeof( pushRange ) );
	pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof( swVkShadowPushConstants_t );

	VkPipelineLayoutCreateInfo pipelineLayoutInfo;
	memset( &pipelineLayoutInfo, 0, sizeof( pipelineLayoutInfo ) );
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &shadowDescriptorSetLayout;
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &pushRange;
	if ( vkCreatePipelineLayout( device, &pipelineLayoutInfo, NULL, &shadowPipelineLayout ) != VK_SUCCESS ) {
		LogFailure( "vkCreatePipelineLayout failed" );
		return false;
	}

	VkShaderModuleCreateInfo shaderInfo;
	memset( &shaderInfo, 0, sizeof( shaderInfo ) );
	shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shaderInfo.codeSize = swRayQueryShadowCompSpvSize;
	shaderInfo.pCode = swRayQueryShadowCompSpv;
	if ( vkCreateShaderModule( device, &shaderInfo, NULL, &shadowShaderModule ) != VK_SUCCESS ) {
		LogFailure( "vkCreateShaderModule failed" );
		return false;
	}

	VkPipelineShaderStageCreateInfo stageInfo;
	memset( &stageInfo, 0, sizeof( stageInfo ) );
	stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stageInfo.module = shadowShaderModule;
	stageInfo.pName = "main";

	VkComputePipelineCreateInfo pipelineInfo;
	memset( &pipelineInfo, 0, sizeof( pipelineInfo ) );
	pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipelineInfo.stage = stageInfo;
	pipelineInfo.layout = shadowPipelineLayout;
	if ( vkCreateComputePipelines( device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &shadowPipeline ) != VK_SUCCESS ) {
		LogFailure( "vkCreateComputePipelines failed" );
		return false;
	}

	VkDescriptorPoolSize poolSizes[2];
	memset( poolSizes, 0, sizeof( poolSizes ) );
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	poolSizes[0].descriptorCount = SW_VK_MAX_SHADOW_JOBS;
	poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	poolSizes[1].descriptorCount = SW_VK_MAX_SHADOW_JOBS * 2;

	VkDescriptorPoolCreateInfo poolInfo;
	memset( &poolInfo, 0, sizeof( poolInfo ) );
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = SW_VK_MAX_SHADOW_JOBS;
	poolInfo.poolSizeCount = 2;
	poolInfo.pPoolSizes = poolSizes;
	if ( vkCreateDescriptorPool( device, &poolInfo, NULL, &shadowDescriptorPool ) != VK_SUCCESS ) {
		LogFailure( "vkCreateDescriptorPool failed" );
		return false;
	}
	return true;
}

uint32_t idSoftwareVulkanBridge::FindMemoryType( uint32_t memoryTypeBits, VkMemoryPropertyFlags properties ) const {
	VkPhysicalDeviceMemoryProperties memoryProperties;
	vkGetPhysicalDeviceMemoryProperties( physicalDevice, &memoryProperties );

	for ( uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++ ) {
		if ( ( memoryTypeBits & ( 1u << i ) ) &&
			 ( memoryProperties.memoryTypes[i].propertyFlags & properties ) == properties ) {
			return i;
		}
	}
	return UINT32_MAX;
}

bool idSoftwareVulkanBridge::EnsureUploadBuffer( int requiredWidth, int requiredHeight ) {
	const VkDeviceSize requiredSize = static_cast<VkDeviceSize>( requiredWidth ) * static_cast<VkDeviceSize>( requiredHeight ) * sizeof( unsigned int );
	if ( uploadBuffer != VK_NULL_HANDLE && uploadBufferSize >= requiredSize ) {
		return true;
	}
	const VkDeviceSize capacitySize = SWVkGrowBufferSize( requiredSize, SW_VK_MIN_UPLOAD_BYTES );

	DestroyUploadBuffer();

	VkBufferCreateInfo bufferInfo;
	memset( &bufferInfo, 0, sizeof( bufferInfo ) );
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = capacitySize;
	bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	if ( vkCreateBuffer( device, &bufferInfo, NULL, &uploadBuffer ) != VK_SUCCESS ) {
		LogFailure( "vkCreateBuffer failed" );
		return false;
	}

	VkMemoryRequirements memoryRequirements;
	vkGetBufferMemoryRequirements( device, uploadBuffer, &memoryRequirements );

	const uint32_t memoryType = FindMemoryType( memoryRequirements.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT );
	if ( memoryType == UINT32_MAX ) {
		LogFailure( "no host-visible Vulkan upload memory type" );
		return false;
	}

	VkMemoryAllocateInfo allocInfo;
	memset( &allocInfo, 0, sizeof( allocInfo ) );
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memoryRequirements.size;
	allocInfo.memoryTypeIndex = memoryType;

	if ( vkAllocateMemory( device, &allocInfo, NULL, &uploadMemory ) != VK_SUCCESS ) {
		LogFailure( "vkAllocateMemory failed" );
		return false;
	}

	if ( vkBindBufferMemory( device, uploadBuffer, uploadMemory, 0 ) != VK_SUCCESS ) {
		LogFailure( "vkBindBufferMemory failed" );
		return false;
	}

	uploadBufferSize = capacitySize;
	StoreFrameContext();
	return true;
}

bool idSoftwareVulkanBridge::UploadBuffer( swVkBuffer_t &buffer, const void *data, VkDeviceSize size, const char *failureText ) {
	if ( !data || buffer.memory == VK_NULL_HANDLE || buffer.size < size ) {
		return false;
	}

	void *mapped = NULL;
	if ( vkMapMemory( device, buffer.memory, 0, size, 0, &mapped ) != VK_SUCCESS ) {
		LogFailure( failureText );
		return false;
	}
	memcpy( mapped, data, static_cast<size_t>( size ) );
	vkUnmapMemory( device, buffer.memory );
	return true;
}

bool idSoftwareVulkanBridge::UploadBufferRange( swVkBuffer_t &buffer, VkDeviceSize offset, const void *data, VkDeviceSize size, const char *failureText ) {
	if ( size == 0 ) {
		return true;
	}
	if ( !data || buffer.memory == VK_NULL_HANDLE || offset > buffer.size || size > buffer.size - offset ) {
		return false;
	}

	void *mapped = NULL;
	if ( vkMapMemory( device, buffer.memory, offset, size, 0, &mapped ) != VK_SUCCESS ) {
		LogFailure( failureText );
		return false;
	}
	memcpy( mapped, data, static_cast<size_t>( size ) );
	vkUnmapMemory( device, buffer.memory );
	return true;
}

bool idSoftwareVulkanBridge::Ensure2DBuffers( int sourcePixelCount ) {
	if ( sourcePixelCount <= 0 ) {
		return false;
	}
	const VkDeviceSize sourceSize = static_cast<VkDeviceSize>( sourcePixelCount ) * sizeof( uint32_t );
	const VkDeviceSize sourceCapacity = SWVkGrowBufferSize( sourceSize, SW_VK_MIN_2D_SOURCE_BYTES );
	const VkMemoryPropertyFlags hostMemory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	if ( hybrid2DSourceBuffer.buffer == VK_NULL_HANDLE || hybrid2DSourceBuffer.size < sourceSize ) {
		if ( !CreateBuffer( sourceCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostMemory, false, hybrid2DSourceBuffer ) ) {
			return false;
		}
	}
	StoreFrameContext();
	return true;
}

bool idSoftwareVulkanBridge::EnsureOverlayBuffers( int triCount ) {
	triCount = Max( 1, triCount );
	const VkDeviceSize triSize = static_cast<VkDeviceSize>( triCount ) * sizeof( swHybridOverlayTri_t );
	const VkDeviceSize triCapacity = SWVkGrowBufferSize( triSize, SW_VK_MIN_OVERLAY_TRI_BYTES );
	const VkMemoryPropertyFlags hostMemory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	if ( hybridOverlayTriBuffer.buffer == VK_NULL_HANDLE || hybridOverlayTriBuffer.size < triSize ) {
		if ( !CreateBuffer( triCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostMemory, false, hybridOverlayTriBuffer ) ) {
			return false;
		}
	}
	StoreFrameContext();
	return true;
}

bool idSoftwareVulkanBridge::EnsureOverlayTileBuffers( int tileCount, int tileIndexCount ) {
	tileCount = Max( 1, tileCount );
	tileIndexCount = Max( 1, tileIndexCount );
	const VkDeviceSize tileSize = static_cast<VkDeviceSize>( tileCount ) * sizeof( swVkOverlayTile_t );
	const VkDeviceSize tileIndexSize = static_cast<VkDeviceSize>( tileIndexCount ) * sizeof( uint32_t );
	const VkDeviceSize tileCapacity = SWVkGrowBufferSize( tileSize, SW_VK_MIN_OVERLAY_TILE_BYTES );
	const VkDeviceSize tileIndexCapacity = SWVkGrowBufferSize( tileIndexSize, SW_VK_MIN_OVERLAY_TILE_INDEX_BYTES );
	const VkMemoryPropertyFlags hostMemory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	if ( hybridOverlayTileBuffer.buffer == VK_NULL_HANDLE || hybridOverlayTileBuffer.size < tileSize ) {
		if ( !CreateBuffer( tileCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostMemory, false, hybridOverlayTileBuffer ) ) {
			return false;
		}
	}
	if ( hybridOverlayTileIndexBuffer.buffer == VK_NULL_HANDLE || hybridOverlayTileIndexBuffer.size < tileIndexSize ) {
		if ( !CreateBuffer( tileIndexCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostMemory, false, hybridOverlayTileIndexBuffer ) ) {
			return false;
		}
	}
	StoreFrameContext();
	return true;
}

bool idSoftwareVulkanBridge::EnsureHybridTextureBuffers( int textureInfoCount, int textureTexelCount, bool &texelBufferPreserved ) {
	textureInfoCount = Max( 1, textureInfoCount );
	textureTexelCount = Max( 1, textureTexelCount );
	texelBufferPreserved = true;

	const VkDeviceSize textureInfoSize = static_cast<VkDeviceSize>( textureInfoCount ) * sizeof( swHybridTextureInfo_t );
	const VkDeviceSize textureTexelSize = static_cast<VkDeviceSize>( textureTexelCount ) * sizeof( uint32_t );
	const VkDeviceSize textureInfoCapacity = SWVkGrowBufferSize( textureInfoSize, SW_VK_MIN_TEXTURE_INFO_BYTES );
	const VkDeviceSize textureTexelCapacity = SWVkGrowBufferSize( textureTexelSize, SW_VK_MIN_TEXTURE_TEXEL_BYTES );
	const VkMemoryPropertyFlags hostMemory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

	if ( hybridTextureInfoBuffer.buffer == VK_NULL_HANDLE || hybridTextureInfoBuffer.size < textureInfoSize ) {
		if ( !CreateBuffer( textureInfoCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostMemory, false, hybridTextureInfoBuffer ) ) {
			return false;
		}
	}
	if ( hybridTextureTexelBuffer.buffer == VK_NULL_HANDLE || hybridTextureTexelBuffer.size < textureTexelSize ) {
		texelBufferPreserved = false;
		if ( !CreateBuffer( textureTexelCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostMemory, false, hybridTextureTexelBuffer ) ) {
			return false;
		}
	}
	return true;
}

bool idSoftwareVulkanBridge::EnsureHybridBuffers( int requiredWidth, int requiredHeight, int textureInfoCount, int textureTexelCount, int lightCount, int lightTileCount, int lightIndexCount ) {
	if ( requiredWidth <= 0 || requiredHeight <= 0 || frameWidth <= 0 || frameHeight <= 0 ) {
		return false;
	}

	textureInfoCount = Max( 1, textureInfoCount );
	textureTexelCount = Max( 1, textureTexelCount );
	lightCount = Max( 1, lightCount );
	lightTileCount = Max( 1, lightTileCount );
	lightIndexCount = Max( 1, lightIndexCount );

	const VkDeviceSize pixelCount = static_cast<VkDeviceSize>( requiredWidth ) * static_cast<VkDeviceSize>( requiredHeight );
	const VkDeviceSize depthSize = pixelCount * sizeof( float );
	const VkDeviceSize uintSize = pixelCount * sizeof( uint32_t );
	const VkDeviceSize worldPositionSize = pixelCount * sizeof( idVec4 );
	const VkDeviceSize frameSize = static_cast<VkDeviceSize>( frameWidth ) * static_cast<VkDeviceSize>( frameHeight ) * sizeof( uint32_t );
	const VkDeviceSize lightSize = static_cast<VkDeviceSize>( lightCount ) * sizeof( swHybridLight_t );
	const VkDeviceSize lightTileSize = static_cast<VkDeviceSize>( lightTileCount ) * sizeof( swHybridLightTile_t );
	const VkDeviceSize lightIndexSize = static_cast<VkDeviceSize>( lightIndexCount ) * sizeof( uint32_t );
	const VkDeviceSize depthCapacity = SWVkGrowBufferSize( depthSize, SW_VK_MIN_HYBRID_PIXEL_BYTES );
	const VkDeviceSize uintCapacity = SWVkGrowBufferSize( uintSize, SW_VK_MIN_HYBRID_PIXEL_BYTES );
	const VkDeviceSize worldPositionCapacity = SWVkGrowBufferSize( worldPositionSize, SW_VK_MIN_HYBRID_WORLD_POSITION_BYTES );
	const VkDeviceSize frameCapacity = SWVkGrowBufferSize( frameSize, SW_VK_MIN_HYBRID_FRAME_BYTES );
	const VkDeviceSize lightCapacity = SWVkGrowBufferSize( lightSize, SW_VK_MIN_HYBRID_LIGHT_BYTES );
	const VkDeviceSize lightTileCapacity = SWVkGrowBufferSize( lightTileSize, SW_VK_MIN_HYBRID_LIGHT_TILE_BYTES );
	const VkDeviceSize lightIndexCapacity = SWVkGrowBufferSize( lightIndexSize, SW_VK_MIN_HYBRID_LIGHT_INDEX_BYTES );
	const VkMemoryPropertyFlags hostMemory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

	if ( hybridDepthBuffer.buffer == VK_NULL_HANDLE || hybridDepthBuffer.size < depthSize ) {
		if ( !CreateBuffer( depthCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostMemory, false, hybridDepthBuffer ) ) {
			return false;
		}
	}
	if ( hybridNormalBuffer.buffer == VK_NULL_HANDLE || hybridNormalBuffer.size < uintSize ) {
		if ( !CreateBuffer( uintCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostMemory, false, hybridNormalBuffer ) ) {
			return false;
		}
	}
	if ( hybridTangentBuffer.buffer == VK_NULL_HANDLE || hybridTangentBuffer.size < uintSize ) {
		if ( !CreateBuffer( uintCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostMemory, false, hybridTangentBuffer ) ) {
			return false;
		}
	}
	if ( hybridBitangentBuffer.buffer == VK_NULL_HANDLE || hybridBitangentBuffer.size < uintSize ) {
		if ( !CreateBuffer( uintCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostMemory, false, hybridBitangentBuffer ) ) {
			return false;
		}
	}
	if ( hybridUVBuffer.buffer == VK_NULL_HANDLE || hybridUVBuffer.size < uintSize ) {
		if ( !CreateBuffer( uintCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostMemory, false, hybridUVBuffer ) ) {
			return false;
		}
	}
	if ( hybridMaterialBuffer.buffer == VK_NULL_HANDLE || hybridMaterialBuffer.size < uintSize ) {
		if ( !CreateBuffer( uintCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostMemory, false, hybridMaterialBuffer ) ) {
			return false;
		}
	}
	if ( hybridAlbedoBuffer.buffer == VK_NULL_HANDLE || hybridAlbedoBuffer.size < uintSize ) {
		if ( !CreateBuffer( uintCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostMemory, false, hybridAlbedoBuffer ) ) {
			return false;
		}
	}
	if ( hybridSpecularBuffer.buffer == VK_NULL_HANDLE || hybridSpecularBuffer.size < uintSize ) {
		if ( !CreateBuffer( uintCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostMemory, false, hybridSpecularBuffer ) ) {
			return false;
		}
	}
	if ( hybridSurfaceBuffer.buffer == VK_NULL_HANDLE || hybridSurfaceBuffer.size < uintSize ) {
		if ( !CreateBuffer( uintCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostMemory, false, hybridSurfaceBuffer ) ) {
			return false;
		}
	}
	if ( hybridWorldPositionBuffer.buffer == VK_NULL_HANDLE || hybridWorldPositionBuffer.size < worldPositionSize ) {
		if ( !CreateBuffer( worldPositionCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostMemory, false, hybridWorldPositionBuffer ) ) {
			return false;
		}
	}
	if ( hybridLitBuffer.buffer == VK_NULL_HANDLE || hybridLitBuffer.size < uintSize ) {
		if ( !CreateBuffer( uintCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, hybridLitBuffer ) ) {
			return false;
		}
	}
	if ( hybridFrameBuffer.buffer == VK_NULL_HANDLE || hybridFrameBuffer.size < frameSize ) {
		if ( !CreateBuffer( frameCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, hybridFrameBuffer ) ) {
			return false;
		}
	}
	if ( hybridOverlayBuffer.buffer == VK_NULL_HANDLE || hybridOverlayBuffer.size < frameSize ) {
		if ( !CreateBuffer( frameCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, hybridOverlayBuffer ) ) {
			return false;
		}
	}
	bool texelBufferPreserved = true;
	if ( !EnsureHybridTextureBuffers( textureInfoCount, textureTexelCount, texelBufferPreserved ) ) {
		return false;
	}
	if ( hybridLightBuffer.buffer == VK_NULL_HANDLE || hybridLightBuffer.size < lightSize ) {
		if ( !CreateBuffer( lightCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostMemory, false, hybridLightBuffer ) ) {
			return false;
		}
	}
	if ( hybridLightTileBuffer.buffer == VK_NULL_HANDLE || hybridLightTileBuffer.size < lightTileSize ) {
		if ( !CreateBuffer( lightTileCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostMemory, false, hybridLightTileBuffer ) ) {
			return false;
		}
	}
	if ( hybridLightIndexBuffer.buffer == VK_NULL_HANDLE || hybridLightIndexBuffer.size < lightIndexSize ) {
		if ( !CreateBuffer( lightIndexCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostMemory, false, hybridLightIndexBuffer ) ) {
			return false;
		}
	}
	StoreFrameContext();
	return true;
}

void idSoftwareVulkanBridge::Update2DDescriptorSet( bool targetFrame ) {
	swVkBuffer_t &targetBuffer = targetFrame ? hybridFrameBuffer : hybridOverlayBuffer;
	if ( twoDDescriptorSet == VK_NULL_HANDLE || hybrid2DSourceBuffer.buffer == VK_NULL_HANDLE || targetBuffer.buffer == VK_NULL_HANDLE ) {
		return;
	}

	VkDescriptorBufferInfo infos[2];
	memset( infos, 0, sizeof( infos ) );
	infos[0].buffer = hybrid2DSourceBuffer.buffer;
	infos[0].range = hybrid2DSourceBuffer.size;
	infos[1].buffer = targetBuffer.buffer;
	infos[1].range = targetBuffer.size;

	VkWriteDescriptorSet writes[2];
	memset( writes, 0, sizeof( writes ) );
	for ( uint32_t i = 0; i < 2; i++ ) {
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = twoDDescriptorSet;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[i].pBufferInfo = &infos[i];
	}
	vkUpdateDescriptorSets( device, 2, writes, 0, NULL );
}

void idSoftwareVulkanBridge::UpdateOverlayDescriptorSet( bool targetFrame ) {
	swVkBuffer_t &targetBuffer = targetFrame ? hybridFrameBuffer : hybridOverlayBuffer;
	if ( overlayDescriptorSet == VK_NULL_HANDLE ||
		 hybridOverlayTriBuffer.buffer == VK_NULL_HANDLE ||
		 hybridDepthBuffer.buffer == VK_NULL_HANDLE ||
		 targetBuffer.buffer == VK_NULL_HANDLE ||
		 hybridTextureInfoBuffer.buffer == VK_NULL_HANDLE ||
		 hybridTextureTexelBuffer.buffer == VK_NULL_HANDLE ||
		 hybrid2DSourceBuffer.buffer == VK_NULL_HANDLE ) {
		return;
	}

	VkDescriptorBufferInfo infos[6];
	memset( infos, 0, sizeof( infos ) );
	infos[0].buffer = hybridOverlayTriBuffer.buffer;
	infos[0].range = hybridOverlayTriBuffer.size;
	infos[1].buffer = hybridDepthBuffer.buffer;
	infos[1].range = hybridDepthBuffer.size;
	infos[2].buffer = targetBuffer.buffer;
	infos[2].range = targetBuffer.size;
	infos[3].buffer = hybridTextureInfoBuffer.buffer;
	infos[3].range = hybridTextureInfoBuffer.size;
	infos[4].buffer = hybridTextureTexelBuffer.buffer;
	infos[4].range = hybridTextureTexelBuffer.size;
	infos[5].buffer = hybrid2DSourceBuffer.buffer;
	infos[5].range = hybrid2DSourceBuffer.size;

	VkWriteDescriptorSet writes[6];
	memset( writes, 0, sizeof( writes ) );
	for ( uint32_t i = 0; i < 6; i++ ) {
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = overlayDescriptorSet;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[i].pBufferInfo = &infos[i];
	}
	vkUpdateDescriptorSets( device, 6, writes, 0, NULL );
}

void idSoftwareVulkanBridge::UpdateOverlayTileDescriptorSet( bool targetFrame ) {
	swVkBuffer_t &targetBuffer = targetFrame ? hybridFrameBuffer : hybridOverlayBuffer;
	if ( overlayTileDescriptorSet == VK_NULL_HANDLE ||
		 hybridOverlayTriBuffer.buffer == VK_NULL_HANDLE ||
		 hybridDepthBuffer.buffer == VK_NULL_HANDLE ||
		 targetBuffer.buffer == VK_NULL_HANDLE ||
		 hybridTextureInfoBuffer.buffer == VK_NULL_HANDLE ||
		 hybridTextureTexelBuffer.buffer == VK_NULL_HANDLE ||
		 hybrid2DSourceBuffer.buffer == VK_NULL_HANDLE ||
		 hybridOverlayTileBuffer.buffer == VK_NULL_HANDLE ||
		 hybridOverlayTileIndexBuffer.buffer == VK_NULL_HANDLE ) {
		return;
	}

	VkDescriptorBufferInfo infos[8];
	memset( infos, 0, sizeof( infos ) );
	infos[0].buffer = hybridOverlayTriBuffer.buffer;
	infos[0].range = hybridOverlayTriBuffer.size;
	infos[1].buffer = hybridDepthBuffer.buffer;
	infos[1].range = hybridDepthBuffer.size;
	infos[2].buffer = targetBuffer.buffer;
	infos[2].range = targetBuffer.size;
	infos[3].buffer = hybridTextureInfoBuffer.buffer;
	infos[3].range = hybridTextureInfoBuffer.size;
	infos[4].buffer = hybridTextureTexelBuffer.buffer;
	infos[4].range = hybridTextureTexelBuffer.size;
	infos[5].buffer = hybrid2DSourceBuffer.buffer;
	infos[5].range = hybrid2DSourceBuffer.size;
	infos[6].buffer = hybridOverlayTileBuffer.buffer;
	infos[6].range = hybridOverlayTileBuffer.size;
	infos[7].buffer = hybridOverlayTileIndexBuffer.buffer;
	infos[7].range = hybridOverlayTileIndexBuffer.size;

	VkWriteDescriptorSet writes[8];
	memset( writes, 0, sizeof( writes ) );
	for ( uint32_t i = 0; i < 8; i++ ) {
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = overlayTileDescriptorSet;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[i].pBufferInfo = &infos[i];
	}
	vkUpdateDescriptorSets( device, 8, writes, 0, NULL );
}

void idSoftwareVulkanBridge::UpdateHybridDescriptorSet() {
	VkDescriptorBufferInfo infos[17];
	memset( infos, 0, sizeof( infos ) );
	infos[0].buffer = hybridDepthBuffer.buffer;
	infos[0].range = hybridDepthBuffer.size;
	infos[1].buffer = hybridNormalBuffer.buffer;
	infos[1].range = hybridNormalBuffer.size;
	infos[2].buffer = hybridUVBuffer.buffer;
	infos[2].range = hybridUVBuffer.size;
	infos[3].buffer = hybridMaterialBuffer.buffer;
	infos[3].range = hybridMaterialBuffer.size;
	infos[4].buffer = hybridAlbedoBuffer.buffer;
	infos[4].range = hybridAlbedoBuffer.size;
	infos[5].buffer = hybridSpecularBuffer.buffer;
	infos[5].range = hybridSpecularBuffer.size;
	infos[6].buffer = hybridSurfaceBuffer.buffer;
	infos[6].range = hybridSurfaceBuffer.size;
	infos[7].buffer = hybridLitBuffer.buffer;
	infos[7].range = hybridLitBuffer.size;
	infos[8].buffer = hybridOverlayBuffer.buffer;
	infos[8].range = hybridOverlayBuffer.size;
	infos[9].buffer = hybridTextureInfoBuffer.buffer;
	infos[9].range = hybridTextureInfoBuffer.size;
	infos[10].buffer = hybridTextureTexelBuffer.buffer;
	infos[10].range = hybridTextureTexelBuffer.size;
	infos[11].buffer = hybridWorldPositionBuffer.buffer;
	infos[11].range = hybridWorldPositionBuffer.size;
	infos[12].buffer = hybridLightBuffer.buffer;
	infos[12].range = hybridLightBuffer.size;
	infos[13].buffer = hybridTangentBuffer.buffer;
	infos[13].range = hybridTangentBuffer.size;
	infos[14].buffer = hybridBitangentBuffer.buffer;
	infos[14].range = hybridBitangentBuffer.size;
	infos[15].buffer = hybridLightTileBuffer.buffer;
	infos[15].range = hybridLightTileBuffer.size;
	infos[16].buffer = hybridLightIndexBuffer.buffer;
	infos[16].range = hybridLightIndexBuffer.size;

	const uint32_t bindingMap[17] = {
		0, 1, 2, 3, 4, 5, 6, 7,
		8, 9, 10, 11, 12, 14, 15, 16, 17
	};

	VkWriteDescriptorSet writes[17];
	memset( writes, 0, sizeof( writes ) );
	for ( uint32_t i = 0; i < 17; i++ ) {
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = hybridDescriptorSet;
		writes[i].dstBinding = bindingMap[i];
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[i].pBufferInfo = &infos[i];
	}
	vkUpdateDescriptorSets( device, 17, writes, 0, NULL );

	if ( tlas != VK_NULL_HANDLE ) {
		VkWriteDescriptorSetAccelerationStructureKHR asInfo;
		memset( &asInfo, 0, sizeof( asInfo ) );
		asInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
		asInfo.accelerationStructureCount = 1;
		asInfo.pAccelerationStructures = &tlas;

		VkWriteDescriptorSet asWrite;
		memset( &asWrite, 0, sizeof( asWrite ) );
		asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		asWrite.pNext = &asInfo;
		asWrite.dstSet = hybridDescriptorSet;
		asWrite.dstBinding = 13;
		asWrite.descriptorCount = 1;
		asWrite.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
		vkUpdateDescriptorSets( device, 1, &asWrite, 0, NULL );
	}
}

void idSoftwareVulkanBridge::UpdateHybridUpscaleDescriptorSet() {
	if ( hybridUpscaleDescriptorSet == VK_NULL_HANDLE ||
		 hybridLitBuffer.buffer == VK_NULL_HANDLE ||
		 hybridFrameBuffer.buffer == VK_NULL_HANDLE ) {
		return;
	}

	VkDescriptorBufferInfo infos[2];
	memset( infos, 0, sizeof( infos ) );
	infos[0].buffer = hybridLitBuffer.buffer;
	infos[0].range = hybridLitBuffer.size;
	infos[1].buffer = hybridFrameBuffer.buffer;
	infos[1].range = hybridFrameBuffer.size;

	VkWriteDescriptorSet writes[2];
	memset( writes, 0, sizeof( writes ) );
	for ( uint32_t i = 0; i < 2; i++ ) {
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = hybridUpscaleDescriptorSet;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[i].pBufferInfo = &infos[i];
	}
	vkUpdateDescriptorSets( device, 2, writes, 0, NULL );
}

swVkHybridCompositeVariant_t idSoftwareVulkanBridge::SelectHybridCompositeVariant( const swHybridGBufferUpload_t &gbuffer, bool shadowsActive ) const {
	switch ( idMath::ClampInt( 0, 6, gbuffer.debugView ) ) {
		case 1: return SW_VK_HYBRID_VARIANT_DEBUG_DEPTH;
		case 2: return SW_VK_HYBRID_VARIANT_DEBUG_NORMAL;
		case 3: return SW_VK_HYBRID_VARIANT_DEBUG_UV;
		case 4: return SW_VK_HYBRID_VARIANT_DEBUG_MATERIAL;
		case 5: return SW_VK_HYBRID_VARIANT_DEBUG_ALBEDO;
		case 6: return SW_VK_HYBRID_VARIANT_DEBUG_SURFACE;
		default: break;
	}

	const int lightCount = Max( 0, gbuffer.lightCount );
	if ( lightCount <= 0 || !gbuffer.lights ) {
		return SW_VK_HYBRID_VARIANT_NO_LIGHTS;
	}

	const int normalCount = idMath::ClampInt( 0, lightCount, gbuffer.normalLightCount );
	const int postCount = idMath::ClampInt( 0, lightCount - normalCount, gbuffer.postLightCount );
	bool hasAmbientNormal = false;
	bool hasProjectedNormal = false;
	bool hasShadowedNormal = false;

	for ( int i = 0; i < normalCount; i++ ) {
		const swHybridLight_t &light = gbuffer.lights[i];
		if ( light.flags[0] != 0u ) {
			hasAmbientNormal = true;
		} else {
			hasProjectedNormal = true;
			if ( light.flags[2] != 0u ) {
				hasShadowedNormal = true;
			}
		}
	}

	const bool useShadowVariant = shadowsActive && hasShadowedNormal && rayQuerySupported;

	if ( postCount <= 0 ) {
		if ( normalCount <= 0 ) {
			return SW_VK_HYBRID_VARIANT_NO_LIGHTS;
		}
		if ( hasAmbientNormal && !hasProjectedNormal ) {
			return SW_VK_HYBRID_VARIANT_AMBIENT_ONLY;
		}
		if ( hasProjectedNormal && !hasAmbientNormal ) {
			return useShadowVariant ? SW_VK_HYBRID_VARIANT_PROJECTED_SHADOW : SW_VK_HYBRID_VARIANT_PROJECTED_NOSHADOW;
		}
		return useShadowVariant ? SW_VK_HYBRID_VARIANT_GENERIC_SHADOW : SW_VK_HYBRID_VARIANT_GENERIC_NOSHADOW;
	}

	bool postAllFog = true;
	bool postAllBlend = true;
	for ( int i = normalCount; i < normalCount + postCount; i++ ) {
		const unsigned int type = gbuffer.lights[i].flags[3];
		postAllFog = postAllFog && type == SW_VK_HYBRID_LIGHT_TYPE_FOG;
		postAllBlend = postAllBlend && type == SW_VK_HYBRID_LIGHT_TYPE_BLEND;
	}

	if ( postAllFog ) {
		return useShadowVariant ? SW_VK_HYBRID_VARIANT_POST_FOG_SHADOW : SW_VK_HYBRID_VARIANT_POST_FOG_NOSHADOW;
	}
	if ( postAllBlend ) {
		return useShadowVariant ? SW_VK_HYBRID_VARIANT_POST_BLEND_SHADOW : SW_VK_HYBRID_VARIANT_POST_BLEND_NOSHADOW;
	}
	return useShadowVariant ? SW_VK_HYBRID_VARIANT_GENERIC_SHADOW : SW_VK_HYBRID_VARIANT_GENERIC_NOSHADOW;
}

bool idSoftwareVulkanBridge::EnsureShadowBuffers( int width, int height ) {
	if ( !rayQuerySupported || width <= 0 || height <= 0 ) {
		return false;
	}

	const VkDeviceSize positionSize = static_cast<VkDeviceSize>( width ) * static_cast<VkDeviceSize>( height ) * sizeof( idVec4 );
	const VkDeviceSize maskSize = static_cast<VkDeviceSize>( width ) * static_cast<VkDeviceSize>( height ) * sizeof( uint32_t );
	const VkDeviceSize positionCapacity = SWVkGrowBufferSize( positionSize, SW_VK_MIN_HYBRID_WORLD_POSITION_BYTES );
	const VkDeviceSize maskCapacity = SWVkGrowBufferSize( maskSize, SW_VK_MIN_HYBRID_PIXEL_BYTES );

	if ( worldPositionBuffer.buffer == VK_NULL_HANDLE || worldPositionBuffer.size < positionSize ) {
		if ( !CreateBuffer( positionCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false, worldPositionBuffer ) ) {
			return false;
		}
	}
	if ( shadowMaskBuffer.buffer == VK_NULL_HANDLE || shadowMaskBuffer.size < maskSize ) {
		if ( !CreateBuffer( maskCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false, shadowMaskBuffer ) ) {
			return false;
		}
	}
	return true;
}

uintptr_t idSoftwareVulkanBridge::ShadowLightKey( const viewLight_t *vLight ) const {
	if ( !vLight ) {
		return 0;
	}
	if ( vLight->lightDef ) {
		return reinterpret_cast<uintptr_t>( vLight->lightDef );
	}

	const int x = idMath::Ftoi( vLight->globalLightOrigin[0] * 8.0f );
	const int y = idMath::Ftoi( vLight->globalLightOrigin[1] * 8.0f );
	const int z = idMath::Ftoi( vLight->globalLightOrigin[2] * 8.0f );
	return ( static_cast<uintptr_t>( x & 0xffff ) << 32 ) ^
		( static_cast<uintptr_t>( y & 0xffff ) << 16 ) ^
		static_cast<uintptr_t>( z & 0xffff );
}

bool idSoftwareVulkanBridge::HasPendingShadowJobs() const {
	for ( int i = 0; i < SW_VK_MAX_SHADOW_JOBS; i++ ) {
		const swVkShadowJob_t &job = shadowJobs[i];
		if ( job.pendingSubmit ) {
			return true;
		}
		if ( job.submitted && job.fence != VK_NULL_HANDLE && vkGetFenceStatus( device, job.fence ) == VK_NOT_READY ) {
			return true;
		}
	}
	return false;
}

bool idSoftwareVulkanBridge::EnsureShadowJobResources( swVkShadowJob_t &job, int width, int height ) {
	if ( !rayQuerySupported || width <= 0 || height <= 0 ) {
		return false;
	}

	if ( job.commandBuffer == VK_NULL_HANDLE ) {
		VkCommandBufferAllocateInfo allocInfo;
		memset( &allocInfo, 0, sizeof( allocInfo ) );
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = commandPool;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 1;
		if ( vkAllocateCommandBuffers( device, &allocInfo, &job.commandBuffer ) != VK_SUCCESS ) {
			LogFailure( "vkAllocateCommandBuffers shadow failed" );
			return false;
		}
	}

	if ( job.fence == VK_NULL_HANDLE ) {
		VkFenceCreateInfo fenceInfo;
		memset( &fenceInfo, 0, sizeof( fenceInfo ) );
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		if ( vkCreateFence( device, &fenceInfo, NULL, &job.fence ) != VK_SUCCESS ) {
			LogFailure( "vkCreateFence shadow failed" );
			return false;
		}
	}

	if ( job.descriptorSet == VK_NULL_HANDLE ) {
		VkDescriptorSetAllocateInfo setInfo;
		memset( &setInfo, 0, sizeof( setInfo ) );
		setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		setInfo.descriptorPool = shadowDescriptorPool;
		setInfo.descriptorSetCount = 1;
		setInfo.pSetLayouts = &shadowDescriptorSetLayout;
		if ( vkAllocateDescriptorSets( device, &setInfo, &job.descriptorSet ) != VK_SUCCESS ) {
			LogFailure( "vkAllocateDescriptorSets shadow failed" );
			return false;
		}
	}

	const VkDeviceSize positionSize = static_cast<VkDeviceSize>( width ) * static_cast<VkDeviceSize>( height ) * sizeof( idVec4 );
	const VkDeviceSize maskSize = static_cast<VkDeviceSize>( width ) * static_cast<VkDeviceSize>( height ) * sizeof( uint32_t );
	const VkDeviceSize positionCapacity = SWVkGrowBufferSize( positionSize, SW_VK_MIN_HYBRID_WORLD_POSITION_BYTES );
	const VkDeviceSize maskCapacity = SWVkGrowBufferSize( maskSize, SW_VK_MIN_HYBRID_PIXEL_BYTES );

	if ( job.worldPositionBuffer.buffer == VK_NULL_HANDLE || job.worldPositionBuffer.size < positionSize ) {
		if ( !CreateBuffer( positionCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false, job.worldPositionBuffer ) ) {
			return false;
		}
	}
	if ( job.shadowMaskBuffer.buffer == VK_NULL_HANDLE || job.shadowMaskBuffer.size < maskSize ) {
		if ( !CreateBuffer( maskCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false, job.shadowMaskBuffer ) ) {
			return false;
		}
	}
	return true;
}

swVkShadowJob_t *idSoftwareVulkanBridge::AllocShadowJob() {
	for ( int i = 0; i < SW_VK_MAX_SHADOW_JOBS; i++ ) {
		swVkShadowJob_t &job = shadowJobs[i];
		if ( job.pendingSubmit ) {
			continue;
		}
		if ( !job.submitted || job.fence == VK_NULL_HANDLE || vkGetFenceStatus( device, job.fence ) == VK_SUCCESS ) {
			job.submitted = false;
			return &job;
		}
	}
	return NULL;
}

bool idSoftwareVulkanBridge::RecordShadowJob( swVkShadowJob_t &job, const viewLight_t *vLight, const idVec4 *worldPositions, int width, int height ) {
	if ( !vLight || !worldPositions || !rayQuerySceneReady || !tlas || !shadowPipeline ) {
		return false;
	}
	if ( !EnsureShadowJobResources( job, width, height ) ) {
		return false;
	}

	const int pixelCount = width * height;
	const VkDeviceSize positionSize = static_cast<VkDeviceSize>( pixelCount ) * sizeof( idVec4 );
	const VkDeviceSize maskSize = static_cast<VkDeviceSize>( pixelCount ) * sizeof( uint32_t );

	void *mapped = NULL;
	if ( vkMapMemory( device, job.worldPositionBuffer.memory, 0, positionSize, 0, &mapped ) != VK_SUCCESS ) {
		LogFailure( "vkMapMemory world positions failed" );
		return false;
	}
	memcpy( mapped, worldPositions, positionSize );
	vkUnmapMemory( device, job.worldPositionBuffer.memory );

	VkWriteDescriptorSetAccelerationStructureKHR asInfo;
	memset( &asInfo, 0, sizeof( asInfo ) );
	asInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
	asInfo.accelerationStructureCount = 1;
	asInfo.pAccelerationStructures = &tlas;

	VkDescriptorBufferInfo positionInfo;
	memset( &positionInfo, 0, sizeof( positionInfo ) );
	positionInfo.buffer = job.worldPositionBuffer.buffer;
	positionInfo.offset = 0;
	positionInfo.range = positionSize;

	VkDescriptorBufferInfo maskInfo;
	memset( &maskInfo, 0, sizeof( maskInfo ) );
	maskInfo.buffer = job.shadowMaskBuffer.buffer;
	maskInfo.offset = 0;
	maskInfo.range = maskSize;

	VkWriteDescriptorSet writes[3];
	memset( writes, 0, sizeof( writes ) );
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].pNext = &asInfo;
	writes[0].dstSet = job.descriptorSet;
	writes[0].dstBinding = 0;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[1].dstSet = job.descriptorSet;
	writes[1].dstBinding = 1;
	writes[1].descriptorCount = 1;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[1].pBufferInfo = &positionInfo;
	writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[2].dstSet = job.descriptorSet;
	writes[2].dstBinding = 2;
	writes[2].descriptorCount = 1;
	writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[2].pBufferInfo = &maskInfo;
	vkUpdateDescriptorSets( device, 3, writes, 0, NULL );

	swVkShadowPushConstants_t push;
	memset( &push, 0, sizeof( push ) );
	push.lightOrigin[0] = vLight->globalLightOrigin[0];
	push.lightOrigin[1] = vLight->globalLightOrigin[1];
	push.lightOrigin[2] = vLight->globalLightOrigin[2];
	push.lightOrigin[3] = 1.0f;
	push.pixelCount = pixelCount;
	push.sampleCount = static_cast<uint32_t>( idMath::ClampInt( 1, 9, r_softwareRayQueryShadowSamples.GetInteger() ) );
	push.bias = 2.0f;
	push.radius = Max( 0.0f, r_softwareRayQueryShadowRadius.GetFloat() );

	vkResetCommandBuffer( job.commandBuffer, 0 );

	VkCommandBufferBeginInfo beginInfo;
	memset( &beginInfo, 0, sizeof( beginInfo ) );
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if ( vkBeginCommandBuffer( job.commandBuffer, &beginInfo ) != VK_SUCCESS ) {
		LogFailure( "vkBeginCommandBuffer shadow failed" );
		return false;
	}

	vkCmdBindPipeline( job.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, shadowPipeline );
	vkCmdBindDescriptorSets( job.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, shadowPipelineLayout, 0, 1, &job.descriptorSet, 0, NULL );
	vkCmdPushConstants( job.commandBuffer, shadowPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( push ), &push );
	vkCmdDispatch( job.commandBuffer, SWVkDispatchGroups( static_cast<uint32_t>( pixelCount ), SW_VK_SHADOW_GROUP_SIZE_X ), 1, 1 );

	VkMemoryBarrier readbackBarrier;
	memset( &readbackBarrier, 0, sizeof( readbackBarrier ) );
	readbackBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	readbackBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	readbackBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier( job.commandBuffer,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_HOST_BIT,
		0, 1, &readbackBarrier, 0, NULL, 0, NULL );

	if ( vkEndCommandBuffer( job.commandBuffer ) != VK_SUCCESS ) {
		LogFailure( "vkEndCommandBuffer shadow failed" );
		return false;
	}

	job.lightKey = ShadowLightKey( vLight );
	job.width = width;
	job.height = height;
	job.pendingSubmit = true;
	job.submitted = false;
	return true;
}

void idSoftwareVulkanBridge::DestroyShadowJobs() {
	for ( int i = 0; i < SW_VK_MAX_SHADOW_JOBS; i++ ) {
		swVkShadowJob_t &job = shadowJobs[i];

		if ( job.commandBuffer != VK_NULL_HANDLE && commandPool != VK_NULL_HANDLE ) {
			vkFreeCommandBuffers( device, commandPool, 1, &job.commandBuffer );
			job.commandBuffer = VK_NULL_HANDLE;
		}
		if ( job.fence != VK_NULL_HANDLE ) {
			vkDestroyFence( device, job.fence, NULL );
			job.fence = VK_NULL_HANDLE;
		}

		DestroyBuffer( job.worldPositionBuffer );
		DestroyBuffer( job.shadowMaskBuffer );

		job.descriptorSet = VK_NULL_HANDLE;
		job.lightKey = 0;
		job.width = 0;
		job.height = 0;
		job.pendingSubmit = false;
		job.submitted = false;
	}
}


bool idSoftwareVulkanBridge::EnsureFrameBuffer() {
	if ( frameWidth <= 0 || frameHeight <= 0 ) {
		return false;
	}

	const int neededPixels = frameWidth * frameHeight;
	if ( framePixels.Num() != neededPixels ) {
		framePixels.SetNum( neededPixels, false );
		frameBegun = false;
	}
	return true;
}

void idSoftwareVulkanBridge::DestroySurfaceBlas( swVkSurfaceBlas_t &entry ) {
	if ( entry.owner && entry.owner->rayQueryBlas == &entry ) {
		entry.owner->rayQueryBlas = NULL;
	}
	if ( entry.blas != VK_NULL_HANDLE ) {
		vkDestroyAccelerationStructureKHRLocal( device, entry.blas, NULL );
		entry.blas = VK_NULL_HANDLE;
	}
	DestroyBuffer( entry.blasBuffer );
	DestroyBuffer( entry.indexBuffer );
	DestroyBuffer( entry.vertexBuffer );
	entry.owner = NULL;
	entry.key = NULL;
	entry.verts = NULL;
	entry.indexes = NULL;
	entry.vertCount = 0;
	entry.indexCount = 0;
	entry.primitiveCount = 0;
	entry.geometryGeneration = 0;
	entry.updatable = false;
	entry.blasAddress = 0;
}

bool idSoftwareVulkanBridge::BuildSurfaceBlas( swVkSurfaceBlas_t &entry, srfTriangles_t *geo ) {
	if ( !rayQuerySupported || !geo || !geo->verts || !geo->indexes || geo->numVerts <= 0 || geo->numIndexes < 3 ) {
		return false;
	}

	idList<swVkShadowVertex_t> vertices;
	vertices.SetNum( geo->numVerts, false );
	for ( int i = 0; i < geo->numVerts; i++ ) {
		vertices[i].xyz[0] = geo->verts[i].xyz[0];
		vertices[i].xyz[1] = geo->verts[i].xyz[1];
		vertices[i].xyz[2] = geo->verts[i].xyz[2];
		vertices[i].pad = 0.0f;
	}

	idList<uint32_t> indexes;
	indexes.SetNum( geo->numIndexes, false );
	int validIndexCount = 0;
	for ( int index = 0; index + 2 < geo->numIndexes; index += 3 ) {
		const int i0 = geo->indexes[index + 0];
		const int i1 = geo->indexes[index + 1];
		const int i2 = geo->indexes[index + 2];
		if ( i0 < 0 || i0 >= geo->numVerts ||
			 i1 < 0 || i1 >= geo->numVerts ||
			 i2 < 0 || i2 >= geo->numVerts ) {
			continue;
		}
		indexes[validIndexCount++] = static_cast<uint32_t>( i0 );
		indexes[validIndexCount++] = static_cast<uint32_t>( i1 );
		indexes[validIndexCount++] = static_cast<uint32_t>( i2 );
	}
	indexes.SetNum( validIndexCount, false );
	if ( indexes.Num() < 3 ) {
		return false;
	}

	const VkDeviceSize vertexSize = static_cast<VkDeviceSize>( vertices.Num() ) * sizeof( vertices[0] );
	const VkDeviceSize indexSize = static_cast<VkDeviceSize>( indexes.Num() ) * sizeof( indexes[0] );
	const uint32_t primitiveCount = indexes.Num() / 3;
	const bool updateBlas = entry.updatable &&
		entry.blas != VK_NULL_HANDLE &&
		entry.blasBuffer.buffer != VK_NULL_HANDLE &&
		entry.blasAddress != 0 &&
		entry.vertCount == geo->numVerts &&
		entry.indexCount == geo->numIndexes &&
		entry.primitiveCount == primitiveCount &&
		entry.vertexBuffer.buffer != VK_NULL_HANDLE &&
		entry.vertexBuffer.size >= vertexSize &&
		entry.indexBuffer.buffer != VK_NULL_HANDLE &&
		entry.indexBuffer.size >= indexSize;

	if ( entry.vertexBuffer.buffer == VK_NULL_HANDLE || entry.vertexBuffer.size < vertexSize ) {
		if ( !CreateBuffer( vertexSize,
				VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				true,
				entry.vertexBuffer ) ) {
			if ( !updateBlas ) {
				DestroySurfaceBlas( entry );
			}
			return false;
		}
	}
	if ( entry.indexBuffer.buffer == VK_NULL_HANDLE || entry.indexBuffer.size < indexSize ) {
		if ( !CreateBuffer( indexSize,
				VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				true,
				entry.indexBuffer ) ) {
			if ( !updateBlas ) {
				DestroySurfaceBlas( entry );
			}
			return false;
		}
	}
	if ( !UploadBuffer( entry.vertexBuffer, vertices.Ptr(), vertexSize, "vkMapMemory surface BLAS vertices failed" ) ||
		 !UploadBuffer( entry.indexBuffer, indexes.Ptr(), indexSize, "vkMapMemory surface BLAS indexes failed" ) ) {
		if ( !updateBlas ) {
			DestroySurfaceBlas( entry );
		}
		return false;
	}

	VkAccelerationStructureGeometryTrianglesDataKHR triangles;
	memset( &triangles, 0, sizeof( triangles ) );
	triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	triangles.vertexData.deviceAddress = GetBufferAddress( entry.vertexBuffer.buffer );
	triangles.vertexStride = sizeof( swVkShadowVertex_t );
	triangles.maxVertex = vertices.Num() - 1;
	triangles.indexType = VK_INDEX_TYPE_UINT32;
	triangles.indexData.deviceAddress = GetBufferAddress( entry.indexBuffer.buffer );

	VkAccelerationStructureGeometryKHR blasGeometry;
	memset( &blasGeometry, 0, sizeof( blasGeometry ) );
	blasGeometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	blasGeometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	blasGeometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	blasGeometry.geometry.triangles = triangles;

	VkAccelerationStructureBuildGeometryInfoKHR buildInfo;
	memset( &buildInfo, 0, sizeof( buildInfo ) );
	buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
	buildInfo.mode = updateBlas ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	buildInfo.srcAccelerationStructure = updateBlas ? entry.blas : VK_NULL_HANDLE;
	buildInfo.geometryCount = 1;
	buildInfo.pGeometries = &blasGeometry;

	VkAccelerationStructureBuildSizesInfoKHR sizeInfo;
	memset( &sizeInfo, 0, sizeof( sizeInfo ) );
	sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	vkGetAccelerationStructureBuildSizesKHRLocal( device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &primitiveCount, &sizeInfo );

	if ( !updateBlas ) {
		if ( entry.blas != VK_NULL_HANDLE ) {
			vkDestroyAccelerationStructureKHRLocal( device, entry.blas, NULL );
			entry.blas = VK_NULL_HANDLE;
		}
		DestroyBuffer( entry.blasBuffer );
		entry.blasAddress = 0;
		entry.updatable = false;

		if ( !CreateBuffer( sizeInfo.accelerationStructureSize,
				VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				true,
				entry.blasBuffer ) ) {
			DestroySurfaceBlas( entry );
			return false;
		}

		VkAccelerationStructureCreateInfoKHR createInfo;
		memset( &createInfo, 0, sizeof( createInfo ) );
		createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
		createInfo.buffer = entry.blasBuffer.buffer;
		createInfo.size = sizeInfo.accelerationStructureSize;
		createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
		if ( vkCreateAccelerationStructureKHRLocal( device, &createInfo, NULL, &entry.blas ) != VK_SUCCESS ) {
			LogFailure( "vkCreateAccelerationStructureKHR surface BLAS failed" );
			DestroySurfaceBlas( entry );
			return false;
		}
	}

	swVkBuffer_t scratchBuffer;
	const VkDeviceSize scratchSize = updateBlas && sizeInfo.updateScratchSize > 0 ? sizeInfo.updateScratchSize : sizeInfo.buildScratchSize;
	if ( scratchSize == 0 ||
		 !CreateBuffer( scratchSize,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			true,
			scratchBuffer ) ) {
		if ( !updateBlas ) {
			DestroySurfaceBlas( entry );
		}
		return false;
	}

	buildInfo.dstAccelerationStructure = entry.blas;
	buildInfo.scratchData.deviceAddress = GetBufferAddress( scratchBuffer.buffer );

	VkAccelerationStructureBuildRangeInfoKHR rangeInfo;
	memset( &rangeInfo, 0, sizeof( rangeInfo ) );
	rangeInfo.primitiveCount = primitiveCount;
	const VkAccelerationStructureBuildRangeInfoKHR *ranges[] = { &rangeInfo };

	VkCommandBufferBeginInfo beginInfo;
	memset( &beginInfo, 0, sizeof( beginInfo ) );
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if ( !ImmediateSubmit( beginInfo ) ) {
		DestroyBuffer( scratchBuffer );
		if ( !updateBlas ) {
			DestroySurfaceBlas( entry );
		}
		return false;
	}

	vkCmdBuildAccelerationStructuresKHRLocal( commandBuffer, 1, &buildInfo, ranges );

	if ( vkEndCommandBuffer( commandBuffer ) != VK_SUCCESS ) {
		LogFailure( "vkEndCommandBuffer surface BLAS failed" );
		DestroyBuffer( scratchBuffer );
		if ( !updateBlas ) {
			DestroySurfaceBlas( entry );
		}
		return false;
	}

	VkSubmitInfo submitInfo;
	memset( &submitInfo, 0, sizeof( submitInfo ) );
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;
	if ( vkQueueSubmit( queue, 1, &submitInfo, inFlightFence ) != VK_SUCCESS ) {
		LogFailure( "vkQueueSubmit surface BLAS failed" );
		DestroyBuffer( scratchBuffer );
		if ( !updateBlas ) {
			DestroySurfaceBlas( entry );
		}
		return false;
	}
	vkWaitForFences( device, 1, &inFlightFence, VK_TRUE, UINT64_MAX );
	DestroyBuffer( scratchBuffer );

	VkAccelerationStructureDeviceAddressInfoKHR addressInfo;
	memset( &addressInfo, 0, sizeof( addressInfo ) );
	addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	addressInfo.accelerationStructure = entry.blas;
	entry.blasAddress = vkGetAccelerationStructureDeviceAddressKHRLocal( device, &addressInfo );
	entry.owner = geo;
	entry.key = geo;
	entry.verts = geo->verts;
	entry.indexes = geo->indexes;
	entry.vertCount = geo->numVerts;
	entry.indexCount = geo->numIndexes;
	entry.primitiveCount = primitiveCount;
	entry.geometryGeneration = geo->rayQueryGeometryGeneration;
	entry.updatable = true;
	if ( entry.blasAddress == 0 ) {
		DestroySurfaceBlas( entry );
		return false;
	}
	return true;
}

swVkEntityTlas_t *idSoftwareVulkanBridge::EnsureEntityTlas( idRenderEntityLocal *entity ) {
	if ( !entity ) {
		return NULL;
	}
	swVkEntityTlas_t *entry = static_cast<swVkEntityTlas_t *>( entity->rayQueryTlas );
	if ( entry && entry->owner != entity ) {
		entry = NULL;
		entity->rayQueryTlas = NULL;
	}
	if ( !entry ) {
		entry = new swVkEntityTlas_t;
		entry->owner = entity;
		entity->rayQueryTlas = entry;
		rayEntityTlasCache.Append( entry );
	}
	return entry;
}

bool idSoftwareVulkanBridge::RayQuerySurfaceCanCastShadow( const srfTriangles_t *geo, const idMaterial *material, const viewEntity_t *space ) const {
	if ( !geo || !geo->verts || !geo->indexes || geo->numVerts <= 0 || geo->numIndexes < 3 || !geo->rayQueryPersistent || !material ) {
		return false;
	}

	const idRenderEntityLocal *entity = space ? space->entityDef : NULL;
	if ( entity && ( entity->parms.noShadow || entity->parms.weaponDepthHack ) ) {
		return false;
	}

	const bool dynamicEntity = entity && entity->parms.hModel && entity->parms.hModel->IsDynamicModel() != DM_STATIC;
	const materialCoverage_t coverage = material->Coverage();
	if ( !material->IsDrawn() || !material->SurfaceCastsShadow() || coverage == MC_TRANSLUCENT ) {
		return false;
	}
	if ( coverage == MC_PERFORATED && !dynamicEntity ) {
		return false;
	}

	const float sort = material->GetSort();
	if ( sort < SS_OPAQUE || sort == SS_PORTAL_SKY || sort >= SS_POST_PROCESS ) {
		return false;
	}
	return true;
}

bool idSoftwareVulkanBridge::RayQuerySurfaceTouchesShadowLight( const viewDef_t *viewDef, const srfTriangles_t *geo, const idMaterial *material, const viewEntity_t *space ) const {
	if ( !viewDef || !geo || !material || !space ) {
		return false;
	}

	idRenderEntityLocal *entity = space->entityDef;
	for ( const viewLight_t *vLight = viewDef->viewLights; vLight; vLight = vLight->next ) {
		if ( !vLight->lightDef || !vLight->lightShader ) {
			continue;
		}
		if ( vLight->lightDef->parms.noShadows || vLight->lightShader->IsFogLight() || vLight->lightShader->IsBlendLight() ||
				vLight->lightShader->IsAmbientLight() || !vLight->lightShader->LightCastsShadows() ) {
			continue;
		}
		if ( material->Spectrum() != vLight->lightShader->Spectrum() ) {
			continue;
		}
		if ( !vLight->localInteractions && !vLight->globalInteractions && !vLight->translucentInteractions ) {
			continue;
		}

		if ( entity ) {
			bool activeInteraction = false;
			for ( idInteraction *inter = entity->firstInteraction; inter != NULL && !inter->IsEmpty(); inter = inter->entityNext ) {
				if ( inter->lightDef == vLight->lightDef ) {
					activeInteraction = true;
					break;
				}
			}
			if ( !activeInteraction ) {
				continue;
			}
		}

		if ( !R_CullLocalBox( geo->bounds, space->modelMatrix, 6, vLight->lightDef->frustum ) ) {
			return true;
		}
	}
	return false;
}

bool idSoftwareVulkanBridge::AddRayQuerySurfaceInstance( idList<swVkRayInstance_t> &instances, srfTriangles_t *geo, const float modelMatrix[16], idRenderEntityLocal *entity ) {
	if ( !geo ) {
		return false;
	}

	swVkSurfaceBlas_t *cachedBlas = static_cast<swVkSurfaceBlas_t *>( geo->rayQueryBlas );
	if ( cachedBlas && cachedBlas->owner != geo ) {
		cachedBlas = NULL;
		geo->rayQueryBlas = NULL;
		geo->rayQueryBlasDirty = true;
	}

	const bool needsBlasBuild = !cachedBlas ||
		geo->rayQueryBlasDirty ||
		cachedBlas->verts != geo->verts ||
		cachedBlas->indexes != geo->indexes ||
		cachedBlas->vertCount != geo->numVerts ||
		cachedBlas->indexCount != geo->numIndexes ||
		cachedBlas->geometryGeneration != geo->rayQueryGeometryGeneration ||
		cachedBlas->blas == VK_NULL_HANDLE ||
		cachedBlas->blasAddress == 0;
	if ( needsBlasBuild ) {
		if ( cachedBlas ) {
			WaitForSubmittedWork();
			DestroyRayQueryTlas();
			if ( !BuildSurfaceBlas( *cachedBlas, geo ) ) {
				return false;
			}
		} else {
			cachedBlas = new swVkSurfaceBlas_t;
			if ( !BuildSurfaceBlas( *cachedBlas, geo ) ) {
				delete cachedBlas;
				return false;
			}
			rayAttachedBlas.Append( cachedBlas );
		}
		geo->rayQueryBlas = cachedBlas;
		geo->rayQueryBlasDirty = false;
	}

	swVkRayInstance_t instance;
	instance.blasAddress = cachedBlas->blasAddress;
	memcpy( instance.modelMatrix, modelMatrix, sizeof( instance.modelMatrix ) );
	instances.Append( instance );

	swVkEntityTlas_t *entityTlas = EnsureEntityTlas( entity );
	if ( entityTlas ) {
		if ( entityTlas->frame != rayQuerySceneFrame ) {
			entityTlas->instances.Clear();
			entityTlas->signature = 1469598103934665603ULL;
			entityTlas->signature = SWVkHashU64( entityTlas->signature, reinterpret_cast<uintptr_t>( entity ) );
			entityTlas->instanceCount = 0;
			entityTlas->frame = rayQuerySceneFrame;
		}
		entityTlas->instances.Append( instance );
		entityTlas->signature = SWVkHashU64( entityTlas->signature, instance.blasAddress );
		for ( int matrixIndex = 0; matrixIndex < 16; matrixIndex++ ) {
			uint32_t bits;
			memcpy( &bits, &instance.modelMatrix[matrixIndex], sizeof( bits ) );
			entityTlas->signature = SWVkHashU32( entityTlas->signature, bits );
		}
		entityTlas->instanceCount = entityTlas->instances.Num();
		entity->rayQueryTlasSignature = entityTlas->signature;
		entity->rayQueryTlasInstanceCount = entityTlas->instanceCount;
		entity->rayQueryTlasFrame = rayQuerySceneFrame;
	}
	return true;
}

void idSoftwareVulkanBridge::DestroyRayQueryBlasHandle( void *blas ) {
	if ( !blas ) {
		return;
	}
	swVkSurfaceBlas_t *entry = static_cast<swVkSurfaceBlas_t *>( blas );
	for ( int i = 0; i < rayAttachedBlas.Num(); i++ ) {
		if ( rayAttachedBlas[i] == entry ) {
			rayAttachedBlas.RemoveIndex( i );
			break;
		}
	}
	if ( initialized && device != VK_NULL_HANDLE ) {
		WaitForSubmittedWork();
		DestroyRayQueryTlas();
		DestroySurfaceBlas( *entry );
	} else if ( entry->owner && entry->owner->rayQueryBlas == entry ) {
		entry->owner->rayQueryBlas = NULL;
	}
	delete entry;
}

void idSoftwareVulkanBridge::DestroyRayQueryEntityHandle( void *entityTlas ) {
	if ( !entityTlas ) {
		return;
	}
	swVkEntityTlas_t *entry = static_cast<swVkEntityTlas_t *>( entityTlas );
	for ( int i = 0; i < rayEntityTlasCache.Num(); i++ ) {
		if ( rayEntityTlasCache[i] == entry ) {
			rayEntityTlasCache.RemoveIndex( i );
			break;
		}
	}
	if ( entry->owner && entry->owner->rayQueryTlas == entry ) {
		entry->owner->rayQueryTlas = NULL;
		entry->owner->rayQueryTlasSignature = 0;
		entry->owner->rayQueryTlasInstanceCount = 0;
		entry->owner->rayQueryTlasFrame = 0;
	}
	delete entry;
}

void idSoftwareVulkanBridge::DestroySurfaceBlasList( idList<swVkSurfaceBlas_t *> &list ) {
	for ( int i = 0; i < list.Num(); i++ ) {
		swVkSurfaceBlas_t *entry = list[i];
		DestroySurfaceBlas( *entry );
		delete entry;
	}
	list.Clear();
}

void idSoftwareVulkanBridge::DestroyRayQueryTlas() {
	rayQuerySceneReady = false;
	rayTlasSignature = 0;
	rayTlasInstanceCount = 0;
	if ( tlas != VK_NULL_HANDLE ) {
		vkDestroyAccelerationStructureKHRLocal( device, tlas, NULL );
		tlas = VK_NULL_HANDLE;
	}
	DestroyBuffer( shadowScratchBuffer );
	DestroyBuffer( instanceBuffer );
	DestroyBuffer( tlasBuffer );
}

bool idSoftwareVulkanBridge::BuildRayQueryTlas( const idList<swVkRayInstance_t> &instances ) {
	if ( !rayQuerySupported || instances.Num() <= 0 ) {
		DestroyRayQueryTlas();
		return false;
	}

	uint64_t signature = 1469598103934665603ULL;
	signature = SWVkHashU32( signature, static_cast<uint32_t>( instances.Num() ) );
	for ( int i = 0; i < instances.Num(); i++ ) {
		signature = SWVkHashU64( signature, instances[i].blasAddress );
		for ( int j = 0; j < 16; j++ ) {
			uint32_t bits;
			memcpy( &bits, &instances[i].modelMatrix[j], sizeof( bits ) );
			signature = SWVkHashU32( signature, bits );
		}
	}
	if ( rayQuerySceneReady && tlas != VK_NULL_HANDLE && rayTlasInstanceCount == instances.Num() && rayTlasSignature == signature ) {
		return true;
	}

	WaitForSubmittedWork();
	DestroyRayQueryTlas();

	idList<VkAccelerationStructureInstanceKHR> vkInstances;
	vkInstances.SetNum( instances.Num(), false );
	for ( int i = 0; i < instances.Num(); i++ ) {
		const float *m = instances[i].modelMatrix;
		VkAccelerationStructureInstanceKHR &instance = vkInstances[i];
		memset( &instance, 0, sizeof( instance ) );
		instance.transform.matrix[0][0] = m[0];
		instance.transform.matrix[0][1] = m[4];
		instance.transform.matrix[0][2] = m[8];
		instance.transform.matrix[0][3] = m[12];
		instance.transform.matrix[1][0] = m[1];
		instance.transform.matrix[1][1] = m[5];
		instance.transform.matrix[1][2] = m[9];
		instance.transform.matrix[1][3] = m[13];
		instance.transform.matrix[2][0] = m[2];
		instance.transform.matrix[2][1] = m[6];
		instance.transform.matrix[2][2] = m[10];
		instance.transform.matrix[2][3] = m[14];
		instance.instanceCustomIndex = i;
		instance.mask = 0xff;
		instance.instanceShaderBindingTableRecordOffset = 0;
		instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		instance.accelerationStructureReference = instances[i].blasAddress;
	}

	const VkDeviceSize instanceSize = static_cast<VkDeviceSize>( vkInstances.Num() ) * sizeof( vkInstances[0] );
	if ( !CreateBuffer( instanceSize,
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			true,
			instanceBuffer ) ||
		 !UploadBuffer( instanceBuffer, vkInstances.Ptr(), instanceSize, "vkMapMemory TLAS instances failed" ) ) {
		DestroyRayQueryTlas();
		return false;
	}

	VkAccelerationStructureGeometryInstancesDataKHR instanceData;
	memset( &instanceData, 0, sizeof( instanceData ) );
	instanceData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	instanceData.arrayOfPointers = VK_FALSE;
	instanceData.data.deviceAddress = GetBufferAddress( instanceBuffer.buffer );

	VkAccelerationStructureGeometryKHR tlasGeometry;
	memset( &tlasGeometry, 0, sizeof( tlasGeometry ) );
	tlasGeometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	tlasGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	tlasGeometry.geometry.instances = instanceData;

	uint32_t primitiveCount = instances.Num();
	VkAccelerationStructureBuildGeometryInfoKHR buildInfo;
	memset( &buildInfo, 0, sizeof( buildInfo ) );
	buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	buildInfo.geometryCount = 1;
	buildInfo.pGeometries = &tlasGeometry;

	VkAccelerationStructureBuildSizesInfoKHR sizeInfo;
	memset( &sizeInfo, 0, sizeof( sizeInfo ) );
	sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	vkGetAccelerationStructureBuildSizesKHRLocal( device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &primitiveCount, &sizeInfo );

	if ( !CreateBuffer( sizeInfo.accelerationStructureSize,
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			true,
			tlasBuffer ) ||
		 !CreateBuffer( sizeInfo.buildScratchSize,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			true,
			shadowScratchBuffer ) ) {
		DestroyRayQueryTlas();
		return false;
	}

	VkAccelerationStructureCreateInfoKHR createInfo;
	memset( &createInfo, 0, sizeof( createInfo ) );
	createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	createInfo.buffer = tlasBuffer.buffer;
	createInfo.size = sizeInfo.accelerationStructureSize;
	createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	if ( vkCreateAccelerationStructureKHRLocal( device, &createInfo, NULL, &tlas ) != VK_SUCCESS ) {
		LogFailure( "vkCreateAccelerationStructureKHR TLAS failed" );
		DestroyRayQueryTlas();
		return false;
	}

	buildInfo.dstAccelerationStructure = tlas;
	buildInfo.scratchData.deviceAddress = GetBufferAddress( shadowScratchBuffer.buffer );

	VkAccelerationStructureBuildRangeInfoKHR rangeInfo;
	memset( &rangeInfo, 0, sizeof( rangeInfo ) );
	rangeInfo.primitiveCount = primitiveCount;
	const VkAccelerationStructureBuildRangeInfoKHR *ranges[] = { &rangeInfo };

	VkCommandBufferBeginInfo beginInfo;
	memset( &beginInfo, 0, sizeof( beginInfo ) );
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if ( !ImmediateSubmit( beginInfo ) ) {
		DestroyRayQueryTlas();
		return false;
	}

	vkCmdBuildAccelerationStructuresKHRLocal( commandBuffer, 1, &buildInfo, ranges );

	if ( vkEndCommandBuffer( commandBuffer ) != VK_SUCCESS ) {
		LogFailure( "vkEndCommandBuffer TLAS failed" );
		DestroyRayQueryTlas();
		return false;
	}

	VkSubmitInfo submitInfo;
	memset( &submitInfo, 0, sizeof( submitInfo ) );
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;
	if ( vkQueueSubmit( queue, 1, &submitInfo, inFlightFence ) != VK_SUCCESS ) {
		LogFailure( "vkQueueSubmit TLAS failed" );
		DestroyRayQueryTlas();
		return false;
	}
	vkWaitForFences( device, 1, &inFlightFence, VK_TRUE, UINT64_MAX );
	rayQuerySceneReady = true;
	rayTlasSignature = signature;
	rayTlasInstanceCount = instances.Num();
	return true;
}

void idSoftwareVulkanBridge::BeginFrame( bool clearFramePixels ) {
	if ( frameBegun ) {
		return;
	}
	const int frameIndex = nextFrameContext;
	nextFrameContext = ( nextFrameContext + 1 ) % SW_VK_FRAMES_IN_FLIGHT;
	WaitForFrameContext( frameIndex );
	LoadFrameContext( frameIndex );

	if ( clearFramePixels && framePixels.Num() > 0 ) {
		memset( framePixels.Ptr(), 0, framePixels.Num() * sizeof( framePixels[0] ) );
	}
	Clear2DJobs();
	frameBegun = true;
	frameDirty = false;
	hybridFrameDirty = false;
	hybridOverlayDirty = false;
	hybridOverlayOnly = false;
}

void idSoftwareVulkanBridge::Clear2DJobs() {
	hybrid2DSourcePixels.SetNum( 0, false );
	hybrid2DJobs.SetNum( 0, false );
	hybridOverlayTris.SetNum( 0, false );
	hybridOverlayTiles.SetNum( 0, false );
	hybridOverlayTileIndices.SetNum( 0, false );
	hybridOverlaySourceWidth = 0;
	hybridOverlaySourceHeight = 0;
	hybridOverlayPresentWidth = 0;
	hybridOverlayPresentHeight = 0;
	hybridOverlayViewportX = 0;
	hybridOverlayViewportY = 0;
}

bool idSoftwareVulkanBridge::Queue2DOverlayBlit( const viewDef_t *viewDef, const unsigned int *bgra, int srcWidth, int srcHeight, int dstWidth, int dstHeight ) {
	if ( !viewDef || !bgra || srcWidth <= 0 || srcHeight <= 0 || dstWidth <= 0 || dstHeight <= 0 ) {
		return false;
	}

	const int sourcePixelCount = srcWidth * srcHeight;
	const int sourceOffset = hybrid2DSourcePixels.Num();
	hybrid2DSourcePixels.SetNum( sourceOffset + sourcePixelCount, false );
	memcpy( hybrid2DSourcePixels.Ptr() + sourceOffset, bgra, sourcePixelCount * sizeof( bgra[0] ) );

	swVk2DJob_t &job = hybrid2DJobs.Alloc();
	job.sourceOffset = sourceOffset;
	job.sourceWidth = srcWidth;
	job.sourceHeight = srcHeight;
	job.viewportX = viewDef->viewport.x1;
	job.viewportY = viewDef->viewport.y1;
	job.presentWidth = dstWidth;
	job.presentHeight = dstHeight;

	hybridOverlayDirty = true;
	frameDirty = true;
	return true;
}

bool idSoftwareVulkanBridge::QueueHybridOverlayTriangles( const viewDef_t *viewDef, const swHybridOverlayTri_t *tris, int triCount, int srcWidth, int srcHeight, int dstWidth, int dstHeight ) {
	return QueueHybridOverlaySourceTriangles( viewDef, tris, triCount, srcWidth, srcHeight, dstWidth, dstHeight, NULL, 0, 0 );
}

bool idSoftwareVulkanBridge::QueueHybridOverlaySourceTriangles( const viewDef_t *viewDef, const swHybridOverlayTri_t *tris, int triCount, int srcWidth, int srcHeight, int dstWidth, int dstHeight, const unsigned int *sourcePixels, int sourceWidth, int sourceHeight ) {
	if ( !viewDef || !tris || triCount <= 0 || srcWidth <= 0 || srcHeight <= 0 || dstWidth <= 0 || dstHeight <= 0 ) {
		return false;
	}
	const bool hasSource = sourcePixels != NULL && sourceWidth > 0 && sourceHeight > 0;
	if ( !EnsureInitialized() || overlayPipeline == VK_NULL_HANDLE ) {
		return false;
	}
	if ( !hybridFrameDirty ) {
		if ( !EnsureFrameBuffer() || frameBegun ) {
			return false;
		}
		BeginFrame( false );
		hybridWidth = srcWidth;
		hybridHeight = srcHeight;
		hybridPresentWidth = dstWidth;
		hybridPresentHeight = dstHeight;
		hybridViewportX = viewDef->viewport.x1;
		hybridViewportY = viewDef->viewport.y1;
		hybridDebugView = 0;
		hybridTextureCount = Max( 1, hybridTextureCount );
		hybridTextureTexelCount = Max( 1, hybridTextureTexelCount );
		hybridLightCount = 0;
		hybridLightTileCount = 1;
		hybridLightIndexCount = 1;
		hybridShadowEnabled = false;
		hybridViewOrigin[0] = 0.0f;
		hybridViewOrigin[1] = 0.0f;
		hybridViewOrigin[2] = 0.0f;
		hybridOverlayOnly = true;
		hybridFrameDirty = true;
	}

	int sourceOffset = 0;
	if ( hasSource ) {
		sourceOffset = hybrid2DSourcePixels.Num();
		const int sourcePixelCount = sourceWidth * sourceHeight;
		hybrid2DSourcePixels.SetNum( sourceOffset + sourcePixelCount, false );
		memcpy( hybrid2DSourcePixels.Ptr() + sourceOffset, sourcePixels, sourcePixelCount * sizeof( sourcePixels[0] ) );
	}

	const int oldCount = hybridOverlayTris.Num();
	hybridOverlayTris.SetNum( oldCount + triCount, false );
	for ( int i = 0; i < triCount; i++ ) {
		swHybridOverlayTri_t &dst = hybridOverlayTris[oldCount + i];
		dst = tris[i];
		dst.source[0] = hasSource ? sourceWidth : srcWidth;
		dst.source[1] = hasSource ? sourceHeight : srcHeight;
		dst.source[2] = sourceOffset;
		dst.source[3] = 0;
		dst.viewport[0] = viewDef->viewport.x1;
		dst.viewport[1] = viewDef->viewport.y1;
		dst.viewport[2] = dstWidth;
		dst.viewport[3] = dstHeight;
	}

	hybridOverlaySourceWidth = srcWidth;
	hybridOverlaySourceHeight = srcHeight;
	hybridOverlayPresentWidth = dstWidth;
	hybridOverlayPresentHeight = dstHeight;
	hybridOverlayViewportX = viewDef->viewport.x1;
	hybridOverlayViewportY = viewDef->viewport.y1;
	hybridOverlayDirty = true;
	frameDirty = true;
	return true;
}

bool idSoftwareVulkanBridge::BlitView( const viewDef_t *viewDef, const unsigned int *bgra, int width, int height, int presentWidth, int presentHeight ) {
	if ( !viewDef || !bgra || width <= 0 || height <= 0 || presentWidth <= 0 || presentHeight <= 0 ) {
		return false;
	}
	if ( !EnsureInitialized() || !EnsureFrameBuffer() ) {
		return false;
	}

	BeginFrame( true );
	if ( hybridFrameDirty && twoDPipeline != VK_NULL_HANDLE ) {
		return Queue2DOverlayBlit( viewDef, bgra, width, height, presentWidth, presentHeight );
	}

	const int dstX0 = viewDef->viewport.x1;
	const int dstY0 = viewDef->viewport.y1;
	const int dstX1 = dstX0 + presentWidth - 1;
	const int dstY1 = dstY0 + presentHeight - 1;

	const int clippedX0 = Max( 0, dstX0 );
	const int clippedY0 = Max( 0, dstY0 );
	const int clippedX1 = Min( frameWidth - 1, dstX1 );
	const int clippedY1 = Min( frameHeight - 1, dstY1 );
	if ( clippedX0 > clippedX1 || clippedY0 > clippedY1 ) {
		return true;
	}

	for ( int dstY = clippedY0; dstY <= clippedY1; dstY++ ) {
		const int viewY = dstY - dstY0;
		const int srcY = Min( height - 1, Max( 0, ( viewY * height ) / presentHeight ) );
		unsigned int *dst = framePixels.Ptr() + dstY * frameWidth + clippedX0;
		const unsigned int *srcRow = bgra + srcY * width;

		for ( int dstX = clippedX0; dstX <= clippedX1; dstX++ ) {
			const int viewX = dstX - dstX0;
			const int srcX = Min( width - 1, Max( 0, ( viewX * width ) / presentWidth ) );
			*dst++ = srcRow[srcX];
		}
	}

	frameDirty = true;
	if ( hybridFrameDirty ) {
		hybridOverlayDirty = true;
	}
	return true;
}

bool idSoftwareVulkanBridge::CompositeHybridGBuffer( const viewDef_t *viewDef, const swHybridGBufferUpload_t &gbuffer, int width, int height, int presentWidth, int presentHeight ) {
	if ( !viewDef || width <= 0 || height <= 0 || presentWidth <= 0 || presentHeight <= 0 ) {
		return false;
	}
	if ( !gbuffer.depth || !gbuffer.normalPacked || !gbuffer.tangentPacked || !gbuffer.bitangentPacked ||
		 !gbuffer.uvPacked || !gbuffer.materialId ||
		 !gbuffer.albedoOrTextureId || !gbuffer.specularAndFlags || !gbuffer.surfaceId ||
		 !gbuffer.worldPositions ||
		 !gbuffer.textureInfos || gbuffer.textureInfoCount <= 0 ||
		 !gbuffer.textureTexels || gbuffer.textureTexelCount <= 0 ) {
		return false;
	}
	const swHybridLight_t emptyLight = {};
	const swHybridLight_t *lights = ( gbuffer.lights && gbuffer.lightCount > 0 ) ? gbuffer.lights : &emptyLight;
	const int uploadLightCount = Max( 1, gbuffer.lightCount );
	const swHybridLightTile_t emptyLightTile = {};
	const uint32_t emptyLightIndex = 0;
	const swHybridLightTile_t *lightTiles = ( gbuffer.lightTiles && gbuffer.lightTileCount > 0 ) ? gbuffer.lightTiles : &emptyLightTile;
	const uint32_t *lightIndices = ( gbuffer.lightIndices && gbuffer.lightIndexCount > 0 ) ? gbuffer.lightIndices : &emptyLightIndex;
	const int uploadLightTileCount = Max( 1, gbuffer.lightTileCount );
	const int uploadLightIndexCount = Max( 1, gbuffer.lightIndexCount );
	if ( !EnsureInitialized() || !EnsureFrameBuffer() ) {
		return false;
	}
	BeginFrame( false );
	const bool textureUploadChanged = hybridUploadedTextureGeneration != gbuffer.textureGeneration ||
		hybridTextureCount != gbuffer.textureInfoCount ||
		hybridTextureTexelCount != gbuffer.textureTexelCount;
	if ( textureUploadChanged ) {
		WaitForAllFrameContexts();
		LoadFrameContext( activeFrameContext );
	}
	const VkBuffer oldTextureTexelBuffer = hybridTextureTexelBuffer.buffer;
	if ( !EnsureHybridBuffers( width, height, gbuffer.textureInfoCount, gbuffer.textureTexelCount, uploadLightCount, uploadLightTileCount, uploadLightIndexCount ) ) {
		return false;
	}

	const VkDeviceSize pixelCount = static_cast<VkDeviceSize>( width ) * static_cast<VkDeviceSize>( height );
	const VkDeviceSize depthSize = pixelCount * sizeof( float );
	const VkDeviceSize uintSize = pixelCount * sizeof( uint32_t );
	const VkDeviceSize worldPositionSize = pixelCount * sizeof( idVec4 );
	const VkDeviceSize lightSize = static_cast<VkDeviceSize>( uploadLightCount ) * sizeof( swHybridLight_t );
	const VkDeviceSize lightTileSize = static_cast<VkDeviceSize>( uploadLightTileCount ) * sizeof( swHybridLightTile_t );
	const VkDeviceSize lightIndexSize = static_cast<VkDeviceSize>( uploadLightIndexCount ) * sizeof( uint32_t );
	const uint32_t emptyMaterialId = 0u;
	const bool uploadMaterialIds = gbuffer.debugView == 4;
	const void *materialData = uploadMaterialIds ? static_cast<const void *>( gbuffer.materialId ) : static_cast<const void *>( &emptyMaterialId );
	const VkDeviceSize materialSize = uploadMaterialIds ? uintSize : sizeof( emptyMaterialId );
	if ( !UploadBuffer( hybridDepthBuffer, gbuffer.depth, depthSize, "vkMapMemory hybrid depth failed" ) ||
		 !UploadBuffer( hybridNormalBuffer, gbuffer.normalPacked, uintSize, "vkMapMemory hybrid normals failed" ) ||
		 !UploadBuffer( hybridTangentBuffer, gbuffer.tangentPacked, uintSize, "vkMapMemory hybrid tangents failed" ) ||
		 !UploadBuffer( hybridBitangentBuffer, gbuffer.bitangentPacked, uintSize, "vkMapMemory hybrid bitangents failed" ) ||
		 !UploadBuffer( hybridUVBuffer, gbuffer.uvPacked, uintSize, "vkMapMemory hybrid uvs failed" ) ||
		 !UploadBuffer( hybridMaterialBuffer, materialData, materialSize, "vkMapMemory hybrid materials failed" ) ||
		 !UploadBuffer( hybridAlbedoBuffer, gbuffer.albedoOrTextureId, uintSize, "vkMapMemory hybrid albedo ids failed" ) ||
		 !UploadBuffer( hybridSpecularBuffer, gbuffer.specularAndFlags, uintSize, "vkMapMemory hybrid specular flags failed" ) ||
		 !UploadBuffer( hybridSurfaceBuffer, gbuffer.surfaceId, uintSize, "vkMapMemory hybrid surfaces failed" ) ||
		 !UploadBuffer( hybridWorldPositionBuffer, gbuffer.worldPositions, worldPositionSize, "vkMapMemory hybrid world positions failed" ) ||
		 !UploadBuffer( hybridLightBuffer, lights, lightSize, "vkMapMemory hybrid lights failed" ) ||
		 !UploadBuffer( hybridLightTileBuffer, lightTiles, lightTileSize, "vkMapMemory hybrid light tiles failed" ) ||
		 !UploadBuffer( hybridLightIndexBuffer, lightIndices, lightIndexSize, "vkMapMemory hybrid light indices failed" ) ) {
		return false;
	}
	if ( !UploadHybridTextures( gbuffer.textureInfos, gbuffer.textureInfoCount, gbuffer.textureTexels, gbuffer.textureTexelCount,
		 gbuffer.textureGeneration, oldTextureTexelBuffer == hybridTextureTexelBuffer.buffer ) ) {
		return false;
	}

	hybridShadowEnabled = false;
	if ( gbuffer.lightCount > 0 && r_softwareRayQueryShadows.GetBool() && r_softwareHybridRayQueryShadows.GetBool() && rayQuerySupported ) {
		hybridShadowEnabled = PrepareRayQueryScene( viewDef ) && tlas != VK_NULL_HANDLE;
	}
	hybridCompositeVariant = SelectHybridCompositeVariant( gbuffer, hybridShadowEnabled );
	UpdateHybridDescriptorSet();

	hybridWidth = width;
	hybridHeight = height;
	hybridPresentWidth = presentWidth;
	hybridPresentHeight = presentHeight;
	hybridViewportX = viewDef->viewport.x1;
	hybridViewportY = viewDef->viewport.y1;
	hybridDebugView = idMath::ClampInt( 0, 6, gbuffer.debugView );
	hybridTextureCount = gbuffer.textureInfoCount;
	hybridTextureTexelCount = gbuffer.textureTexelCount;
	hybridLightCount = Max( 0, gbuffer.lightCount );
	hybridLightTileCount = uploadLightTileCount;
	hybridLightIndexCount = uploadLightIndexCount;
	hybridViewOrigin[0] = viewDef->renderView.vieworg[0];
	hybridViewOrigin[1] = viewDef->renderView.vieworg[1];
	hybridViewOrigin[2] = viewDef->renderView.vieworg[2];
	hybridOverlayOnly = false;
	hybridFrameDirty = true;
	frameDirty = true;
	return true;
}

bool idSoftwareVulkanBridge::UploadHybridTextures( const swHybridTextureInfo_t *textureInfos, int textureInfoCount, const unsigned int *textureTexels, int textureTexelCount, unsigned int textureGeneration, bool texelBufferPreserved ) {
	if ( hybridUploadedTextureGeneration == textureGeneration &&
		 hybridTextureCount == textureInfoCount &&
		 hybridTextureTexelCount == textureTexelCount ) {
		return true;
	}

	const VkDeviceSize textureInfoSize = static_cast<VkDeviceSize>( textureInfoCount ) * sizeof( swHybridTextureInfo_t );
	const VkDeviceSize textureTexelSize = static_cast<VkDeviceSize>( textureTexelCount ) * sizeof( uint32_t );
	const int oldTextureTexelCount = hybridTextureTexelCount;
	const bool appendOnly = texelBufferPreserved &&
		hybridUploadedTextureGeneration != 0 &&
		oldTextureTexelCount > 0 &&
		textureTexelCount >= oldTextureTexelCount;

	if ( !UploadBuffer( hybridTextureInfoBuffer, textureInfos, textureInfoSize, "vkMapMemory hybrid texture infos failed" ) ) {
		return false;
	}

	if ( appendOnly ) {
		const int appendedTexels = textureTexelCount - oldTextureTexelCount;
		const VkDeviceSize appendedSize = static_cast<VkDeviceSize>( appendedTexels ) * sizeof( uint32_t );
		const VkDeviceSize appendedOffset = static_cast<VkDeviceSize>( oldTextureTexelCount ) * sizeof( uint32_t );
		if ( !UploadBufferRange( hybridTextureTexelBuffer, appendedOffset, textureTexels + oldTextureTexelCount,
			 appendedSize, "vkMapMemory hybrid texture append failed" ) ) {
			return false;
		}
	} else if ( !UploadBuffer( hybridTextureTexelBuffer, textureTexels, textureTexelSize, "vkMapMemory hybrid texture texels failed" ) ) {
		return false;
	}

	hybridTextureCount = textureInfoCount;
	hybridTextureTexelCount = textureTexelCount;
	hybridUploadedTextureGeneration = textureGeneration;
	return true;
}

bool idSoftwareVulkanBridge::UpdateHybridTextures( const swHybridTextureInfo_t *textureInfos, int textureInfoCount, const unsigned int *textureTexels, int textureTexelCount, unsigned int textureGeneration ) {
	if ( !textureInfos || textureInfoCount <= 0 || !textureTexels || textureTexelCount <= 0 ) {
		return false;
	}
	if ( !EnsureInitialized() ) {
		return false;
	}
	const bool textureUploadChanged = hybridUploadedTextureGeneration != textureGeneration ||
		hybridTextureCount != textureInfoCount ||
		hybridTextureTexelCount != textureTexelCount;
	if ( textureUploadChanged ) {
		WaitForAllFrameContexts();
		LoadFrameContext( activeFrameContext );
	}
	if ( hybridFrameDirty && ( hybridWidth <= 0 || hybridHeight <= 0 ||
		 !EnsureHybridBuffers( hybridWidth, hybridHeight, textureInfoCount, textureTexelCount, Max( 1, hybridLightCount ), Max( 1, hybridLightTileCount ), Max( 1, hybridLightIndexCount ) ) ) ) {
		return false;
	}
	if ( hybridUploadedTextureGeneration == textureGeneration &&
		 hybridTextureCount == textureInfoCount &&
		 hybridTextureTexelCount == textureTexelCount ) {
		if ( hybridFrameDirty ) {
			UpdateHybridDescriptorSet();
		}
		return true;
	}

	bool texelBufferPreserved = true;
	if ( !EnsureHybridTextureBuffers( textureInfoCount, textureTexelCount, texelBufferPreserved ) ||
		 !UploadHybridTextures( textureInfos, textureInfoCount, textureTexels, textureTexelCount, textureGeneration, texelBufferPreserved ) ) {
		return false;
	}

	if ( hybridFrameDirty ) {
		UpdateHybridDescriptorSet();
	}
	return true;
}

bool idSoftwareVulkanBridge::ResolveHybridFrameToCpu() {
	if ( !initialized || !hybridFrameDirty ) {
		return false;
	}
	if ( !EnsureFrameBuffer() ||
		 !EnsureHybridBuffers( hybridWidth, hybridHeight, hybridTextureCount, hybridTextureTexelCount, Max( 1, hybridLightCount ), Max( 1, hybridLightTileCount ), Max( 1, hybridLightIndexCount ) ) ||
		 !EnsureUploadBuffer( frameWidth, frameHeight ) ) {
		return false;
	}

	UpdateHybridDescriptorSet();
	if ( hybridOverlayDirty ) {
		if ( !EnsureOverlayBuffers( hybridOverlayTris.Num() ) ||
			 !Ensure2DBuffers( Max( 1, hybrid2DSourcePixels.Num() ) ) ) {
			return false;
		}

		const swHybridOverlayTri_t dummyTri = {};
		const VkDeviceSize triSize = static_cast<VkDeviceSize>( Max( 1, hybridOverlayTris.Num() ) ) * sizeof( swHybridOverlayTri_t );
		const void *triData = hybridOverlayTris.Num() > 0 ? hybridOverlayTris.Ptr() : &dummyTri;
		if ( !UploadBuffer( hybridOverlayTriBuffer, triData, triSize, "vkMapMemory overlay tris failed" ) ) {
			return false;
		}
		if ( hybrid2DSourcePixels.Num() > 0 ) {
			const VkDeviceSize sourceSize = static_cast<VkDeviceSize>( hybrid2DSourcePixels.Num() ) * sizeof( uint32_t );
			if ( !UploadBuffer( hybrid2DSourceBuffer, hybrid2DSourcePixels.Ptr(), sourceSize, "vkMapMemory 2D source failed" ) ) {
				return false;
			}
		}
	}

	WaitForFrameContext( activeFrameContext );
	LoadFrameContext( activeFrameContext );
	vkResetFences( device, 1, &inFlightFence );
	vkResetCommandBuffer( commandBuffer, 0 );

	VkCommandBufferBeginInfo beginInfo;
	memset( &beginInfo, 0, sizeof( beginInfo ) );
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if ( vkBeginCommandBuffer( commandBuffer, &beginInfo ) != VK_SUCCESS ) {
		LogFailure( "vkBeginCommandBuffer hybrid readback failed" );
		return false;
	}

	const bool directHybridOverlay = hybridOverlayDirty;
	if ( !RecordHybridCompositeCommands( directHybridOverlay ) ) {
		return false;
	}
	if ( directHybridOverlay ) {
		if ( hybridOverlayTris.Num() > 0 ) {
			UpdateOverlayDescriptorSet( true );
			if ( !RecordOverlayRasterCommands( true ) ) {
				return false;
			}
		}
		if ( hybrid2DSourcePixels.Num() > 0 ) {
			Update2DDescriptorSet( true );
			if ( !Record2DCompositeCommands( true ) ) {
				return false;
			}
		}
		RecordHybridFrameTransferBarrier();
	}

	VkBufferCopy copyRegion;
	memset( &copyRegion, 0, sizeof( copyRegion ) );
	copyRegion.size = static_cast<VkDeviceSize>( frameWidth ) * static_cast<VkDeviceSize>( frameHeight ) * sizeof( uint32_t );
	vkCmdCopyBuffer( commandBuffer, hybridFrameBuffer.buffer, uploadBuffer, 1, &copyRegion );

	if ( vkEndCommandBuffer( commandBuffer ) != VK_SUCCESS ) {
		LogFailure( "vkEndCommandBuffer hybrid readback failed" );
		return false;
	}

	VkSubmitInfo submitInfo;
	memset( &submitInfo, 0, sizeof( submitInfo ) );
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;
	if ( vkQueueSubmit( queue, 1, &submitInfo, inFlightFence ) != VK_SUCCESS ) {
		LogFailure( "vkQueueSubmit hybrid readback failed" );
		return false;
	}
	vkWaitForFences( device, 1, &inFlightFence, VK_TRUE, UINT64_MAX );

	void *mapped = NULL;
	if ( vkMapMemory( device, uploadMemory, 0, uploadBufferSize, 0, &mapped ) != VK_SUCCESS ) {
		LogFailure( "vkMapMemory hybrid readback failed" );
		return false;
	}

	const unsigned int *uploadPixels = reinterpret_cast<const unsigned int *>( mapped );
	for ( int y = 0; y < frameHeight; y++ ) {
		const unsigned int *src = uploadPixels + ( frameHeight - 1 - y ) * frameWidth;
		unsigned int *dst = framePixels.Ptr() + y * frameWidth;
		memcpy( dst, src, frameWidth * sizeof( unsigned int ) );
	}
	vkUnmapMemory( device, uploadMemory );

	hybridFrameDirty = false;
	hybridOverlayDirty = false;
	hybridOverlayOnly = false;
	Clear2DJobs();
	frameBegun = true;
	frameDirty = true;
	return true;
}

bool idSoftwareVulkanBridge::ReadView( const viewDef_t *viewDef, unsigned int *bgra, int width, int height ) {
	if ( !viewDef || !bgra || width <= 0 || height <= 0 ) {
		return false;
	}
	if ( initialized && hybridFrameDirty ) {
		if ( !ResolveHybridFrameToCpu() ) {
			return false;
		}
	}
	if ( !initialized || !frameBegun || framePixels.Num() != frameWidth * frameHeight ) {
		return false;
	}

	const int viewportWidth = Max( 1, viewDef->viewport.x2 - viewDef->viewport.x1 + 1 );
	const int viewportHeight = Max( 1, viewDef->viewport.y2 - viewDef->viewport.y1 + 1 );

	for ( int y = 0; y < height; y++ ) {
		const int viewY = ( y * viewportHeight ) / height;
		const int frameY = viewDef->viewport.y1 + viewY;
		unsigned int *dst = bgra + y * width;

		if ( frameY < 0 || frameY >= frameHeight ) {
			memset( dst, 0, width * sizeof( unsigned int ) );
			continue;
		}

		for ( int x = 0; x < width; x++ ) {
			const int viewX = ( x * viewportWidth ) / width;
			const int frameX = viewDef->viewport.x1 + viewX;
			dst[x] = ( frameX >= 0 && frameX < frameWidth ) ? framePixels[frameY * frameWidth + frameX] : 0;
		}
	}
	return true;
}

bool idSoftwareVulkanBridge::Record2DCompositeCommands( bool targetFrame ) {
	if ( hybrid2DJobs.Num() <= 0 ) {
		return true;
	}
	if ( twoDDescriptorSet == VK_NULL_HANDLE || twoDPipeline == VK_NULL_HANDLE || twoDPipelineLayout == VK_NULL_HANDLE ||
		 hybrid2DSourceBuffer.buffer == VK_NULL_HANDLE || ( targetFrame ? hybridFrameBuffer.buffer : hybridOverlayBuffer.buffer ) == VK_NULL_HANDLE ) {
		return false;
	}

	VkMemoryBarrier inputBarrier;
	memset( &inputBarrier, 0, sizeof( inputBarrier ) );
	inputBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	inputBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	inputBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	vkCmdPipelineBarrier( commandBuffer,
		VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 1, &inputBarrier, 0, NULL, 0, NULL );

	vkCmdBindPipeline( commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, twoDPipeline );
	vkCmdBindDescriptorSets( commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, twoDPipelineLayout, 0, 1, &twoDDescriptorSet, 0, NULL );

	for ( int i = 0; i < hybrid2DJobs.Num(); i++ ) {
		const swVk2DJob_t &job = hybrid2DJobs[i];
		swVk2DPushConstants_t push;
		memset( &push, 0, sizeof( push ) );
		push.src[0] = static_cast<uint32_t>( job.sourceOffset );
		push.src[1] = static_cast<uint32_t>( job.sourceWidth );
		push.src[2] = static_cast<uint32_t>( job.sourceHeight );
		push.src[3] = 0u;
		push.frame[0] = static_cast<uint32_t>( frameWidth );
		push.frame[1] = static_cast<uint32_t>( frameHeight );
		push.viewport[0] = job.viewportX;
		push.viewport[1] = job.viewportY;
		push.viewport[2] = job.presentWidth;
		push.viewport[3] = job.presentHeight;
		push.targetFrame = targetFrame ? 1u : 0u;

		vkCmdPushConstants( commandBuffer, twoDPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( push ), &push );
		const uint32_t dispatchWidth = ( i == 0 ) ? static_cast<uint32_t>( frameWidth ) : static_cast<uint32_t>( job.presentWidth );
		const uint32_t dispatchHeight = ( i == 0 ) ? static_cast<uint32_t>( frameHeight ) : static_cast<uint32_t>( job.presentHeight );
		vkCmdDispatch( commandBuffer,
			SWVkDispatchGroups( dispatchWidth, SW_VK_LINEAR_GROUP_SIZE_X ),
			SWVkDispatchGroups( dispatchHeight, SW_VK_LINEAR_GROUP_SIZE_Y ), 1 );

		VkMemoryBarrier jobBarrier;
		memset( &jobBarrier, 0, sizeof( jobBarrier ) );
		jobBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		jobBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		jobBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		vkCmdPipelineBarrier( commandBuffer,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			0, 1, &jobBarrier, 0, NULL, 0, NULL );
	}
	return true;
}

bool idSoftwareVulkanBridge::BuildOverlayTileBins( bool targetFrame, int &dispatchX0, int &dispatchY0, int &dispatchX1, int &dispatchY1 ) {
	dispatchX0 = targetFrame ? frameWidth : 0;
	dispatchY0 = targetFrame ? frameHeight : 0;
	dispatchX1 = targetFrame ? 0 : frameWidth;
	dispatchY1 = targetFrame ? 0 : frameHeight;

	if ( frameWidth <= 0 || frameHeight <= 0 ) {
		return false;
	}

	const int tileCountX = static_cast<int>( SWVkDispatchGroups( static_cast<uint32_t>( frameWidth ), SW_VK_OVERLAY_TILE_WIDTH ) );
	const int tileCountY = static_cast<int>( SWVkDispatchGroups( static_cast<uint32_t>( frameHeight ), SW_VK_OVERLAY_TILE_HEIGHT ) );
	const int tileCount = tileCountX * tileCountY;
	if ( tileCount <= 0 ) {
		return false;
	}

	hybridOverlayTiles.SetNum( tileCount, false );
	idList<uint32_t> tileCounts;
	tileCounts.SetNum( tileCount, false );
	memset( tileCounts.Ptr(), 0, tileCounts.Num() * sizeof( tileCounts[0] ) );

	int totalRefs = 0;
	bool anyVisible = false;
	for ( int triIndex = 0; triIndex < hybridOverlayTris.Num(); triIndex++ ) {
		const swHybridOverlayTri_t &tri = hybridOverlayTris[triIndex];
		int x0, y0, x1, y1;
		if ( !SWVkOverlayTriFrameBounds( tri, frameWidth, frameHeight, x0, y0, x1, y1 ) ) {
			continue;
		}

		anyVisible = true;
		if ( targetFrame ) {
			dispatchX0 = Min( dispatchX0, x0 );
			dispatchY0 = Min( dispatchY0, y0 );
			dispatchX1 = Max( dispatchX1, x1 );
			dispatchY1 = Max( dispatchY1, y1 );
		}

		const int tileX0 = Max( 0, Min( tileCountX - 1, x0 / static_cast<int>( SW_VK_OVERLAY_TILE_WIDTH ) ) );
		const int tileY0 = Max( 0, Min( tileCountY - 1, y0 / static_cast<int>( SW_VK_OVERLAY_TILE_HEIGHT ) ) );
		const int tileX1 = Max( tileX0 + 1, Min( tileCountX, ( x1 + static_cast<int>( SW_VK_OVERLAY_TILE_WIDTH ) - 1 ) / static_cast<int>( SW_VK_OVERLAY_TILE_WIDTH ) ) );
		const int tileY1 = Max( tileY0 + 1, Min( tileCountY, ( y1 + static_cast<int>( SW_VK_OVERLAY_TILE_HEIGHT ) - 1 ) / static_cast<int>( SW_VK_OVERLAY_TILE_HEIGHT ) ) );
		for ( int tileY = tileY0; tileY < tileY1; tileY++ ) {
			for ( int tileX = tileX0; tileX < tileX1; tileX++ ) {
				tileCounts[tileY * tileCountX + tileX]++;
				totalRefs++;
				if ( totalRefs > SW_VK_OVERLAY_TILE_MAX_REFS ) {
					return false;
				}
			}
		}
	}

	if ( targetFrame && !anyVisible ) {
		hybridOverlayTiles.SetNum( 0, false );
		hybridOverlayTileIndices.SetNum( 0, false );
		dispatchX0 = dispatchY0 = dispatchX1 = dispatchY1 = 0;
		return true;
	}

	uint32_t offset = 0;
	for ( int tile = 0; tile < tileCount; tile++ ) {
		hybridOverlayTiles[tile].offset = offset;
		hybridOverlayTiles[tile].count = tileCounts[tile];
		hybridOverlayTiles[tile].pad[0] = 0;
		hybridOverlayTiles[tile].pad[1] = 0;
		offset += tileCounts[tile];
	}

	hybridOverlayTileIndices.SetNum( Max( 1, totalRefs ), false );
	if ( totalRefs <= 0 ) {
		hybridOverlayTileIndices[0] = 0;
		return true;
	}

	idList<uint32_t> tileCursors;
	tileCursors.SetNum( tileCount, false );
	for ( int tile = 0; tile < tileCount; tile++ ) {
		tileCursors[tile] = hybridOverlayTiles[tile].offset;
	}

	for ( int triIndex = 0; triIndex < hybridOverlayTris.Num(); triIndex++ ) {
		const swHybridOverlayTri_t &tri = hybridOverlayTris[triIndex];
		int x0, y0, x1, y1;
		if ( !SWVkOverlayTriFrameBounds( tri, frameWidth, frameHeight, x0, y0, x1, y1 ) ) {
			continue;
		}

		const int tileX0 = Max( 0, Min( tileCountX - 1, x0 / static_cast<int>( SW_VK_OVERLAY_TILE_WIDTH ) ) );
		const int tileY0 = Max( 0, Min( tileCountY - 1, y0 / static_cast<int>( SW_VK_OVERLAY_TILE_HEIGHT ) ) );
		const int tileX1 = Max( tileX0 + 1, Min( tileCountX, ( x1 + static_cast<int>( SW_VK_OVERLAY_TILE_WIDTH ) - 1 ) / static_cast<int>( SW_VK_OVERLAY_TILE_WIDTH ) ) );
		const int tileY1 = Max( tileY0 + 1, Min( tileCountY, ( y1 + static_cast<int>( SW_VK_OVERLAY_TILE_HEIGHT ) - 1 ) / static_cast<int>( SW_VK_OVERLAY_TILE_HEIGHT ) ) );
		for ( int tileY = tileY0; tileY < tileY1; tileY++ ) {
			for ( int tileX = tileX0; tileX < tileX1; tileX++ ) {
				const int tile = tileY * tileCountX + tileX;
				hybridOverlayTileIndices[tileCursors[tile]++] = static_cast<uint32_t>( triIndex );
			}
		}
	}
	return true;
}

bool idSoftwareVulkanBridge::RecordOverlayTileCompositeCommands( bool targetFrame ) {
	if ( overlayTileDescriptorSet == VK_NULL_HANDLE || overlayTilePipeline == VK_NULL_HANDLE || overlayTilePipelineLayout == VK_NULL_HANDLE ) {
		return false;
	}

	int dispatchX0, dispatchY0, dispatchX1, dispatchY1;
	if ( !BuildOverlayTileBins( targetFrame, dispatchX0, dispatchY0, dispatchX1, dispatchY1 ) ) {
		return false;
	}
	if ( dispatchX1 <= dispatchX0 || dispatchY1 <= dispatchY0 ) {
		return true;
	}

	const swVkOverlayTile_t dummyTile = {};
	const uint32_t dummyIndex = 0;
	const int tileCount = Max( 1, hybridOverlayTiles.Num() );
	const int tileIndexCount = Max( 1, hybridOverlayTileIndices.Num() );
	if ( !EnsureOverlayTileBuffers( tileCount, tileIndexCount ) ) {
		return false;
	}
	if ( !UploadBuffer( hybridOverlayTileBuffer, hybridOverlayTiles.Num() > 0 ? hybridOverlayTiles.Ptr() : &dummyTile,
		 static_cast<VkDeviceSize>( tileCount ) * sizeof( swVkOverlayTile_t ), "vkMapMemory overlay tiles failed" ) ) {
		return false;
	}
	if ( !UploadBuffer( hybridOverlayTileIndexBuffer, hybridOverlayTileIndices.Num() > 0 ? hybridOverlayTileIndices.Ptr() : &dummyIndex,
		 static_cast<VkDeviceSize>( tileIndexCount ) * sizeof( uint32_t ), "vkMapMemory overlay tile indices failed" ) ) {
		return false;
	}

	UpdateOverlayTileDescriptorSet( targetFrame );

	VkMemoryBarrier inputBarrier;
	memset( &inputBarrier, 0, sizeof( inputBarrier ) );
	inputBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	inputBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	inputBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	vkCmdPipelineBarrier( commandBuffer,
		VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 1, &inputBarrier, 0, NULL, 0, NULL );

	vkCmdBindPipeline( commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, overlayTilePipeline );
	vkCmdBindDescriptorSets( commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, overlayTilePipelineLayout, 0, 1, &overlayTileDescriptorSet, 0, NULL );

	swVkOverlayTilePushConstants_t push;
	memset( &push, 0, sizeof( push ) );
	push.frame[0] = static_cast<uint32_t>( Max( 1, frameWidth ) );
	push.frame[1] = static_cast<uint32_t>( Max( 1, frameHeight ) );
	push.dispatch[0] = dispatchX0;
	push.dispatch[1] = dispatchY0;
	push.dispatch[2] = dispatchX1 - dispatchX0;
	push.dispatch[3] = dispatchY1 - dispatchY0;
	push.tileGrid[0] = SW_VK_OVERLAY_TILE_WIDTH;
	push.tileGrid[1] = SW_VK_OVERLAY_TILE_HEIGHT;
	push.tileGrid[2] = SWVkDispatchGroups( static_cast<uint32_t>( frameWidth ), SW_VK_OVERLAY_TILE_WIDTH );
	push.tileGrid[3] = SWVkDispatchGroups( static_cast<uint32_t>( frameHeight ), SW_VK_OVERLAY_TILE_HEIGHT );
	push.textureCount = static_cast<uint32_t>( Max( 0, hybridTextureCount ) );
	push.targetFrame = targetFrame ? 1u : 0u;

	vkCmdPushConstants( commandBuffer, overlayTilePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( push ), &push );
	vkCmdDispatch( commandBuffer,
		SWVkDispatchGroups( static_cast<uint32_t>( dispatchX1 - dispatchX0 ), SW_VK_OVERLAY_GROUP_SIZE_X ),
		SWVkDispatchGroups( static_cast<uint32_t>( dispatchY1 - dispatchY0 ), SW_VK_OVERLAY_GROUP_SIZE_Y ), 1 );
	SWVkComputeMemoryBarrier( commandBuffer, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT );
	return true;
}

bool idSoftwareVulkanBridge::RecordOverlayRasterCommands( bool targetFrame ) {
	if ( !hybridOverlayDirty ) {
		return true;
	}
	if ( hybridOverlayTris.Num() <= 0 ) {
		return true;
	}
	if ( overlayDescriptorSet == VK_NULL_HANDLE || overlayPipeline == VK_NULL_HANDLE || overlayPipelineLayout == VK_NULL_HANDLE ||
		 hybridOverlayTriBuffer.buffer == VK_NULL_HANDLE || ( targetFrame ? hybridFrameBuffer.buffer : hybridOverlayBuffer.buffer ) == VK_NULL_HANDLE ) {
		return false;
	}
	if ( RecordOverlayTileCompositeCommands( targetFrame ) ) {
		return true;
	}

	VkMemoryBarrier inputBarrier;
	memset( &inputBarrier, 0, sizeof( inputBarrier ) );
	inputBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	inputBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	inputBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	vkCmdPipelineBarrier( commandBuffer,
		VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 1, &inputBarrier, 0, NULL, 0, NULL );

	vkCmdBindPipeline( commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, overlayPipeline );
	vkCmdBindDescriptorSets( commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, overlayPipelineLayout, 0, 1, &overlayDescriptorSet, 0, NULL );

	swVkOverlayPushConstants_t push;
	if ( !targetFrame ) {
		memset( &push, 0, sizeof( push ) );
		push.tri[0] = 0;
		push.tri[1] = 1;
		push.tri[2] = static_cast<uint32_t>( Max( 1, frameWidth ) );
		push.tri[3] = static_cast<uint32_t>( Max( 1, frameHeight ) );
		push.dims[0] = static_cast<uint32_t>( Max( 1, hybridOverlaySourceWidth ) );
		push.dims[1] = static_cast<uint32_t>( Max( 1, hybridOverlaySourceHeight ) );
		push.dims[2] = static_cast<uint32_t>( Max( 1, frameWidth ) );
		push.dims[3] = static_cast<uint32_t>( Max( 1, frameHeight ) );
		push.dispatch[0] = 0;
		push.dispatch[1] = 0;
		push.dispatch[2] = frameWidth;
		push.dispatch[3] = frameHeight;
		push.textureCount = static_cast<uint32_t>( Max( 0, hybridTextureCount ) );
		push.targetFrame = 0u;
		vkCmdPushConstants( commandBuffer, overlayPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( push ), &push );
		vkCmdDispatch( commandBuffer,
			SWVkDispatchGroups( static_cast<uint32_t>( frameWidth ), SW_VK_OVERLAY_GROUP_SIZE_X ),
			SWVkDispatchGroups( static_cast<uint32_t>( frameHeight ), SW_VK_OVERLAY_GROUP_SIZE_Y ), 1 );

		SWVkComputeMemoryBarrier( commandBuffer, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT );
	}

	bool pendingOverlayWrites = false;
	int dirtyX0 = 0;
	int dirtyY0 = 0;
	int dirtyX1 = 0;
	int dirtyY1 = 0;

	for ( int i = 0; i < hybridOverlayTris.Num(); i++ ) {
		const swHybridOverlayTri_t &tri = hybridOverlayTris[i];
		const int sourceWidth = Max( 1, tri.source[0] );
		const int sourceHeight = Max( 1, tri.source[1] );
		const int viewportX = tri.viewport[0];
		const int viewportY = tri.viewport[1];
		const int presentW = Max( 1, tri.viewport[2] );
		const int presentH = Max( 1, tri.viewport[3] );
		const int minFrameX = viewportX + ( tri.bounds[0] * presentW ) / sourceWidth;
		const int minFrameY = viewportY + ( tri.bounds[1] * presentH ) / sourceHeight;
		const int maxFrameX = viewportX + ( ( tri.bounds[2] + 1 ) * presentW + sourceWidth - 1 ) / sourceWidth;
		const int maxFrameY = viewportY + ( ( tri.bounds[3] + 1 ) * presentH + sourceHeight - 1 ) / sourceHeight;
		const int x0 = Max( 0, Min( frameWidth - 1, minFrameX ) );
		const int y0 = Max( 0, Min( frameHeight - 1, minFrameY ) );
		const int x1 = Max( 0, Min( frameWidth, maxFrameX ) );
		const int y1 = Max( 0, Min( frameHeight, maxFrameY ) );
		if ( x1 <= x0 || y1 <= y0 ) {
			continue;
		}

		if ( pendingOverlayWrites && SWVkRectsOverlap( dirtyX0, dirtyY0, dirtyX1, dirtyY1, x0, y0, x1, y1 ) ) {
			SWVkComputeMemoryBarrier( commandBuffer, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT );
			pendingOverlayWrites = false;
		}

		memset( &push, 0, sizeof( push ) );
		push.tri[0] = static_cast<uint32_t>( i );
		push.tri[1] = 0;
		push.tri[2] = static_cast<uint32_t>( x1 - x0 );
		push.tri[3] = static_cast<uint32_t>( y1 - y0 );
		push.dims[0] = static_cast<uint32_t>( sourceWidth );
		push.dims[1] = static_cast<uint32_t>( sourceHeight );
		push.dims[2] = static_cast<uint32_t>( Max( 1, frameWidth ) );
		push.dims[3] = static_cast<uint32_t>( Max( 1, frameHeight ) );
		push.viewport[0] = viewportX;
		push.viewport[1] = viewportY;
		push.viewport[2] = presentW;
		push.viewport[3] = presentH;
		push.dispatch[0] = x0;
		push.dispatch[1] = y0;
		push.dispatch[2] = x1 - x0;
		push.dispatch[3] = y1 - y0;
		push.textureCount = static_cast<uint32_t>( Max( 0, hybridTextureCount ) );
		push.targetFrame = targetFrame ? 1u : 0u;

		vkCmdPushConstants( commandBuffer, overlayPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( push ), &push );
		vkCmdDispatch( commandBuffer,
			SWVkDispatchGroups( static_cast<uint32_t>( x1 - x0 ), SW_VK_OVERLAY_GROUP_SIZE_X ),
			SWVkDispatchGroups( static_cast<uint32_t>( y1 - y0 ), SW_VK_OVERLAY_GROUP_SIZE_Y ), 1 );

		if ( pendingOverlayWrites ) {
			dirtyX0 = Min( dirtyX0, x0 );
			dirtyY0 = Min( dirtyY0, y0 );
			dirtyX1 = Max( dirtyX1, x1 );
			dirtyY1 = Max( dirtyY1, y1 );
		} else {
			dirtyX0 = x0;
			dirtyY0 = y0;
			dirtyX1 = x1;
			dirtyY1 = y1;
			pendingOverlayWrites = true;
		}
	}
	if ( pendingOverlayWrites ) {
		SWVkComputeMemoryBarrier( commandBuffer, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT );
	}
	return true;
}

bool idSoftwareVulkanBridge::RecordHybridCompositeCommands( bool outputForCompute ) {
	int variant = static_cast<int>( hybridCompositeVariant );
	if ( variant < 0 || variant >= SW_VK_HYBRID_VARIANT_COUNT || hybridPipelines[variant] == VK_NULL_HANDLE ) {
		variant = static_cast<int>( SW_VK_HYBRID_VARIANT_GENERIC_NOSHADOW );
	}

	if ( !hybridFrameDirty ||
		 hybridDescriptorSet == VK_NULL_HANDLE || hybridUpscaleDescriptorSet == VK_NULL_HANDLE ||
		 hybridPipelines[variant] == VK_NULL_HANDLE || twoDPipeline == VK_NULL_HANDLE || twoDPipelineLayout == VK_NULL_HANDLE ||
		 hybridLitBuffer.buffer == VK_NULL_HANDLE || hybridOverlayBuffer.buffer == VK_NULL_HANDLE ||
		 hybridFrameBuffer.buffer == VK_NULL_HANDLE ) {
		return false;
	}

	VkMemoryBarrier inputBarrier;
	memset( &inputBarrier, 0, sizeof( inputBarrier ) );
	inputBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	inputBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
	inputBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	vkCmdPipelineBarrier( commandBuffer,
		VK_PIPELINE_STAGE_HOST_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 1, &inputBarrier, 0, NULL, 0, NULL );

	const int dispatchX0 = Max( 0, hybridViewportX );
	const int dispatchY0 = Max( 0, hybridViewportY );
	const int dispatchX1 = Max( dispatchX0, Min( frameWidth, hybridViewportX + hybridPresentWidth ) );
	const int dispatchY1 = Max( dispatchY0, Min( frameHeight, hybridViewportY + hybridPresentHeight ) );
	const int viewDispatchX0 = Max( 0, dispatchX0 - hybridViewportX );
	const int viewDispatchY0 = Max( 0, dispatchY0 - hybridViewportY );
	const int viewDispatchX1 = Max( viewDispatchX0, Min( hybridPresentWidth, dispatchX1 - hybridViewportX ) );
	const int viewDispatchY1 = Max( viewDispatchY0, Min( hybridPresentHeight, dispatchY1 - hybridViewportY ) );
	const int sourceDispatchX0 = hybridOverlayOnly ? 0 : Min( hybridWidth, ( viewDispatchX0 * hybridWidth ) / hybridPresentWidth );
	const int sourceDispatchY0 = hybridOverlayOnly ? 0 : Min( hybridHeight, ( viewDispatchY0 * hybridHeight ) / hybridPresentHeight );
	const int sourceDispatchX1 = hybridOverlayOnly ? 0 : Min( hybridWidth, ( viewDispatchX1 * hybridWidth + hybridPresentWidth - 1 ) / hybridPresentWidth );
	const int sourceDispatchY1 = hybridOverlayOnly ? 0 : Min( hybridHeight, ( viewDispatchY1 * hybridHeight + hybridPresentHeight - 1 ) / hybridPresentHeight );
	const uint32_t outputDispatchWidth = static_cast<uint32_t>( dispatchX1 - dispatchX0 );
	const uint32_t outputDispatchHeight = static_cast<uint32_t>( dispatchY1 - dispatchY0 );
	const uint32_t dispatchWidth = static_cast<uint32_t>( Max( 0, sourceDispatchX1 - sourceDispatchX0 ) );
	const uint32_t dispatchHeight = static_cast<uint32_t>( Max( 0, sourceDispatchY1 - sourceDispatchY0 ) );
	const bool dispatchComposite = dispatchWidth != 0u && dispatchHeight != 0u;
	const bool fullFrameComposite = dispatchComposite &&
		dispatchX0 == 0 && dispatchY0 == 0 &&
		outputDispatchWidth == static_cast<uint32_t>( frameWidth ) &&
		outputDispatchHeight == static_cast<uint32_t>( frameHeight );

	if ( !fullFrameComposite ) {
		const VkDeviceSize frameSize = static_cast<VkDeviceSize>( frameWidth ) * static_cast<VkDeviceSize>( frameHeight ) * sizeof( uint32_t );
		vkCmdFillBuffer( commandBuffer, hybridFrameBuffer.buffer, 0, frameSize, 0xff000000u );

		VkMemoryBarrier clearBarrier;
		memset( &clearBarrier, 0, sizeof( clearBarrier ) );
		clearBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		clearBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		clearBarrier.dstAccessMask = !dispatchComposite ?
			( outputForCompute ? ( VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT ) : VK_ACCESS_TRANSFER_READ_BIT ) :
			VK_ACCESS_SHADER_WRITE_BIT;
		const VkPipelineStageFlags clearDstStage = dispatchComposite || outputForCompute ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT;
		vkCmdPipelineBarrier( commandBuffer,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			clearDstStage,
			0, 1, &clearBarrier, 0, NULL, 0, NULL );
	}

	if ( !dispatchComposite ) {
		return true;
	}

	swVkHybridPushConstants_t push;
	memset( &push, 0, sizeof( push ) );
	push.dims[0] = static_cast<uint32_t>( hybridWidth );
	push.dims[1] = static_cast<uint32_t>( hybridHeight );
	push.dims[2] = static_cast<uint32_t>( frameWidth );
	push.dims[3] = static_cast<uint32_t>( frameHeight );
	push.viewport[0] = hybridViewportX;
	push.viewport[1] = hybridViewportY;
	push.viewport[2] = hybridPresentWidth;
	push.viewport[3] = hybridPresentHeight;
	push.dispatch[0] = sourceDispatchX0;
	push.dispatch[1] = sourceDispatchY0;
	push.dispatch[2] = static_cast<int32_t>( dispatchWidth );
	push.dispatch[3] = static_cast<int32_t>( dispatchHeight );
	push.debugView = static_cast<uint32_t>( hybridDebugView );
	push.textureCount = static_cast<uint32_t>( Max( 0, hybridTextureCount ) );
	push.overlayEnabled = 0u;
	push.lightCount = static_cast<uint32_t>( Max( 0, hybridLightCount ) );
	push.shadowEnabled = hybridShadowEnabled ? 1u : 0u;
	push.overlayOnly = hybridOverlayOnly ? 1u : 0u;
	push.shadowSamples = static_cast<uint32_t>( idMath::ClampInt( 1, 9, r_softwareRayQueryShadowSamples.GetInteger() ) );
	push.shadowRadius = Max( 0.0f, r_softwareRayQueryShadowRadius.GetFloat() );
	push.viewOrigin[0] = hybridViewOrigin[0];
	push.viewOrigin[1] = hybridViewOrigin[1];
	push.viewOrigin[2] = hybridViewOrigin[2];
	push.viewOrigin[3] = 1.0f;

	vkCmdBindPipeline( commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, hybridPipelines[variant] );
	vkCmdBindDescriptorSets( commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, hybridPipelineLayout, 0, 1, &hybridDescriptorSet, 0, NULL );
	vkCmdPushConstants( commandBuffer, hybridPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( push ), &push );
	vkCmdDispatch( commandBuffer,
		SWVkDispatchGroups( dispatchWidth, SW_VK_LINEAR_GROUP_SIZE_X ),
		SWVkDispatchGroups( dispatchHeight, SW_VK_LINEAR_GROUP_SIZE_Y ), 1 );

	SWVkComputeMemoryBarrier( commandBuffer, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT );
	UpdateHybridUpscaleDescriptorSet();

	swVk2DPushConstants_t upscalePush;
	memset( &upscalePush, 0, sizeof( upscalePush ) );
	upscalePush.src[0] = 0u;
	upscalePush.src[1] = static_cast<uint32_t>( hybridWidth );
	upscalePush.src[2] = static_cast<uint32_t>( hybridHeight );
	upscalePush.src[3] = 0u;
	upscalePush.frame[0] = static_cast<uint32_t>( frameWidth );
	upscalePush.frame[1] = static_cast<uint32_t>( frameHeight );
	upscalePush.viewport[0] = hybridViewportX;
	upscalePush.viewport[1] = hybridViewportY;
	upscalePush.viewport[2] = hybridPresentWidth;
	upscalePush.viewport[3] = hybridPresentHeight;
	upscalePush.targetFrame = 1u;

	vkCmdBindPipeline( commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, twoDPipeline );
	vkCmdBindDescriptorSets( commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, twoDPipelineLayout, 0, 1, &hybridUpscaleDescriptorSet, 0, NULL );
	vkCmdPushConstants( commandBuffer, twoDPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof( upscalePush ), &upscalePush );
	vkCmdDispatch( commandBuffer,
		SWVkDispatchGroups( static_cast<uint32_t>( hybridPresentWidth ), SW_VK_LINEAR_GROUP_SIZE_X ),
		SWVkDispatchGroups( static_cast<uint32_t>( hybridPresentHeight ), SW_VK_LINEAR_GROUP_SIZE_Y ), 1 );

	VkMemoryBarrier outputBarrier;
	memset( &outputBarrier, 0, sizeof( outputBarrier ) );
	outputBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	outputBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	outputBarrier.dstAccessMask = outputForCompute ? ( VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT ) : VK_ACCESS_TRANSFER_READ_BIT;
	vkCmdPipelineBarrier( commandBuffer,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		outputForCompute ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 1, &outputBarrier, 0, NULL, 0, NULL );
	return true;
}

void idSoftwareVulkanBridge::RecordHybridFrameTransferBarrier() {
	VkMemoryBarrier outputBarrier;
	memset( &outputBarrier, 0, sizeof( outputBarrier ) );
	outputBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	outputBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	outputBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	vkCmdPipelineBarrier( commandBuffer,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 1, &outputBarrier, 0, NULL, 0, NULL );
}

bool idSoftwareVulkanBridge::PresentFrame() {
	if ( !initialized || !frameDirty ) {
		return false;
	}

	swVkPresentPerfStats_t perf;
	perf.Clear();
	perf.enabled = r_showSoftwarePerf.GetInteger() != 0;
	swVkScopedPresentPerfFrame perfFrame( perf );

	const bool useHybridFrame = hybridFrameDirty;
	const bool directHybridOverlay = useHybridFrame && hybridOverlayDirty;
	const bool timestampActive = timestampQuerySupported && timestampQueryPool != VK_NULL_HANDLE && r_showSoftwarePerf.GetInteger() != 0;
	perf.useHybridFrame = useHybridFrame;
	perf.directHybridOverlay = directHybridOverlay;
	perf.hybridWidth = useHybridFrame ? hybridWidth : frameWidth;
	perf.hybridHeight = useHybridFrame ? hybridHeight : frameHeight;
	perf.overlayTris = hybridOverlayTris.Num();
	perf.sourcePixels = hybrid2DSourcePixels.Num();

	{
		swVkScopedPerfTimer timer( perf, perf.prepMsec );
		if ( !EnsureFrameBuffer() ) {
			return false;
		}
		if ( useHybridFrame ) {
			if ( !EnsureHybridBuffers( hybridWidth, hybridHeight, hybridTextureCount, hybridTextureTexelCount, Max( 1, hybridLightCount ), Max( 1, hybridLightTileCount ), Max( 1, hybridLightIndexCount ) ) ) {
				return false;
			}
			UpdateHybridDescriptorSet();
			if ( hybridOverlayDirty ) {
				if ( !EnsureOverlayBuffers( hybridOverlayTris.Num() ) ) {
					return false;
				}
				if ( !Ensure2DBuffers( Max( 1, hybrid2DSourcePixels.Num() ) ) ) {
					return false;
				}
			}
		} else if ( !EnsureUploadBuffer( frameWidth, frameHeight ) ) {
			return false;
		}
	}
	perf.frameWidth = frameWidth;
	perf.frameHeight = frameHeight;
	if ( !useHybridFrame ) {
		perf.hybridWidth = frameWidth;
		perf.hybridHeight = frameHeight;
	}

	swVkGpuTimestampFrame_t submitTimestamps;
	submitTimestamps.Clear();
	submitTimestamps.useHybridFrame = useHybridFrame;
	submitTimestamps.directHybridOverlay = directHybridOverlay;
	submitTimestamps.frameWidth = frameWidth;
	submitTimestamps.frameHeight = frameHeight;
	submitTimestamps.hybridWidth = perf.hybridWidth;
	submitTimestamps.hybridHeight = perf.hybridHeight;
	submitTimestamps.overlayTris = hybridOverlayTris.Num();
	submitTimestamps.sourcePixels = hybrid2DSourcePixels.Num();

	{
		swVkScopedPerfTimer timer( perf, perf.waitMsec );
		WaitForFrameContext( activeFrameContext );
		LoadFrameContext( activeFrameContext );
	}
	if ( timestampQueryPending ) {
		if ( r_showSoftwarePerf.GetInteger() != 0 ) {
			ReadTimestampQueries();
		} else {
			timestampQueryPending = false;
			for ( int i = 0; i < SW_VK_FRAMES_IN_FLIGHT; i++ ) {
				frameContexts[i].timestampQueryPending = false;
			}
		}
	}

	{
		swVkScopedPerfTimer timer( perf, perf.uploadMsec );
		if ( useHybridFrame && hybridOverlayDirty ) {
			const swHybridOverlayTri_t dummyTri = {};
			const VkDeviceSize triSize = static_cast<VkDeviceSize>( Max( 1, hybridOverlayTris.Num() ) ) * sizeof( swHybridOverlayTri_t );
			const void *triData = hybridOverlayTris.Num() > 0 ? hybridOverlayTris.Ptr() : &dummyTri;
			if ( !UploadBuffer( hybridOverlayTriBuffer, triData, triSize, "vkMapMemory overlay tris failed" ) ) {
				return false;
			}
			if ( hybrid2DSourcePixels.Num() > 0 ) {
				const VkDeviceSize sourceSize = static_cast<VkDeviceSize>( hybrid2DSourcePixels.Num() ) * sizeof( uint32_t );
				if ( !UploadBuffer( hybrid2DSourceBuffer, hybrid2DSourcePixels.Ptr(), sourceSize, "vkMapMemory 2D source failed" ) ) {
					return false;
				}
			}
		} else if ( !useHybridFrame ) {
			void *mapped = NULL;
			if ( vkMapMemory( device, uploadMemory, 0, uploadBufferSize, 0, &mapped ) != VK_SUCCESS ) {
				LogFailure( "vkMapMemory failed" );
				return false;
			}

			unsigned int *uploadPixels = reinterpret_cast<unsigned int *>( mapped );
			for ( int y = 0; y < frameHeight; y++ ) {
				const unsigned int *src = framePixels.Ptr() + ( frameHeight - 1 - y ) * frameWidth;
				unsigned int *dst = uploadPixels + y * frameWidth;
				memcpy( dst, src, frameWidth * sizeof( unsigned int ) );
			}
			vkUnmapMemory( device, uploadMemory );
		}
	}

	uint32_t imageIndex = 0;
	VkResult acquireResult;
	{
		swVkScopedPerfTimer timer( perf, perf.acquireMsec );
		acquireResult = vkAcquireNextImageKHR( device, swapchain, UINT64_MAX, imageAvailableSemaphore, VK_NULL_HANDLE, &imageIndex );
	}
	if ( acquireResult == VK_ERROR_OUT_OF_DATE_KHR || acquireResult == VK_SUBOPTIMAL_KHR ) {
		vkDeviceWaitIdle( device );
		DestroySwapchain();
		if ( !CreateSwapchain() ) {
			return false;
		}
		frameBegun = false;
		frameDirty = false;
		hybridFrameDirty = false;
		hybridOverlayDirty = false;
		hybridOverlayOnly = false;
		Clear2DJobs();
		return false;
	}
	if ( acquireResult != VK_SUCCESS ) {
		LogFailure( "vkAcquireNextImageKHR failed" );
		return false;
	}

	{
		swVkScopedPerfTimer timer( perf, perf.recordMsec );
		vkResetCommandBuffer( commandBuffer, 0 );

		VkCommandBufferBeginInfo beginInfo;
		memset( &beginInfo, 0, sizeof( beginInfo ) );
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if ( vkBeginCommandBuffer( commandBuffer, &beginInfo ) != VK_SUCCESS ) {
			LogFailure( "vkBeginCommandBuffer failed" );
			return false;
		}
		if ( timestampActive ) {
			vkCmdResetQueryPool( commandBuffer, timestampQueryPool, activeFrameContext * SW_VK_GPU_TIMESTAMP_COUNT, SW_VK_GPU_TIMESTAMP_COUNT );
			WriteTimestamp( VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, SW_VK_GPU_TIMESTAMP_TOTAL_BEGIN );
			if ( !useHybridFrame ) {
				WriteTimestamp( VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, SW_VK_GPU_TIMESTAMP_HYBRID_BEGIN );
				WriteTimestamp( VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, SW_VK_GPU_TIMESTAMP_HYBRID_END );
			}
			if ( !directHybridOverlay ) {
				WriteTimestamp( VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, SW_VK_GPU_TIMESTAMP_OVERLAY_BEGIN );
				WriteTimestamp( VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, SW_VK_GPU_TIMESTAMP_OVERLAY_END );
			}
		}

		if ( useHybridFrame ) {
			WriteTimestamp( VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, SW_VK_GPU_TIMESTAMP_HYBRID_BEGIN );
			if ( !RecordHybridCompositeCommands( directHybridOverlay ) ) {
				return false;
			}
			WriteTimestamp( VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, SW_VK_GPU_TIMESTAMP_HYBRID_END );
		}
		if ( directHybridOverlay ) {
			WriteTimestamp( VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, SW_VK_GPU_TIMESTAMP_OVERLAY_BEGIN );
			if ( hybridOverlayTris.Num() > 0 ) {
				UpdateOverlayDescriptorSet( true );
				if ( !RecordOverlayRasterCommands( true ) ) {
					return false;
				}
			}
			if ( hybrid2DSourcePixels.Num() > 0 ) {
				Update2DDescriptorSet( true );
				if ( !Record2DCompositeCommands( true ) ) {
					return false;
				}
			}
			RecordHybridFrameTransferBarrier();
			WriteTimestamp( VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, SW_VK_GPU_TIMESTAMP_OVERLAY_END );
		}

		VkImageMemoryBarrier toTransfer;
		memset( &toTransfer, 0, sizeof( toTransfer ) );
		toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		toTransfer.oldLayout = swapchainLayouts[imageIndex];
		toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toTransfer.image = swapchainImages[imageIndex];
		toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		toTransfer.subresourceRange.baseMipLevel = 0;
		toTransfer.subresourceRange.levelCount = 1;
		toTransfer.subresourceRange.baseArrayLayer = 0;
		toTransfer.subresourceRange.layerCount = 1;
		toTransfer.srcAccessMask = 0;
		toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier( commandBuffer,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 0, NULL, 0, NULL, 1, &toTransfer );

		WriteTimestamp( VK_PIPELINE_STAGE_TRANSFER_BIT, SW_VK_GPU_TIMESTAMP_COPY_BEGIN );
		VkBufferImageCopy copyRegion;
		memset( &copyRegion, 0, sizeof( copyRegion ) );
		copyRegion.bufferOffset = 0;
		copyRegion.bufferRowLength = frameWidth;
		copyRegion.bufferImageHeight = frameHeight;
		copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegion.imageSubresource.mipLevel = 0;
		copyRegion.imageSubresource.baseArrayLayer = 0;
		copyRegion.imageSubresource.layerCount = 1;
		copyRegion.imageOffset.x = 0;
		copyRegion.imageOffset.y = 0;
		copyRegion.imageOffset.z = 0;
		copyRegion.imageExtent.width = frameWidth;
		copyRegion.imageExtent.height = frameHeight;
		copyRegion.imageExtent.depth = 1;
		vkCmdCopyBufferToImage( commandBuffer, useHybridFrame ? hybridFrameBuffer.buffer : uploadBuffer, swapchainImages[imageIndex],
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion );
		WriteTimestamp( VK_PIPELINE_STAGE_TRANSFER_BIT, SW_VK_GPU_TIMESTAMP_COPY_END );

		VkImageMemoryBarrier toPresent;
		memset( &toPresent, 0, sizeof( toPresent ) );
		toPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toPresent.image = swapchainImages[imageIndex];
		toPresent.subresourceRange = toTransfer.subresourceRange;
		toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		toPresent.dstAccessMask = 0;
		vkCmdPipelineBarrier( commandBuffer,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			0, 0, NULL, 0, NULL, 1, &toPresent );
		WriteTimestamp( VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, SW_VK_GPU_TIMESTAMP_TOTAL_END );

		if ( vkEndCommandBuffer( commandBuffer ) != VK_SUCCESS ) {
			LogFailure( "vkEndCommandBuffer failed" );
			return false;
		}
	}

	VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkSubmitInfo submitInfo;
	memset( &submitInfo, 0, sizeof( submitInfo ) );
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = &imageAvailableSemaphore;
	submitInfo.pWaitDstStageMask = &waitStage;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = &renderFinishedSemaphore;

	{
		swVkScopedPerfTimer timer( perf, perf.submitMsec );
		vkResetFences( device, 1, &inFlightFence );
		if ( vkQueueSubmit( queue, 1, &submitInfo, inFlightFence ) != VK_SUCCESS ) {
			LogFailure( "vkQueueSubmit failed" );
			return false;
		}
	}
	if ( timestampActive ) {
		frameContexts[activeFrameContext].timestampFrame = submitTimestamps;
		frameContexts[activeFrameContext].timestampQueryPending = true;
		timestampQueryPending = true;
	}
	frameContexts[activeFrameContext].submitted = true;
	StoreFrameContext();

	VkPresentInfoKHR presentInfo;
	memset( &presentInfo, 0, sizeof( presentInfo ) );
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = &renderFinishedSemaphore;
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = &swapchain;
	presentInfo.pImageIndices = &imageIndex;

	VkResult presentResult;
	{
		swVkScopedPerfTimer timer( perf, perf.presentMsec );
		presentResult = vkQueuePresentKHR( queue, &presentInfo );
	}
	swapchainLayouts[imageIndex] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	frameBegun = false;
	frameDirty = false;
	hybridFrameDirty = false;
	hybridOverlayDirty = false;
	hybridOverlayOnly = false;
	Clear2DJobs();

	if ( presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR ) {
		vkDeviceWaitIdle( device );
		DestroySwapchain();
		CreateSwapchain();
		return true;
	}
	if ( presentResult != VK_SUCCESS ) {
		LogFailure( "vkQueuePresentKHR failed" );
		return false;
	}
	return true;
}

bool idSoftwareVulkanBridge::PrepareRayQueryScene( const viewDef_t *viewDef ) {
	if ( !viewDef || ( session && session->IsLoadingMap() ) || !EnsureInitialized() || !rayQuerySupported ) {
		return false;
	}
	if ( HasPendingShadowJobs() ) {
		WaitForSubmittedWork();
	}

	rayQuerySceneFrame++;
	idList<swVkRayInstance_t> instances;
	const int instanceGranularity = viewDef->numDrawSurfs > 16 ? viewDef->numDrawSurfs : 16;
	instances.SetGranularity( instanceGranularity );

	for ( const viewEntity_t *vEntity = viewDef->viewEntitys; vEntity; vEntity = vEntity->next ) {
		idRenderEntityLocal *def = vEntity->entityDef;
		if ( !def || !def->parms.hModel ) {
			continue;
		}

		idRenderModel *model = def->dynamicModel ? def->dynamicModel : def->parms.hModel;
		if ( !model || model->NumSurfaces() <= 0 ) {
			continue;
		}
		if ( !def->dynamicModel && model->IsDynamicModel() != DM_STATIC ) {
			continue;
		}

		for ( int surfaceIndex = 0; surfaceIndex < model->NumSurfaces(); surfaceIndex++ ) {
			const modelSurface_t *surface = model->Surface( surfaceIndex );
			if ( !surface || !surface->geometry ) {
				continue;
			}

			const idMaterial *material = surface->shader;
			material = R_RemapShaderBySkin( material, def->parms.customSkin, def->parms.customShader );
			if ( !material ) {
				continue;
			}
			R_GlobalShaderOverride( &material );
			if ( !material ) {
				continue;
			}

			srfTriangles_t *geo = surface->geometry;
			if ( !RayQuerySurfaceCanCastShadow( geo, material, vEntity ) ) {
				continue;
			}
			if ( !RayQuerySurfaceTouchesShadowLight( viewDef, geo, material, vEntity ) ) {
				continue;
			}

			AddRayQuerySurfaceInstance( instances, geo, vEntity->modelMatrix, def );
		}
	}

	if ( instances.Num() <= 0 ) {
		WaitForSubmittedWork();
		DestroyRayQueryTlas();
		return false;
	}

	return BuildRayQueryTlas( instances );
}

bool idSoftwareVulkanBridge::BeginLightShadowMask( const viewLight_t *vLight, const idVec4 *worldPositions, int width, int height ) {
	if ( !vLight || !worldPositions || !rayQuerySceneReady || !tlas || !shadowPipeline ) {
		return false;
	}

	swVkShadowJob_t *job = AllocShadowJob();
	if ( !job || !RecordShadowJob( *job, vLight, worldPositions, width, height ) ) {
		return false;
	}

	if ( job->commandBuffer == VK_NULL_HANDLE || job->fence == VK_NULL_HANDLE || !job->pendingSubmit ) {
		job->pendingSubmit = false;
		job->submitted = false;
		return false;
	}

	vkResetFences( device, 1, &job->fence );

	VkSubmitInfo submitInfo;
	memset( &submitInfo, 0, sizeof( submitInfo ) );
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &job->commandBuffer;

	if ( vkQueueSubmit( queue, 1, &submitInfo, job->fence ) != VK_SUCCESS ) {
		LogFailure( "vkQueueSubmit shadow failed" );
		job->pendingSubmit = false;
		job->submitted = false;
		return false;
	}

	job->pendingSubmit = false;
	job->submitted = true;
	return true;
}

bool idSoftwareVulkanBridge::FinishLightShadowMask( const viewLight_t *vLight, int width, int height, unsigned char *shadowMask ) {
	if ( !vLight || !shadowMask || !rayQuerySceneReady ) {
		return false;
	}

	const uintptr_t key = ShadowLightKey( vLight );
	swVkShadowJob_t *job = NULL;
	for ( int i = 0; i < SW_VK_MAX_SHADOW_JOBS; i++ ) {
		swVkShadowJob_t &candidate = shadowJobs[i];
		if ( candidate.lightKey == key && candidate.width == width && candidate.height == height && candidate.submitted && !candidate.pendingSubmit ) {
			job = &candidate;
			break;
		}
	}
	if ( !job || job->fence == VK_NULL_HANDLE ) {
		return false;
	}

	if ( vkWaitForFences( device, 1, &job->fence, VK_TRUE, UINT64_MAX ) != VK_SUCCESS ) {
		LogFailure( "vkWaitForFences shadow failed" );
		job->submitted = false;
		return false;
	}

	const int pixelCount = width * height;
	const VkDeviceSize maskSize = static_cast<VkDeviceSize>( pixelCount ) * sizeof( uint32_t );
	void *mapped = NULL;
	if ( vkMapMemory( device, job->shadowMaskBuffer.memory, 0, maskSize, 0, &mapped ) != VK_SUCCESS ) {
		LogFailure( "vkMapMemory shadow mask read failed" );
		job->submitted = false;
		return false;
	}

	const uint32_t *gpuMask = reinterpret_cast<const uint32_t *>( mapped );
	for ( int pixel = 0; pixel < pixelCount; pixel++ ) {
		shadowMask[pixel] = static_cast<unsigned char>( gpuMask[pixel] > 255 ? 255 : gpuMask[pixel] );
	}
	vkUnmapMemory( device, job->shadowMaskBuffer.memory );
	job->submitted = false;
	return true;
}

bool idSoftwareVulkanBridge::TraceLightShadowMask( const viewLight_t *vLight, const idVec4 *worldPositions, int width, int height, unsigned char *shadowMask ) {
	if ( !BeginLightShadowMask( vLight, worldPositions, width, height ) ) {
		return false;
	}
	return FinishLightShadowMask( vLight, width, height, shadowMask );
}

bool idSoftwareVulkanBridge::RayQueryAvailable() {
	if ( session && session->IsLoadingMap() ) {
		return false;
	}
	return EnsureInitialized() && rayQuerySupported;
}

void idSoftwareVulkanBridge::DestroyUploadBuffer() {
	if ( uploadBuffer != VK_NULL_HANDLE ) {
		vkDestroyBuffer( device, uploadBuffer, NULL );
		uploadBuffer = VK_NULL_HANDLE;
	}
	if ( uploadMemory != VK_NULL_HANDLE ) {
		vkFreeMemory( device, uploadMemory, NULL );
		uploadMemory = VK_NULL_HANDLE;
	}
	uploadBufferSize = 0;
	StoreFrameContext();
}

void idSoftwareVulkanBridge::DestroyRayQueryScene() {
	DestroyRayQueryTlas();
	DestroySurfaceBlasList( rayAttachedBlas );
	for ( int i = 0; i < rayEntityTlasCache.Num(); i++ ) {
		swVkEntityTlas_t *entry = rayEntityTlasCache[i];
		if ( entry->owner && entry->owner->rayQueryTlas == entry ) {
			entry->owner->rayQueryTlas = NULL;
			entry->owner->rayQueryTlasSignature = 0;
			entry->owner->rayQueryTlasInstanceCount = 0;
			entry->owner->rayQueryTlasFrame = 0;
		}
		delete entry;
	}
	rayEntityTlasCache.Clear();
	rayQuerySceneFrame = 0;
}

void idSoftwareVulkanBridge::ClearRayQueryScene() {
	if ( !initialized || device == VK_NULL_HANDLE ) {
		rayQuerySceneReady = false;
		rayTlasSignature = 0;
		rayTlasInstanceCount = 0;
		rayQuerySceneFrame = 0;
		return;
	}
	WaitForSubmittedWork();
	DestroyRayQueryScene();
}

void idSoftwareVulkanBridge::DestroyHybridBuffers() {
	WaitForAllFrameContexts();
	for ( int i = 0; i < SW_VK_FRAMES_IN_FLIGHT; i++ ) {
		DestroyFrameContextBuffers( frameContexts[i] );
	}
	LoadFrameContext( activeFrameContext );
	DestroyBuffer( hybridTextureTexelBuffer );
	DestroyBuffer( hybridTextureInfoBuffer );
	hybridWidth = 0;
	hybridHeight = 0;
	hybridPresentWidth = 0;
	hybridPresentHeight = 0;
	hybridViewportX = 0;
	hybridViewportY = 0;
	hybridDebugView = 0;
	hybridTextureCount = 0;
	hybridTextureTexelCount = 0;
	hybridUploadedTextureGeneration = 0;
	hybridLightCount = 0;
	hybridLightTileCount = 0;
	hybridLightIndexCount = 0;
	hybridShadowEnabled = false;
	hybridCompositeVariant = SW_VK_HYBRID_VARIANT_GENERIC_NOSHADOW;
	hybridFrameDirty = false;
	hybridOverlayDirty = false;
	hybridOverlayOnly = false;
	Clear2DJobs();
}

void idSoftwareVulkanBridge::Destroy2DPipeline() {
	Clear2DJobs();

	if ( twoDPipeline != VK_NULL_HANDLE ) {
		vkDestroyPipeline( device, twoDPipeline, NULL );
		twoDPipeline = VK_NULL_HANDLE;
	}
	if ( twoDShaderModule != VK_NULL_HANDLE ) {
		vkDestroyShaderModule( device, twoDShaderModule, NULL );
		twoDShaderModule = VK_NULL_HANDLE;
	}
	if ( twoDPipelineLayout != VK_NULL_HANDLE ) {
		vkDestroyPipelineLayout( device, twoDPipelineLayout, NULL );
		twoDPipelineLayout = VK_NULL_HANDLE;
	}
	if ( twoDDescriptorPool != VK_NULL_HANDLE ) {
		vkDestroyDescriptorPool( device, twoDDescriptorPool, NULL );
		twoDDescriptorPool = VK_NULL_HANDLE;
		twoDDescriptorSet = VK_NULL_HANDLE;
		hybridUpscaleDescriptorSet = VK_NULL_HANDLE;
		for ( int i = 0; i < SW_VK_FRAMES_IN_FLIGHT; i++ ) {
			frameContexts[i].twoDDescriptorSet = VK_NULL_HANDLE;
			frameContexts[i].hybridUpscaleDescriptorSet = VK_NULL_HANDLE;
		}
	}
	if ( twoDDescriptorSetLayout != VK_NULL_HANDLE ) {
		vkDestroyDescriptorSetLayout( device, twoDDescriptorSetLayout, NULL );
		twoDDescriptorSetLayout = VK_NULL_HANDLE;
	}
}

void idSoftwareVulkanBridge::DestroyOverlayPipeline() {
	if ( overlayPipeline != VK_NULL_HANDLE ) {
		vkDestroyPipeline( device, overlayPipeline, NULL );
		overlayPipeline = VK_NULL_HANDLE;
	}
	if ( overlayShaderModule != VK_NULL_HANDLE ) {
		vkDestroyShaderModule( device, overlayShaderModule, NULL );
		overlayShaderModule = VK_NULL_HANDLE;
	}
	if ( overlayPipelineLayout != VK_NULL_HANDLE ) {
		vkDestroyPipelineLayout( device, overlayPipelineLayout, NULL );
		overlayPipelineLayout = VK_NULL_HANDLE;
	}
	if ( overlayDescriptorPool != VK_NULL_HANDLE ) {
		vkDestroyDescriptorPool( device, overlayDescriptorPool, NULL );
		overlayDescriptorPool = VK_NULL_HANDLE;
		overlayDescriptorSet = VK_NULL_HANDLE;
		for ( int i = 0; i < SW_VK_FRAMES_IN_FLIGHT; i++ ) {
			frameContexts[i].overlayDescriptorSet = VK_NULL_HANDLE;
		}
	}
	if ( overlayDescriptorSetLayout != VK_NULL_HANDLE ) {
		vkDestroyDescriptorSetLayout( device, overlayDescriptorSetLayout, NULL );
		overlayDescriptorSetLayout = VK_NULL_HANDLE;
	}
}

void idSoftwareVulkanBridge::DestroyOverlayTilePipeline() {
	if ( overlayTilePipeline != VK_NULL_HANDLE ) {
		vkDestroyPipeline( device, overlayTilePipeline, NULL );
		overlayTilePipeline = VK_NULL_HANDLE;
	}
	if ( overlayTileShaderModule != VK_NULL_HANDLE ) {
		vkDestroyShaderModule( device, overlayTileShaderModule, NULL );
		overlayTileShaderModule = VK_NULL_HANDLE;
	}
	if ( overlayTilePipelineLayout != VK_NULL_HANDLE ) {
		vkDestroyPipelineLayout( device, overlayTilePipelineLayout, NULL );
		overlayTilePipelineLayout = VK_NULL_HANDLE;
	}
	if ( overlayTileDescriptorPool != VK_NULL_HANDLE ) {
		vkDestroyDescriptorPool( device, overlayTileDescriptorPool, NULL );
		overlayTileDescriptorPool = VK_NULL_HANDLE;
		overlayTileDescriptorSet = VK_NULL_HANDLE;
		for ( int i = 0; i < SW_VK_FRAMES_IN_FLIGHT; i++ ) {
			frameContexts[i].overlayTileDescriptorSet = VK_NULL_HANDLE;
		}
	}
	if ( overlayTileDescriptorSetLayout != VK_NULL_HANDLE ) {
		vkDestroyDescriptorSetLayout( device, overlayTileDescriptorSetLayout, NULL );
		overlayTileDescriptorSetLayout = VK_NULL_HANDLE;
	}
}

void idSoftwareVulkanBridge::DestroyHybridPipeline() {
	DestroyHybridBuffers();

	for ( int i = 0; i < SW_VK_HYBRID_VARIANT_COUNT; i++ ) {
		if ( hybridPipelines[i] != VK_NULL_HANDLE ) {
			vkDestroyPipeline( device, hybridPipelines[i], NULL );
			hybridPipelines[i] = VK_NULL_HANDLE;
		}
		if ( hybridShaderModules[i] != VK_NULL_HANDLE ) {
			vkDestroyShaderModule( device, hybridShaderModules[i], NULL );
			hybridShaderModules[i] = VK_NULL_HANDLE;
		}
	}
	if ( hybridPipelineLayout != VK_NULL_HANDLE ) {
		vkDestroyPipelineLayout( device, hybridPipelineLayout, NULL );
		hybridPipelineLayout = VK_NULL_HANDLE;
	}
	if ( hybridDescriptorPool != VK_NULL_HANDLE ) {
		vkDestroyDescriptorPool( device, hybridDescriptorPool, NULL );
		hybridDescriptorPool = VK_NULL_HANDLE;
		hybridDescriptorSet = VK_NULL_HANDLE;
		for ( int i = 0; i < SW_VK_FRAMES_IN_FLIGHT; i++ ) {
			frameContexts[i].hybridDescriptorSet = VK_NULL_HANDLE;
		}
	}
	if ( hybridDescriptorSetLayout != VK_NULL_HANDLE ) {
		vkDestroyDescriptorSetLayout( device, hybridDescriptorSetLayout, NULL );
		hybridDescriptorSetLayout = VK_NULL_HANDLE;
	}
}

void idSoftwareVulkanBridge::DestroyShadowPipeline() {
	DestroyShadowJobs();
	DestroyRayQueryScene();
	DestroyBuffer( shadowMaskBuffer );
	DestroyBuffer( worldPositionBuffer );

	if ( shadowPipeline != VK_NULL_HANDLE ) {
		vkDestroyPipeline( device, shadowPipeline, NULL );
		shadowPipeline = VK_NULL_HANDLE;
	}
	if ( shadowShaderModule != VK_NULL_HANDLE ) {
		vkDestroyShaderModule( device, shadowShaderModule, NULL );
		shadowShaderModule = VK_NULL_HANDLE;
	}
	if ( shadowPipelineLayout != VK_NULL_HANDLE ) {
		vkDestroyPipelineLayout( device, shadowPipelineLayout, NULL );
		shadowPipelineLayout = VK_NULL_HANDLE;
	}
	if ( shadowDescriptorPool != VK_NULL_HANDLE ) {
		vkDestroyDescriptorPool( device, shadowDescriptorPool, NULL );
		shadowDescriptorPool = VK_NULL_HANDLE;
		shadowDescriptorSet = VK_NULL_HANDLE;
	}
	if ( shadowDescriptorSetLayout != VK_NULL_HANDLE ) {
		vkDestroyDescriptorSetLayout( device, shadowDescriptorSetLayout, NULL );
		shadowDescriptorSetLayout = VK_NULL_HANDLE;
	}
}

void idSoftwareVulkanBridge::DestroySwapchain() {
	DestroyHybridBuffers();
	if ( swapchain != VK_NULL_HANDLE ) {
		vkDestroySwapchainKHR( device, swapchain, NULL );
		swapchain = VK_NULL_HANDLE;
	}
	swapchainImages.Clear();
	swapchainLayouts.Clear();
	swapchainFormat = VK_FORMAT_UNDEFINED;
	swapchainPresentMode = VK_PRESENT_MODE_FIFO_KHR;
	swapchainExtent.width = 0;
	swapchainExtent.height = 0;
}

void idSoftwareVulkanBridge::Shutdown() {
	if ( device != VK_NULL_HANDLE ) {
		vkDeviceWaitIdle( device );
	}

	DestroyHybridPipeline();
	Destroy2DPipeline();
	DestroyOverlayTilePipeline();
	DestroyOverlayPipeline();
	DestroyShadowPipeline();
	framePixels.Clear();
	hybrid2DSourcePixels.Clear();
	hybrid2DJobs.Clear();
	frameBegun = false;
	frameDirty = false;
	hybridFrameDirty = false;
	hybridOverlayDirty = false;
	hybridOverlayOnly = false;
	activeFrameContext = 0;
	nextFrameContext = 0;

	for ( int i = 0; i < SW_VK_FRAMES_IN_FLIGHT; i++ ) {
		swVkFrameContext_t &ctx = frameContexts[i];
		if ( ctx.inFlightFence != VK_NULL_HANDLE ) {
			vkDestroyFence( device, ctx.inFlightFence, NULL );
			ctx.inFlightFence = VK_NULL_HANDLE;
		}
		if ( ctx.renderFinishedSemaphore != VK_NULL_HANDLE ) {
			vkDestroySemaphore( device, ctx.renderFinishedSemaphore, NULL );
			ctx.renderFinishedSemaphore = VK_NULL_HANDLE;
		}
		if ( ctx.imageAvailableSemaphore != VK_NULL_HANDLE ) {
			vkDestroySemaphore( device, ctx.imageAvailableSemaphore, NULL );
			ctx.imageAvailableSemaphore = VK_NULL_HANDLE;
		}
		ctx.submitted = false;
	}
	inFlightFence = VK_NULL_HANDLE;
	renderFinishedSemaphore = VK_NULL_HANDLE;
	imageAvailableSemaphore = VK_NULL_HANDLE;
	DestroyTimestampQueries();
	if ( commandPool != VK_NULL_HANDLE ) {
		vkDestroyCommandPool( device, commandPool, NULL );
		commandPool = VK_NULL_HANDLE;
		commandBuffer = VK_NULL_HANDLE;
		for ( int i = 0; i < SW_VK_FRAMES_IN_FLIGHT; i++ ) {
			frameContexts[i].commandBuffer = VK_NULL_HANDLE;
		}
	}

	DestroySwapchain();

	if ( device != VK_NULL_HANDLE ) {
		vkDestroyDevice( device, NULL );
		device = VK_NULL_HANDLE;
	}
	if ( surface != VK_NULL_HANDLE ) {
		vkDestroySurfaceKHR( instance, surface, NULL );
		surface = VK_NULL_HANDLE;
	}
	if ( instance != VK_NULL_HANDLE ) {
		vkDestroyInstance( instance, NULL );
		instance = VK_NULL_HANDLE;
	}

	initialized = false;
	failed = false;
	printedFailure = false;
	rayQuerySupported = false;
	timestampQuerySupported = false;
	timestampQueryPending = false;
	timestampPeriod = 0.0f;
	timestampValidBits = 0;
	timestampFrame.Clear();
	rayQuerySceneReady = false;
	rayTlasSignature = 0;
	rayTlasInstanceCount = 0;
	physicalDevice = VK_NULL_HANDLE;
	queue = VK_NULL_HANDLE;
	queueFamily = 0;
	frameWidth = 0;
	frameHeight = 0;
	hybridWidth = 0;
	hybridHeight = 0;
	hybridPresentWidth = 0;
	hybridPresentHeight = 0;
	hybridViewportX = 0;
	hybridViewportY = 0;
	hybridDebugView = 0;
	hybridTextureCount = 0;
	hybridTextureTexelCount = 0;
	hybridLightCount = 0;
	hybridLightTileCount = 0;
	hybridLightIndexCount = 0;
	hybridShadowEnabled = false;
	hybridCompositeVariant = SW_VK_HYBRID_VARIANT_GENERIC_NOSHADOW;
	hybridOverlayDirty = false;
	hybridOverlayOnly = false;
	twoDDescriptorSet = VK_NULL_HANDLE;
	hybridUpscaleDescriptorSet = VK_NULL_HANDLE;
	overlayTileDescriptorSet = VK_NULL_HANDLE;

	vkGetPhysicalDeviceFeatures2Local = NULL;
	vkGetBufferDeviceAddressKHRLocal = NULL;
	vkCreateAccelerationStructureKHRLocal = NULL;
	vkDestroyAccelerationStructureKHRLocal = NULL;
	vkGetAccelerationStructureDeviceAddressKHRLocal = NULL;
	vkGetAccelerationStructureBuildSizesKHRLocal = NULL;
	vkCmdBuildAccelerationStructuresKHRLocal = NULL;
}

bool SWVulkan_BlitView( const viewDef_t *viewDef, const unsigned int *bgra, int width, int height, int presentWidth, int presentHeight ) {
	return swVulkanBridge.BlitView( viewDef, bgra, width, height, presentWidth, presentHeight );
}

bool SWVulkan_CompositeHybridGBuffer( const viewDef_t *viewDef, const swHybridGBufferUpload_t &gbuffer, int width, int height, int presentWidth, int presentHeight ) {
	return swVulkanBridge.CompositeHybridGBuffer( viewDef, gbuffer, width, height, presentWidth, presentHeight );
}

bool SWVulkan_UpdateHybridTextures( const swHybridTextureInfo_t *textureInfos, int textureInfoCount, const unsigned int *textureTexels, int textureTexelCount, unsigned int textureGeneration ) {
	return swVulkanBridge.UpdateHybridTextures( textureInfos, textureInfoCount, textureTexels, textureTexelCount, textureGeneration );
}

bool SWVulkan_QueueHybridOverlayTriangles( const viewDef_t *viewDef, const swHybridOverlayTri_t *tris, int triCount, int width, int height, int presentWidth, int presentHeight ) {
	return swVulkanBridge.QueueHybridOverlayTriangles( viewDef, tris, triCount, width, height, presentWidth, presentHeight );
}

bool SWVulkan_QueueHybridOverlaySourceTriangles( const viewDef_t *viewDef, const swHybridOverlayTri_t *tris, int triCount, int width, int height, int presentWidth, int presentHeight, const unsigned int *sourcePixels, int sourceWidth, int sourceHeight ) {
	return swVulkanBridge.QueueHybridOverlaySourceTriangles( viewDef, tris, triCount, width, height, presentWidth, presentHeight, sourcePixels, sourceWidth, sourceHeight );
}

bool SWVulkan_ReadView( const viewDef_t *viewDef, unsigned int *bgra, int width, int height ) {
	return swVulkanBridge.ReadView( viewDef, bgra, width, height );
}

bool SWVulkan_PresentFrame( void ) {
	return swVulkanBridge.PresentFrame();
}

bool SWVulkan_RayQueryAvailable( void ) {
	return swVulkanBridge.RayQueryAvailable();
}

bool SWVulkan_PrepareRayQueryScene( const viewDef_t *viewDef ) {
	return swVulkanBridge.PrepareRayQueryScene( viewDef );
}

bool SWVulkan_BeginLightShadowMask( const viewLight_t *vLight, const idVec4 *worldPositions, int width, int height ) {
	return swVulkanBridge.BeginLightShadowMask( vLight, worldPositions, width, height );
}

bool SWVulkan_FinishLightShadowMask( const viewLight_t *vLight, int width, int height, unsigned char *shadowMask ) {
	return swVulkanBridge.FinishLightShadowMask( vLight, width, height, shadowMask );
}

bool SWVulkan_TraceLightShadowMask( const viewLight_t *vLight, const idVec4 *worldPositions, int width, int height, unsigned char *shadowMask ) {
	return swVulkanBridge.TraceLightShadowMask( vLight, worldPositions, width, height, shadowMask );
}

void SWVulkan_DestroyRayQueryBlas( void *blas ) {
	swVulkanBridge.DestroyRayQueryBlasHandle( blas );
}

void SWVulkan_DestroyRayQueryEntity( void *entityTlas ) {
	swVulkanBridge.DestroyRayQueryEntityHandle( entityTlas );
}

void SWVulkan_DestroyRayQueryScene( void ) {
	swVulkanBridge.ClearRayQueryScene();
}

void SWVulkan_Shutdown( void ) {
	swVulkanBridge.Shutdown();
}

