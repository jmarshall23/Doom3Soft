/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 2026 Justin Marshall

This file is part of the idTech 4 software renderer source code

===========================================================================
*/

#ifndef __SOFTWARE_VULKAN_BRIDGE_H__
#define __SOFTWARE_VULKAN_BRIDGE_H__

struct viewDef_s;
typedef struct viewDef_s viewDef_t;
struct viewLight_s;
typedef struct viewLight_s viewLight_t;
class idVec4;

struct swHybridTextureInfo_t {
	unsigned int offset;
	unsigned int width;
	unsigned int height;
	unsigned int repeat;
};

struct swHybridLight_t {
	float originRadius[4];
	float color[4];
	int scissor[4];
	unsigned int flags[4];		// normal: ambient, parallel, shadow, type; blend: mode, src, dst, type
	float lightProject[4][4];
	unsigned int textureIds[4];
};

struct swHybridOverlayTri_t {
	float p0[4];
	float p1[4];
	float p2[4];
	float color0[4];
	float color1[4];
	float color2[4];
	float uv0uv1[4];
	float uv2pad[4];
	float zPlane[4];
	float invWPlane[4];
	float sPlane[4];
	float tPlane[4];
	float colorPlane0[4];
	float colorPlane1[4];
	float colorPlane2[4];
	float colorPlane3[4];
	unsigned int params0[4];	// texture id, fallback color, flags, alpha-test byte
	int params1[4];			// blend mode, write mask, source blend, dest blend
	int bounds[4];			// source-space min x/y/max x/y
	int source[4];			// source width/height
	int viewport[4];		// frame x/y, present width/height
};

struct swHybridGBufferUpload_t {
	swHybridGBufferUpload_t() {
		depth = NULL;
		normalPacked = NULL;
		tangentPacked = NULL;
		bitangentPacked = NULL;
		uvPacked = NULL;
		materialId = NULL;
		albedoOrTextureId = NULL;
		specularAndFlags = NULL;
		surfaceId = NULL;
		worldPositions = NULL;
		textureInfos = NULL;
		textureInfoCount = 0;
		textureTexels = NULL;
		textureTexelCount = 0;
		textureGeneration = 0;
		lights = NULL;
		lightCount = 0;
		debugView = 0;
	}

	const float *depth;
	const unsigned int *normalPacked;
	const unsigned int *tangentPacked;
	const unsigned int *bitangentPacked;
	const unsigned int *uvPacked;
	const unsigned int *materialId;
	const unsigned int *albedoOrTextureId;
	const unsigned int *specularAndFlags;
	const unsigned int *surfaceId;
	const idVec4 *worldPositions;
	const swHybridTextureInfo_t *textureInfos;
	int textureInfoCount;
	const unsigned int *textureTexels;
	int textureTexelCount;
	unsigned int textureGeneration;
	const swHybridLight_t *lights;
	int lightCount;
	int debugView;
};

bool SWVulkan_BlitView( const viewDef_t *viewDef, const unsigned int *bgra, int width, int height, int presentWidth, int presentHeight );
bool SWVulkan_CompositeHybridGBuffer( const viewDef_t *viewDef, const swHybridGBufferUpload_t &gbuffer, int width, int height, int presentWidth, int presentHeight );
bool SWVulkan_UpdateHybridTextures( const swHybridTextureInfo_t *textureInfos, int textureInfoCount, const unsigned int *textureTexels, int textureTexelCount, unsigned int textureGeneration );
bool SWVulkan_QueueHybridOverlayTriangles( const viewDef_t *viewDef, const swHybridOverlayTri_t *tris, int triCount, int width, int height, int presentWidth, int presentHeight );
bool SWVulkan_QueueHybridOverlaySourceTriangles( const viewDef_t *viewDef, const swHybridOverlayTri_t *tris, int triCount, int width, int height, int presentWidth, int presentHeight, const unsigned int *sourcePixels, int sourceWidth, int sourceHeight );
bool SWVulkan_ReadView( const viewDef_t *viewDef, unsigned int *bgra, int width, int height );
bool SWVulkan_PresentFrame( void );
bool SWVulkan_RayQueryAvailable( void );
bool SWVulkan_PrepareRayQueryScene( const viewDef_t *viewDef );
bool SWVulkan_BeginLightShadowMask( const viewLight_t *vLight, const idVec4 *worldPositions, int width, int height );
bool SWVulkan_FinishLightShadowMask( const viewLight_t *vLight, int width, int height, unsigned char *shadowMask );
bool SWVulkan_TraceLightShadowMask( const viewLight_t *vLight, const idVec4 *worldPositions, int width, int height, unsigned char *shadowMask );
void SWVulkan_DestroyRayQueryBlas( void *blas );
void SWVulkan_DestroyRayQueryEntity( void *entityTlas );
void SWVulkan_Shutdown( void );

#endif /* !__SOFTWARE_VULKAN_BRIDGE_H__ */
