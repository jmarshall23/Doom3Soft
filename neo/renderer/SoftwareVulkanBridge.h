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
	unsigned int flags[4];
	float lightProject[4][4];
	unsigned int textureIds[4];
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
bool SWVulkan_ReadView( const viewDef_t *viewDef, unsigned int *bgra, int width, int height );
bool SWVulkan_PresentFrame( void );
bool SWVulkan_RayQueryAvailable( void );
bool SWVulkan_PrepareRayQueryScene( const viewDef_t *viewDef );
bool SWVulkan_BeginLightShadowMask( const viewLight_t *vLight, const idVec4 *worldPositions, int width, int height );
bool SWVulkan_FinishLightShadowMask( const viewLight_t *vLight, int width, int height, unsigned char *shadowMask );
bool SWVulkan_TraceLightShadowMask( const viewLight_t *vLight, const idVec4 *worldPositions, int width, int height, unsigned char *shadowMask );
void SWVulkan_Shutdown( void );

#endif /* !__SOFTWARE_VULKAN_BRIDGE_H__ */
