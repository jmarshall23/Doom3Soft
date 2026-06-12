/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 2026 Justin Marshall 

This file is part of the idTech 4 software renderer source code

===========================================================================
*/

#include "precompiled.h"
#pragma hdrstop

#include "tr_local.h"
#include "SoftwareRasterizer.h"
#include "SoftwareVulkanBridge.h"

#if defined( SW_COMPILE_AVX2 ) || defined( __AVX2__ ) || defined( _M_AVX2 )
#include <immintrin.h>
#define SW_AVX2_DEPTH_PATH 1
#else
#define SW_AVX2_DEPTH_PATH 0
#endif

/*
===============================================================================

	Fast CPU tile-binned triangle rasterizer.

	This backend uses the existing Doom 3 front end drawSurf list and transforms
	indexed srfTriangles_t geometry into screen-space triangles, bins them into
	16x16 tiles, and gives each tile to exactly one worker during rasterization.

===============================================================================
*/

static const int SW_TILE_SIZE = 16;
static const int SW_MAX_CLIP_VERTS = 12;
static const int SW_INTERACTION_ATTR_COUNT = 18;
static const float SW_DEPTH_EQUAL_EPSILON = 0.0001f;
static const int SW_FP_SHIFT = 8;
static const int SW_FP_ONE = 1 << SW_FP_SHIFT;
static const int SW_FP_HALF = SW_FP_ONE >> 1;
static const int SW_WRITE_RED = BIT(0);
static const int SW_WRITE_GREEN = BIT(1);
static const int SW_WRITE_BLUE = BIT(2);
static const int SW_WRITE_ALPHA = BIT(3);
static const int SW_WRITE_COLOR = SW_WRITE_RED | SW_WRITE_GREEN | SW_WRITE_BLUE | SW_WRITE_ALPHA;
static const int SW_WRITE_DYNAMIC = -1;
static const long long SW_INT32_MIN = -2147483647LL - 1LL;
static const long long SW_INT32_MAX = 2147483647LL;
static const unsigned int SW_OVERLAY_FLAG_TEXTURE = BIT(0);
static const unsigned int SW_OVERLAY_FLAG_COLOR_MOD = BIT(1);
static const unsigned int SW_OVERLAY_FLAG_ALPHA_TEST = BIT(2);
static const unsigned int SW_OVERLAY_FLAG_DEPTH_TEST = BIT(3);
static const unsigned int SW_OVERLAY_FLAG_DEPTH_EQUAL = BIT(4);
static const unsigned int SW_OVERLAY_FLAG_SOURCE_IMAGE = BIT(5);
static const unsigned int SW_HYBRID_LIGHT_NORMAL = 0;
static const unsigned int SW_HYBRID_LIGHT_FOG = 1;
static const unsigned int SW_HYBRID_LIGHT_BLEND = 2;
static const int SW_FOG_SIZE = 128;
static const float SW_FOG_RAMP_RANGE = 8.0f;
static const float SW_FOG_DEEP_RANGE = -30.0f;

static void SWFillPattern( void *dst, const void *pattern, int count, size_t elementSize ) {
	if ( !dst || !pattern || count <= 0 || elementSize == 0 ) {
		return;
	}

	byte *out = reinterpret_cast<byte *>( dst );
	const size_t totalBytes = static_cast<size_t>( count ) * elementSize;
	memcpy( out, pattern, elementSize );
	size_t copiedBytes = elementSize;
	while ( copiedBytes < totalBytes ) {
		const size_t remainingBytes = totalBytes - copiedBytes;
		const size_t copyBytes = copiedBytes < remainingBytes ? copiedBytes : remainingBytes;
		memcpy( out + copiedBytes, out, copyBytes );
		copiedBytes += copyBytes;
	}
}

template<class type>
static void SWFillList( idList<type> &list, const type &value ) {
	SWFillPattern( list.Ptr(), &value, list.Num(), sizeof( type ) );
}

template<class type>
static void SWZeroList( idList<type> &list ) {
	if ( list.Num() > 0 ) {
		memset( list.Ptr(), 0, static_cast<size_t>( list.Num() ) * sizeof( type ) );
	}
}

static float SWFogFraction( float viewHeight, float targetHeight ) {
	const float total = idMath::Fabs( targetHeight - viewHeight );
	if ( targetHeight > 0.0f && viewHeight > 0.0f ) {
		return 0.0f;
	}
	if ( targetHeight < -SW_FOG_RAMP_RANGE && viewHeight < -SW_FOG_RAMP_RANGE ) {
		return 1.0f;
	}

	float above;
	if ( targetHeight > 0.0f ) {
		above = targetHeight;
	} else if ( viewHeight > 0.0f ) {
		above = viewHeight;
	} else {
		above = 0.0f;
	}

	float rampTop;
	float rampBottom;
	if ( viewHeight > targetHeight ) {
		rampTop = viewHeight;
		rampBottom = targetHeight;
	} else {
		rampTop = targetHeight;
		rampBottom = viewHeight;
	}
	if ( rampTop > 0.0f ) {
		rampTop = 0.0f;
	}
	if ( rampBottom < -SW_FOG_RAMP_RANGE ) {
		rampBottom = -SW_FOG_RAMP_RANGE;
	}

	const float rampSlope = 1.0f / SW_FOG_RAMP_RANGE;
	if ( total == 0.0f ) {
		return -viewHeight * rampSlope;
	}

	const float ramp = ( 1.0f - ( rampTop * rampSlope + rampBottom * rampSlope ) * -0.5f ) * ( rampTop - rampBottom );
	float frac = ( total - above - ramp ) / total;

	const float deepest = viewHeight < targetHeight ? viewHeight : targetHeight;
	const float deepFrac = deepest / SW_FOG_DEEP_RANGE;
	if ( deepFrac >= 1.0f ) {
		return 1.0f;
	}

	frac = frac * ( 1.0f - deepFrac ) + deepFrac;
	return idMath::ClampFloat( 0.0f, 1.0f, frac );
}

enum swBlendMode_t {
	SW_BLEND_REPLACE,
	SW_BLEND_ALPHA,
	SW_BLEND_ADD,
	SW_BLEND_FILTER,
	SW_BLEND_DST_ALPHA,
	SW_BLEND_GENERIC
};

enum swRasterPass_t {
	SW_RASTER_SURFACE,
	SW_RASTER_INTERACTION
};

enum swInteractionAttr_t {
	SW_ATTR_POS_X,
	SW_ATTR_POS_Y,
	SW_ATTR_POS_Z,
	SW_ATTR_ST_X,
	SW_ATTR_ST_Y,
	SW_ATTR_NORMAL_X,
	SW_ATTR_NORMAL_Y,
	SW_ATTR_NORMAL_Z,
	SW_ATTR_TANGENT0_X,
	SW_ATTR_TANGENT0_Y,
	SW_ATTR_TANGENT0_Z,
	SW_ATTR_TANGENT1_X,
	SW_ATTR_TANGENT1_Y,
	SW_ATTR_TANGENT1_Z,
	SW_ATTR_COLOR_R,
	SW_ATTR_COLOR_G,
	SW_ATTR_COLOR_B,
	SW_ATTR_COLOR_A
};

struct swScreenVert_t {
	int x;
	int y;
	float z;
	float invW;
	float sOverW;
	float tOverW;
	float colorOverW[4];
	float globalOverW[3];
	float normalOverW[3];
	float tangent0OverW[3];
	float tangent1OverW[3];
};

struct swClipVert_t {
	float clip[4];
	float s;
	float t;
	float color[4];
	float global[3];
	float normal[3];
	float tangent[2][3];
	float attr[SW_INTERACTION_ATTR_COUNT];
};

struct swSurfaceStage_t {
	int textureIndex;
	float matrix[2][3];
	float color[4];
	stageVertexColor_t vertexColor;
	int srcBlend;
	int dstBlend;
	int blendMode;
	int writeMask;
	bool depthTest;
	bool depthWrite;
	bool depthEqual;
	bool alphaBlend;
	bool drawStage;
	bool needsColorModulation;
	bool alphaTest;
	bool writeWorldPosition;
	bool writeGBuffer;
	float alphaTestValue;
	int alphaTestByte;
	unsigned int fallbackColor;
	unsigned int materialId;
	unsigned int surfaceId;
	unsigned int albedoOrTextureId;
	unsigned int specularAndFlags;
};

struct swTexture_t {
	swTexture_t() {
		image = NULL;
		width = 0;
		height = 0;
		mipCount = 0;
		repeat = TR_REPEAT;
	}

	const idImage *image;
	idStr name;
	int width;
	int height;
	int mipCount;
	textureRepeat_t repeat;
	idList<unsigned int> texels;
	idList<int> mipOffsets;
	idList<int> mipWidths;
	idList<int> mipHeights;
};

struct swFogLightState_t {
	idVec4 color;
	idVec4 fogPlane;
	idVec4 enterPlane;
	float enterS;
	int fogTextureIndex;
	int fogEnterTextureIndex;
	idScreenRect scissorRect;
};

struct swBlendLightStage_t {
	idVec4 color;
	idVec4 lightProject[4];
	int lightTextureIndex;
	int falloffTextureIndex;
	int srcBlend;
	int dstBlend;
	int blendMode;
	int writeMask;
	idScreenRect scissorRect;
};

struct swHybridGBuffer_t {
	idList<float> depth;
	idList<unsigned int> normalPacked;
	idList<unsigned int> tangentPacked;
	idList<unsigned int> bitangentPacked;
	idList<unsigned int> uvPacked;
	idList<unsigned int> materialId;
	idList<unsigned int> albedoOrTextureId;
	idList<unsigned int> specularAndFlags;
	idList<unsigned int> surfaceId;
};

struct swHybridGBufferPointers_t {
	float *depth;
	unsigned int *normalPacked;
	unsigned int *tangentPacked;
	unsigned int *bitangentPacked;
	unsigned int *uvPacked;
	unsigned int *materialId;
	unsigned int *albedoOrTextureId;
	unsigned int *specularAndFlags;
	unsigned int *surfaceId;
};

struct swTriSetup_t {
	int x[3];
	int y[3];
	float z[3];
	float invW[3];
	float sOverW[3];
	float tOverW[3];
	float colorOverW[3][4];

	int minX;
	int minY;
	int maxX;
	int maxY;

	int tileMinX;
	int tileMinY;
	int tileMaxX;
	int tileMaxY;

	long long A[3];
	long long B[3];
	long long C[3];

	float z0;
	float dzdx;
	float dzdy;

	float invW0;
	float dinvWdx;
	float dinvWdy;

	float sOverW0;
	float dsOverWdx;
	float dsOverWdy;

	float tOverW0;
	float dtOverWdx;
	float dtOverWdy;

	int textureIndex;
	float colorOverW0[4];
	float dColorOverWdx[4];
	float dColorOverWdy[4];
	float globalOverW[3][3];
	float globalOverW0[3];
	float dGlobalOverWdx[3];
	float dGlobalOverWdy[3];
	float normalOverW[3][3];
	float normalOverW0[3];
	float dNormalOverWdx[3];
	float dNormalOverWdy[3];
	float tangent0OverW[3][3];
	float tangent0OverW0[3];
	float dTangent0OverWdx[3];
	float dTangent0OverWdy[3];
	float tangent1OverW[3][3];
	float tangent1OverW0[3];
	float dTangent1OverWdx[3];
	float dTangent1OverWdy[3];
	int srcBlend;
	int dstBlend;
	int blendMode;
	int writeMask;
	bool depthTest;
	bool depthWrite;
	bool depthEqual;
	bool alphaBlend;
	bool needsColorModulation;
	bool alphaTest;
	bool writeWorldPosition;
	bool writeGBuffer;
	float alphaTestValue;
	int alphaTestByte;
	unsigned int fallbackColor;
	unsigned int materialId;
	unsigned int surfaceId;
	unsigned int albedoOrTextureId;
	unsigned int specularAndFlags;
};

struct swInteractionVert_t {
	int x;
	int y;
	float z;
	float invW;
	float attrOverW[SW_INTERACTION_ATTR_COUNT];
};

struct swInteractionTri_t {
	int x[3];
	int y[3];
	float z[3];
	float invW[3];
	float attrOverW[3][SW_INTERACTION_ATTR_COUNT];

	int minX;
	int minY;
	int maxX;
	int maxY;

	int tileMinX;
	int tileMinY;
	int tileMaxX;
	int tileMaxY;

	long long A[3];
	long long B[3];
	long long C[3];

	float z0;
	float dzdx;
	float dzdy;

	float invW0;
	float dinvWdx;
	float dinvWdy;

	float attrOverW0[SW_INTERACTION_ATTR_COUNT];
	float dAttrOverWdx[SW_INTERACTION_ATTR_COUNT];
	float dAttrOverWdy[SW_INTERACTION_ATTR_COUNT];

	int bumpTextureIndex;
	int diffuseTextureIndex;
	int specularTextureIndex;
	int lightTextureIndex;
	int falloffTextureIndex;

	idVec4 diffuseColor;
	idVec4 specularColor;
	stageVertexColor_t vertexColor;
	bool ambientLight;
	bool depthEqual;

	idVec4 localLightOrigin;
	idVec4 localViewOrigin;
	idVec4 lightProjection[4];
	idVec4 bumpMatrix[2];
	idVec4 diffuseMatrix[2];
	idVec4 specularMatrix[2];
};

struct swTileBin_t {
	idList<int> tris;
};

class idSoftwareRasterizer;

struct swRasterWorkerJob_t {
	idSoftwareRasterizer *rasterizer;
#if defined( _WIN32 ) && !defined( _D3SDK )
	HANDLE startEvent;
	HANDLE doneEvent;
#endif
	int firstTile;
	int endTile;
#if defined( _WIN32 ) && !defined( _D3SDK )
	volatile bool shutdown;
#endif
};

class idSoftwareRasterizer {
public:
	idSoftwareRasterizer();
	~idSoftwareRasterizer();

	void DrawView( const viewDef_t *viewDef );
	void DrawInteraction( const drawInteraction_t *interaction );

private:
	void Resize( int width, int height );
	void Clear( bool clearColor );
	void ClearTileBins();
	void ClearHybridGBuffer();
	bool ViewNeedsWorldPosition( const viewDef_t *viewDef ) const;
	bool ViewContainsSubviewSurface( const viewDef_t *viewDef ) const;
	bool IsGuiCarrierSurface( const drawSurf_t *surf ) const;
	void BeginSurfacePass();
	void BeginInteractionPass();
	void ReadCurrentFramebuffer( const viewDef_t *viewDef );
	void SetupTriangles( const viewDef_t *viewDef );
	void SetupDepthPrepass( const viewDef_t *viewDef );
	void SetupAmbientTriangles( const viewDef_t *viewDef );
	void SetupTranslucentTriangles( const viewDef_t *viewDef );
	void SetupSubviewTriangles( const viewDef_t *viewDef );
	void SetupDrawSurf( const viewDef_t *viewDef, const drawSurf_t *surf );
	void SetupDepthDrawSurf( const viewDef_t *viewDef, const drawSurf_t *surf );
	void SetupDrawSurfStage( const viewDef_t *viewDef, const drawSurf_t *surf, const swSurfaceStage_t &stage );
	void ConfigureStageForView( const viewDef_t *viewDef, const drawSurf_t *surf, swSurfaceStage_t &stage ) const;
	void ConfigureDepthStage( const drawSurf_t *surf, swSurfaceStage_t &stage, const shaderStage_t *sourceStage );
	bool TransformVertex( const idDrawVert &src, const float *modelViewMatrix, const float *projectionMatrix, swScreenVert_t &dst ) const;
	void ApplyTextureCoordinates( const idDrawVert &src, const swSurfaceStage_t &stage, swScreenVert_t &dst ) const;
	bool BuildClipVertex( const idDrawVert &src, const float *modelMatrix, const float *modelViewMatrix, const float *projectionMatrix, const swSurfaceStage_t &stage, swClipVert_t &dst ) const;
	bool BuildInteractionClipVertex( const idDrawVert &src, const float *modelViewMatrix, const float *projectionMatrix, swClipVert_t &dst ) const;
	int ClipTriangleToFrustum( const swClipVert_t &v0, const swClipVert_t &v1, const swClipVert_t &v2, swClipVert_t clipped[SW_MAX_CLIP_VERTS] ) const;
	bool ProjectClipVertex( const swClipVert_t &src, swScreenVert_t &dst ) const;
	bool ProjectInteractionClipVertex( const swClipVert_t &src, swInteractionVert_t &dst ) const;
	bool SetupTriangle( const swScreenVert_t &v0, const swScreenVert_t &v1, const swScreenVert_t &v2, int cullType, const swSurfaceStage_t &stage, swTriSetup_t &tri ) const;
	bool SetupInteractionTriangle( const swInteractionVert_t &v0, const swInteractionVert_t &v1, const swInteractionVert_t &v2, int cullType, const drawInteraction_t &interaction, swInteractionTri_t &tri );
	void BinTriangle( int triIndex );
	void BinInteractionTriangle( int triIndex );
	void DrawLights( const viewDef_t *viewDef );
	void ApplyFogLights( const viewDef_t *viewDef );
	void ApplyFogLight( const swFogLightState_t &fog );
	void ApplyBlendLight( const swBlendLightStage_t &blend );
	void WriteHybridDebugView();
	void BuildHybridTextureUpload( swHybridGBufferUpload_t &gbuffer );
	void BuildHybridLightUpload( const viewDef_t *viewDef, swHybridGBufferUpload_t &gbuffer );
	bool BuildFogLightState( const viewDef_t *viewDef, const viewLight_t *vLight, swFogLightState_t &fog );
	bool BuildBlendLightStage( const viewLight_t *vLight, const shaderStage_t *stage, swBlendLightStage_t &blend );
	void AddHybridFogLight( const swFogLightState_t &fog );
	void AddHybridBlendLight( const swBlendLightStage_t &blend );
	void BuildHybridOverlayTriangles( idList<swHybridOverlayTri_t> &overlayTris ) const;
	void RasterizeTiles();
	void RasterizeTileRange( int firstTile, int endTile );
	void RasterizeTile( int tileX, int tileY );
	void RasterizeTriangleInTile( const swTriSetup_t &tri, int tileX, int tileY );
	void RasterizeDepthTriangleInTile( const swTriSetup_t &tri, int tileX, int tileY );
	void RasterizeAlphaTestDepthTriangleInTile( const swTriSetup_t &tri, int tileX, int tileY );
	template<int TEXTURE_MODE>
	void RasterizeTriangleInTileTexture( const swTriSetup_t &tri, int tileX, int tileY );
	template<bool HAS_TEXTURE, int TEXTURE_MODE>
	void RasterizeTriangleInTileTextured( const swTriSetup_t &tri, int tileX, int tileY );
	template<bool HAS_TEXTURE, int TEXTURE_MODE, bool COLOR_MOD>
	void RasterizeTriangleInTileColorMod( const swTriSetup_t &tri, int tileX, int tileY );
	template<bool HAS_TEXTURE, int TEXTURE_MODE, bool COLOR_MOD, int BLEND_MODE>
	void RasterizeTriangleInTileBlend( const swTriSetup_t &tri, int tileX, int tileY );
	template<bool HAS_TEXTURE, int TEXTURE_MODE, bool COLOR_MOD, int BLEND_MODE, int WRITE_MASK>
	void RasterizeTriangleInTileWriteMask( const swTriSetup_t &tri, int tileX, int tileY );
	template<bool HAS_TEXTURE, int TEXTURE_MODE, bool COLOR_MOD, int BLEND_MODE, int WRITE_MASK, bool DEPTH_TEST, bool DEPTH_WRITE, bool ALPHA_TEST>
	void RasterizeTriangleInTileSpecialized( const swTriSetup_t &tri, int tileX, int tileY );
	void RasterizeInteractionTriangleInTile( const swInteractionTri_t &tri, int tileX, int tileY );
	ID_INLINE swHybridGBufferPointers_t GetHybridGBufferPointers();
	ID_INLINE void WriteHybridGBufferPixel( const swHybridGBufferPointers_t &gbuffer, int pixelIndex, const swTriSetup_t &tri, float z, float invW, float sOverW, float tOverW, const float normalOverW[3], const float tangent0OverW[3], const float tangent1OverW[3] );
	unsigned int ShadeInteractionPixel( const swInteractionTri_t &tri, int x, int y, float invW, const float attrOverW[SW_INTERACTION_ATTR_COUNT] ) const;
	float ShadowVisibility( int x, int y ) const;
	void ApplyLightScale();
	void Present() const;

	void SelectSurfaceStage( int surfIndex, const drawSurf_t *surf, swSurfaceStage_t &stage, int stageIndex );
	void PrimeHybridTextureCacheFromImageManager();
	bool ShouldPrimeHybridTexture( const idImage *image ) const;
	bool IsHybridGeneratedTexture( const idImage *image ) const;
	int FindTextureCacheIndex( const idImage *image ) const;
	int AddHybridTextureToCache( const idImage *image, bool printDefaulted );
	int TextureIndexForImage( const idImage *image );
	bool LoadSoftwareTexture( swTexture_t &texture, const idImage *image ) const;
	bool LoadBoundTexture( swTexture_t &texture, const idImage *image ) const;
	bool LoadGeneratedTexture( swTexture_t &texture, const idImage *image ) const;
	void LoadDefaultTexture( swTexture_t &texture ) const;
	void BuildTextureMipChain( swTexture_t &texture ) const;
	unsigned int SampleTexture( int textureIndex, float s, float t ) const;
	unsigned int SampleTextureLinear( int textureIndex, float s, float t ) const;
	unsigned int SampleTextureMip( int textureIndex, float s, float t, float lod ) const;
	unsigned int SampleTextureLinearMip( int textureIndex, float s, float t, float lod ) const;
	unsigned int SampleTextureLevelLinear( const swTexture_t &texture, int level, float s, float t ) const;
	unsigned int TextureTexel( const swTexture_t &texture, int level, int x, int y ) const;
	float TextureLodForGradients( int textureIndex, float invW, float sOverW, float tOverW, float dsOverWdx, float dtOverWdx, float dinvWdx, float dsOverWdy, float dtOverWdy, float dinvWdy ) const;
	float TextureLodForDerivatives( int textureIndex, float dsdx, float dtdx, float dsdy, float dtdy ) const;
	unsigned int SurfaceColor( int surfIndex, const drawSurf_t *surf ) const;
	static unsigned int DebugTextureColor( int mode, float s, float t );
	static unsigned int BlendSourceOver( unsigned int src, unsigned int dst );
	static unsigned int BlendColor( unsigned int src, unsigned int dst, int srcBlend, int dstBlend, int blendMode );
	template<bool HAS_TEXTURE, int TEXTURE_MODE, bool COLOR_MOD>
	ID_INLINE unsigned int ShadePixel( const swTriSetup_t &tri, float invW, float sOverW, float tOverW, const float colorOverW[4] ) const;
	template<int BLEND_MODE>
	static ID_INLINE unsigned int BlendColorSpecialized( unsigned int src, unsigned int dst, const swTriSetup_t &tri );
	template<int BLEND_MODE, int WRITE_MASK>
	static ID_INLINE void WritePixelColor( const swTriSetup_t &tri, unsigned int src, unsigned int *color );
	static int BlendFactor( int blend, unsigned int src, unsigned int dst, int channel, bool sourceBlend );
	static int BlendModeForBits( int srcBlend, int dstBlend );
	static unsigned int ApplyWriteMask( unsigned int src, unsigned int dst, int writeMask );
	static void SetupFloatPlane( float v0, float v1, float v2, float fx0, float fy0, float fx1, float fy1, float fx2, float fy2, float denom, float &base, float &dx, float &dy );
	static unsigned int PackColor( int r, int g, int b, int a = 255 );
	static ID_INLINE int PackSnorm10( float value );
	static float UnpackSnorm10( unsigned int packed, int shift );
	static ID_INLINE unsigned int PackNormal10( const idVec3 &normal );
	static ID_INLINE unsigned int PackNormal10( float x, float y, float z );
	static ID_INLINE unsigned int PackUV16( float s, float t );
	static unsigned int PackOptionalTextureId16( int textureIndex );
	static unsigned int IdDebugColor( unsigned int value );
	static unsigned int ModulateColor( unsigned int color, const float scale[4] );
	static unsigned int AverageTexels( unsigned int c00, unsigned int c10, unsigned int c01, unsigned int c11 );
	static unsigned int LerpTexels( unsigned int c0, unsigned int c1, float fraction );
	static unsigned int AdditiveColor( unsigned int src, unsigned int dst );
	static float ColorChannel( unsigned int color, int shift );
	static int TextureImageHashKey( const idImage *image );
	static idVec3 DecodeNormal( unsigned int color );
	static float DotPlanePoint( const idVec4 &plane, const idVec3 &point );
	static int ByteFromFloat( float value );
	static float ClipPlaneDistance( const swClipVert_t &v, int plane );
	static void LerpClipVertex( const swClipVert_t &a, const swClipVert_t &b, float fraction, swClipVert_t &out );
	static float WrapTextureCoordinate( float value, textureRepeat_t repeat );
	static bool FitsInt32( long long value );
	static bool FitsEdgePacket32( long long start, long long step, int lanes );
	static long long EdgeValue( int ax, int ay, int bx, int by, int px, int py );
	static int TopLeftBias( int ax, int ay, int bx, int by );
#if defined( _WIN32 ) && !defined( _D3SDK )
	void StartWorkers();
	void ShutdownWorkers();
	static DWORD WINAPI RasterizeWorker( LPVOID data );
#endif

	int width;
	int height;
	int presentWidth;
	int presentHeight;
	int tileCountX;
	int tileCountY;

	idList<float> depthBuffer;
	idList<unsigned int> colorBuffer;
	swHybridGBuffer_t hybridGBuffer;
	idList<idVec4> worldPositionBuffer;
	idList<unsigned char> shadowMask;
	idList<swTriSetup_t> triangles;
	idList<swInteractionTri_t> interactionTriangles;
	idList<swTileBin_t> tileBins;
	idList<swTexture_t> textureCache;
	idHashIndex textureCacheHash;
	idList<swHybridTextureInfo_t> hybridTextureInfos;
	idList<unsigned int> hybridTextureTexels;
	idList<swHybridLight_t> hybridLights;
	idList<unsigned int> subviewSourcePixels;
	unsigned int textureCacheGeneration;
	unsigned int hybridTextureAtlasGeneration;
	int hybridImageManagerCount;
	int subviewSourceWidth;
	int subviewSourceHeight;
	bool hybridTextureCachePrimed;
	swRasterPass_t rasterPass;
	bool shadowMaskActive;
	unsigned int hybridSurfaceSerial;
	bool captureWorldPosition;
	bool hybridComputeActive;
	bool hasSubviewSource;

#if defined( _WIN32 ) && !defined( _D3SDK )
	bool workersStarted;
	int workerCount;
	HANDLE workerHandles[MAXIMUM_WAIT_OBJECTS];
	swRasterWorkerJob_t workerJobs[MAXIMUM_WAIT_OBJECTS];
#endif
};

static idSoftwareRasterizer swRasterizer;

static void RB_SW_DrawInteractionCallback( const drawInteraction_t *interaction ) {
	swRasterizer.DrawInteraction( interaction );
}

idSoftwareRasterizer::idSoftwareRasterizer() {
	width = 0;
	height = 0;
	presentWidth = 0;
	presentHeight = 0;
	tileCountX = 0;
	tileCountY = 0;
	rasterPass = SW_RASTER_SURFACE;
	shadowMaskActive = false;
	hybridSurfaceSerial = 1;
	captureWorldPosition = false;
	hybridComputeActive = false;
	textureCacheGeneration = 1;
	hybridTextureAtlasGeneration = 0;
	hybridImageManagerCount = 0;
	subviewSourceWidth = 0;
	subviewSourceHeight = 0;
	hybridTextureCachePrimed = false;
	hasSubviewSource = false;

#if defined( _WIN32 ) && !defined( _D3SDK )
	workersStarted = false;
	workerCount = 0;
	memset( workerHandles, 0, sizeof( workerHandles ) );
	memset( workerJobs, 0, sizeof( workerJobs ) );
#endif
}

idSoftwareRasterizer::~idSoftwareRasterizer() {
#if defined( _WIN32 ) && !defined( _D3SDK )
	ShutdownWorkers();
#endif
}

void idSoftwareRasterizer::Resize( int newWidth, int newHeight ) {
	if ( newWidth == width && newHeight == height ) {
		return;
	}

	width = Max( newWidth, 1 );
	height = Max( newHeight, 1 );
	tileCountX = ( width + SW_TILE_SIZE - 1 ) / SW_TILE_SIZE;
	tileCountY = ( height + SW_TILE_SIZE - 1 ) / SW_TILE_SIZE;

	depthBuffer.SetNum( width * height, false );
	colorBuffer.SetNum( width * height, false );
	hybridGBuffer.depth.SetNum( width * height, false );
	hybridGBuffer.normalPacked.SetNum( width * height, false );
	hybridGBuffer.tangentPacked.SetNum( width * height, false );
	hybridGBuffer.bitangentPacked.SetNum( width * height, false );
	hybridGBuffer.uvPacked.SetNum( width * height, false );
	hybridGBuffer.materialId.SetNum( width * height, false );
	hybridGBuffer.albedoOrTextureId.SetNum( width * height, false );
	hybridGBuffer.specularAndFlags.SetNum( width * height, false );
	hybridGBuffer.surfaceId.SetNum( width * height, false );
	worldPositionBuffer.SetNum( width * height, false );
	shadowMask.SetNum( width * height, false );
	tileBins.SetNum( tileCountX * tileCountY, false );
}

void idSoftwareRasterizer::Clear( bool clearColor ) {
	const float clearDepth = 1.0f;
	SWFillList( depthBuffer, clearDepth );
	SWZeroList( worldPositionBuffer );
	ClearHybridGBuffer();
	shadowMaskActive = false;
	hybridSurfaceSerial = 1;
	if ( clearColor ) {
		const unsigned int clearColorValue = 0xff000000u;
		SWFillList( colorBuffer, clearColorValue );
	}

	triangles.SetNum( 0, false );
	interactionTriangles.SetNum( 0, false );
	ClearTileBins();
}

void idSoftwareRasterizer::ClearHybridGBuffer() {
	const int pixelCount = width * height;
	if ( hybridGBuffer.depth.Num() != pixelCount ||
		 hybridGBuffer.normalPacked.Num() != pixelCount ||
		 hybridGBuffer.tangentPacked.Num() != pixelCount ||
		 hybridGBuffer.bitangentPacked.Num() != pixelCount ||
		 hybridGBuffer.uvPacked.Num() != pixelCount ||
		 hybridGBuffer.materialId.Num() != pixelCount ||
		 hybridGBuffer.albedoOrTextureId.Num() != pixelCount ||
		 hybridGBuffer.specularAndFlags.Num() != pixelCount ||
		 hybridGBuffer.surfaceId.Num() != pixelCount ) {
		return;
	}

	const float clearDepth = 1.0f;
	const unsigned int invalidTextureId = 0xffffffffu;
	SWFillList( hybridGBuffer.depth, clearDepth );
	SWZeroList( hybridGBuffer.normalPacked );
	SWZeroList( hybridGBuffer.tangentPacked );
	SWZeroList( hybridGBuffer.bitangentPacked );
	SWZeroList( hybridGBuffer.uvPacked );
	SWZeroList( hybridGBuffer.materialId );
	SWFillList( hybridGBuffer.albedoOrTextureId, invalidTextureId );
	SWZeroList( hybridGBuffer.specularAndFlags );
	SWZeroList( hybridGBuffer.surfaceId );
}

bool idSoftwareRasterizer::ViewNeedsWorldPosition( const viewDef_t *viewDef ) const {
	if ( !viewDef || !viewDef->viewEntitys ) {
		return false;
	}
	if ( r_softwareRayQueryShadows.GetBool() || r_softwareHybridComputeLighting.GetBool() ) {
		return true;
	}
	if ( r_skipFogLights.GetBool() || r_showOverDraw.GetInteger() != 0 || viewDef->isXraySubview ) {
		return false;
	}
	for ( const viewLight_t *vLight = viewDef->viewLights; vLight; vLight = vLight->next ) {
		if ( !vLight->lightShader || vLight->scissorRect.IsEmpty() ) {
			continue;
		}
		if ( vLight->lightShader->IsFogLight() ) {
			return true;
		}
		if ( !r_skipBlendLights.GetBool() && vLight->lightShader->IsBlendLight() &&
			 ( vLight->localInteractions || vLight->globalInteractions ) ) {
			return true;
		}
	}
	return false;
}

void idSoftwareRasterizer::ClearTileBins() {
	for ( int i = 0; i < tileBins.Num(); i++ ) {
		tileBins[i].tris.Clear();
	}
}

void idSoftwareRasterizer::BeginSurfacePass() {
	rasterPass = SW_RASTER_SURFACE;
	triangles.SetNum( 0, false );
	ClearTileBins();
}

void idSoftwareRasterizer::BeginInteractionPass() {
	rasterPass = SW_RASTER_INTERACTION;
	interactionTriangles.SetNum( 0, false );
	ClearTileBins();
}

void idSoftwareRasterizer::ReadCurrentFramebuffer( const viewDef_t *viewDef ) {
	if ( !viewDef || colorBuffer.Num() != width * height ) {
		return;
	}
	if ( r_softwareVulkanPresent.GetBool() && SWVulkan_ReadView( viewDef, colorBuffer.Ptr(), width, height ) ) {
		return;
	}
	qglReadPixels( viewDef->viewport.x1, viewDef->viewport.y1, width, height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, colorBuffer.Ptr() );
}
void idSoftwareRasterizer::DrawView( const viewDef_t *viewDef ) {
	if ( !viewDef ) {
		return;
	}

	const int viewportWidth = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	const int viewportHeight = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;
	presentWidth = Max( viewportWidth, 1 );
	presentHeight = Max( viewportHeight, 1 );

	const bool is3DView = viewDef->viewEntitys != NULL;
	const float renderScale = is3DView ? idMath::ClampFloat( 0.01f, 1.0f, r_softwareRenderScale.GetFloat() ) : 1.0f;
	const int renderWidth = Max( 1, idMath::Ftoi( static_cast<float>( presentWidth ) * renderScale + 0.5f ) );
	const int renderHeight = Max( 1, idMath::Ftoi( static_cast<float>( presentHeight ) * renderScale + 0.5f ) );
	const bool captureSubviewSource = is3DView && ViewContainsSubviewSurface( viewDef ) && r_softwareVulkanPresent.GetBool();
	const bool allowHybridCompute = is3DView && r_softwareHybridComputeLighting.GetBool();
	const bool tryCompute2DOverlay = !is3DView && r_softwareHybridComputeLighting.GetBool() && r_softwareVulkanPresent.GetBool();
	hybridComputeActive = allowHybridCompute;
	captureWorldPosition = is3DView && ViewNeedsWorldPosition( viewDef );
	Resize( renderWidth, renderHeight );
	hasSubviewSource = false;
	subviewSourceWidth = 0;
	subviewSourceHeight = 0;
	if ( viewDef->viewEntitys ) {
		if ( captureSubviewSource ) {
			subviewSourcePixels.SetNum( width * height, false );
			hasSubviewSource = SWVulkan_ReadView( viewDef, subviewSourcePixels.Ptr(), width, height );
			if ( hasSubviewSource ) {
				subviewSourceWidth = width;
				subviewSourceHeight = height;
			} else {
				subviewSourcePixels.SetNum( 0, false );
			}
		}
		Clear( true );
	} else {
		if ( !tryCompute2DOverlay ) {
			ReadCurrentFramebuffer( viewDef );
		}
		Clear( false );
	}

	if ( viewDef->viewEntitys ) {
		RB_DetermineLightScale();
		if ( allowHybridCompute ) {
			PrimeHybridTextureCacheFromImageManager();
		}

		BeginSurfacePass();
		SetupDepthPrepass( viewDef );
		RasterizeTiles();

		if ( allowHybridCompute ) {
			swHybridGBufferUpload_t gbuffer;
			gbuffer.depth = hybridGBuffer.depth.Ptr();
			gbuffer.normalPacked = hybridGBuffer.normalPacked.Ptr();
			gbuffer.tangentPacked = hybridGBuffer.tangentPacked.Ptr();
			gbuffer.bitangentPacked = hybridGBuffer.bitangentPacked.Ptr();
			gbuffer.uvPacked = hybridGBuffer.uvPacked.Ptr();
			gbuffer.materialId = hybridGBuffer.materialId.Ptr();
			gbuffer.albedoOrTextureId = hybridGBuffer.albedoOrTextureId.Ptr();
			gbuffer.specularAndFlags = hybridGBuffer.specularAndFlags.Ptr();
			gbuffer.surfaceId = hybridGBuffer.surfaceId.Ptr();
			gbuffer.worldPositions = worldPositionBuffer.Ptr();
			gbuffer.debugView = r_softwareHybridDebugView.GetInteger();

			idList<swHybridOverlayTri_t> subviewOverlayTris;
			idList<swHybridOverlayTri_t> overlayTris;
			if ( gbuffer.debugView == 0 ) {
				if ( hasSubviewSource ) {
					BeginSurfacePass();
					SetupSubviewTriangles( viewDef );
					BuildHybridOverlayTriangles( subviewOverlayTris );
					for ( int i = 0; i < subviewOverlayTris.Num(); i++ ) {
						subviewOverlayTris[i].params0[2] |= SW_OVERLAY_FLAG_SOURCE_IMAGE;
					}
				}
				BeginSurfacePass();
				SetupTranslucentTriangles( viewDef );
				BuildHybridOverlayTriangles( overlayTris );
			}

			BuildHybridLightUpload( viewDef, gbuffer );
			BuildHybridTextureUpload( gbuffer );
			if ( SWVulkan_CompositeHybridGBuffer( viewDef, gbuffer, width, height, presentWidth, presentHeight ) ) {
				if ( r_softwareHybridDebugView.GetInteger() == 0 ) {
					bool queuedOverlays = true;
					if ( subviewOverlayTris.Num() > 0 ) {
						queuedOverlays = SWVulkan_QueueHybridOverlaySourceTriangles( viewDef, subviewOverlayTris.Ptr(), subviewOverlayTris.Num(),
							width, height, presentWidth, presentHeight, subviewSourcePixels.Ptr(), subviewSourceWidth, subviewSourceHeight );
					}
					if ( queuedOverlays && overlayTris.Num() > 0 ) {
						queuedOverlays = SWVulkan_QueueHybridOverlayTriangles( viewDef, overlayTris.Ptr(), overlayTris.Num(), width, height, presentWidth, presentHeight );
					}
					if ( queuedOverlays ) {
						return;
					}

					SWZeroList( colorBuffer );
					RasterizeTiles();
					Present();
				}
				return;
			}
			if ( r_softwareHybridDebugView.GetInteger() != 0 ) {
				WriteHybridDebugView();
				Present();
				return;
			}
		}

		DrawLights( viewDef );
		ApplyLightScale();

		BeginSurfacePass();
		SetupAmbientTriangles( viewDef );
		RasterizeTiles();
		ApplyFogLights( viewDef );
	} else {
		if ( tryCompute2DOverlay ) {
			PrimeHybridTextureCacheFromImageManager();
		}
		BeginSurfacePass();
		SetupTriangles( viewDef );
		if ( tryCompute2DOverlay ) {
			idList<swHybridOverlayTri_t> overlayTris;
			BuildHybridOverlayTriangles( overlayTris );
			if ( overlayTris.Num() > 0 ) {
				swHybridGBufferUpload_t textureUpload;
				BuildHybridTextureUpload( textureUpload );
				if ( SWVulkan_UpdateHybridTextures( textureUpload.textureInfos, textureUpload.textureInfoCount,
					 textureUpload.textureTexels, textureUpload.textureTexelCount, textureUpload.textureGeneration ) &&
					 SWVulkan_QueueHybridOverlayTriangles( viewDef, overlayTris.Ptr(), overlayTris.Num(), width, height, presentWidth, presentHeight ) ) {
					return;
				}
			}
		}
		if ( tryCompute2DOverlay ) {
			ReadCurrentFramebuffer( viewDef );
		}
		RasterizeTiles();
	}
	Present();
}

void idSoftwareRasterizer::SetupTriangles( const viewDef_t *viewDef ) {
	const bool is2DView = viewDef->viewEntitys == NULL;
	for ( int i = 0; i < viewDef->numDrawSurfs; i++ ) {
		const drawSurf_t *surf = viewDef->drawSurfs[i];
		if ( !surf || !surf->geo || !surf->material ) {
			continue;
		}
		const float sort = surf->material->GetSort();
		if ( !is2DView && ( sort < SS_OPAQUE || sort == SS_PORTAL_SKY || sort >= SS_POST_PROCESS ) ) {
			continue;
		}
		SetupDrawSurf( viewDef, surf );
	}
}

bool idSoftwareRasterizer::ViewContainsSubviewSurface( const viewDef_t *viewDef ) const {
	if ( !viewDef ) {
		return false;
	}
	for ( int i = 0; i < viewDef->numDrawSurfs; i++ ) {
		const drawSurf_t *surf = viewDef->drawSurfs[i];
		if ( surf && surf->material && surf->material->GetSort() == SS_SUBVIEW ) {
			return true;
		}
	}
	return false;
}

bool idSoftwareRasterizer::IsGuiCarrierSurface( const drawSurf_t *surf ) const {
	return surf && surf->material && surf->material->HasGui() && surf->material->GetNumStages() == 0;
}

void idSoftwareRasterizer::SetupDepthPrepass( const viewDef_t *viewDef ) {
	for ( int i = 0; i < viewDef->numDrawSurfs; i++ ) {
		const drawSurf_t *surf = viewDef->drawSurfs[i];
		if ( !surf || !surf->geo || !surf->material ) {
			continue;
		}
		const float sort = surf->material->GetSort();
		if ( sort != SS_OPAQUE && sort != SS_SUBVIEW ) {
			continue;
		}
		SetupDepthDrawSurf( viewDef, surf );
	}
}

void idSoftwareRasterizer::SetupAmbientTriangles( const viewDef_t *viewDef ) {
	for ( int i = 0; i < viewDef->numDrawSurfs; i++ ) {
		const drawSurf_t *surf = viewDef->drawSurfs[i];
		if ( !surf || !surf->geo || !surf->material ) {
			continue;
		}
		if ( IsGuiCarrierSurface( surf ) ) {
			continue;
		}
		const float sort = surf->material->GetSort();
		const bool guiSort = sort >= SS_GUI && sort < SS_OPAQUE;
		if ( ( sort < SS_OPAQUE && !guiSort ) || sort == SS_PORTAL_SKY || sort >= SS_POST_PROCESS ) {
			continue;
		}

		const idMaterial *material = surf->material;
		for ( int stageIndex = 0; stageIndex < material->GetNumStages(); stageIndex++ ) {
			swSurfaceStage_t stage;
			SelectSurfaceStage( triangles.Num(), surf, stage, stageIndex );
			if ( !stage.drawStage ) {
				continue;
			}
			ConfigureStageForView( viewDef, surf, stage );
			const bool translucent = surf->material->Coverage() == MC_TRANSLUCENT || surf->material->GetSort() > SS_OPAQUE;
			stage.depthWrite = false;
			stage.depthEqual = !translucent;
			SetupDrawSurfStage( viewDef, surf, stage );
		}
	}
}

void idSoftwareRasterizer::SetupTranslucentTriangles( const viewDef_t *viewDef ) {
	for ( int i = 0; i < viewDef->numDrawSurfs; i++ ) {
		const drawSurf_t *surf = viewDef->drawSurfs[i];
		if ( !surf || !surf->geo || !surf->material ) {
			continue;
		}
		if ( IsGuiCarrierSurface( surf ) ) {
			continue;
		}
		const float sort = surf->material->GetSort();
		const bool guiSort = sort >= SS_GUI && sort < SS_OPAQUE;
		if ( sort >= SS_POST_PROCESS ) {
			continue;
		}
		if ( sort <= SS_OPAQUE && !guiSort && surf->material->Coverage() != MC_TRANSLUCENT ) {
			continue;
		}
		SetupDrawSurf( viewDef, surf );
	}
}

void idSoftwareRasterizer::SetupSubviewTriangles( const viewDef_t *viewDef ) {
	for ( int i = 0; i < viewDef->numDrawSurfs; i++ ) {
		const drawSurf_t *surf = viewDef->drawSurfs[i];
		if ( !surf || !surf->geo || !surf->material ) {
			continue;
		}
		if ( surf->material->GetSort() != SS_SUBVIEW ) {
			continue;
		}

		swSurfaceStage_t stage;
		ConfigureDepthStage( surf, stage, NULL );
		stage.writeMask = SW_WRITE_COLOR;
		stage.depthWrite = false;
		stage.depthEqual = true;
		stage.writeWorldPosition = false;
		stage.writeGBuffer = false;
		SetupDrawSurfStage( viewDef, surf, stage );
	}
}

void idSoftwareRasterizer::SetupDrawSurf( const viewDef_t *viewDef, const drawSurf_t *surf ) {
	if ( IsGuiCarrierSurface( surf ) ) {
		return;
	}
	const srfTriangles_t *geo = surf->geo;
	if ( !geo->verts || !geo->indexes || geo->numVerts <= 0 || geo->numIndexes < 3 || !surf->space ) {
		return;
	}

	const bool is2DView = viewDef->viewEntitys == NULL;
	const bool drawAllStages = is2DView || surf->material->Coverage() == MC_TRANSLUCENT || surf->material->GetSort() > SS_OPAQUE;
	if ( drawAllStages ) {
		const int startTriCount = triangles.Num();
		const idMaterial *material = surf->material;
		for ( int stageIndex = 0; stageIndex < material->GetNumStages(); stageIndex++ ) {
			swSurfaceStage_t stage;
			SelectSurfaceStage( triangles.Num(), surf, stage, stageIndex );
			if ( !stage.drawStage ) {
				continue;
			}
			ConfigureStageForView( viewDef, surf, stage );
			SetupDrawSurfStage( viewDef, surf, stage );
		}
		if ( triangles.Num() != startTriCount ) {
			return;
		}
	}

	swSurfaceStage_t stage;
	SelectSurfaceStage( triangles.Num(), surf, stage, -1 );
	if ( !stage.drawStage ) {
		return;
	}
	ConfigureStageForView( viewDef, surf, stage );
	SetupDrawSurfStage( viewDef, surf, stage );
}

void idSoftwareRasterizer::SetupDepthDrawSurf( const viewDef_t *viewDef, const drawSurf_t *surf ) {
	if ( IsGuiCarrierSurface( surf ) ) {
		return;
	}
	const srfTriangles_t *geo = surf->geo;
	if ( !geo->indexes || geo->numIndexes < 3 || !surf->space || !surf->material->IsDrawn() ) {
		return;
	}
	if ( !geo->verts ) {
		return;
	}
	if ( surf->material->Coverage() == MC_TRANSLUCENT ) {
		return;
	}

	unsigned int surfaceId = 0;
	unsigned int materialId = 0;
	unsigned int albedoOrTextureId = 0xffffffffu;
	unsigned int specularAndFlags = 0;
	if ( hybridComputeActive ) {
		surfaceId = hybridSurfaceSerial++;
		materialId = surfaceId;

		swSurfaceStage_t materialStage;
		SelectSurfaceStage( static_cast<int>( surfaceId ), surf, materialStage, -1 );
		if ( materialStage.textureIndex >= 0 ) {
			albedoOrTextureId = static_cast<unsigned int>( materialStage.textureIndex );
		}

		int bumpTextureIndex = -1;
		int specularTextureIndex = -1;
		const idMaterial *material = surf->material;
		const float *regs = surf->shaderRegisters;
		for ( int stageIndex = 0; stageIndex < material->GetNumStages(); stageIndex++ ) {
			const shaderStage_t *stage = material->GetStage( stageIndex );
			if ( !stage || !stage->texture.image ) {
				continue;
			}
			if ( regs && regs[stage->conditionRegister] == 0.0f ) {
				continue;
			}
			if ( stage->texture.texgen != TG_EXPLICIT || stage->texture.cinematic || stage->texture.dynamic != DI_STATIC ) {
				continue;
			}
			if ( stage->lighting == SL_DIFFUSE && albedoOrTextureId == 0xffffffffu ) {
				const int textureIndex = TextureIndexForImage( stage->texture.image );
				if ( textureIndex >= 0 ) {
					albedoOrTextureId = static_cast<unsigned int>( textureIndex );
				}
			} else if ( stage->lighting == SL_BUMP && bumpTextureIndex < 0 ) {
				bumpTextureIndex = TextureIndexForImage( stage->texture.image );
			} else if ( stage->lighting == SL_SPECULAR && specularTextureIndex < 0 ) {
				specularTextureIndex = TextureIndexForImage( stage->texture.image );
			}
		}
		specularAndFlags = PackOptionalTextureId16( bumpTextureIndex ) |
			( PackOptionalTextureId16( specularTextureIndex ) << 16 );
	}

	bool drewAlphaTest = false;
	if ( surf->material->Coverage() == MC_PERFORATED ) {
		const idMaterial *material = surf->material;
		const float *regs = surf->shaderRegisters;
		for ( int stageIndex = 0; stageIndex < material->GetNumStages(); stageIndex++ ) {
			const shaderStage_t *sourceStage = material->GetStage( stageIndex );
			if ( !sourceStage || !sourceStage->hasAlphaTest || !sourceStage->texture.image ) {
				continue;
			}
			if ( regs && regs[sourceStage->conditionRegister] == 0.0f ) {
				continue;
			}
			if ( sourceStage->texture.texgen != TG_EXPLICIT || sourceStage->texture.cinematic || sourceStage->texture.dynamic != DI_STATIC ) {
				continue;
			}

			swSurfaceStage_t stage;
			ConfigureDepthStage( surf, stage, sourceStage );
			stage.materialId = materialId;
			stage.surfaceId = surfaceId;
			stage.albedoOrTextureId = albedoOrTextureId;
			stage.specularAndFlags = specularAndFlags;
			if ( stage.color[3] <= 0.0f ) {
				drewAlphaTest = true;
				continue;
			}
			drewAlphaTest = true;
			SetupDrawSurfStage( viewDef, surf, stage );
		}
	}

	if ( !drewAlphaTest ) {
		swSurfaceStage_t stage;
		ConfigureDepthStage( surf, stage, NULL );
		stage.materialId = materialId;
		stage.surfaceId = surfaceId;
		stage.albedoOrTextureId = albedoOrTextureId;
		stage.specularAndFlags = specularAndFlags;
		SetupDrawSurfStage( viewDef, surf, stage );
	}
}

void idSoftwareRasterizer::ConfigureStageForView( const viewDef_t *viewDef, const drawSurf_t *surf, swSurfaceStage_t &stage ) const {
	const bool is2DView = viewDef->viewEntitys == NULL;
	const bool materialTranslucent = surf->material->Coverage() == MC_TRANSLUCENT;
	const float sort = surf->material->GetSort();
	const bool guiSort = sort >= SS_GUI && sort < SS_OPAQUE;
	const bool sortedAfterOpaque = sort > SS_OPAQUE;
	const bool translucent = is2DView || materialTranslucent || sortedAfterOpaque || guiSort;
	stage.depthTest = viewDef->viewEntitys != NULL;
	stage.depthWrite = viewDef->viewEntitys != NULL && !translucent;
	stage.depthEqual = viewDef->viewEntitys != NULL && sortedAfterOpaque && !translucent;
	if ( is2DView && ( stage.writeMask & ( SW_WRITE_RED | SW_WRITE_GREEN | SW_WRITE_BLUE ) ) &&
		 stage.srcBlend == GLS_SRCBLEND_ONE && stage.dstBlend == GLS_DSTBLEND_ZERO ) {
		stage.srcBlend = GLS_SRCBLEND_SRC_ALPHA;
		stage.dstBlend = GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA;
	}
	stage.alphaBlend = stage.srcBlend != GLS_SRCBLEND_ONE || stage.dstBlend != GLS_DSTBLEND_ZERO;
	stage.blendMode = BlendModeForBits( stage.srcBlend, stage.dstBlend );
	stage.needsColorModulation = stage.vertexColor != SVC_IGNORE ||
		stage.color[0] != 1.0f || stage.color[1] != 1.0f ||
		stage.color[2] != 1.0f || stage.color[3] != 1.0f;
}

void idSoftwareRasterizer::ConfigureDepthStage( const drawSurf_t *surf, swSurfaceStage_t &stage, const shaderStage_t *sourceStage ) {
	stage.textureIndex = -1;
	stage.matrix[0][0] = 1.0f;
	stage.matrix[0][1] = 0.0f;
	stage.matrix[0][2] = 0.0f;
	stage.matrix[1][0] = 0.0f;
	stage.matrix[1][1] = 1.0f;
	stage.matrix[1][2] = 0.0f;
	stage.color[0] = 1.0f;
	stage.color[1] = 1.0f;
	stage.color[2] = 1.0f;
	stage.color[3] = 1.0f;
	stage.vertexColor = SVC_IGNORE;
	stage.srcBlend = GLS_SRCBLEND_ONE;
	stage.dstBlend = GLS_DSTBLEND_ZERO;
	stage.blendMode = SW_BLEND_REPLACE;
	stage.writeMask = 0;
	stage.depthTest = true;
	stage.depthWrite = true;
	stage.depthEqual = false;
	stage.alphaBlend = false;
	stage.drawStage = true;
	stage.needsColorModulation = false;
	stage.alphaTest = false;
	stage.writeWorldPosition = captureWorldPosition;
	stage.writeGBuffer = hybridComputeActive;
	stage.alphaTestValue = 0.0f;
	stage.alphaTestByte = 0;
	stage.fallbackColor = PackColor( 255, 255, 255, 255 );
	stage.materialId = 0;
	stage.surfaceId = 0;
	stage.albedoOrTextureId = 0xffffffffu;
	stage.specularAndFlags = 0;

	if ( !sourceStage ) {
		return;
	}

	const float *regs = surf->shaderRegisters;
	stage.textureIndex = TextureIndexForImage( sourceStage->texture.image );
	if ( regs ) {
		stage.color[0] = idMath::ClampFloat( 0.0f, 1.0f, regs[sourceStage->color.registers[0]] );
		stage.color[1] = idMath::ClampFloat( 0.0f, 1.0f, regs[sourceStage->color.registers[1]] );
		stage.color[2] = idMath::ClampFloat( 0.0f, 1.0f, regs[sourceStage->color.registers[2]] );
		stage.color[3] = idMath::ClampFloat( 0.0f, 1.0f, regs[sourceStage->color.registers[3]] );

		if ( sourceStage->texture.hasMatrix ) {
			stage.matrix[0][0] = regs[sourceStage->texture.matrix[0][0]];
			stage.matrix[0][1] = regs[sourceStage->texture.matrix[0][1]];
			stage.matrix[0][2] = regs[sourceStage->texture.matrix[0][2]];
			stage.matrix[1][0] = regs[sourceStage->texture.matrix[1][0]];
			stage.matrix[1][1] = regs[sourceStage->texture.matrix[1][1]];
			stage.matrix[1][2] = regs[sourceStage->texture.matrix[1][2]];
		}

		stage.alphaTestValue = idMath::ClampFloat( 0.0f, 1.0f, regs[sourceStage->alphaTestRegister] );
	} else {
		stage.alphaTestValue = 0.5f;
	}
	stage.alphaTest = true;
	stage.alphaTestByte = ByteFromFloat( stage.alphaTestValue );
	stage.needsColorModulation = stage.color[0] != 1.0f || stage.color[1] != 1.0f ||
		stage.color[2] != 1.0f || stage.color[3] != 1.0f;
}

void idSoftwareRasterizer::SetupDrawSurfStage( const viewDef_t *viewDef, const drawSurf_t *surf, const swSurfaceStage_t &stage ) {
	const srfTriangles_t *geo = surf->geo;
	const idDrawVert *verts = geo->verts;
	if ( !verts ) {
		return;
	}
	const int numIndexes = r_singleTriangle.GetBool() ? Min( geo->numIndexes, 3 ) : geo->numIndexes;
	const int cullType = surf->material->GetCullType();

	backEnd.pc.c_drawElements++;
	backEnd.pc.c_drawIndexes += numIndexes;
	backEnd.pc.c_drawVertexes += geo->numVerts;

	float projectionMatrix[16];
	const float *stageProjectionMatrix = viewDef->projectionMatrix;
	const bool useWeaponDepthRange = surf->space->weaponDepthHack && surf->space->modelDepthHack == 0.0f;
	if ( surf->space->weaponDepthHack || surf->space->modelDepthHack != 0.0f ) {
		memcpy( projectionMatrix, viewDef->projectionMatrix, sizeof( projectionMatrix ) );
		if ( surf->space->modelDepthHack != 0.0f ) {
			projectionMatrix[14] -= surf->space->modelDepthHack;
		} else if ( surf->space->weaponDepthHack ) {
			projectionMatrix[14] *= 0.25f;
		}
		stageProjectionMatrix = projectionMatrix;
	}

	for ( int i = 0; i + 2 < numIndexes; i += 3 ) {
		const int index0 = geo->indexes[i + 0];
		const int index1 = geo->indexes[i + 1];
		const int index2 = geo->indexes[i + 2];
		if ( index0 < 0 || index0 >= geo->numVerts ||
			 index1 < 0 || index1 >= geo->numVerts ||
			 index2 < 0 || index2 >= geo->numVerts ) {
			continue;
		}

		const idDrawVert &dv0 = verts[index0];
		const idDrawVert &dv1 = verts[index1];
		const idDrawVert &dv2 = verts[index2];

		swClipVert_t cv0, cv1, cv2;
		if ( !BuildClipVertex( dv0, surf->space->modelMatrix, surf->space->modelViewMatrix, stageProjectionMatrix, stage, cv0 ) ||
			 !BuildClipVertex( dv1, surf->space->modelMatrix, surf->space->modelViewMatrix, stageProjectionMatrix, stage, cv1 ) ||
			 !BuildClipVertex( dv2, surf->space->modelMatrix, surf->space->modelViewMatrix, stageProjectionMatrix, stage, cv2 ) ) {
			continue;
		}

		swClipVert_t clipped[SW_MAX_CLIP_VERTS];
		const int clippedVerts = ClipTriangleToFrustum( cv0, cv1, cv2, clipped );
		for ( int clippedTri = 1; clippedTri + 1 < clippedVerts; clippedTri++ ) {
			swScreenVert_t v0, v1, v2;
			if ( !ProjectClipVertex( clipped[0], v0 ) ||
				 !ProjectClipVertex( clipped[clippedTri], v1 ) ||
				 !ProjectClipVertex( clipped[clippedTri + 1], v2 ) ) {
				continue;
			}
			if ( useWeaponDepthRange ) {
				v0.z *= 0.5f;
				v1.z *= 0.5f;
				v2.z *= 0.5f;
			}

			swTriSetup_t tri;
			if ( SetupTriangle( v0, v1, v2, cullType, stage, tri ) ) {
				const int triIndex = triangles.Append( tri );
				BinTriangle( triIndex );
			}
		}
	}
}

bool idSoftwareRasterizer::TransformVertex( const idDrawVert &src, const float *modelViewMatrix, const float *projectionMatrix, swScreenVert_t &dst ) const {
	if ( !modelViewMatrix || !projectionMatrix ) {
		return false;
	}

	idPlane eye;
	idPlane clip;
	R_TransformModelToClip( src.xyz, modelViewMatrix, projectionMatrix, eye, clip );

	if ( clip[3] <= 0.0f ) {
		return false;
	}

	const float ndcX = clip[0] / clip[3];
	const float ndcY = clip[1] / clip[3];
	const float ndcZ = clip[2] / clip[3];

	if ( ndcX < -8.0f || ndcX > 8.0f ||
		 ndcY < -8.0f || ndcY > 8.0f ||
		 ndcZ < -1.0f || ndcZ > 1.0f ) {
		return false;
	}

	const float sx = ( ndcX * 0.5f + 0.5f ) * static_cast<float>( width );
	const float sy = ( ndcY * 0.5f + 0.5f ) * static_cast<float>( height );

	dst.x = idMath::FtoiFast( sx * static_cast<float>( SW_FP_ONE ) + 0.5f );
	dst.y = idMath::FtoiFast( sy * static_cast<float>( SW_FP_ONE ) + 0.5f );
	dst.z = ndcZ * 0.5f + 0.5f;
	dst.invW = 1.0f / clip[3];
	dst.sOverW = 0.0f;
	dst.tOverW = 0.0f;
	for ( int i = 0; i < 4; i++ ) {
		dst.colorOverW[i] = dst.invW;
	}
	for ( int i = 0; i < 3; i++ ) {
		dst.globalOverW[i] = 0.0f;
		dst.normalOverW[i] = 0.0f;
		dst.tangent0OverW[i] = 0.0f;
		dst.tangent1OverW[i] = 0.0f;
	}
	return true;
}

void idSoftwareRasterizer::ApplyTextureCoordinates( const idDrawVert &src, const swSurfaceStage_t &stage, swScreenVert_t &dst ) const {
	const float s = src.st[0] * stage.matrix[0][0] + src.st[1] * stage.matrix[0][1] + stage.matrix[0][2];
	const float t = src.st[0] * stage.matrix[1][0] + src.st[1] * stage.matrix[1][1] + stage.matrix[1][2];
	dst.sOverW = s * dst.invW;
	dst.tOverW = t * dst.invW;

	float vertexColor[4];
	if ( stage.vertexColor == SVC_IGNORE ) {
		vertexColor[0] = 1.0f;
		vertexColor[1] = 1.0f;
		vertexColor[2] = 1.0f;
		vertexColor[3] = 1.0f;
	} else if ( stage.vertexColor == SVC_INVERSE_MODULATE ) {
		vertexColor[0] = 1.0f - static_cast<float>( src.color[0] ) / 255.0f;
		vertexColor[1] = 1.0f - static_cast<float>( src.color[1] ) / 255.0f;
		vertexColor[2] = 1.0f - static_cast<float>( src.color[2] ) / 255.0f;
		vertexColor[3] = static_cast<float>( src.color[3] ) / 255.0f;
	} else {
		vertexColor[0] = static_cast<float>( src.color[0] ) / 255.0f;
		vertexColor[1] = static_cast<float>( src.color[1] ) / 255.0f;
		vertexColor[2] = static_cast<float>( src.color[2] ) / 255.0f;
		vertexColor[3] = static_cast<float>( src.color[3] ) / 255.0f;
	}

	for ( int i = 0; i < 4; i++ ) {
		dst.colorOverW[i] = stage.needsColorModulation ? stage.color[i] * vertexColor[i] * dst.invW : 0.0f;
	}
}

bool idSoftwareRasterizer::BuildClipVertex( const idDrawVert &src, const float *modelMatrix, const float *modelViewMatrix, const float *projectionMatrix, const swSurfaceStage_t &stage, swClipVert_t &dst ) const {
	if ( !modelMatrix || !modelViewMatrix || !projectionMatrix ) {
		return false;
	}

	idPlane eye;
	idPlane clip;
	R_TransformModelToClip( src.xyz, modelViewMatrix, projectionMatrix, eye, clip );
	for ( int i = 0; i < 4; i++ ) {
		dst.clip[i] = clip[i];
	}

	dst.s = src.st[0] * stage.matrix[0][0] + src.st[1] * stage.matrix[0][1] + stage.matrix[0][2];
	dst.t = src.st[0] * stage.matrix[1][0] + src.st[1] * stage.matrix[1][1] + stage.matrix[1][2];

	idVec3 global;
	R_LocalPointToGlobal( modelMatrix, src.xyz, global );
	dst.global[0] = global[0];
	dst.global[1] = global[1];
	dst.global[2] = global[2];

	idVec3 globalNormal;
	R_LocalVectorToGlobal( modelMatrix, src.normal, globalNormal );
	if ( globalNormal.Normalize() == 0.0f ) {
		globalNormal.Set( 0.0f, 0.0f, 1.0f );
	}
	dst.normal[0] = globalNormal[0];
	dst.normal[1] = globalNormal[1];
	dst.normal[2] = globalNormal[2];

	for ( int tangentIndex = 0; tangentIndex < 2; tangentIndex++ ) {
		idVec3 globalTangent;
		R_LocalVectorToGlobal( modelMatrix, src.tangents[tangentIndex], globalTangent );
		if ( globalTangent.Normalize() == 0.0f ) {
			if ( tangentIndex == 0 ) {
				globalTangent.Set( 1.0f, 0.0f, 0.0f );
			} else {
				globalTangent.Set( 0.0f, 1.0f, 0.0f );
			}
		}
		dst.tangent[tangentIndex][0] = globalTangent[0];
		dst.tangent[tangentIndex][1] = globalTangent[1];
		dst.tangent[tangentIndex][2] = globalTangent[2];
	}

	float vertexColor[4];
	if ( stage.vertexColor == SVC_IGNORE ) {
		vertexColor[0] = 1.0f;
		vertexColor[1] = 1.0f;
		vertexColor[2] = 1.0f;
		vertexColor[3] = 1.0f;
	} else if ( stage.vertexColor == SVC_INVERSE_MODULATE ) {
		vertexColor[0] = 1.0f - static_cast<float>( src.color[0] ) / 255.0f;
		vertexColor[1] = 1.0f - static_cast<float>( src.color[1] ) / 255.0f;
		vertexColor[2] = 1.0f - static_cast<float>( src.color[2] ) / 255.0f;
		vertexColor[3] = static_cast<float>( src.color[3] ) / 255.0f;
	} else {
		vertexColor[0] = static_cast<float>( src.color[0] ) / 255.0f;
		vertexColor[1] = static_cast<float>( src.color[1] ) / 255.0f;
		vertexColor[2] = static_cast<float>( src.color[2] ) / 255.0f;
		vertexColor[3] = static_cast<float>( src.color[3] ) / 255.0f;
	}

	for ( int i = 0; i < 4; i++ ) {
		dst.color[i] = stage.needsColorModulation ? stage.color[i] * vertexColor[i] : 0.0f;
	}
	for ( int i = 0; i < SW_INTERACTION_ATTR_COUNT; i++ ) {
		dst.attr[i] = 0.0f;
	}
	return true;
}

bool idSoftwareRasterizer::BuildInteractionClipVertex( const idDrawVert &src, const float *modelViewMatrix, const float *projectionMatrix, swClipVert_t &dst ) const {
	if ( !modelViewMatrix || !projectionMatrix ) {
		return false;
	}

	idPlane eye;
	idPlane clip;
	R_TransformModelToClip( src.xyz, modelViewMatrix, projectionMatrix, eye, clip );
	for ( int i = 0; i < 4; i++ ) {
		dst.clip[i] = clip[i];
		dst.color[i] = 0.0f;
	}
	for ( int i = 0; i < 3; i++ ) {
		dst.global[i] = 0.0f;
		dst.normal[i] = 0.0f;
		dst.tangent[0][i] = 0.0f;
		dst.tangent[1][i] = 0.0f;
	}
	dst.s = 0.0f;
	dst.t = 0.0f;

	dst.attr[SW_ATTR_POS_X] = src.xyz[0];
	dst.attr[SW_ATTR_POS_Y] = src.xyz[1];
	dst.attr[SW_ATTR_POS_Z] = src.xyz[2];
	dst.attr[SW_ATTR_ST_X] = src.st[0];
	dst.attr[SW_ATTR_ST_Y] = src.st[1];
	dst.attr[SW_ATTR_NORMAL_X] = src.normal[0];
	dst.attr[SW_ATTR_NORMAL_Y] = src.normal[1];
	dst.attr[SW_ATTR_NORMAL_Z] = src.normal[2];
	dst.attr[SW_ATTR_TANGENT0_X] = src.tangents[0][0];
	dst.attr[SW_ATTR_TANGENT0_Y] = src.tangents[0][1];
	dst.attr[SW_ATTR_TANGENT0_Z] = src.tangents[0][2];
	dst.attr[SW_ATTR_TANGENT1_X] = src.tangents[1][0];
	dst.attr[SW_ATTR_TANGENT1_Y] = src.tangents[1][1];
	dst.attr[SW_ATTR_TANGENT1_Z] = src.tangents[1][2];
	dst.attr[SW_ATTR_COLOR_R] = static_cast<float>( src.color[0] ) / 255.0f;
	dst.attr[SW_ATTR_COLOR_G] = static_cast<float>( src.color[1] ) / 255.0f;
	dst.attr[SW_ATTR_COLOR_B] = static_cast<float>( src.color[2] ) / 255.0f;
	dst.attr[SW_ATTR_COLOR_A] = static_cast<float>( src.color[3] ) / 255.0f;
	return true;
}

int idSoftwareRasterizer::ClipTriangleToFrustum( const swClipVert_t &v0, const swClipVert_t &v1, const swClipVert_t &v2, swClipVert_t clipped[SW_MAX_CLIP_VERTS] ) const {
	swClipVert_t clipBuffers[2][SW_MAX_CLIP_VERTS];
	swClipVert_t *input = clipBuffers[0];
	swClipVert_t *output = clipBuffers[1];
	int inputCount = 3;

	input[0] = v0;
	input[1] = v1;
	input[2] = v2;

	for ( int plane = 0; plane < 6; plane++ ) {
		if ( inputCount < 3 ) {
			return 0;
		}

		int outputCount = 0;
		swClipVert_t previous = input[inputCount - 1];
		float previousDistance = ClipPlaneDistance( previous, plane );
		bool previousInside = previousDistance >= 0.0f;

		for ( int i = 0; i < inputCount; i++ ) {
			const swClipVert_t &current = input[i];
			const float currentDistance = ClipPlaneDistance( current, plane );
			const bool currentInside = currentDistance >= 0.0f;

			if ( currentInside != previousInside ) {
				const float denom = previousDistance - currentDistance;
				const float fraction = denom != 0.0f ? idMath::ClampFloat( 0.0f, 1.0f, previousDistance / denom ) : 0.0f;
				if ( outputCount < SW_MAX_CLIP_VERTS ) {
					LerpClipVertex( previous, current, fraction, output[outputCount++] );
				}
			}
			if ( currentInside && outputCount < SW_MAX_CLIP_VERTS ) {
				output[outputCount++] = current;
			}

			previous = current;
			previousDistance = currentDistance;
			previousInside = currentInside;
		}

		swClipVert_t *swap = input;
		input = output;
		output = swap;
		inputCount = outputCount;
	}

	for ( int i = 0; i < inputCount; i++ ) {
		clipped[i] = input[i];
	}
	return inputCount;
}

bool idSoftwareRasterizer::ProjectClipVertex( const swClipVert_t &src, swScreenVert_t &dst ) const {
	if ( src.clip[3] <= 0.00001f ) {
		return false;
	}

	const float invW = 1.0f / src.clip[3];
	const float ndcX = src.clip[0] * invW;
	const float ndcY = src.clip[1] * invW;
	const float ndcZ = src.clip[2] * invW;

	const float sx = ( idMath::ClampFloat( -1.0f, 1.0f, ndcX ) * 0.5f + 0.5f ) * static_cast<float>( width );
	const float sy = ( idMath::ClampFloat( -1.0f, 1.0f, ndcY ) * 0.5f + 0.5f ) * static_cast<float>( height );

	dst.x = idMath::FtoiFast( sx * static_cast<float>( SW_FP_ONE ) + 0.5f );
	dst.y = idMath::FtoiFast( sy * static_cast<float>( SW_FP_ONE ) + 0.5f );
	dst.z = idMath::ClampFloat( 0.0f, 1.0f, ndcZ * 0.5f + 0.5f );
	dst.invW = invW;
	dst.sOverW = src.s * invW;
	dst.tOverW = src.t * invW;
	for ( int i = 0; i < 4; i++ ) {
		dst.colorOverW[i] = src.color[i] * invW;
	}
	for ( int i = 0; i < 3; i++ ) {
		dst.globalOverW[i] = src.global[i] * invW;
		dst.normalOverW[i] = src.normal[i] * invW;
		dst.tangent0OverW[i] = src.tangent[0][i] * invW;
		dst.tangent1OverW[i] = src.tangent[1][i] * invW;
	}
	return true;
}

bool idSoftwareRasterizer::ProjectInteractionClipVertex( const swClipVert_t &src, swInteractionVert_t &dst ) const {
	if ( src.clip[3] <= 0.00001f ) {
		return false;
	}

	const float invW = 1.0f / src.clip[3];
	const float ndcX = src.clip[0] * invW;
	const float ndcY = src.clip[1] * invW;
	const float ndcZ = src.clip[2] * invW;

	const float sx = ( idMath::ClampFloat( -1.0f, 1.0f, ndcX ) * 0.5f + 0.5f ) * static_cast<float>( width );
	const float sy = ( idMath::ClampFloat( -1.0f, 1.0f, ndcY ) * 0.5f + 0.5f ) * static_cast<float>( height );

	dst.x = idMath::FtoiFast( sx * static_cast<float>( SW_FP_ONE ) + 0.5f );
	dst.y = idMath::FtoiFast( sy * static_cast<float>( SW_FP_ONE ) + 0.5f );
	dst.z = idMath::ClampFloat( 0.0f, 1.0f, ndcZ * 0.5f + 0.5f );
	dst.invW = invW;
	for ( int i = 0; i < SW_INTERACTION_ATTR_COUNT; i++ ) {
		dst.attrOverW[i] = src.attr[i] * invW;
	}
	return true;
}

bool idSoftwareRasterizer::SetupTriangle( const swScreenVert_t &in0, const swScreenVert_t &in1, const swScreenVert_t &in2, int cullType, const swSurfaceStage_t &stage, swTriSetup_t &tri ) const {
	swScreenVert_t v0 = in0;
	swScreenVert_t v1 = in1;
	swScreenVert_t v2 = in2;

	const long long area = EdgeValue( v0.x, v0.y, v1.x, v1.y, v2.x, v2.y );
	if ( area == 0 ) {
		return false;
	}

	if ( cullType != CT_TWO_SIDED ) {
		const bool mirrored = backEnd.viewDef && backEnd.viewDef->isMirror;
		const bool drawPositive = ( cullType == CT_BACK_SIDED ) ^ mirrored;
		if ( drawPositive ) {
			if ( area < 0 ) {
				return false;
			}
		} else if ( area > 0 ) {
			return false;
		}
	}

	if ( area < 0 ) {
		idSwap( v1, v2 );
	}

	tri.x[0] = v0.x;
	tri.y[0] = v0.y;
	tri.z[0] = v0.z;
	tri.invW[0] = v0.invW;
	tri.sOverW[0] = v0.sOverW;
	tri.tOverW[0] = v0.tOverW;
	for ( int i = 0; i < 3; i++ ) {
		tri.globalOverW[0][i] = v0.globalOverW[i];
		tri.normalOverW[0][i] = v0.normalOverW[i];
		tri.tangent0OverW[0][i] = v0.tangent0OverW[i];
		tri.tangent1OverW[0][i] = v0.tangent1OverW[i];
	}
	if ( stage.needsColorModulation ) {
		for ( int i = 0; i < 4; i++ ) {
			tri.colorOverW[0][i] = v0.colorOverW[i];
		}
	}
	tri.x[1] = v1.x;
	tri.y[1] = v1.y;
	tri.z[1] = v1.z;
	tri.invW[1] = v1.invW;
	tri.sOverW[1] = v1.sOverW;
	tri.tOverW[1] = v1.tOverW;
	for ( int i = 0; i < 3; i++ ) {
		tri.globalOverW[1][i] = v1.globalOverW[i];
		tri.normalOverW[1][i] = v1.normalOverW[i];
		tri.tangent0OverW[1][i] = v1.tangent0OverW[i];
		tri.tangent1OverW[1][i] = v1.tangent1OverW[i];
	}
	if ( stage.needsColorModulation ) {
		for ( int i = 0; i < 4; i++ ) {
			tri.colorOverW[1][i] = v1.colorOverW[i];
		}
	}
	tri.x[2] = v2.x;
	tri.y[2] = v2.y;
	tri.z[2] = v2.z;
	tri.invW[2] = v2.invW;
	tri.sOverW[2] = v2.sOverW;
	tri.tOverW[2] = v2.tOverW;
	for ( int i = 0; i < 3; i++ ) {
		tri.globalOverW[2][i] = v2.globalOverW[i];
		tri.normalOverW[2][i] = v2.normalOverW[i];
		tri.tangent0OverW[2][i] = v2.tangent0OverW[i];
		tri.tangent1OverW[2][i] = v2.tangent1OverW[i];
	}
	if ( stage.needsColorModulation ) {
		for ( int i = 0; i < 4; i++ ) {
			tri.colorOverW[2][i] = v2.colorOverW[i];
		}
	}
	tri.textureIndex = stage.textureIndex;
	tri.srcBlend = stage.srcBlend;
	tri.dstBlend = stage.dstBlend;
	tri.blendMode = stage.blendMode;
	tri.writeMask = stage.writeMask;
	tri.depthTest = stage.depthTest;
	tri.depthWrite = stage.depthWrite;
	tri.depthEqual = stage.depthEqual;
	tri.alphaBlend = stage.alphaBlend;
	tri.needsColorModulation = stage.needsColorModulation;
	tri.alphaTest = stage.alphaTest;
	tri.writeWorldPosition = stage.writeWorldPosition;
	tri.writeGBuffer = stage.writeGBuffer;
	tri.alphaTestValue = stage.alphaTestValue;
	tri.alphaTestByte = stage.alphaTestByte;
	tri.fallbackColor = stage.fallbackColor;
	tri.materialId = stage.materialId;
	tri.surfaceId = stage.surfaceId;
	tri.albedoOrTextureId = stage.albedoOrTextureId;
	tri.specularAndFlags = stage.specularAndFlags;

	tri.minX = Max( 0, ( Min3( tri.x[0], tri.x[1], tri.x[2] ) + SW_FP_ONE - 1 ) >> SW_FP_SHIFT );
	tri.minY = Max( 0, ( Min3( tri.y[0], tri.y[1], tri.y[2] ) + SW_FP_ONE - 1 ) >> SW_FP_SHIFT );
	tri.maxX = Min( width - 1, Max3( tri.x[0], tri.x[1], tri.x[2] ) >> SW_FP_SHIFT );
	tri.maxY = Min( height - 1, Max3( tri.y[0], tri.y[1], tri.y[2] ) >> SW_FP_SHIFT );

	if ( tri.minX > tri.maxX || tri.minY > tri.maxY ) {
		return false;
	}

	tri.tileMinX = tri.minX / SW_TILE_SIZE;
	tri.tileMinY = tri.minY / SW_TILE_SIZE;
	tri.tileMaxX = tri.maxX / SW_TILE_SIZE;
	tri.tileMaxY = tri.maxY / SW_TILE_SIZE;

	for ( int i = 0; i < 3; i++ ) {
		const int j = ( i + 1 ) % 3;
		tri.A[i] = tri.y[i] - tri.y[j];
		tri.B[i] = tri.x[j] - tri.x[i];
		tri.C[i] = static_cast<long long>( tri.x[i] ) * tri.y[j] - static_cast<long long>( tri.y[i] ) * tri.x[j] + TopLeftBias( tri.x[i], tri.y[i], tri.x[j], tri.y[j] );
	}

	const float fx0 = static_cast<float>( tri.x[0] ) / static_cast<float>( SW_FP_ONE );
	const float fy0 = static_cast<float>( tri.y[0] ) / static_cast<float>( SW_FP_ONE );
	const float fx1 = static_cast<float>( tri.x[1] ) / static_cast<float>( SW_FP_ONE );
	const float fy1 = static_cast<float>( tri.y[1] ) / static_cast<float>( SW_FP_ONE );
	const float fx2 = static_cast<float>( tri.x[2] ) / static_cast<float>( SW_FP_ONE );
	const float fy2 = static_cast<float>( tri.y[2] ) / static_cast<float>( SW_FP_ONE );
	const float denom = ( fx1 - fx0 ) * ( fy2 - fy0 ) - ( fy1 - fy0 ) * ( fx2 - fx0 );

	if ( denom == 0.0f ) {
		return false;
	}

	SetupFloatPlane( tri.z[0], tri.z[1], tri.z[2], fx0, fy0, fx1, fy1, fx2, fy2, denom, tri.z0, tri.dzdx, tri.dzdy );
	SetupFloatPlane( tri.invW[0], tri.invW[1], tri.invW[2], fx0, fy0, fx1, fy1, fx2, fy2, denom, tri.invW0, tri.dinvWdx, tri.dinvWdy );
	SetupFloatPlane( tri.sOverW[0], tri.sOverW[1], tri.sOverW[2], fx0, fy0, fx1, fy1, fx2, fy2, denom, tri.sOverW0, tri.dsOverWdx, tri.dsOverWdy );
	SetupFloatPlane( tri.tOverW[0], tri.tOverW[1], tri.tOverW[2], fx0, fy0, fx1, fy1, fx2, fy2, denom, tri.tOverW0, tri.dtOverWdx, tri.dtOverWdy );
	if ( tri.needsColorModulation ) {
		for ( int i = 0; i < 4; i++ ) {
			SetupFloatPlane( tri.colorOverW[0][i], tri.colorOverW[1][i], tri.colorOverW[2][i], fx0, fy0, fx1, fy1, fx2, fy2, denom, tri.colorOverW0[i], tri.dColorOverWdx[i], tri.dColorOverWdy[i] );
		}
	}
	if ( tri.writeWorldPosition ) {
		for ( int i = 0; i < 3; i++ ) {
			SetupFloatPlane( tri.globalOverW[0][i], tri.globalOverW[1][i], tri.globalOverW[2][i], fx0, fy0, fx1, fy1, fx2, fy2, denom, tri.globalOverW0[i], tri.dGlobalOverWdx[i], tri.dGlobalOverWdy[i] );
		}
	}
	if ( tri.writeGBuffer ) {
		for ( int i = 0; i < 3; i++ ) {
			SetupFloatPlane( tri.normalOverW[0][i], tri.normalOverW[1][i], tri.normalOverW[2][i], fx0, fy0, fx1, fy1, fx2, fy2, denom, tri.normalOverW0[i], tri.dNormalOverWdx[i], tri.dNormalOverWdy[i] );
			SetupFloatPlane( tri.tangent0OverW[0][i], tri.tangent0OverW[1][i], tri.tangent0OverW[2][i], fx0, fy0, fx1, fy1, fx2, fy2, denom, tri.tangent0OverW0[i], tri.dTangent0OverWdx[i], tri.dTangent0OverWdy[i] );
			SetupFloatPlane( tri.tangent1OverW[0][i], tri.tangent1OverW[1][i], tri.tangent1OverW[2][i], fx0, fy0, fx1, fy1, fx2, fy2, denom, tri.tangent1OverW0[i], tri.dTangent1OverWdx[i], tri.dTangent1OverWdy[i] );
		}
	}

	return true;
}

bool idSoftwareRasterizer::SetupInteractionTriangle( const swInteractionVert_t &in0, const swInteractionVert_t &in1, const swInteractionVert_t &in2, int cullType, const drawInteraction_t &interaction, swInteractionTri_t &tri ) {
	swInteractionVert_t v0 = in0;
	swInteractionVert_t v1 = in1;
	swInteractionVert_t v2 = in2;

	const long long area = EdgeValue( v0.x, v0.y, v1.x, v1.y, v2.x, v2.y );
	if ( area == 0 ) {
		return false;
	}

	if ( cullType != CT_TWO_SIDED ) {
		const bool mirrored = backEnd.viewDef && backEnd.viewDef->isMirror;
		const bool drawPositive = ( cullType == CT_BACK_SIDED ) ^ mirrored;
		if ( drawPositive ) {
			if ( area < 0 ) {
				return false;
			}
		} else if ( area > 0 ) {
			return false;
		}
	}

	if ( area < 0 ) {
		idSwap( v1, v2 );
	}

	tri.x[0] = v0.x;
	tri.y[0] = v0.y;
	tri.z[0] = v0.z;
	tri.invW[0] = v0.invW;
	for ( int i = 0; i < SW_INTERACTION_ATTR_COUNT; i++ ) {
		tri.attrOverW[0][i] = v0.attrOverW[i];
	}
	tri.x[1] = v1.x;
	tri.y[1] = v1.y;
	tri.z[1] = v1.z;
	tri.invW[1] = v1.invW;
	for ( int i = 0; i < SW_INTERACTION_ATTR_COUNT; i++ ) {
		tri.attrOverW[1][i] = v1.attrOverW[i];
	}
	tri.x[2] = v2.x;
	tri.y[2] = v2.y;
	tri.z[2] = v2.z;
	tri.invW[2] = v2.invW;
	for ( int i = 0; i < SW_INTERACTION_ATTR_COUNT; i++ ) {
		tri.attrOverW[2][i] = v2.attrOverW[i];
	}

	tri.minX = Max( 0, ( Min3( tri.x[0], tri.x[1], tri.x[2] ) + SW_FP_ONE - 1 ) >> SW_FP_SHIFT );
	tri.minY = Max( 0, ( Min3( tri.y[0], tri.y[1], tri.y[2] ) + SW_FP_ONE - 1 ) >> SW_FP_SHIFT );
	tri.maxX = Min( width - 1, Max3( tri.x[0], tri.x[1], tri.x[2] ) >> SW_FP_SHIFT );
	tri.maxY = Min( height - 1, Max3( tri.y[0], tri.y[1], tri.y[2] ) >> SW_FP_SHIFT );

	if ( tri.minX > tri.maxX || tri.minY > tri.maxY ) {
		return false;
	}

	tri.tileMinX = tri.minX / SW_TILE_SIZE;
	tri.tileMinY = tri.minY / SW_TILE_SIZE;
	tri.tileMaxX = tri.maxX / SW_TILE_SIZE;
	tri.tileMaxY = tri.maxY / SW_TILE_SIZE;

	for ( int i = 0; i < 3; i++ ) {
		const int j = ( i + 1 ) % 3;
		tri.A[i] = tri.y[i] - tri.y[j];
		tri.B[i] = tri.x[j] - tri.x[i];
		tri.C[i] = static_cast<long long>( tri.x[i] ) * tri.y[j] - static_cast<long long>( tri.y[i] ) * tri.x[j] + TopLeftBias( tri.x[i], tri.y[i], tri.x[j], tri.y[j] );
	}

	const float fx0 = static_cast<float>( tri.x[0] ) / static_cast<float>( SW_FP_ONE );
	const float fy0 = static_cast<float>( tri.y[0] ) / static_cast<float>( SW_FP_ONE );
	const float fx1 = static_cast<float>( tri.x[1] ) / static_cast<float>( SW_FP_ONE );
	const float fy1 = static_cast<float>( tri.y[1] ) / static_cast<float>( SW_FP_ONE );
	const float fx2 = static_cast<float>( tri.x[2] ) / static_cast<float>( SW_FP_ONE );
	const float fy2 = static_cast<float>( tri.y[2] ) / static_cast<float>( SW_FP_ONE );
	const float denom = ( fx1 - fx0 ) * ( fy2 - fy0 ) - ( fy1 - fy0 ) * ( fx2 - fx0 );

	if ( denom == 0.0f ) {
		return false;
	}

	SetupFloatPlane( tri.z[0], tri.z[1], tri.z[2], fx0, fy0, fx1, fy1, fx2, fy2, denom, tri.z0, tri.dzdx, tri.dzdy );
	SetupFloatPlane( tri.invW[0], tri.invW[1], tri.invW[2], fx0, fy0, fx1, fy1, fx2, fy2, denom, tri.invW0, tri.dinvWdx, tri.dinvWdy );
	for ( int i = 0; i < SW_INTERACTION_ATTR_COUNT; i++ ) {
		SetupFloatPlane( tri.attrOverW[0][i], tri.attrOverW[1][i], tri.attrOverW[2][i], fx0, fy0, fx1, fy1, fx2, fy2, denom, tri.attrOverW0[i], tri.dAttrOverWdx[i], tri.dAttrOverWdy[i] );
	}

	tri.bumpTextureIndex = TextureIndexForImage( interaction.bumpImage );
	tri.diffuseTextureIndex = TextureIndexForImage( interaction.diffuseImage );
	tri.specularTextureIndex = TextureIndexForImage( interaction.specularImage );
	tri.lightTextureIndex = TextureIndexForImage( interaction.lightImage );
	tri.falloffTextureIndex = TextureIndexForImage( interaction.lightFalloffImage );
	tri.diffuseColor = interaction.diffuseColor;
	tri.specularColor = interaction.specularColor;
	tri.vertexColor = interaction.vertexColor;
	tri.ambientLight = interaction.ambientLight != 0;
	tri.depthEqual = backEnd.depthFunc == GLS_DEPTHFUNC_EQUAL;
	tri.localLightOrigin = interaction.localLightOrigin;
	tri.localViewOrigin = interaction.localViewOrigin;
	for ( int i = 0; i < 4; i++ ) {
		tri.lightProjection[i] = interaction.lightProjection[i];
	}
	for ( int i = 0; i < 2; i++ ) {
		tri.bumpMatrix[i] = interaction.bumpMatrix[i];
		tri.diffuseMatrix[i] = interaction.diffuseMatrix[i];
		tri.specularMatrix[i] = interaction.specularMatrix[i];
	}
	return true;
}

void idSoftwareRasterizer::BinTriangle( int triIndex ) {
	const swTriSetup_t &tri = triangles[triIndex];
	for ( int ty = tri.tileMinY; ty <= tri.tileMaxY; ty++ ) {
		for ( int tx = tri.tileMinX; tx <= tri.tileMaxX; tx++ ) {
			tileBins[ty * tileCountX + tx].tris.Append( triIndex );
		}
	}
}

void idSoftwareRasterizer::BinInteractionTriangle( int triIndex ) {
	const swInteractionTri_t &tri = interactionTriangles[triIndex];
	for ( int ty = tri.tileMinY; ty <= tri.tileMaxY; ty++ ) {
		for ( int tx = tri.tileMinX; tx <= tri.tileMaxX; tx++ ) {
			tileBins[ty * tileCountX + tx].tris.Append( triIndex );
		}
	}
}

void idSoftwareRasterizer::DrawLights( const viewDef_t *viewDef ) {
	if ( !viewDef || r_skipInteractions.GetBool() ) {
		return;
	}

	const bool rayQueryShadows = r_softwareRayQueryShadows.GetBool() &&
		SWVulkan_RayQueryAvailable() &&
		SWVulkan_PrepareRayQueryScene( viewDef );

	const int oldDepthFunc = backEnd.depthFunc;
	const viewEntity_t *oldSpace = backEnd.currentSpace;
	viewLight_t *oldLight = backEnd.vLight;

	for ( viewLight_t *vLight = viewDef->viewLights; vLight; vLight = vLight->next ) {
		backEnd.vLight = vLight;
		backEnd.currentSpace = NULL;
		shadowMaskActive = false;

		if ( vLight->lightShader->IsFogLight() || vLight->lightShader->IsBlendLight() ) {
			continue;
		}
		if ( !vLight->localInteractions && !vLight->globalInteractions && !vLight->translucentInteractions ) {
			continue;
		}

		const bool wantsRayQueryShadow = rayQueryShadows && !vLight->lightShader->IsAmbientLight();
		const bool shadowMaskPending = wantsRayQueryShadow &&
			SWVulkan_BeginLightShadowMask( vLight, worldPositionBuffer.Ptr(), width, height );

		BeginInteractionPass();
		backEnd.depthFunc = GLS_DEPTHFUNC_EQUAL;
		for ( const drawSurf_t *surf = vLight->localInteractions; surf; surf = surf->nextOnLight ) {
			RB_CreateSingleDrawInteractions( surf, RB_SW_DrawInteractionCallback );
		}
		for ( const drawSurf_t *surf = vLight->globalInteractions; surf; surf = surf->nextOnLight ) {
			RB_CreateSingleDrawInteractions( surf, RB_SW_DrawInteractionCallback );
		}

		if ( !r_skipTranslucent.GetBool() ) {
			backEnd.depthFunc = GLS_DEPTHFUNC_LESS;
			for ( const drawSurf_t *surf = vLight->translucentInteractions; surf; surf = surf->nextOnLight ) {
				RB_CreateSingleDrawInteractions( surf, RB_SW_DrawInteractionCallback );
			}
		}

		if ( interactionTriangles.Num() > 0 ) {
			shadowMaskActive = shadowMaskPending &&
				SWVulkan_FinishLightShadowMask( vLight, width, height, shadowMask.Ptr() );
			RasterizeTiles();
		} else if ( shadowMaskPending ) {
			SWVulkan_FinishLightShadowMask( vLight, width, height, shadowMask.Ptr() );
		}
	}

	shadowMaskActive = false;
	backEnd.depthFunc = oldDepthFunc;
	backEnd.currentSpace = oldSpace;
	backEnd.vLight = oldLight;
}

bool idSoftwareRasterizer::BuildFogLightState( const viewDef_t *viewDef, const viewLight_t *vLight, swFogLightState_t &fog ) {
	if ( !viewDef || !vLight || !vLight->lightShader || !vLight->shaderRegisters || vLight->scissorRect.IsEmpty() || !globalImages ) {
		return false;
	}

	const shaderStage_t *stage = vLight->lightShader->GetStage( 0 );
	if ( !stage || vLight->shaderRegisters[stage->conditionRegister] == 0.0f ) {
		return false;
	}

	fog.color[0] = vLight->shaderRegisters[stage->color.registers[0]];
	fog.color[1] = vLight->shaderRegisters[stage->color.registers[1]];
	fog.color[2] = vLight->shaderRegisters[stage->color.registers[2]];
	fog.color[3] = vLight->shaderRegisters[stage->color.registers[3]];

	const float densityDistance = fog.color[3] <= 1.0f ? DEFAULT_FOG_DISTANCE : fog.color[3];
	const float a = -0.5f / densityDistance;
	const float *modelView = viewDef->worldSpace.modelViewMatrix;
	fog.fogPlane[0] = a * modelView[2];
	fog.fogPlane[1] = a * modelView[6];
	fog.fogPlane[2] = a * modelView[10];
	fog.fogPlane[3] = a * modelView[14];

	for ( int i = 0; i < 4; i++ ) {
		fog.enterPlane[i] = 0.001f * vLight->fogPlane[i];
	}
	fog.enterS = FOG_ENTER + viewDef->renderView.vieworg * fog.enterPlane.ToVec3() + fog.enterPlane[3];
	fog.fogTextureIndex = TextureIndexForImage( globalImages->fogImage );
	fog.fogEnterTextureIndex = TextureIndexForImage( globalImages->fogEnterImage );
	fog.scissorRect = vLight->scissorRect;
	return true;
}

bool idSoftwareRasterizer::BuildBlendLightStage( const viewLight_t *vLight, const shaderStage_t *stage, swBlendLightStage_t &blend ) {
	if ( !vLight || !stage || !stage->texture.image || !vLight->shaderRegisters || !vLight->falloffImage || vLight->scissorRect.IsEmpty() ) {
		return false;
	}
	if ( vLight->shaderRegisters[stage->conditionRegister] == 0.0f ) {
		return false;
	}

	blend.srcBlend = stage->drawStateBits & GLS_SRCBLEND_BITS;
	blend.dstBlend = stage->drawStateBits & GLS_DSTBLEND_BITS;
	if ( blend.srcBlend == GLS_SRCBLEND_ZERO && blend.dstBlend == GLS_DSTBLEND_ONE ) {
		return false;
	}
	blend.blendMode = BlendModeForBits( blend.srcBlend, blend.dstBlend );
	blend.writeMask = SW_WRITE_COLOR;
	if ( stage->drawStateBits & GLS_REDMASK ) {
		blend.writeMask &= ~SW_WRITE_RED;
	}
	if ( stage->drawStateBits & GLS_GREENMASK ) {
		blend.writeMask &= ~SW_WRITE_GREEN;
	}
	if ( stage->drawStateBits & GLS_BLUEMASK ) {
		blend.writeMask &= ~SW_WRITE_BLUE;
	}
	if ( stage->drawStateBits & GLS_ALPHAMASK ) {
		blend.writeMask &= ~SW_WRITE_ALPHA;
	}
	if ( blend.writeMask == 0 ) {
		return false;
	}

	for ( int i = 0; i < 4; i++ ) {
		blend.color[i] = vLight->shaderRegisters[stage->color.registers[i]];
	}

	idPlane projectedLightProject[4];
	for ( int i = 0; i < 4; i++ ) {
		projectedLightProject[i] = vLight->lightProject[i];
	}
	if ( stage->texture.hasMatrix ) {
		RB_GetShaderTextureMatrix( vLight->shaderRegisters, &stage->texture, backEnd.lightTextureMatrix );
		RB_BakeTextureMatrixIntoTexgen( projectedLightProject, backEnd.lightTextureMatrix );
	}
	for ( int plane = 0; plane < 4; plane++ ) {
		for ( int component = 0; component < 4; component++ ) {
			blend.lightProject[plane][component] = projectedLightProject[plane][component];
		}
	}

	blend.lightTextureIndex = TextureIndexForImage( stage->texture.image );
	blend.falloffTextureIndex = TextureIndexForImage( vLight->falloffImage );
	blend.scissorRect = vLight->scissorRect;
	return true;
}

void idSoftwareRasterizer::ApplyFogLights( const viewDef_t *viewDef ) {
	if ( !viewDef || !viewDef->viewLights || r_skipFogLights.GetBool() ||
		 r_showOverDraw.GetInteger() != 0 || viewDef->isXraySubview ) {
		return;
	}
	if ( worldPositionBuffer.Num() != width * height || depthBuffer.Num() != width * height || colorBuffer.Num() != width * height ) {
		return;
	}

	for ( const viewLight_t *vLight = viewDef->viewLights; vLight; vLight = vLight->next ) {
		if ( !vLight->lightShader ) {
			continue;
		}

		if ( vLight->lightShader->IsFogLight() ) {
			swFogLightState_t fog;
			if ( BuildFogLightState( viewDef, vLight, fog ) ) {
				ApplyFogLight( fog );
			}
			continue;
		}

		if ( r_skipBlendLights.GetBool() || !vLight->lightShader->IsBlendLight() ||
			 ( !vLight->globalInteractions && !vLight->localInteractions ) ) {
			continue;
		}
		for ( int stageIndex = 0; stageIndex < vLight->lightShader->GetNumStages(); stageIndex++ ) {
			const shaderStage_t *stage = vLight->lightShader->GetStage( stageIndex );
			swBlendLightStage_t blend;
			if ( BuildBlendLightStage( vLight, stage, blend ) ) {
				ApplyBlendLight( blend );
			}
		}
	}
}

void idSoftwareRasterizer::ApplyFogLight( const swFogLightState_t &fog ) {
	const int denomX = Max( 1, presentWidth );
	const int denomY = Max( 1, presentHeight );
	const int x0 = Max( 0, Min( width, ( static_cast<int>( fog.scissorRect.x1 ) * width ) / denomX ) );
	const int y0 = Max( 0, Min( height, ( static_cast<int>( fog.scissorRect.y1 ) * height ) / denomY ) );
	const int x1 = Max( x0, Min( width, ( ( static_cast<int>( fog.scissorRect.x2 ) + 1 ) * width + denomX - 1 ) / denomX ) );
	const int y1 = Max( y0, Min( height, ( ( static_cast<int>( fog.scissorRect.y2 ) + 1 ) * height + denomY - 1 ) / denomY ) );

	for ( int y = y0; y < y1; y++ ) {
		for ( int x = x0; x < x1; x++ ) {
			const int index = y * width + x;
			if ( depthBuffer[index] >= 1.0f || worldPositionBuffer[index][3] <= 0.0f ) {
				continue;
			}

			const idVec3 world = worldPositionBuffer[index].ToVec3();
			const float fogS = DotPlanePoint( fog.fogPlane, world ) + 0.5f;
			const float enterT = DotPlanePoint( fog.enterPlane, world ) + FOG_ENTER;
			const unsigned int fogTexel = SampleTextureLinear( fog.fogTextureIndex, fogS, 0.5f );
			const unsigned int enterTexel = SampleTextureLinear( fog.fogEnterTextureIndex, fog.enterS, enterT );
			const float alpha = ColorChannel( fogTexel, 24 ) * ColorChannel( enterTexel, 24 );
			if ( alpha <= 0.0f ) {
				continue;
			}

			const unsigned int src = PackColor(
				ByteFromFloat( fog.color[0] * ColorChannel( fogTexel, 16 ) * ColorChannel( enterTexel, 16 ) ),
				ByteFromFloat( fog.color[1] * ColorChannel( fogTexel, 8 ) * ColorChannel( enterTexel, 8 ) ),
				ByteFromFloat( fog.color[2] * ColorChannel( fogTexel, 0 ) * ColorChannel( enterTexel, 0 ) ),
				ByteFromFloat( alpha ) );
			colorBuffer[index] = BlendSourceOver( src, colorBuffer[index] );
		}
	}
}

void idSoftwareRasterizer::ApplyBlendLight( const swBlendLightStage_t &blend ) {
	const int denomX = Max( 1, presentWidth );
	const int denomY = Max( 1, presentHeight );
	const int x0 = Max( 0, Min( width, ( static_cast<int>( blend.scissorRect.x1 ) * width ) / denomX ) );
	const int y0 = Max( 0, Min( height, ( static_cast<int>( blend.scissorRect.y1 ) * height ) / denomY ) );
	const int x1 = Max( x0, Min( width, ( ( static_cast<int>( blend.scissorRect.x2 ) + 1 ) * width + denomX - 1 ) / denomX ) );
	const int y1 = Max( y0, Min( height, ( ( static_cast<int>( blend.scissorRect.y2 ) + 1 ) * height + denomY - 1 ) / denomY ) );

	for ( int y = y0; y < y1; y++ ) {
		for ( int x = x0; x < x1; x++ ) {
			const int index = y * width + x;
			if ( depthBuffer[index] >= 1.0f || worldPositionBuffer[index][3] <= 0.0f ) {
				continue;
			}

			const idVec3 world = worldPositionBuffer[index].ToVec3();
			const float lightQ = DotPlanePoint( blend.lightProject[2], world );
			if ( lightQ <= 0.00001f ) {
				continue;
			}
			const float lightS = DotPlanePoint( blend.lightProject[0], world ) / lightQ;
			const float lightT = DotPlanePoint( blend.lightProject[1], world ) / lightQ;
			const float falloffS = DotPlanePoint( blend.lightProject[3], world );
			const unsigned int lightTexel = SampleTextureLinear( blend.lightTextureIndex, lightS, lightT );
			const unsigned int falloffTexel = SampleTextureLinear( blend.falloffTextureIndex, falloffS, 0.5f );

			const unsigned int src = PackColor(
				ByteFromFloat( blend.color[0] * ColorChannel( lightTexel, 16 ) * ColorChannel( falloffTexel, 16 ) ),
				ByteFromFloat( blend.color[1] * ColorChannel( lightTexel, 8 ) * ColorChannel( falloffTexel, 8 ) ),
				ByteFromFloat( blend.color[2] * ColorChannel( lightTexel, 0 ) * ColorChannel( falloffTexel, 0 ) ),
				ByteFromFloat( blend.color[3] * ColorChannel( lightTexel, 24 ) * ColorChannel( falloffTexel, 24 ) ) );
			const unsigned int dst = colorBuffer[index];
			const unsigned int blended = BlendColor( src, dst, blend.srcBlend, blend.dstBlend, blend.blendMode );
			colorBuffer[index] = ApplyWriteMask( blended, dst, blend.writeMask );
		}
	}
}

void idSoftwareRasterizer::WriteHybridDebugView() {
	const int pixelCount = width * height;
	if ( colorBuffer.Num() != pixelCount || hybridGBuffer.depth.Num() != pixelCount ) {
		return;
	}

	const int debugView = r_softwareHybridDebugView.GetInteger();
	idVec3 debugLightDir( -0.35f, 0.45f, 0.82f );
	debugLightDir.Normalize();
	for ( int i = 0; i < pixelCount; i++ ) {
		if ( hybridGBuffer.depth[i] >= 1.0f || hybridGBuffer.surfaceId[i] == 0 ) {
			colorBuffer[i] = 0xff000000u;
			continue;
		}

		switch ( debugView ) {
		case 0: {
			const unsigned int textureId = hybridGBuffer.albedoOrTextureId[i];
			const int textureIndex = ( textureId != 0xffffffffu && textureId < 0x7fffffffu ) ? static_cast<int>( textureId ) : -1;
			const unsigned int packedUV = hybridGBuffer.uvPacked[i];
			const float s = static_cast<float>( packedUV & 0xffffu ) * ( 1.0f / 65535.0f );
			const float t = static_cast<float>( ( packedUV >> 16 ) & 0xffffu ) * ( 1.0f / 65535.0f );
			const unsigned int texel = SampleTexture( textureIndex, s, t );
			idVec3 normal( UnpackSnorm10( hybridGBuffer.normalPacked[i], 0 ),
				UnpackSnorm10( hybridGBuffer.normalPacked[i], 10 ),
				UnpackSnorm10( hybridGBuffer.normalPacked[i], 20 ) );
			if ( normal.Normalize() == 0.0f ) {
				normal.Set( 0.0f, 0.0f, 1.0f );
			}
			const float nDotL = Max( 0.0f, normal[0] * debugLightDir[0] + normal[1] * debugLightDir[1] + normal[2] * debugLightDir[2] );
			const float light = 0.20f + 0.80f * nDotL;
			const int r = ByteFromFloat( ColorChannel( texel, 16 ) * light );
			const int g = ByteFromFloat( ColorChannel( texel, 8 ) * light );
			const int b = ByteFromFloat( ColorChannel( texel, 0 ) * light );
			const int a = ( texel >> 24 ) & 255;
			colorBuffer[i] = PackColor( r, g, b, a );
			break;
		}
		case 1: {
			const int v = ByteFromFloat( 1.0f - hybridGBuffer.depth[i] );
			colorBuffer[i] = PackColor( v, v, v, 255 );
			break;
		}
		case 2: {
			const unsigned int packed = hybridGBuffer.normalPacked[i];
			const int r = ByteFromFloat( UnpackSnorm10( packed, 0 ) * 0.5f + 0.5f );
			const int g = ByteFromFloat( UnpackSnorm10( packed, 10 ) * 0.5f + 0.5f );
			const int b = ByteFromFloat( UnpackSnorm10( packed, 20 ) * 0.5f + 0.5f );
			colorBuffer[i] = PackColor( r, g, b, 255 );
			break;
		}
		case 3: {
			const unsigned int packed = hybridGBuffer.uvPacked[i];
			const int s = static_cast<int>( packed & 0xffffu ) >> 8;
			const int t = static_cast<int>( ( packed >> 16 ) & 0xffffu ) >> 8;
			colorBuffer[i] = PackColor( s, t, 64, 255 );
			break;
		}
		case 4:
			colorBuffer[i] = IdDebugColor( hybridGBuffer.materialId[i] );
			break;
		case 5: {
			const unsigned int textureId = hybridGBuffer.albedoOrTextureId[i];
			const int textureIndex = ( textureId != 0xffffffffu && textureId < 0x7fffffffu ) ? static_cast<int>( textureId ) : -1;
			const unsigned int packedUV = hybridGBuffer.uvPacked[i];
			const float s = static_cast<float>( packedUV & 0xffffu ) * ( 1.0f / 65535.0f );
			const float t = static_cast<float>( ( packedUV >> 16 ) & 0xffffu ) * ( 1.0f / 65535.0f );
			colorBuffer[i] = SampleTexture( textureIndex, s, t );
			break;
		}
		case 6:
			colorBuffer[i] = IdDebugColor( hybridGBuffer.surfaceId[i] );
			break;
		default:
			colorBuffer[i] = 0xff000000u;
			break;
		}
	}
}

void idSoftwareRasterizer::BuildHybridTextureUpload( swHybridGBufferUpload_t &gbuffer ) {
	PrimeHybridTextureCacheFromImageManager();

	if ( hybridTextureAtlasGeneration != textureCacheGeneration ||
		 hybridTextureInfos.Num() == 0 ||
		 hybridTextureTexels.Num() == 0 ) {
		const unsigned int whiteTexel = PackColor( 255, 255, 255, 255 );
		const int maxHybridTextureTexels = 256 * 1024 * 1024;
		const int textureCount = Max( 1, textureCache.Num() );

		hybridTextureInfos.SetNum( textureCount, false );
		swHybridTextureInfo_t defaultInfo;
		defaultInfo.offset = 0;
		defaultInfo.width = 1;
		defaultInfo.height = 1;
		defaultInfo.repeat = TR_REPEAT;
		SWFillList( hybridTextureInfos, defaultInfo );

		int totalTexels = 1;
		for ( int i = 0; i < textureCache.Num(); i++ ) {
			const swTexture_t &texture = textureCache[i];
			if ( texture.width <= 0 || texture.height <= 0 || texture.texels.Num() <= 0 ) {
				continue;
			}
			const int texelCount = texture.texels.Num();
			if ( texelCount <= 0 || totalTexels > maxHybridTextureTexels - texelCount ) {
				continue;
			}

			swHybridTextureInfo_t &info = hybridTextureInfos[i];
			info.offset = static_cast<unsigned int>( totalTexels );
			info.width = static_cast<unsigned int>( texture.width );
			info.height = static_cast<unsigned int>( texture.height );
			info.repeat = static_cast<unsigned int>( texture.repeat );
			totalTexels += texelCount;
		}

		hybridTextureTexels.SetNum( totalTexels, false );
		hybridTextureTexels[0] = whiteTexel;
		int outTexel = 1;
		for ( int i = 0; i < textureCache.Num(); i++ ) {
			swHybridTextureInfo_t &info = hybridTextureInfos[i];
			const swTexture_t &texture = textureCache[i];
			if ( info.offset != static_cast<unsigned int>( outTexel ) ) {
				continue;
			}

			const int texelCount = texture.texels.Num();
			for ( int texel = 0; texel < texelCount; texel++ ) {
				hybridTextureTexels[outTexel++] = texel < texture.texels.Num() ? texture.texels[texel] : whiteTexel;
			}
		}
		hybridTextureAtlasGeneration = textureCacheGeneration;
	}

	gbuffer.textureInfos = hybridTextureInfos.Ptr();
	gbuffer.textureInfoCount = hybridTextureInfos.Num();
	gbuffer.textureTexels = hybridTextureTexels.Ptr();
	gbuffer.textureTexelCount = hybridTextureTexels.Num();
	gbuffer.textureGeneration = hybridTextureAtlasGeneration;
}

void idSoftwareRasterizer::BuildHybridLightUpload( const viewDef_t *viewDef, swHybridGBufferUpload_t &gbuffer ) {
	hybridLights.SetNum( 0, false );
	if ( !viewDef ) {
		gbuffer.lights = NULL;
		gbuffer.lightCount = 0;
		return;
	}

	static const int MAX_HYBRID_LIGHTS = 64;
	const bool skipFogSystem = r_skipFogLights.GetBool() || r_showOverDraw.GetInteger() != 0 || viewDef->isXraySubview;
	for ( const viewLight_t *vLight = viewDef->viewLights; vLight && hybridLights.Num() < MAX_HYBRID_LIGHTS; vLight = vLight->next ) {
		if ( !vLight->lightShader ) {
			continue;
		}
		if ( vLight->lightShader->IsFogLight() ) {
			if ( skipFogSystem ) {
				continue;
			}
			swFogLightState_t fog;
			if ( BuildFogLightState( viewDef, vLight, fog ) ) {
				AddHybridFogLight( fog );
			}
			continue;
		}
		if ( vLight->lightShader->IsBlendLight() ) {
			if ( skipFogSystem || r_skipBlendLights.GetBool() || ( !vLight->globalInteractions && !vLight->localInteractions ) ) {
				continue;
			}
			for ( int stageIndex = 0; stageIndex < vLight->lightShader->GetNumStages() && hybridLights.Num() < MAX_HYBRID_LIGHTS; stageIndex++ ) {
				const shaderStage_t *stage = vLight->lightShader->GetStage( stageIndex );
				swBlendLightStage_t blend;
				if ( BuildBlendLightStage( vLight, stage, blend ) ) {
					AddHybridBlendLight( blend );
				}
			}
			continue;
		}
		if ( !vLight->localInteractions && !vLight->globalInteractions && !vLight->translucentInteractions ) {
			continue;
		}
		if ( vLight->scissorRect.IsEmpty() ) {
			continue;
		}

		float lightColor[3] = { 0.0f, 0.0f, 0.0f };
		idPlane projectedLightProject[4];
		for ( int i = 0; i < 4; i++ ) {
			projectedLightProject[i] = vLight->lightProject[i];
		}
		int lightTextureIndex = -1;
		if ( vLight->shaderRegisters ) {
			for ( int stageIndex = 0; stageIndex < vLight->lightShader->GetNumStages(); stageIndex++ ) {
				const shaderStage_t *stage = vLight->lightShader->GetStage( stageIndex );
				if ( !stage || vLight->shaderRegisters[stage->conditionRegister] == 0.0f ) {
					continue;
				}
				lightColor[0] += backEnd.lightScale * vLight->shaderRegisters[stage->color.registers[0]];
				lightColor[1] += backEnd.lightScale * vLight->shaderRegisters[stage->color.registers[1]];
				lightColor[2] += backEnd.lightScale * vLight->shaderRegisters[stage->color.registers[2]];
				if ( lightTextureIndex < 0 && stage->texture.image ) {
					lightTextureIndex = TextureIndexForImage( stage->texture.image );
					for ( int i = 0; i < 4; i++ ) {
						projectedLightProject[i] = vLight->lightProject[i];
					}
					if ( stage->texture.hasMatrix ) {
						RB_GetShaderTextureMatrix( vLight->shaderRegisters, &stage->texture, backEnd.lightTextureMatrix );
						RB_BakeTextureMatrixIntoTexgen( projectedLightProject, backEnd.lightTextureMatrix );
					}
				}
			}
		}
		if ( lightColor[0] <= 0.0f && lightColor[1] <= 0.0f && lightColor[2] <= 0.0f && vLight->lightDef ) {
			lightColor[0] = backEnd.lightScale * vLight->lightDef->parms.shaderParms[SHADERPARM_RED];
			lightColor[1] = backEnd.lightScale * vLight->lightDef->parms.shaderParms[SHADERPARM_GREEN];
			lightColor[2] = backEnd.lightScale * vLight->lightDef->parms.shaderParms[SHADERPARM_BLUE];
		}
		if ( lightColor[0] <= 0.0f && lightColor[1] <= 0.0f && lightColor[2] <= 0.0f ) {
			continue;
		}

		float radius = 1024.0f;
		bool parallel = false;
		if ( vLight->lightDef ) {
			const renderLight_t &parms = vLight->lightDef->parms;
			parallel = parms.parallel;
			radius = Max( idMath::Fabs( parms.lightRadius[0] ), Max( idMath::Fabs( parms.lightRadius[1] ), idMath::Fabs( parms.lightRadius[2] ) ) );
			radius = Max( radius, 1.0f );
		}
		if ( parallel ) {
			radius = 1000000.0f;
		}
		const int falloffTextureIndex = TextureIndexForImage( vLight->falloffImage );

		swHybridLight_t &light = hybridLights.Alloc();
		light.originRadius[0] = vLight->globalLightOrigin[0];
		light.originRadius[1] = vLight->globalLightOrigin[1];
		light.originRadius[2] = vLight->globalLightOrigin[2];
		light.originRadius[3] = radius;
		light.color[0] = lightColor[0];
		light.color[1] = lightColor[1];
		light.color[2] = lightColor[2];
		light.color[3] = 1.0f;
		light.scissor[0] = Max( 0, Min( presentWidth - 1, static_cast<int>( vLight->scissorRect.x1 ) ) );
		light.scissor[1] = Max( 0, Min( presentHeight - 1, static_cast<int>( vLight->scissorRect.y1 ) ) );
		light.scissor[2] = Max( 0, Min( presentWidth - 1, static_cast<int>( vLight->scissorRect.x2 ) ) );
		light.scissor[3] = Max( 0, Min( presentHeight - 1, static_cast<int>( vLight->scissorRect.y2 ) ) );
		light.flags[0] = vLight->lightShader->IsAmbientLight() ? 1u : 0u;
		light.flags[1] = parallel ? 1u : 0u;
		light.flags[2] = ( !vLight->lightShader->IsAmbientLight() && r_softwareRayQueryShadows.GetBool() && r_softwareHybridRayQueryShadows.GetBool() ) ? 1u : 0u;
		light.flags[3] = SW_HYBRID_LIGHT_NORMAL;
		for ( int plane = 0; plane < 4; plane++ ) {
			for ( int component = 0; component < 4; component++ ) {
				light.lightProject[plane][component] = projectedLightProject[plane][component];
			}
		}
		light.textureIds[0] = PackOptionalTextureId16( lightTextureIndex );
		light.textureIds[1] = PackOptionalTextureId16( falloffTextureIndex );
		light.textureIds[2] = 0u;
		light.textureIds[3] = 0u;
	}

	gbuffer.lights = hybridLights.Num() > 0 ? hybridLights.Ptr() : NULL;
	gbuffer.lightCount = hybridLights.Num();
}

void idSoftwareRasterizer::AddHybridFogLight( const swFogLightState_t &fog ) {
	swHybridLight_t &light = hybridLights.Alloc();
	memset( &light, 0, sizeof( light ) );
	light.originRadius[3] = fog.enterS;
	for ( int i = 0; i < 4; i++ ) {
		light.color[i] = fog.color[i];
	}
	light.scissor[0] = Max( 0, Min( presentWidth - 1, static_cast<int>( fog.scissorRect.x1 ) ) );
	light.scissor[1] = Max( 0, Min( presentHeight - 1, static_cast<int>( fog.scissorRect.y1 ) ) );
	light.scissor[2] = Max( 0, Min( presentWidth - 1, static_cast<int>( fog.scissorRect.x2 ) ) );
	light.scissor[3] = Max( 0, Min( presentHeight - 1, static_cast<int>( fog.scissorRect.y2 ) ) );
	light.flags[3] = SW_HYBRID_LIGHT_FOG;
	for ( int i = 0; i < 4; i++ ) {
		light.lightProject[0][i] = fog.fogPlane[i];
		light.lightProject[1][i] = fog.enterPlane[i];
	}
	light.textureIds[0] = PackOptionalTextureId16( fog.fogTextureIndex );
	light.textureIds[1] = PackOptionalTextureId16( fog.fogEnterTextureIndex );
}

void idSoftwareRasterizer::AddHybridBlendLight( const swBlendLightStage_t &blend ) {
	swHybridLight_t &light = hybridLights.Alloc();
	memset( &light, 0, sizeof( light ) );
	for ( int i = 0; i < 4; i++ ) {
		light.color[i] = blend.color[i];
	}
	light.scissor[0] = Max( 0, Min( presentWidth - 1, static_cast<int>( blend.scissorRect.x1 ) ) );
	light.scissor[1] = Max( 0, Min( presentHeight - 1, static_cast<int>( blend.scissorRect.y1 ) ) );
	light.scissor[2] = Max( 0, Min( presentWidth - 1, static_cast<int>( blend.scissorRect.x2 ) ) );
	light.scissor[3] = Max( 0, Min( presentHeight - 1, static_cast<int>( blend.scissorRect.y2 ) ) );
	light.flags[0] = static_cast<unsigned int>( blend.blendMode );
	light.flags[1] = static_cast<unsigned int>( blend.srcBlend );
	light.flags[2] = static_cast<unsigned int>( blend.dstBlend );
	light.flags[3] = SW_HYBRID_LIGHT_BLEND;
	for ( int plane = 0; plane < 4; plane++ ) {
		for ( int component = 0; component < 4; component++ ) {
			light.lightProject[plane][component] = blend.lightProject[plane][component];
		}
	}
	light.textureIds[0] = PackOptionalTextureId16( blend.lightTextureIndex );
	light.textureIds[1] = PackOptionalTextureId16( blend.falloffTextureIndex );
	light.textureIds[2] = static_cast<unsigned int>( blend.writeMask );
}

void idSoftwareRasterizer::BuildHybridOverlayTriangles( idList<swHybridOverlayTri_t> &overlayTris ) const {
	overlayTris.SetNum( 0, false );

	for ( int i = 0; i < triangles.Num(); i++ ) {
		const swTriSetup_t &tri = triangles[i];
		if ( tri.writeGBuffer || tri.writeMask == 0 ) {
			continue;
		}

		swHybridOverlayTri_t &dst = overlayTris.Alloc();
		memset( &dst, 0, sizeof( dst ) );

		dst.p0[0] = static_cast<float>( tri.x[0] ) * ( 1.0f / static_cast<float>( SW_FP_ONE ) );
		dst.p0[1] = static_cast<float>( tri.y[0] ) * ( 1.0f / static_cast<float>( SW_FP_ONE ) );
		dst.p0[2] = tri.z[0];
		dst.p0[3] = tri.invW[0];
		dst.p1[0] = static_cast<float>( tri.x[1] ) * ( 1.0f / static_cast<float>( SW_FP_ONE ) );
		dst.p1[1] = static_cast<float>( tri.y[1] ) * ( 1.0f / static_cast<float>( SW_FP_ONE ) );
		dst.p1[2] = tri.z[1];
		dst.p1[3] = tri.invW[1];
		dst.p2[0] = static_cast<float>( tri.x[2] ) * ( 1.0f / static_cast<float>( SW_FP_ONE ) );
		dst.p2[1] = static_cast<float>( tri.y[2] ) * ( 1.0f / static_cast<float>( SW_FP_ONE ) );
		dst.p2[2] = tri.z[2];
		dst.p2[3] = tri.invW[2];

		if ( tri.needsColorModulation ) {
			for ( int channel = 0; channel < 4; channel++ ) {
				dst.color0[channel] = tri.colorOverW[0][channel];
				dst.color1[channel] = tri.colorOverW[1][channel];
				dst.color2[channel] = tri.colorOverW[2][channel];
			}
		}

		dst.uv0uv1[0] = tri.sOverW[0];
		dst.uv0uv1[1] = tri.tOverW[0];
		dst.uv0uv1[2] = tri.sOverW[1];
		dst.uv0uv1[3] = tri.tOverW[1];
		dst.uv2pad[0] = tri.sOverW[2];
		dst.uv2pad[1] = tri.tOverW[2];
		dst.zPlane[0] = tri.z0;
		dst.zPlane[1] = tri.dzdx;
		dst.zPlane[2] = tri.dzdy;
		dst.invWPlane[0] = tri.invW0;
		dst.invWPlane[1] = tri.dinvWdx;
		dst.invWPlane[2] = tri.dinvWdy;
		dst.sPlane[0] = tri.sOverW0;
		dst.sPlane[1] = tri.dsOverWdx;
		dst.sPlane[2] = tri.dsOverWdy;
		dst.tPlane[0] = tri.tOverW0;
		dst.tPlane[1] = tri.dtOverWdx;
		dst.tPlane[2] = tri.dtOverWdy;
		if ( tri.needsColorModulation ) {
			for ( int channel = 0; channel < 4; channel++ ) {
				dst.colorPlane0[channel] = tri.colorOverW0[channel];
				dst.colorPlane1[channel] = tri.dColorOverWdx[channel];
				dst.colorPlane2[channel] = tri.dColorOverWdy[channel];
			}
		}

		unsigned int flags = 0;
		if ( tri.textureIndex >= 0 ) {
			flags |= SW_OVERLAY_FLAG_TEXTURE;
		}
		if ( tri.needsColorModulation ) {
			flags |= SW_OVERLAY_FLAG_COLOR_MOD;
		}
		if ( tri.alphaTest ) {
			flags |= SW_OVERLAY_FLAG_ALPHA_TEST;
		}
		if ( tri.depthTest ) {
			flags |= SW_OVERLAY_FLAG_DEPTH_TEST;
		}
		if ( tri.depthEqual ) {
			flags |= SW_OVERLAY_FLAG_DEPTH_EQUAL;
		}

		dst.params0[0] = tri.textureIndex >= 0 ? static_cast<unsigned int>( tri.textureIndex ) : 0xffffffffu;
		dst.params0[1] = tri.fallbackColor;
		dst.params0[2] = flags;
		dst.params0[3] = static_cast<unsigned int>( Max( 0, Min( 255, tri.alphaTestByte ) ) );
		dst.params1[0] = tri.alphaBlend ? tri.blendMode : SW_BLEND_REPLACE;
		dst.params1[1] = tri.writeMask;
		dst.params1[2] = tri.srcBlend;
		dst.params1[3] = tri.dstBlend;
		dst.bounds[0] = tri.minX;
		dst.bounds[1] = tri.minY;
		dst.bounds[2] = tri.maxX;
		dst.bounds[3] = tri.maxY;
	}
}

void idSoftwareRasterizer::DrawInteraction( const drawInteraction_t *interaction ) {
	if ( !interaction || !interaction->surf || !interaction->surf->geo || !interaction->surf->space ) {
		return;
	}

	const drawSurf_t *surf = interaction->surf;
	const srfTriangles_t *geo = surf->geo;
	const idDrawVert *verts = geo->verts;
	if ( !verts || !geo->indexes || geo->numVerts <= 0 || geo->numIndexes < 3 ) {
		return;
	}

	const int numIndexes = r_singleTriangle.GetBool() ? Min( geo->numIndexes, 3 ) : geo->numIndexes;
	const int cullType = surf->material ? surf->material->GetCullType() : CT_TWO_SIDED;

	for ( int i = 0; i + 2 < numIndexes; i += 3 ) {
		const int index0 = geo->indexes[i + 0];
		const int index1 = geo->indexes[i + 1];
		const int index2 = geo->indexes[i + 2];
		if ( index0 < 0 || index0 >= geo->numVerts ||
			 index1 < 0 || index1 >= geo->numVerts ||
			 index2 < 0 || index2 >= geo->numVerts ) {
			continue;
		}

		swClipVert_t cv0, cv1, cv2;
		if ( !BuildInteractionClipVertex( verts[index0], surf->space->modelViewMatrix, backEnd.viewDef->projectionMatrix, cv0 ) ||
			 !BuildInteractionClipVertex( verts[index1], surf->space->modelViewMatrix, backEnd.viewDef->projectionMatrix, cv1 ) ||
			 !BuildInteractionClipVertex( verts[index2], surf->space->modelViewMatrix, backEnd.viewDef->projectionMatrix, cv2 ) ) {
			continue;
		}

		swClipVert_t clipped[SW_MAX_CLIP_VERTS];
		const int clippedVerts = ClipTriangleToFrustum( cv0, cv1, cv2, clipped );
		for ( int clippedTri = 1; clippedTri + 1 < clippedVerts; clippedTri++ ) {
			swInteractionVert_t v0, v1, v2;
			if ( !ProjectInteractionClipVertex( clipped[0], v0 ) ||
				 !ProjectInteractionClipVertex( clipped[clippedTri], v1 ) ||
				 !ProjectInteractionClipVertex( clipped[clippedTri + 1], v2 ) ) {
				continue;
			}

			swInteractionTri_t tri;
			if ( SetupInteractionTriangle( v0, v1, v2, cullType, *interaction, tri ) ) {
				const int triIndex = interactionTriangles.Append( tri );
				BinInteractionTriangle( triIndex );
			}
		}
	}
}

#if defined( _WIN32 ) && !defined( _D3SDK )
void idSoftwareRasterizer::StartWorkers() {
	if ( workersStarted ) {
		return;
	}

	workersStarted = true;

	SYSTEM_INFO systemInfo;
	GetSystemInfo( &systemInfo );

	const int cpuCount = Max( 1, static_cast<int>( systemInfo.dwNumberOfProcessors ) );
	const int desiredWorkers = Max( 0, Min( cpuCount - 1, static_cast<int>( MAXIMUM_WAIT_OBJECTS ) ) );

	for ( int i = 0; i < desiredWorkers; i++ ) {
		swRasterWorkerJob_t &job = workerJobs[i];
		job.rasterizer = this;
		job.firstTile = 0;
		job.endTile = 0;
		job.shutdown = false;
		job.startEvent = CreateEvent( NULL, FALSE, FALSE, NULL );
		job.doneEvent = CreateEvent( NULL, TRUE, TRUE, NULL );
		if ( !job.startEvent || !job.doneEvent ) {
			if ( job.startEvent ) {
				CloseHandle( job.startEvent );
				job.startEvent = NULL;
			}
			if ( job.doneEvent ) {
				CloseHandle( job.doneEvent );
				job.doneEvent = NULL;
			}
			break;
		}

		HANDLE handle = CreateThread( NULL, 0, RasterizeWorker, &job, 0, NULL );
		if ( !handle ) {
			CloseHandle( job.startEvent );
			CloseHandle( job.doneEvent );
			job.startEvent = NULL;
			job.doneEvent = NULL;
			break;
		}

		workerHandles[workerCount++] = handle;
	}
}

void idSoftwareRasterizer::ShutdownWorkers() {
	if ( !workersStarted ) {
		return;
	}

	for ( int i = 0; i < workerCount; i++ ) {
		workerJobs[i].shutdown = true;
		SetEvent( workerJobs[i].startEvent );
	}

	if ( workerCount > 0 ) {
		WaitForMultipleObjects( workerCount, workerHandles, TRUE, INFINITE );
	}

	for ( int i = 0; i < workerCount; i++ ) {
		CloseHandle( workerHandles[i] );
		CloseHandle( workerJobs[i].startEvent );
		CloseHandle( workerJobs[i].doneEvent );
		workerHandles[i] = NULL;
		workerJobs[i].startEvent = NULL;
		workerJobs[i].doneEvent = NULL;
		workerJobs[i].shutdown = false;
	}

	workerCount = 0;
	workersStarted = false;
}
#endif

void idSoftwareRasterizer::RasterizeTiles() {
	const int numTiles = tileCountX * tileCountY;
	if ( numTiles <= 0 ) {
		return;
	}

#if defined( _WIN32 ) && !defined( _D3SDK )
	StartWorkers();

	const int activeWorkers = Min( workerCount, Max( 0, numTiles - 1 ) );
	if ( activeWorkers <= 0 ) {
		RasterizeTileRange( 0, numTiles );
		return;
	}

	const int numJobs = activeWorkers + 1;
	HANDLE doneEvents[MAXIMUM_WAIT_OBJECTS];

	for ( int i = 0; i < activeWorkers; i++ ) {
		swRasterWorkerJob_t &job = workerJobs[i];
		job.firstTile = ( numTiles * i ) / numJobs;
		job.endTile = ( numTiles * ( i + 1 ) ) / numJobs;
		doneEvents[i] = job.doneEvent;
		ResetEvent( job.doneEvent );
		SetEvent( job.startEvent );
	}

	RasterizeTileRange( ( numTiles * activeWorkers ) / numJobs, numTiles );
	WaitForMultipleObjects( activeWorkers, doneEvents, TRUE, INFINITE );
#else
	RasterizeTileRange( 0, numTiles );
#endif
}

void idSoftwareRasterizer::RasterizeTileRange( int firstTile, int endTile ) {
	for ( int tileIndex = firstTile; tileIndex < endTile; tileIndex++ ) {
		RasterizeTile( tileIndex % tileCountX, tileIndex / tileCountX );
	}
}

void idSoftwareRasterizer::RasterizeTile( int tileX, int tileY ) {
	const swTileBin_t &bin = tileBins[tileY * tileCountX + tileX];
	if ( rasterPass == SW_RASTER_INTERACTION ) {
		for ( int i = 0; i < bin.tris.Num(); i++ ) {
			RasterizeInteractionTriangleInTile( interactionTriangles[bin.tris[i]], tileX, tileY );
		}
	} else {
		for ( int i = 0; i < bin.tris.Num(); i++ ) {
			RasterizeTriangleInTile( triangles[bin.tris[i]], tileX, tileY );
		}
	}
}

void idSoftwareRasterizer::RasterizeTriangleInTile( const swTriSetup_t &tri, int tileX, int tileY ) {
	if ( tri.writeMask == 0 && tri.depthTest && tri.depthWrite && !tri.depthEqual && !tri.alphaTest ) {
		RasterizeDepthTriangleInTile( tri, tileX, tileY );
		return;
	}
	if ( tri.writeMask == 0 && tri.depthTest && tri.depthWrite && !tri.depthEqual && tri.alphaTest && tri.writeGBuffer ) {
		RasterizeAlphaTestDepthTriangleInTile( tri, tileX, tileY );
		return;
	}

	const int textureMode = r_softwareTextureMode.GetInteger();
	if ( textureMode == 1 ) {
		RasterizeTriangleInTileTexture<1>( tri, tileX, tileY );
	} else if ( textureMode == 2 ) {
		RasterizeTriangleInTileTexture<2>( tri, tileX, tileY );
	} else {
		RasterizeTriangleInTileTexture<0>( tri, tileX, tileY );
	}
}

void idSoftwareRasterizer::RasterizeDepthTriangleInTile( const swTriSetup_t &tri, int tileX, int tileY ) {
	const int x0 = Max( tri.minX, tileX * SW_TILE_SIZE );
	const int y0 = Max( tri.minY, tileY * SW_TILE_SIZE );
	const int x1 = Min( tri.maxX, ( tileX + 1 ) * SW_TILE_SIZE - 1 );
	const int y1 = Min( tri.maxY, ( tileY + 1 ) * SW_TILE_SIZE - 1 );

	const int basePx = ( x0 << SW_FP_SHIFT ) + SW_FP_HALF;
	const int basePy = ( y0 << SW_FP_SHIFT ) + SW_FP_HALF;

	long long rowE0 = tri.A[0] * basePx + tri.B[0] * basePy + tri.C[0];
	long long rowE1 = tri.A[1] * basePx + tri.B[1] * basePy + tri.C[1];
	long long rowE2 = tri.A[2] * basePx + tri.B[2] * basePy + tri.C[2];
	float rowZ = tri.z0 + tri.dzdx * ( static_cast<float>( x0 ) + 0.5f ) + tri.dzdy * ( static_cast<float>( y0 ) + 0.5f );
	float rowInvW = tri.invW0 + tri.dinvWdx * ( static_cast<float>( x0 ) + 0.5f ) + tri.dinvWdy * ( static_cast<float>( y0 ) + 0.5f );
	float rowSOverW = tri.sOverW0 + tri.dsOverWdx * ( static_cast<float>( x0 ) + 0.5f ) + tri.dsOverWdy * ( static_cast<float>( y0 ) + 0.5f );
	float rowTOverW = tri.tOverW0 + tri.dtOverWdx * ( static_cast<float>( x0 ) + 0.5f ) + tri.dtOverWdy * ( static_cast<float>( y0 ) + 0.5f );
	float rowGlobalOverW[3];
	if ( tri.writeWorldPosition ) {
		for ( int i = 0; i < 3; i++ ) {
			rowGlobalOverW[i] = tri.globalOverW0[i] + tri.dGlobalOverWdx[i] * ( static_cast<float>( x0 ) + 0.5f ) + tri.dGlobalOverWdy[i] * ( static_cast<float>( y0 ) + 0.5f );
		}
	}

	const long long stepX0 = tri.A[0] * SW_FP_ONE;
	const long long stepX1 = tri.A[1] * SW_FP_ONE;
	const long long stepX2 = tri.A[2] * SW_FP_ONE;
	const long long stepY0 = tri.B[0] * SW_FP_ONE;
	const long long stepY1 = tri.B[1] * SW_FP_ONE;
	const long long stepY2 = tri.B[2] * SW_FP_ONE;

	float rowNormalOverW[3];
	float rowTangent0OverW[3];
	float rowTangent1OverW[3];
	if ( tri.writeGBuffer ) {
		for ( int i = 0; i < 3; i++ ) {
			rowNormalOverW[i] = tri.normalOverW0[i] + tri.dNormalOverWdx[i] * ( static_cast<float>( x0 ) + 0.5f ) + tri.dNormalOverWdy[i] * ( static_cast<float>( y0 ) + 0.5f );
			rowTangent0OverW[i] = tri.tangent0OverW0[i] + tri.dTangent0OverWdx[i] * ( static_cast<float>( x0 ) + 0.5f ) + tri.dTangent0OverWdy[i] * ( static_cast<float>( y0 ) + 0.5f );
			rowTangent1OverW[i] = tri.tangent1OverW0[i] + tri.dTangent1OverWdx[i] * ( static_cast<float>( x0 ) + 0.5f ) + tri.dTangent1OverWdy[i] * ( static_cast<float>( y0 ) + 0.5f );
		}
	}
	swHybridGBufferPointers_t gbuffer;
	if ( tri.writeGBuffer ) {
		gbuffer = GetHybridGBufferPointers();
	}

#if SW_AVX2_DEPTH_PATH
	const bool useAVX2 = r_softwareDepthSIMD.GetBool() && !tri.writeWorldPosition && !tri.writeGBuffer;
	const __m256i laneI = _mm256_setr_epi32( 0, 1, 2, 3, 4, 5, 6, 7 );
	const __m256 laneF = _mm256_cvtepi32_ps( laneI );
	const __m256i minusOne = _mm256_set1_epi32( -1 );
	const __m256 dzdx8 = _mm256_set1_ps( tri.dzdx );
#endif

	for ( int y = y0; y <= y1; y++ ) {
		long long e0 = rowE0;
		long long e1 = rowE1;
		long long e2 = rowE2;
		float z = rowZ;
		float invW = rowInvW;
		float sOverW = rowSOverW;
		float tOverW = rowTOverW;
		float globalOverW[3];
		if ( tri.writeWorldPosition ) {
			globalOverW[0] = rowGlobalOverW[0];
			globalOverW[1] = rowGlobalOverW[1];
			globalOverW[2] = rowGlobalOverW[2];
		}
		float normalOverW[3];
		float tangent0OverW[3];
		float tangent1OverW[3];
		if ( tri.writeGBuffer ) {
			normalOverW[0] = rowNormalOverW[0];
			normalOverW[1] = rowNormalOverW[1];
			normalOverW[2] = rowNormalOverW[2];
			tangent0OverW[0] = rowTangent0OverW[0];
			tangent0OverW[1] = rowTangent0OverW[1];
			tangent0OverW[2] = rowTangent0OverW[2];
			tangent1OverW[0] = rowTangent1OverW[0];
			tangent1OverW[1] = rowTangent1OverW[1];
			tangent1OverW[2] = rowTangent1OverW[2];
		}
		float *depth = depthBuffer.Ptr() + y * width + x0;
		idVec4 *worldPos = worldPositionBuffer.Ptr() + y * width + x0;
		int pixelIndex = y * width + x0;
		int x = x0;

#if SW_AVX2_DEPTH_PATH
		if ( useAVX2 ) {
			while ( x + 7 <= x1 &&
					FitsEdgePacket32( e0, stepX0, 8 ) &&
					FitsEdgePacket32( e1, stepX1, 8 ) &&
					FitsEdgePacket32( e2, stepX2, 8 ) ) {
				const __m256i e0v = _mm256_add_epi32( _mm256_set1_epi32( static_cast<int>( e0 ) ), _mm256_mullo_epi32( _mm256_set1_epi32( static_cast<int>( stepX0 ) ), laneI ) );
				const __m256i e1v = _mm256_add_epi32( _mm256_set1_epi32( static_cast<int>( e1 ) ), _mm256_mullo_epi32( _mm256_set1_epi32( static_cast<int>( stepX1 ) ), laneI ) );
				const __m256i e2v = _mm256_add_epi32( _mm256_set1_epi32( static_cast<int>( e2 ) ), _mm256_mullo_epi32( _mm256_set1_epi32( static_cast<int>( stepX2 ) ), laneI ) );

				const __m256i c0 = _mm256_cmpgt_epi32( e0v, minusOne );
				const __m256i c1 = _mm256_cmpgt_epi32( e1v, minusOne );
				const __m256i c2 = _mm256_cmpgt_epi32( e2v, minusOne );
				const __m256 coverage = _mm256_castsi256_ps( _mm256_and_si256( _mm256_and_si256( c0, c1 ), c2 ) );

				const __m256 zv = _mm256_add_ps( _mm256_set1_ps( z ), _mm256_mul_ps( dzdx8, laneF ) );
				const __m256 oldDepth = _mm256_loadu_ps( depth );
				const __m256 depthPass = _mm256_cmp_ps( zv, oldDepth, _CMP_LT_OQ );
				const __m256 mask = _mm256_and_ps( coverage, depthPass );
				const __m256 newDepth = _mm256_blendv_ps( oldDepth, zv, mask );
				_mm256_storeu_ps( depth, newDepth );

				e0 += stepX0 * 8;
				e1 += stepX1 * 8;
				e2 += stepX2 * 8;
				z += tri.dzdx * 8.0f;
				depth += 8;
				x += 8;
			}
		}
#endif

		if ( x <= x1 ) {
			const long long rowEndE0 = e0 + stepX0 * ( x1 - x );
			const long long rowEndE1 = e1 + stepX1 * ( x1 - x );
			const long long rowEndE2 = e2 + stepX2 * ( x1 - x );
			if ( ( e0 | e1 | e2 | rowEndE0 | rowEndE1 | rowEndE2 ) >= 0 ) {
				for ( ; x <= x1; x++ ) {
					if ( z < *depth ) {
						*depth = z;
						if ( tri.writeWorldPosition && invW != 0.0f ) {
							const float invPerspective = 1.0f / invW;
							worldPos->Set( globalOverW[0] * invPerspective, globalOverW[1] * invPerspective, globalOverW[2] * invPerspective, 1.0f );
						}
						if ( tri.writeGBuffer ) {
							WriteHybridGBufferPixel( gbuffer, pixelIndex, tri, z, invW, sOverW, tOverW, normalOverW, tangent0OverW, tangent1OverW );
						}
					}

					z += tri.dzdx;
					invW += tri.dinvWdx;
					sOverW += tri.dsOverWdx;
					tOverW += tri.dtOverWdx;
					if ( tri.writeWorldPosition ) {
						globalOverW[0] += tri.dGlobalOverWdx[0];
						globalOverW[1] += tri.dGlobalOverWdx[1];
						globalOverW[2] += tri.dGlobalOverWdx[2];
					}
					if ( tri.writeGBuffer ) {
						normalOverW[0] += tri.dNormalOverWdx[0];
						normalOverW[1] += tri.dNormalOverWdx[1];
						normalOverW[2] += tri.dNormalOverWdx[2];
						tangent0OverW[0] += tri.dTangent0OverWdx[0];
						tangent0OverW[1] += tri.dTangent0OverWdx[1];
						tangent0OverW[2] += tri.dTangent0OverWdx[2];
						tangent1OverW[0] += tri.dTangent1OverWdx[0];
						tangent1OverW[1] += tri.dTangent1OverWdx[1];
						tangent1OverW[2] += tri.dTangent1OverWdx[2];
					}
					depth++;
					worldPos++;
					pixelIndex++;
				}
			} else {
				for ( ; x <= x1; x++ ) {
					if ( ( e0 | e1 | e2 ) >= 0 && z < *depth ) {
						*depth = z;
						if ( tri.writeWorldPosition && invW != 0.0f ) {
							const float invPerspective = 1.0f / invW;
							worldPos->Set( globalOverW[0] * invPerspective, globalOverW[1] * invPerspective, globalOverW[2] * invPerspective, 1.0f );
						}
						if ( tri.writeGBuffer ) {
							WriteHybridGBufferPixel( gbuffer, pixelIndex, tri, z, invW, sOverW, tOverW, normalOverW, tangent0OverW, tangent1OverW );
						}
					}

					e0 += stepX0;
					e1 += stepX1;
					e2 += stepX2;
					z += tri.dzdx;
					invW += tri.dinvWdx;
					sOverW += tri.dsOverWdx;
					tOverW += tri.dtOverWdx;
					if ( tri.writeWorldPosition ) {
						globalOverW[0] += tri.dGlobalOverWdx[0];
						globalOverW[1] += tri.dGlobalOverWdx[1];
						globalOverW[2] += tri.dGlobalOverWdx[2];
					}
					if ( tri.writeGBuffer ) {
						normalOverW[0] += tri.dNormalOverWdx[0];
						normalOverW[1] += tri.dNormalOverWdx[1];
						normalOverW[2] += tri.dNormalOverWdx[2];
						tangent0OverW[0] += tri.dTangent0OverWdx[0];
						tangent0OverW[1] += tri.dTangent0OverWdx[1];
						tangent0OverW[2] += tri.dTangent0OverWdx[2];
						tangent1OverW[0] += tri.dTangent1OverWdx[0];
						tangent1OverW[1] += tri.dTangent1OverWdx[1];
						tangent1OverW[2] += tri.dTangent1OverWdx[2];
					}
					depth++;
					worldPos++;
					pixelIndex++;
				}
			}
		}

		rowE0 += stepY0;
		rowE1 += stepY1;
		rowE2 += stepY2;
		rowZ += tri.dzdy;
		rowInvW += tri.dinvWdy;
		rowSOverW += tri.dsOverWdy;
		rowTOverW += tri.dtOverWdy;
		if ( tri.writeWorldPosition ) {
			rowGlobalOverW[0] += tri.dGlobalOverWdy[0];
			rowGlobalOverW[1] += tri.dGlobalOverWdy[1];
			rowGlobalOverW[2] += tri.dGlobalOverWdy[2];
		}
		if ( tri.writeGBuffer ) {
			rowNormalOverW[0] += tri.dNormalOverWdy[0];
			rowNormalOverW[1] += tri.dNormalOverWdy[1];
			rowNormalOverW[2] += tri.dNormalOverWdy[2];
			rowTangent0OverW[0] += tri.dTangent0OverWdy[0];
			rowTangent0OverW[1] += tri.dTangent0OverWdy[1];
			rowTangent0OverW[2] += tri.dTangent0OverWdy[2];
			rowTangent1OverW[0] += tri.dTangent1OverWdy[0];
			rowTangent1OverW[1] += tri.dTangent1OverWdy[1];
			rowTangent1OverW[2] += tri.dTangent1OverWdy[2];
		}
	}
}

void idSoftwareRasterizer::RasterizeAlphaTestDepthTriangleInTile( const swTriSetup_t &tri, int tileX, int tileY ) {
	const int x0 = Max( tri.minX, tileX * SW_TILE_SIZE );
	const int y0 = Max( tri.minY, tileY * SW_TILE_SIZE );
	const int x1 = Min( tri.maxX, ( tileX + 1 ) * SW_TILE_SIZE - 1 );
	const int y1 = Min( tri.maxY, ( tileY + 1 ) * SW_TILE_SIZE - 1 );

	const int basePx = ( x0 << SW_FP_SHIFT ) + SW_FP_HALF;
	const int basePy = ( y0 << SW_FP_SHIFT ) + SW_FP_HALF;

	long long rowE0 = tri.A[0] * basePx + tri.B[0] * basePy + tri.C[0];
	long long rowE1 = tri.A[1] * basePx + tri.B[1] * basePy + tri.C[1];
	long long rowE2 = tri.A[2] * basePx + tri.B[2] * basePy + tri.C[2];
	float rowZ = tri.z0 + tri.dzdx * ( static_cast<float>( x0 ) + 0.5f ) + tri.dzdy * ( static_cast<float>( y0 ) + 0.5f );
	float rowInvW = tri.invW0 + tri.dinvWdx * ( static_cast<float>( x0 ) + 0.5f ) + tri.dinvWdy * ( static_cast<float>( y0 ) + 0.5f );
	float rowSOverW = tri.sOverW0 + tri.dsOverWdx * ( static_cast<float>( x0 ) + 0.5f ) + tri.dsOverWdy * ( static_cast<float>( y0 ) + 0.5f );
	float rowTOverW = tri.tOverW0 + tri.dtOverWdx * ( static_cast<float>( x0 ) + 0.5f ) + tri.dtOverWdy * ( static_cast<float>( y0 ) + 0.5f );

	float rowGlobalOverW[3];
	if ( tri.writeWorldPosition ) {
		for ( int i = 0; i < 3; i++ ) {
			rowGlobalOverW[i] = tri.globalOverW0[i] + tri.dGlobalOverWdx[i] * ( static_cast<float>( x0 ) + 0.5f ) + tri.dGlobalOverWdy[i] * ( static_cast<float>( y0 ) + 0.5f );
		}
	}

	float rowColorOverW[4];
	if ( tri.needsColorModulation ) {
		for ( int i = 0; i < 4; i++ ) {
			rowColorOverW[i] = tri.colorOverW0[i] + tri.dColorOverWdx[i] * ( static_cast<float>( x0 ) + 0.5f ) + tri.dColorOverWdy[i] * ( static_cast<float>( y0 ) + 0.5f );
		}
	}

	float rowNormalOverW[3];
	float rowTangent0OverW[3];
	float rowTangent1OverW[3];
	for ( int i = 0; i < 3; i++ ) {
		rowNormalOverW[i] = tri.normalOverW0[i] + tri.dNormalOverWdx[i] * ( static_cast<float>( x0 ) + 0.5f ) + tri.dNormalOverWdy[i] * ( static_cast<float>( y0 ) + 0.5f );
		rowTangent0OverW[i] = tri.tangent0OverW0[i] + tri.dTangent0OverWdx[i] * ( static_cast<float>( x0 ) + 0.5f ) + tri.dTangent0OverWdy[i] * ( static_cast<float>( y0 ) + 0.5f );
		rowTangent1OverW[i] = tri.tangent1OverW0[i] + tri.dTangent1OverWdx[i] * ( static_cast<float>( x0 ) + 0.5f ) + tri.dTangent1OverWdy[i] * ( static_cast<float>( y0 ) + 0.5f );
	}

	const long long stepX0 = tri.A[0] * SW_FP_ONE;
	const long long stepX1 = tri.A[1] * SW_FP_ONE;
	const long long stepX2 = tri.A[2] * SW_FP_ONE;
	const long long stepY0 = tri.B[0] * SW_FP_ONE;
	const long long stepY1 = tri.B[1] * SW_FP_ONE;
	const long long stepY2 = tri.B[2] * SW_FP_ONE;
	swHybridGBufferPointers_t gbuffer = GetHybridGBufferPointers();

	for ( int y = y0; y <= y1; y++ ) {
		long long e0 = rowE0;
		long long e1 = rowE1;
		long long e2 = rowE2;
		float z = rowZ;
		float invW = rowInvW;
		float sOverW = rowSOverW;
		float tOverW = rowTOverW;
		float globalOverW[3];
		if ( tri.writeWorldPosition ) {
			globalOverW[0] = rowGlobalOverW[0];
			globalOverW[1] = rowGlobalOverW[1];
			globalOverW[2] = rowGlobalOverW[2];
		}
		float colorOverW[4];
		if ( tri.needsColorModulation ) {
			colorOverW[0] = rowColorOverW[0];
			colorOverW[1] = rowColorOverW[1];
			colorOverW[2] = rowColorOverW[2];
			colorOverW[3] = rowColorOverW[3];
		}
		float normalOverW[3];
		float tangent0OverW[3];
		float tangent1OverW[3];
		normalOverW[0] = rowNormalOverW[0];
		normalOverW[1] = rowNormalOverW[1];
		normalOverW[2] = rowNormalOverW[2];
		tangent0OverW[0] = rowTangent0OverW[0];
		tangent0OverW[1] = rowTangent0OverW[1];
		tangent0OverW[2] = rowTangent0OverW[2];
		tangent1OverW[0] = rowTangent1OverW[0];
		tangent1OverW[1] = rowTangent1OverW[1];
		tangent1OverW[2] = rowTangent1OverW[2];

		float *depth = depthBuffer.Ptr() + y * width + x0;
		idVec4 *worldPos = worldPositionBuffer.Ptr() + y * width + x0;
		int pixelIndex = y * width + x0;

		for ( int x = x0; x <= x1; x++ ) {
			if ( ( e0 | e1 | e2 ) >= 0 && z < *depth && invW != 0.0f ) {
				const float invPerspective = 1.0f / invW;
				const float s = sOverW * invPerspective;
				const float t = tOverW * invPerspective;
				const float lod = TextureLodForGradients( tri.textureIndex, invW, sOverW, tOverW,
					tri.dsOverWdx, tri.dtOverWdx, tri.dinvWdx, tri.dsOverWdy, tri.dtOverWdy, tri.dinvWdy );
				const unsigned int texel = SampleTextureMip( tri.textureIndex, s, t, lod );
				int alpha = static_cast<int>( ( texel >> 24 ) & 255u );
				if ( tri.needsColorModulation ) {
					alpha = ByteFromFloat( static_cast<float>( alpha ) * ( 1.0f / 255.0f ) * colorOverW[3] * invPerspective );
				}
				if ( alpha >= tri.alphaTestByte ) {
					*depth = z;
					if ( tri.writeWorldPosition ) {
						worldPos->Set( globalOverW[0] * invPerspective, globalOverW[1] * invPerspective, globalOverW[2] * invPerspective, 1.0f );
					}
					WriteHybridGBufferPixel( gbuffer, pixelIndex, tri, z, invW, sOverW, tOverW, normalOverW, tangent0OverW, tangent1OverW );
				}
			}

			e0 += stepX0;
			e1 += stepX1;
			e2 += stepX2;
			z += tri.dzdx;
			invW += tri.dinvWdx;
			sOverW += tri.dsOverWdx;
			tOverW += tri.dtOverWdx;
			if ( tri.writeWorldPosition ) {
				globalOverW[0] += tri.dGlobalOverWdx[0];
				globalOverW[1] += tri.dGlobalOverWdx[1];
				globalOverW[2] += tri.dGlobalOverWdx[2];
			}
			if ( tri.needsColorModulation ) {
				colorOverW[0] += tri.dColorOverWdx[0];
				colorOverW[1] += tri.dColorOverWdx[1];
				colorOverW[2] += tri.dColorOverWdx[2];
				colorOverW[3] += tri.dColorOverWdx[3];
			}
			normalOverW[0] += tri.dNormalOverWdx[0];
			normalOverW[1] += tri.dNormalOverWdx[1];
			normalOverW[2] += tri.dNormalOverWdx[2];
			tangent0OverW[0] += tri.dTangent0OverWdx[0];
			tangent0OverW[1] += tri.dTangent0OverWdx[1];
			tangent0OverW[2] += tri.dTangent0OverWdx[2];
			tangent1OverW[0] += tri.dTangent1OverWdx[0];
			tangent1OverW[1] += tri.dTangent1OverWdx[1];
			tangent1OverW[2] += tri.dTangent1OverWdx[2];
			depth++;
			worldPos++;
			pixelIndex++;
		}

		rowE0 += stepY0;
		rowE1 += stepY1;
		rowE2 += stepY2;
		rowZ += tri.dzdy;
		rowInvW += tri.dinvWdy;
		rowSOverW += tri.dsOverWdy;
		rowTOverW += tri.dtOverWdy;
		if ( tri.writeWorldPosition ) {
			rowGlobalOverW[0] += tri.dGlobalOverWdy[0];
			rowGlobalOverW[1] += tri.dGlobalOverWdy[1];
			rowGlobalOverW[2] += tri.dGlobalOverWdy[2];
		}
		if ( tri.needsColorModulation ) {
			rowColorOverW[0] += tri.dColorOverWdy[0];
			rowColorOverW[1] += tri.dColorOverWdy[1];
			rowColorOverW[2] += tri.dColorOverWdy[2];
			rowColorOverW[3] += tri.dColorOverWdy[3];
		}
		rowNormalOverW[0] += tri.dNormalOverWdy[0];
		rowNormalOverW[1] += tri.dNormalOverWdy[1];
		rowNormalOverW[2] += tri.dNormalOverWdy[2];
		rowTangent0OverW[0] += tri.dTangent0OverWdy[0];
		rowTangent0OverW[1] += tri.dTangent0OverWdy[1];
		rowTangent0OverW[2] += tri.dTangent0OverWdy[2];
		rowTangent1OverW[0] += tri.dTangent1OverWdy[0];
		rowTangent1OverW[1] += tri.dTangent1OverWdy[1];
		rowTangent1OverW[2] += tri.dTangent1OverWdy[2];
	}
}

template<int TEXTURE_MODE>
void idSoftwareRasterizer::RasterizeTriangleInTileTexture( const swTriSetup_t &tri, int tileX, int tileY ) {
	if ( TEXTURE_MODE == 0 && tri.textureIndex < 0 ) {
		RasterizeTriangleInTileTextured<false, TEXTURE_MODE>( tri, tileX, tileY );
	} else {
		RasterizeTriangleInTileTextured<true, TEXTURE_MODE>( tri, tileX, tileY );
	}
}

template<bool HAS_TEXTURE, int TEXTURE_MODE>
void idSoftwareRasterizer::RasterizeTriangleInTileTextured( const swTriSetup_t &tri, int tileX, int tileY ) {
	if ( tri.needsColorModulation ) {
		RasterizeTriangleInTileColorMod<HAS_TEXTURE, TEXTURE_MODE, true>( tri, tileX, tileY );
	} else {
		RasterizeTriangleInTileColorMod<HAS_TEXTURE, TEXTURE_MODE, false>( tri, tileX, tileY );
	}
}

template<bool HAS_TEXTURE, int TEXTURE_MODE, bool COLOR_MOD>
void idSoftwareRasterizer::RasterizeTriangleInTileColorMod( const swTriSetup_t &tri, int tileX, int tileY ) {
	const int blendMode = tri.alphaBlend ? tri.blendMode : SW_BLEND_REPLACE;
	switch ( blendMode ) {
	case SW_BLEND_REPLACE:
		RasterizeTriangleInTileBlend<HAS_TEXTURE, TEXTURE_MODE, COLOR_MOD, SW_BLEND_REPLACE>( tri, tileX, tileY );
		break;
	case SW_BLEND_ALPHA:
		RasterizeTriangleInTileBlend<HAS_TEXTURE, TEXTURE_MODE, COLOR_MOD, SW_BLEND_ALPHA>( tri, tileX, tileY );
		break;
	case SW_BLEND_ADD:
		RasterizeTriangleInTileBlend<HAS_TEXTURE, TEXTURE_MODE, COLOR_MOD, SW_BLEND_ADD>( tri, tileX, tileY );
		break;
	case SW_BLEND_FILTER:
		RasterizeTriangleInTileBlend<HAS_TEXTURE, TEXTURE_MODE, COLOR_MOD, SW_BLEND_FILTER>( tri, tileX, tileY );
		break;
	case SW_BLEND_DST_ALPHA:
		RasterizeTriangleInTileBlend<HAS_TEXTURE, TEXTURE_MODE, COLOR_MOD, SW_BLEND_DST_ALPHA>( tri, tileX, tileY );
		break;
	default:
		RasterizeTriangleInTileBlend<HAS_TEXTURE, TEXTURE_MODE, COLOR_MOD, SW_BLEND_GENERIC>( tri, tileX, tileY );
		break;
	}
}

template<bool HAS_TEXTURE, int TEXTURE_MODE, bool COLOR_MOD, int BLEND_MODE>
void idSoftwareRasterizer::RasterizeTriangleInTileBlend( const swTriSetup_t &tri, int tileX, int tileY ) {
	switch ( tri.writeMask ) {
	case 0:
		RasterizeTriangleInTileWriteMask<HAS_TEXTURE, TEXTURE_MODE, COLOR_MOD, BLEND_MODE, 0>( tri, tileX, tileY );
		break;
	case SW_WRITE_COLOR:
		RasterizeTriangleInTileWriteMask<HAS_TEXTURE, TEXTURE_MODE, COLOR_MOD, BLEND_MODE, SW_WRITE_COLOR>( tri, tileX, tileY );
		break;
	case SW_WRITE_ALPHA:
		RasterizeTriangleInTileWriteMask<HAS_TEXTURE, TEXTURE_MODE, COLOR_MOD, BLEND_MODE, SW_WRITE_ALPHA>( tri, tileX, tileY );
		break;
	default:
		RasterizeTriangleInTileWriteMask<HAS_TEXTURE, TEXTURE_MODE, COLOR_MOD, BLEND_MODE, SW_WRITE_DYNAMIC>( tri, tileX, tileY );
		break;
	}
}

template<bool HAS_TEXTURE, int TEXTURE_MODE, bool COLOR_MOD, int BLEND_MODE, int WRITE_MASK>
void idSoftwareRasterizer::RasterizeTriangleInTileWriteMask( const swTriSetup_t &tri, int tileX, int tileY ) {
	if ( tri.depthTest ) {
		if ( tri.depthWrite ) {
			if ( tri.alphaTest ) {
				RasterizeTriangleInTileSpecialized<HAS_TEXTURE, TEXTURE_MODE, COLOR_MOD, BLEND_MODE, WRITE_MASK, true, true, true>( tri, tileX, tileY );
			} else {
				RasterizeTriangleInTileSpecialized<HAS_TEXTURE, TEXTURE_MODE, COLOR_MOD, BLEND_MODE, WRITE_MASK, true, true, false>( tri, tileX, tileY );
			}
		} else {
			if ( tri.alphaTest ) {
				RasterizeTriangleInTileSpecialized<HAS_TEXTURE, TEXTURE_MODE, COLOR_MOD, BLEND_MODE, WRITE_MASK, true, false, true>( tri, tileX, tileY );
			} else {
				RasterizeTriangleInTileSpecialized<HAS_TEXTURE, TEXTURE_MODE, COLOR_MOD, BLEND_MODE, WRITE_MASK, true, false, false>( tri, tileX, tileY );
			}
		}
	} else {
		if ( tri.alphaTest ) {
			RasterizeTriangleInTileSpecialized<HAS_TEXTURE, TEXTURE_MODE, COLOR_MOD, BLEND_MODE, WRITE_MASK, false, false, true>( tri, tileX, tileY );
		} else {
			RasterizeTriangleInTileSpecialized<HAS_TEXTURE, TEXTURE_MODE, COLOR_MOD, BLEND_MODE, WRITE_MASK, false, false, false>( tri, tileX, tileY );
		}
	}
}

template<bool HAS_TEXTURE, int TEXTURE_MODE, bool COLOR_MOD, int BLEND_MODE, int WRITE_MASK, bool DEPTH_TEST, bool DEPTH_WRITE, bool ALPHA_TEST>
void idSoftwareRasterizer::RasterizeTriangleInTileSpecialized( const swTriSetup_t &tri, int tileX, int tileY ) {
	const int x0 = Max( tri.minX, tileX * SW_TILE_SIZE );
	const int y0 = Max( tri.minY, tileY * SW_TILE_SIZE );
	const int x1 = Min( tri.maxX, ( tileX + 1 ) * SW_TILE_SIZE - 1 );
	const int y1 = Min( tri.maxY, ( tileY + 1 ) * SW_TILE_SIZE - 1 );

	const int basePx = ( x0 << SW_FP_SHIFT ) + SW_FP_HALF;
	const int basePy = ( y0 << SW_FP_SHIFT ) + SW_FP_HALF;

	long long rowE0 = tri.A[0] * basePx + tri.B[0] * basePy + tri.C[0];
	long long rowE1 = tri.A[1] * basePx + tri.B[1] * basePy + tri.C[1];
	long long rowE2 = tri.A[2] * basePx + tri.B[2] * basePy + tri.C[2];
	float rowZ = tri.z0 + tri.dzdx * ( static_cast<float>( x0 ) + 0.5f ) + tri.dzdy * ( static_cast<float>( y0 ) + 0.5f );
	float rowInvW = tri.invW0 + tri.dinvWdx * ( static_cast<float>( x0 ) + 0.5f ) + tri.dinvWdy * ( static_cast<float>( y0 ) + 0.5f );
	float rowSOverW = tri.sOverW0 + tri.dsOverWdx * ( static_cast<float>( x0 ) + 0.5f ) + tri.dsOverWdy * ( static_cast<float>( y0 ) + 0.5f );
	float rowTOverW = tri.tOverW0 + tri.dtOverWdx * ( static_cast<float>( x0 ) + 0.5f ) + tri.dtOverWdy * ( static_cast<float>( y0 ) + 0.5f );
	float rowColorOverW[4];
	if ( COLOR_MOD ) {
		for ( int i = 0; i < 4; i++ ) {
			rowColorOverW[i] = tri.colorOverW0[i] + tri.dColorOverWdx[i] * ( static_cast<float>( x0 ) + 0.5f ) + tri.dColorOverWdy[i] * ( static_cast<float>( y0 ) + 0.5f );
		}
	}
	float rowGlobalOverW[3];
	if ( tri.writeWorldPosition ) {
		for ( int i = 0; i < 3; i++ ) {
			rowGlobalOverW[i] = tri.globalOverW0[i] + tri.dGlobalOverWdx[i] * ( static_cast<float>( x0 ) + 0.5f ) + tri.dGlobalOverWdy[i] * ( static_cast<float>( y0 ) + 0.5f );
		}
	}

	const long long stepX0 = tri.A[0] * SW_FP_ONE;
	const long long stepX1 = tri.A[1] * SW_FP_ONE;
	const long long stepX2 = tri.A[2] * SW_FP_ONE;
	const long long stepY0 = tri.B[0] * SW_FP_ONE;
	const long long stepY1 = tri.B[1] * SW_FP_ONE;
	const long long stepY2 = tri.B[2] * SW_FP_ONE;

	for ( int y = y0; y <= y1; y++ ) {
		long long e0 = rowE0;
		long long e1 = rowE1;
		long long e2 = rowE2;
		float z = rowZ;
		float invW = rowInvW;
		float sOverW = rowSOverW;
		float tOverW = rowTOverW;
		float colorOverW[4];
		if ( COLOR_MOD ) {
			colorOverW[0] = rowColorOverW[0];
			colorOverW[1] = rowColorOverW[1];
			colorOverW[2] = rowColorOverW[2];
			colorOverW[3] = rowColorOverW[3];
		}
		float globalOverW[3];
		if ( tri.writeWorldPosition ) {
			globalOverW[0] = rowGlobalOverW[0];
			globalOverW[1] = rowGlobalOverW[1];
			globalOverW[2] = rowGlobalOverW[2];
		}
		float *depth = depthBuffer.Ptr() + y * width + x0;
		unsigned int *color = colorBuffer.Ptr() + y * width + x0;
		idVec4 *worldPos = worldPositionBuffer.Ptr() + y * width + x0;

		for ( int x = x0; x <= x1; x++ ) {
			if ( ( e0 | e1 | e2 ) >= 0 ) {
				const bool depthPass = !DEPTH_TEST || ( tri.depthEqual ? idMath::Fabs( z - *depth ) <= SW_DEPTH_EQUAL_EPSILON : z < *depth );
				if ( depthPass ) {
					const unsigned int srcColor = ShadePixel<HAS_TEXTURE, TEXTURE_MODE, COLOR_MOD>( tri, invW, sOverW, tOverW, colorOverW );
					if ( !ALPHA_TEST || static_cast<int>( ( srcColor >> 24 ) & 255u ) >= tri.alphaTestByte ) {
						if ( DEPTH_WRITE ) {
							*depth = z;
							if ( tri.writeWorldPosition && invW != 0.0f ) {
								const float invPerspective = 1.0f / invW;
								worldPos->Set( globalOverW[0] * invPerspective, globalOverW[1] * invPerspective, globalOverW[2] * invPerspective, 1.0f );
							}
						}
						WritePixelColor<BLEND_MODE, WRITE_MASK>( tri, srcColor, color );
					}
				}
			}
			e0 += stepX0;
			e1 += stepX1;
			e2 += stepX2;
			z += tri.dzdx;
			invW += tri.dinvWdx;
			sOverW += tri.dsOverWdx;
			tOverW += tri.dtOverWdx;
			if ( COLOR_MOD ) {
				colorOverW[0] += tri.dColorOverWdx[0];
				colorOverW[1] += tri.dColorOverWdx[1];
				colorOverW[2] += tri.dColorOverWdx[2];
				colorOverW[3] += tri.dColorOverWdx[3];
			}
			if ( tri.writeWorldPosition ) {
				globalOverW[0] += tri.dGlobalOverWdx[0];
				globalOverW[1] += tri.dGlobalOverWdx[1];
				globalOverW[2] += tri.dGlobalOverWdx[2];
			}
			depth++;
			color++;
			worldPos++;
		}

		rowE0 += stepY0;
		rowE1 += stepY1;
		rowE2 += stepY2;
		rowZ += tri.dzdy;
		rowInvW += tri.dinvWdy;
		rowSOverW += tri.dsOverWdy;
		rowTOverW += tri.dtOverWdy;
		if ( COLOR_MOD ) {
			rowColorOverW[0] += tri.dColorOverWdy[0];
			rowColorOverW[1] += tri.dColorOverWdy[1];
			rowColorOverW[2] += tri.dColorOverWdy[2];
			rowColorOverW[3] += tri.dColorOverWdy[3];
		}
		if ( tri.writeWorldPosition ) {
			rowGlobalOverW[0] += tri.dGlobalOverWdy[0];
			rowGlobalOverW[1] += tri.dGlobalOverWdy[1];
			rowGlobalOverW[2] += tri.dGlobalOverWdy[2];
		}
	}
}

ID_INLINE swHybridGBufferPointers_t idSoftwareRasterizer::GetHybridGBufferPointers() {
	swHybridGBufferPointers_t gbuffer;
	gbuffer.depth = hybridGBuffer.depth.Ptr();
	gbuffer.normalPacked = hybridGBuffer.normalPacked.Ptr();
	gbuffer.tangentPacked = hybridGBuffer.tangentPacked.Ptr();
	gbuffer.bitangentPacked = hybridGBuffer.bitangentPacked.Ptr();
	gbuffer.uvPacked = hybridGBuffer.uvPacked.Ptr();
	gbuffer.materialId = hybridGBuffer.materialId.Ptr();
	gbuffer.albedoOrTextureId = hybridGBuffer.albedoOrTextureId.Ptr();
	gbuffer.specularAndFlags = hybridGBuffer.specularAndFlags.Ptr();
	gbuffer.surfaceId = hybridGBuffer.surfaceId.Ptr();
	return gbuffer;
}

ID_INLINE void idSoftwareRasterizer::WriteHybridGBufferPixel( const swHybridGBufferPointers_t &gbuffer, int pixelIndex, const swTriSetup_t &tri, float z, float invW, float sOverW, float tOverW, const float normalOverW[3], const float tangent0OverW[3], const float tangent1OverW[3] ) {
	float s = 0.0f;
	float t = 0.0f;
	float normalX = 0.0f;
	float normalY = 0.0f;
	float normalZ = 1.0f;
	float tangent0X = 1.0f;
	float tangent0Y = 0.0f;
	float tangent0Z = 0.0f;
	float tangent1X = 0.0f;
	float tangent1Y = 1.0f;
	float tangent1Z = 0.0f;
	if ( invW != 0.0f ) {
		const float invPerspective = 1.0f / invW;
		s = sOverW * invPerspective;
		t = tOverW * invPerspective;
		normalX = normalOverW[0] * invPerspective;
		normalY = normalOverW[1] * invPerspective;
		normalZ = normalOverW[2] * invPerspective;
		tangent0X = tangent0OverW[0] * invPerspective;
		tangent0Y = tangent0OverW[1] * invPerspective;
		tangent0Z = tangent0OverW[2] * invPerspective;
		tangent1X = tangent1OverW[0] * invPerspective;
		tangent1Y = tangent1OverW[1] * invPerspective;
		tangent1Z = tangent1OverW[2] * invPerspective;
	}

	gbuffer.depth[pixelIndex] = z;
	gbuffer.normalPacked[pixelIndex] = PackNormal10( normalX, normalY, normalZ );
	gbuffer.tangentPacked[pixelIndex] = PackNormal10( tangent0X, tangent0Y, tangent0Z );
	gbuffer.bitangentPacked[pixelIndex] = PackNormal10( tangent1X, tangent1Y, tangent1Z );
	gbuffer.uvPacked[pixelIndex] = PackUV16( s, t );
	gbuffer.materialId[pixelIndex] = tri.materialId;
	gbuffer.albedoOrTextureId[pixelIndex] = tri.albedoOrTextureId;
	gbuffer.specularAndFlags[pixelIndex] = tri.specularAndFlags;
	gbuffer.surfaceId[pixelIndex] = tri.surfaceId;
}

void idSoftwareRasterizer::RasterizeInteractionTriangleInTile( const swInteractionTri_t &tri, int tileX, int tileY ) {
	const int x0 = Max( tri.minX, tileX * SW_TILE_SIZE );
	const int y0 = Max( tri.minY, tileY * SW_TILE_SIZE );
	const int x1 = Min( tri.maxX, ( tileX + 1 ) * SW_TILE_SIZE - 1 );
	const int y1 = Min( tri.maxY, ( tileY + 1 ) * SW_TILE_SIZE - 1 );

	const int basePx = ( x0 << SW_FP_SHIFT ) + SW_FP_HALF;
	const int basePy = ( y0 << SW_FP_SHIFT ) + SW_FP_HALF;

	long long rowE0 = tri.A[0] * basePx + tri.B[0] * basePy + tri.C[0];
	long long rowE1 = tri.A[1] * basePx + tri.B[1] * basePy + tri.C[1];
	long long rowE2 = tri.A[2] * basePx + tri.B[2] * basePy + tri.C[2];
	float rowZ = tri.z0 + tri.dzdx * ( static_cast<float>( x0 ) + 0.5f ) + tri.dzdy * ( static_cast<float>( y0 ) + 0.5f );
	float rowInvW = tri.invW0 + tri.dinvWdx * ( static_cast<float>( x0 ) + 0.5f ) + tri.dinvWdy * ( static_cast<float>( y0 ) + 0.5f );
	float rowAttrOverW[SW_INTERACTION_ATTR_COUNT];
	for ( int i = 0; i < SW_INTERACTION_ATTR_COUNT; i++ ) {
		rowAttrOverW[i] = tri.attrOverW0[i] + tri.dAttrOverWdx[i] * ( static_cast<float>( x0 ) + 0.5f ) + tri.dAttrOverWdy[i] * ( static_cast<float>( y0 ) + 0.5f );
	}

	const long long stepX0 = tri.A[0] * SW_FP_ONE;
	const long long stepX1 = tri.A[1] * SW_FP_ONE;
	const long long stepX2 = tri.A[2] * SW_FP_ONE;
	const long long stepY0 = tri.B[0] * SW_FP_ONE;
	const long long stepY1 = tri.B[1] * SW_FP_ONE;
	const long long stepY2 = tri.B[2] * SW_FP_ONE;

	for ( int y = y0; y <= y1; y++ ) {
		long long e0 = rowE0;
		long long e1 = rowE1;
		long long e2 = rowE2;
		float z = rowZ;
		float invW = rowInvW;
		float attrOverW[SW_INTERACTION_ATTR_COUNT];
		for ( int i = 0; i < SW_INTERACTION_ATTR_COUNT; i++ ) {
			attrOverW[i] = rowAttrOverW[i];
		}

		float *depth = depthBuffer.Ptr() + y * width + x0;
		unsigned int *color = colorBuffer.Ptr() + y * width + x0;

		for ( int x = x0; x <= x1; x++ ) {
			if ( ( e0 | e1 | e2 ) >= 0 ) {
				const bool depthPass = tri.depthEqual ? idMath::Fabs( z - *depth ) <= SW_DEPTH_EQUAL_EPSILON : z < *depth;
				if ( depthPass ) {
					const unsigned int srcColor = ShadeInteractionPixel( tri, x, y, invW, attrOverW );
					if ( ( srcColor & 0x00ffffffu ) != 0 ) {
						*color = AdditiveColor( srcColor, *color );
					}
				}
			}

			e0 += stepX0;
			e1 += stepX1;
			e2 += stepX2;
			z += tri.dzdx;
			invW += tri.dinvWdx;
			for ( int i = 0; i < SW_INTERACTION_ATTR_COUNT; i++ ) {
				attrOverW[i] += tri.dAttrOverWdx[i];
			}
			depth++;
			color++;
		}

		rowE0 += stepY0;
		rowE1 += stepY1;
		rowE2 += stepY2;
		rowZ += tri.dzdy;
		rowInvW += tri.dinvWdy;
		for ( int i = 0; i < SW_INTERACTION_ATTR_COUNT; i++ ) {
			rowAttrOverW[i] += tri.dAttrOverWdy[i];
		}
	}
}

float idSoftwareRasterizer::ShadowVisibility( int x, int y ) const {
	if ( !shadowMaskActive || x < 0 || y < 0 || x >= width || y >= height || shadowMask.Num() != width * height ) {
		return 1.0f;
	}
	return static_cast<float>( shadowMask[y * width + x] ) * ( 1.0f / 255.0f );
}

unsigned int idSoftwareRasterizer::ShadeInteractionPixel( const swInteractionTri_t &tri, int x, int y, float invW, const float attrOverW[SW_INTERACTION_ATTR_COUNT] ) const {
	if ( invW == 0.0f ) {
		return 0;
	}

	const float invPerspective = 1.0f / invW;
	float attr[SW_INTERACTION_ATTR_COUNT];
	for ( int i = 0; i < SW_INTERACTION_ATTR_COUNT; i++ ) {
		attr[i] = attrOverW[i] * invPerspective;
	}

	idVec3 localPos( attr[SW_ATTR_POS_X], attr[SW_ATTR_POS_Y], attr[SW_ATTR_POS_Z] );
	idVec2 st( attr[SW_ATTR_ST_X], attr[SW_ATTR_ST_Y] );
	idVec3 normal( attr[SW_ATTR_NORMAL_X], attr[SW_ATTR_NORMAL_Y], attr[SW_ATTR_NORMAL_Z] );
	idVec3 tangent0( attr[SW_ATTR_TANGENT0_X], attr[SW_ATTR_TANGENT0_Y], attr[SW_ATTR_TANGENT0_Z] );
	idVec3 tangent1( attr[SW_ATTR_TANGENT1_X], attr[SW_ATTR_TANGENT1_Y], attr[SW_ATTR_TANGENT1_Z] );

	if ( normal.LengthSqr() <= 0.000001f ) {
		normal.Set( 0.0f, 0.0f, 1.0f );
	} else {
		normal.Normalize();
	}
	if ( tangent0.LengthSqr() <= 0.000001f ) {
		tangent0.Set( 1.0f, 0.0f, 0.0f );
	} else {
		tangent0.Normalize();
	}
	if ( tangent1.LengthSqr() <= 0.000001f ) {
		tangent1.Set( 0.0f, 1.0f, 0.0f );
	} else {
		tangent1.Normalize();
	}

	float lightQ = DotPlanePoint( tri.lightProjection[2], localPos );
	if ( lightQ <= 0.00001f ) {
		return 0;
	}

	const float lightS = DotPlanePoint( tri.lightProjection[0], localPos ) / lightQ;
	const float lightT = DotPlanePoint( tri.lightProjection[1], localPos ) / lightQ;
	const float falloffS = DotPlanePoint( tri.lightProjection[3], localPos );

	const unsigned int lightTexel = SampleTextureLinear( tri.lightTextureIndex, lightS, lightT );
	const unsigned int falloffTexel = SampleTextureLinear( tri.falloffTextureIndex, falloffS, 0.5f );
	const float falloff = ColorChannel( falloffTexel, 16 );
	if ( falloff <= 0.0f || ( lightTexel & 0x00ffffffu ) == 0 ) {
		return 0;
	}

	const float bumpS = st[0] * tri.bumpMatrix[0][0] + st[1] * tri.bumpMatrix[0][1] + tri.bumpMatrix[0][3];
	const float bumpT = st[0] * tri.bumpMatrix[1][0] + st[1] * tri.bumpMatrix[1][1] + tri.bumpMatrix[1][3];
	const float diffuseS = st[0] * tri.diffuseMatrix[0][0] + st[1] * tri.diffuseMatrix[0][1] + tri.diffuseMatrix[0][3];
	const float diffuseT = st[0] * tri.diffuseMatrix[1][0] + st[1] * tri.diffuseMatrix[1][1] + tri.diffuseMatrix[1][3];
	const float specularS = st[0] * tri.specularMatrix[0][0] + st[1] * tri.specularMatrix[0][1] + tri.specularMatrix[0][3];
	const float specularT = st[0] * tri.specularMatrix[1][0] + st[1] * tri.specularMatrix[1][1] + tri.specularMatrix[1][3];
	const float invW2 = invW * invW;
	float stDsdx = 0.0f;
	float stDtdx = 0.0f;
	float stDsdy = 0.0f;
	float stDtdy = 0.0f;
	if ( invW2 > 0.000000001f ) {
		stDsdx = ( tri.dAttrOverWdx[SW_ATTR_ST_X] * invW - attrOverW[SW_ATTR_ST_X] * tri.dinvWdx ) / invW2;
		stDtdx = ( tri.dAttrOverWdx[SW_ATTR_ST_Y] * invW - attrOverW[SW_ATTR_ST_Y] * tri.dinvWdx ) / invW2;
		stDsdy = ( tri.dAttrOverWdy[SW_ATTR_ST_X] * invW - attrOverW[SW_ATTR_ST_X] * tri.dinvWdy ) / invW2;
		stDtdy = ( tri.dAttrOverWdy[SW_ATTR_ST_Y] * invW - attrOverW[SW_ATTR_ST_Y] * tri.dinvWdy ) / invW2;
	}
	const float bumpLod = TextureLodForDerivatives( tri.bumpTextureIndex,
		stDsdx * tri.bumpMatrix[0][0] + stDtdx * tri.bumpMatrix[0][1],
		stDsdx * tri.bumpMatrix[1][0] + stDtdx * tri.bumpMatrix[1][1],
		stDsdy * tri.bumpMatrix[0][0] + stDtdy * tri.bumpMatrix[0][1],
		stDsdy * tri.bumpMatrix[1][0] + stDtdy * tri.bumpMatrix[1][1] );
	const float diffuseLod = TextureLodForDerivatives( tri.diffuseTextureIndex,
		stDsdx * tri.diffuseMatrix[0][0] + stDtdx * tri.diffuseMatrix[0][1],
		stDsdx * tri.diffuseMatrix[1][0] + stDtdx * tri.diffuseMatrix[1][1],
		stDsdy * tri.diffuseMatrix[0][0] + stDtdy * tri.diffuseMatrix[0][1],
		stDsdy * tri.diffuseMatrix[1][0] + stDtdy * tri.diffuseMatrix[1][1] );
	const float specularLod = TextureLodForDerivatives( tri.specularTextureIndex,
		stDsdx * tri.specularMatrix[0][0] + stDtdx * tri.specularMatrix[0][1],
		stDsdx * tri.specularMatrix[1][0] + stDtdx * tri.specularMatrix[1][1],
		stDsdy * tri.specularMatrix[0][0] + stDtdy * tri.specularMatrix[0][1],
		stDsdy * tri.specularMatrix[1][0] + stDtdy * tri.specularMatrix[1][1] );

	idVec3 bumpNormal = DecodeNormal( SampleTextureMip( tri.bumpTextureIndex, bumpS, bumpT, bumpLod ) );
	if ( bumpNormal.LengthSqr() <= 0.000001f ) {
		bumpNormal.Set( 0.0f, 0.0f, 1.0f );
	} else {
		bumpNormal.Normalize();
	}

	float nDotL = 1.0f;
	idVec3 lightTS( 0.0f, 0.0f, 1.0f );
	if ( !tri.ambientLight ) {
		idVec3 lightVec = tri.localLightOrigin.ToVec3() - localPos;
		if ( lightVec.LengthSqr() <= 0.000001f ) {
			return 0;
		}
		lightVec.Normalize();
		lightTS.Set( lightVec * tangent0, lightVec * tangent1, lightVec * normal );
		if ( lightTS.LengthSqr() > 0.000001f ) {
			lightTS.Normalize();
		}
		nDotL = Max( 0.0f, bumpNormal * lightTS );
	}
	if ( nDotL <= 0.0f ) {
		return 0;
	}

	const float shadowVisibility = tri.ambientLight ? 1.0f : ShadowVisibility( x, y );
	if ( shadowVisibility <= 0.0f ) {
		return 0;
	}

	float vertexScale[4];
	if ( tri.vertexColor == SVC_MODULATE ) {
		vertexScale[0] = attr[SW_ATTR_COLOR_R];
		vertexScale[1] = attr[SW_ATTR_COLOR_G];
		vertexScale[2] = attr[SW_ATTR_COLOR_B];
		vertexScale[3] = attr[SW_ATTR_COLOR_A];
	} else if ( tri.vertexColor == SVC_INVERSE_MODULATE ) {
		vertexScale[0] = 1.0f - attr[SW_ATTR_COLOR_R];
		vertexScale[1] = 1.0f - attr[SW_ATTR_COLOR_G];
		vertexScale[2] = 1.0f - attr[SW_ATTR_COLOR_B];
		vertexScale[3] = attr[SW_ATTR_COLOR_A];
	} else {
		vertexScale[0] = vertexScale[1] = vertexScale[2] = vertexScale[3] = 1.0f;
	}

	const unsigned int diffuseTexel = SampleTextureMip( tri.diffuseTextureIndex, diffuseS, diffuseT, diffuseLod );
	float r = ColorChannel( diffuseTexel, 16 ) * tri.diffuseColor[0] * ColorChannel( lightTexel, 16 ) * falloff * nDotL * shadowVisibility * vertexScale[0];
	float g = ColorChannel( diffuseTexel, 8 ) * tri.diffuseColor[1] * ColorChannel( lightTexel, 8 ) * falloff * nDotL * shadowVisibility * vertexScale[1];
	float b = ColorChannel( diffuseTexel, 0 ) * tri.diffuseColor[2] * ColorChannel( lightTexel, 0 ) * falloff * nDotL * shadowVisibility * vertexScale[2];

	if ( !tri.ambientLight && ( tri.specularColor[0] > 0.0f || tri.specularColor[1] > 0.0f || tri.specularColor[2] > 0.0f ) ) {
		idVec3 viewVec = tri.localViewOrigin.ToVec3() - localPos;
		if ( viewVec.LengthSqr() > 0.000001f ) {
			viewVec.Normalize();
			idVec3 viewTS( viewVec * tangent0, viewVec * tangent1, viewVec * normal );
			if ( viewTS.LengthSqr() > 0.000001f ) {
				viewTS.Normalize();
				idVec3 halfTS = lightTS + viewTS;
				if ( halfTS.LengthSqr() > 0.000001f ) {
					halfTS.Normalize();
					const float specular = idMath::Pow( Max( 0.0f, bumpNormal * halfTS ), 16.0f );
					if ( specular > 0.0f ) {
						const unsigned int specularTexel = SampleTextureMip( tri.specularTextureIndex, specularS, specularT, specularLod );
						r += ColorChannel( specularTexel, 16 ) * tri.specularColor[0] * ColorChannel( lightTexel, 16 ) * falloff * specular * shadowVisibility * vertexScale[0];
						g += ColorChannel( specularTexel, 8 ) * tri.specularColor[1] * ColorChannel( lightTexel, 8 ) * falloff * specular * shadowVisibility * vertexScale[1];
						b += ColorChannel( specularTexel, 0 ) * tri.specularColor[2] * ColorChannel( lightTexel, 0 ) * falloff * specular * shadowVisibility * vertexScale[2];
					}
				}
			}
		}
	}

	return PackColor( ByteFromFloat( r ), ByteFromFloat( g ), ByteFromFloat( b ), 255 );
}

template<bool HAS_TEXTURE, int TEXTURE_MODE, bool COLOR_MOD>
ID_INLINE unsigned int idSoftwareRasterizer::ShadePixel( const swTriSetup_t &tri, float invW, float sOverW, float tOverW, const float colorOverW[4] ) const {
	unsigned int srcColor;

	if ( TEXTURE_MODE == 1 || TEXTURE_MODE == 2 ) {
		const float invPerspective = 1.0f / invW;
		const float s = sOverW * invPerspective;
		const float t = tOverW * invPerspective;
		return DebugTextureColor( TEXTURE_MODE, s, t );
	}

	if ( HAS_TEXTURE ) {
		const float invPerspective = 1.0f / invW;
		const float s = sOverW * invPerspective;
		const float t = tOverW * invPerspective;
		const float lod = TextureLodForGradients( tri.textureIndex, invW, sOverW, tOverW,
			tri.dsOverWdx, tri.dtOverWdx, tri.dinvWdx, tri.dsOverWdy, tri.dtOverWdy, tri.dinvWdy );
		srcColor = SampleTextureMip( tri.textureIndex, s, t, lod );
	} else {
		srcColor = tri.fallbackColor;
	}

	if ( !COLOR_MOD ) {
		return srcColor;
	}

	const float invPerspective = 1.0f / invW;
	float colorScale[4];
	colorScale[0] = colorOverW[0] * invPerspective;
	colorScale[1] = colorOverW[1] * invPerspective;
	colorScale[2] = colorOverW[2] * invPerspective;
	colorScale[3] = colorOverW[3] * invPerspective;
	return ModulateColor( srcColor, colorScale );
}

template<int BLEND_MODE>
ID_INLINE unsigned int idSoftwareRasterizer::BlendColorSpecialized( unsigned int src, unsigned int dst, const swTriSetup_t &tri ) {
	if ( BLEND_MODE == SW_BLEND_REPLACE ) {
		return src;
	}
	if ( BLEND_MODE == SW_BLEND_ALPHA ) {
		return BlendSourceOver( src, dst );
	}
	if ( BLEND_MODE == SW_BLEND_ADD ) {
		const int r = Min( 255, static_cast<int>( ( src >> 16 ) & 255 ) + static_cast<int>( ( dst >> 16 ) & 255 ) );
		const int g = Min( 255, static_cast<int>( ( src >> 8 ) & 255 ) + static_cast<int>( ( dst >> 8 ) & 255 ) );
		const int b = Min( 255, static_cast<int>( src & 255 ) + static_cast<int>( dst & 255 ) );
		const int a = static_cast<int>( ( dst >> 24 ) & 255 );
		return PackColor( r, g, b, a );
	}
	if ( BLEND_MODE == SW_BLEND_FILTER ) {
		const int r = ( static_cast<int>( ( src >> 16 ) & 255 ) * static_cast<int>( ( dst >> 16 ) & 255 ) + 127 ) / 255;
		const int g = ( static_cast<int>( ( src >> 8 ) & 255 ) * static_cast<int>( ( dst >> 8 ) & 255 ) + 127 ) / 255;
		const int b = ( static_cast<int>( src & 255 ) * static_cast<int>( dst & 255 ) + 127 ) / 255;
		const int a = ( static_cast<int>( ( src >> 24 ) & 255 ) * static_cast<int>( ( dst >> 24 ) & 255 ) + 127 ) / 255;
		return PackColor( r, g, b, a );
	}
	if ( BLEND_MODE == SW_BLEND_DST_ALPHA ) {
		const int dstA = ( dst >> 24 ) & 255;
		const int invA = 255 - dstA;
		const int r = ( static_cast<int>( ( src >> 16 ) & 255 ) * dstA + static_cast<int>( ( dst >> 16 ) & 255 ) * invA + 127 ) / 255;
		const int g = ( static_cast<int>( ( src >> 8 ) & 255 ) * dstA + static_cast<int>( ( dst >> 8 ) & 255 ) * invA + 127 ) / 255;
		const int b = ( static_cast<int>( src & 255 ) * dstA + static_cast<int>( dst & 255 ) * invA + 127 ) / 255;
		const int a = ( static_cast<int>( ( src >> 24 ) & 255 ) * dstA + static_cast<int>( ( dst >> 24 ) & 255 ) * invA + 127 ) / 255;
		return PackColor( r, g, b, a );
	}
	return BlendColor( src, dst, tri.srcBlend, tri.dstBlend, tri.blendMode );
}

template<int BLEND_MODE, int WRITE_MASK>
ID_INLINE void idSoftwareRasterizer::WritePixelColor( const swTriSetup_t &tri, unsigned int src, unsigned int *color ) {
	if ( BLEND_MODE == SW_BLEND_REPLACE && WRITE_MASK == SW_WRITE_COLOR ) {
		*color = src;
		return;
	}
	if ( BLEND_MODE == SW_BLEND_ALPHA && ( ( src >> 24 ) & 255 ) == 0 ) {
		return;
	}
	if ( BLEND_MODE == SW_BLEND_ADD && ( src & 0x00ffffffu ) == 0 ) {
		return;
	}

	const unsigned int dst = *color;
	const unsigned int blendedColor = BlendColorSpecialized<BLEND_MODE>( src, dst, tri );
	if ( WRITE_MASK == SW_WRITE_COLOR ) {
		*color = blendedColor;
	} else if ( WRITE_MASK == SW_WRITE_ALPHA ) {
		*color = ( dst & 0x00ffffffu ) | ( blendedColor & 0xff000000u );
	} else if ( WRITE_MASK == SW_WRITE_DYNAMIC ) {
		*color = ApplyWriteMask( blendedColor, dst, tri.writeMask );
	} else {
		unsigned int out = dst;
		if ( WRITE_MASK & SW_WRITE_RED ) {
			out = ( out & 0xff00ffffu ) | ( blendedColor & 0x00ff0000u );
		}
		if ( WRITE_MASK & SW_WRITE_GREEN ) {
			out = ( out & 0xffff00ffu ) | ( blendedColor & 0x0000ff00u );
		}
		if ( WRITE_MASK & SW_WRITE_BLUE ) {
			out = ( out & 0xffffff00u ) | ( blendedColor & 0x000000ffu );
		}
		if ( WRITE_MASK & SW_WRITE_ALPHA ) {
			out = ( out & 0x00ffffffu ) | ( blendedColor & 0xff000000u );
		}
		*color = out;
	}
}

void idSoftwareRasterizer::ApplyLightScale() {
	if ( backEnd.overBright <= 1.01f ) {
		return;
	}
	for ( int i = 0; i < colorBuffer.Num(); i++ ) {
		const unsigned int color = colorBuffer[i];
		const int r = ByteFromFloat( ColorChannel( color, 16 ) * backEnd.overBright );
		const int g = ByteFromFloat( ColorChannel( color, 8 ) * backEnd.overBright );
		const int b = ByteFromFloat( ColorChannel( color, 0 ) * backEnd.overBright );
		const int a = ( color >> 24 ) & 255;
		colorBuffer[i] = PackColor( r, g, b, a );
	}
}

void idSoftwareRasterizer::Present() const {
	if ( width <= 0 || height <= 0 || colorBuffer.Num() != width * height ) {
		return;
	}

	if ( r_softwareVulkanPresent.GetBool() &&
		 SWVulkan_BlitView( backEnd.viewDef, colorBuffer.Ptr(), width, height, presentWidth, presentHeight ) ) {
		return;
	}

	RB_SetGL2D();
	GL_State( GLS_DEPTHFUNC_ALWAYS | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO );
	qglDisable( GL_DEPTH_TEST );
	qglDisable( GL_STENCIL_TEST );

	qglViewport( backEnd.viewDef->viewport.x1, backEnd.viewDef->viewport.y1, presentWidth, presentHeight );
	qglScissor( backEnd.viewDef->viewport.x1, backEnd.viewDef->viewport.y1, presentWidth, presentHeight );

	qglMatrixMode( GL_PROJECTION );
	qglLoadIdentity();
	qglOrtho( 0, presentWidth, 0, presentHeight, -1, 1 );
	qglMatrixMode( GL_MODELVIEW );
	qglLoadIdentity();
	qglRasterPos2i( 0, 0 );
	qglPixelZoom( static_cast<float>( presentWidth ) / static_cast<float>( width ), static_cast<float>( presentHeight ) / static_cast<float>( height ) );
	qglDrawPixels( width, height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, colorBuffer.Ptr() );
	qglPixelZoom( 1.0f, 1.0f );
	GL_SelectTexture( 0 );
	qglEnable( GL_TEXTURE_2D );
}

void idSoftwareRasterizer::SelectSurfaceStage( int surfIndex, const drawSurf_t *surf, swSurfaceStage_t &stageInfo, int stageIndex ) {
	stageInfo.textureIndex = -1;
	stageInfo.matrix[0][0] = 1.0f;
	stageInfo.matrix[0][1] = 0.0f;
	stageInfo.matrix[0][2] = 0.0f;
	stageInfo.matrix[1][0] = 0.0f;
	stageInfo.matrix[1][1] = 1.0f;
	stageInfo.matrix[1][2] = 0.0f;
	stageInfo.color[0] = 1.0f;
	stageInfo.color[1] = 1.0f;
	stageInfo.color[2] = 1.0f;
	stageInfo.color[3] = 1.0f;
	stageInfo.vertexColor = SVC_IGNORE;
	stageInfo.srcBlend = GLS_SRCBLEND_ONE;
	stageInfo.dstBlend = GLS_DSTBLEND_ZERO;
	stageInfo.blendMode = SW_BLEND_REPLACE;
	stageInfo.writeMask = SW_WRITE_COLOR;
	stageInfo.depthTest = true;
	stageInfo.depthWrite = true;
	stageInfo.depthEqual = false;
	stageInfo.alphaBlend = false;
	stageInfo.drawStage = true;
	stageInfo.needsColorModulation = false;
	stageInfo.alphaTest = false;
	stageInfo.writeWorldPosition = false;
	stageInfo.writeGBuffer = false;
	stageInfo.alphaTestValue = 0.0f;
	stageInfo.alphaTestByte = 0;
	stageInfo.fallbackColor = SurfaceColor( surfIndex, surf );
	stageInfo.materialId = 0;
	stageInfo.surfaceId = 0;
	stageInfo.albedoOrTextureId = 0xffffffffu;
	stageInfo.specularAndFlags = 0;

	const idMaterial *material = surf->material;
	const float *regs = surf->shaderRegisters;
	const shaderStage_t *selectedStage = NULL;

	if ( stageIndex >= 0 ) {
		if ( stageIndex < material->GetNumStages() ) {
			const shaderStage_t *stage = material->GetStage( stageIndex );
			if ( stage && stage->texture.image &&
				 ( !regs || regs[stage->conditionRegister] ) &&
				 stage->lighting == SL_AMBIENT &&
				 stage->texture.texgen == TG_EXPLICIT &&
				 !stage->texture.cinematic && stage->texture.dynamic == DI_STATIC ) {
				selectedStage = stage;
			}
		}
	} else {
		for ( int pass = 0; pass < 2 && !selectedStage; pass++ ) {
			for ( int i = 0; i < material->GetNumStages(); i++ ) {
				const shaderStage_t *stage = material->GetStage( i );
				if ( !stage || !stage->texture.image ) {
					continue;
				}
				if ( regs && !regs[stage->conditionRegister] ) {
					continue;
				}
				if ( pass == 0 && stage->lighting != SL_DIFFUSE ) {
					continue;
				}
				if ( stage->texture.texgen != TG_EXPLICIT || stage->texture.cinematic || stage->texture.dynamic != DI_STATIC ) {
					continue;
				}
				selectedStage = stage;
				break;
			}
		}
	}

	if ( !selectedStage ) {
		if ( stageIndex >= 0 ) {
			stageInfo.drawStage = false;
			return;
		}
		idImage *editorImage = material->GetEditorImage();
		stageInfo.textureIndex = TextureIndexForImage( editorImage );
		return;
	}

	stageInfo.textureIndex = TextureIndexForImage( selectedStage->texture.image );
	stageInfo.vertexColor = selectedStage->vertexColor;
	stageInfo.srcBlend = selectedStage->drawStateBits & GLS_SRCBLEND_BITS;
	stageInfo.dstBlend = selectedStage->drawStateBits & GLS_DSTBLEND_BITS;
	stageInfo.blendMode = BlendModeForBits( stageInfo.srcBlend, stageInfo.dstBlend );
	stageInfo.writeMask = SW_WRITE_COLOR;
	if ( selectedStage->drawStateBits & GLS_REDMASK ) {
		stageInfo.writeMask &= ~SW_WRITE_RED;
	}
	if ( selectedStage->drawStateBits & GLS_GREENMASK ) {
		stageInfo.writeMask &= ~SW_WRITE_GREEN;
	}
	if ( selectedStage->drawStateBits & GLS_BLUEMASK ) {
		stageInfo.writeMask &= ~SW_WRITE_BLUE;
	}
	if ( selectedStage->drawStateBits & GLS_ALPHAMASK ) {
		stageInfo.writeMask &= ~SW_WRITE_ALPHA;
	}
	if ( stageInfo.srcBlend == GLS_SRCBLEND_ZERO && stageInfo.dstBlend == GLS_DSTBLEND_ONE ) {
		stageInfo.drawStage = false;
		return;
	}

	if ( regs ) {
		stageInfo.color[0] = idMath::ClampFloat( 0.0f, 1.0f, regs[selectedStage->color.registers[0]] );
		stageInfo.color[1] = idMath::ClampFloat( 0.0f, 1.0f, regs[selectedStage->color.registers[1]] );
		stageInfo.color[2] = idMath::ClampFloat( 0.0f, 1.0f, regs[selectedStage->color.registers[2]] );
		stageInfo.color[3] = idMath::ClampFloat( 0.0f, 1.0f, regs[selectedStage->color.registers[3]] );

		if ( selectedStage->texture.hasMatrix ) {
			stageInfo.matrix[0][0] = regs[selectedStage->texture.matrix[0][0]];
			stageInfo.matrix[0][1] = regs[selectedStage->texture.matrix[0][1]];
			stageInfo.matrix[0][2] = regs[selectedStage->texture.matrix[0][2]];
			stageInfo.matrix[1][0] = regs[selectedStage->texture.matrix[1][0]];
			stageInfo.matrix[1][1] = regs[selectedStage->texture.matrix[1][1]];
			stageInfo.matrix[1][2] = regs[selectedStage->texture.matrix[1][2]];

			if ( stageInfo.matrix[0][2] < -40.0f || stageInfo.matrix[0][2] > 40.0f ) {
				stageInfo.matrix[0][2] -= static_cast<int>( stageInfo.matrix[0][2] );
			}
			if ( stageInfo.matrix[1][2] < -40.0f || stageInfo.matrix[1][2] > 40.0f ) {
				stageInfo.matrix[1][2] -= static_cast<int>( stageInfo.matrix[1][2] );
			}
		}

		if ( selectedStage->hasAlphaTest ) {
			stageInfo.alphaTest = true;
			stageInfo.alphaTestValue = idMath::ClampFloat( 0.0f, 1.0f, regs[selectedStage->alphaTestRegister] );
			stageInfo.alphaTestByte = ByteFromFloat( stageInfo.alphaTestValue );
		}
	}
}

void idSoftwareRasterizer::PrimeHybridTextureCacheFromImageManager() {
	if ( !globalImages ) {
		return;
	}

	const int imageCount = globalImages->images.Num();
	if ( hybridTextureCachePrimed && hybridImageManagerCount == imageCount ) {
		return;
	}

	const int oldTextureCount = textureCache.Num();
	for ( int i = 0; i < imageCount; i++ ) {
		const idImage *image = globalImages->images[i];
		if ( ShouldPrimeHybridTexture( image ) ) {
			AddHybridTextureToCache( image, false );
		}
	}

	hybridImageManagerCount = globalImages->images.Num();
	hybridTextureCachePrimed = true;
	if ( textureCache.Num() != oldTextureCount ) {
		common->DPrintf( "software renderer: primed hybrid texture cache with %d images (%d total)\n",
			textureCache.Num() - oldTextureCount, textureCache.Num() );
	}
}

bool idSoftwareRasterizer::ShouldPrimeHybridTexture( const idImage *image ) const {
	if ( !image ) {
		return false;
	}
	if ( IsHybridGeneratedTexture( image ) ) {
		return true;
	}
	if ( image->isPartialImage || image->backgroundLoadInProgress ) {
		return false;
	}
	if ( image->generatorFunction ) {
		return false;
	}
	if ( image->type != TT_2D || image->cubeFiles != CF_2D ) {
		return false;
	}
	if ( image->imgName[0] == '\0' ) {
		return false;
	}
	if ( !globalImages ) {
		return true;
	}

	return image != globalImages->cinematicImage &&
		image != globalImages->scratchImage &&
		image != globalImages->scratchImage2 &&
		image != globalImages->accumImage &&
		image != globalImages->currentRenderImage &&
		image != globalImages->scratchCubeMapImage;
}

bool idSoftwareRasterizer::IsHybridGeneratedTexture( const idImage *image ) const {
	if ( !globalImages || !image ) {
		return false;
	}

	return image == globalImages->whiteImage ||
		image == globalImages->noFalloffImage ||
		image == globalImages->blackImage ||
		image == globalImages->flatNormalMap ||
		image == globalImages->ambientNormalMap ||
		image == globalImages->defaultImage;
}

int idSoftwareRasterizer::TextureImageHashKey( const idImage *image ) {
	const size_t value = reinterpret_cast<size_t>( image );
	return static_cast<int>( ( value >> 4 ) ^ ( value >> 12 ) );
}

int idSoftwareRasterizer::FindTextureCacheIndex( const idImage *image ) const {
	if ( !image ) {
		return -1;
	}

	const int hashKey = TextureImageHashKey( image );
	for ( int i = textureCacheHash.First( hashKey ); i != -1; i = textureCacheHash.Next( i ) ) {
		if ( i >= 0 && i < textureCache.Num() && textureCache[i].image == image ) {
			return i;
		}
	}

	return -1;
}

int idSoftwareRasterizer::AddHybridTextureToCache( const idImage *image, bool printDefaulted ) {
	if ( !image ) {
		return -1;
	}

	const int existingIndex = FindTextureCacheIndex( image );
	if ( existingIndex >= 0 ) {
		return existingIndex;
	}

	swTexture_t &texture = textureCache.Alloc();
	if ( !LoadSoftwareTexture( texture, image ) ) {
		if ( !printDefaulted ) {
			textureCache.RemoveIndex( textureCache.Num() - 1 );
			return -1;
		}
		common->Printf( "software renderer: using default texture for '%s'\n", image->imgName.c_str() );
		texture.image = image;
		texture.name = image->imgName;
		LoadDefaultTexture( texture );
		BuildTextureMipChain( texture );
	}

	textureCacheGeneration++;
	if ( textureCacheGeneration == 0 ) {
		textureCacheGeneration = 1;
		hybridTextureAtlasGeneration = 0;
	}
	const int textureIndex = textureCache.Num() - 1;
	textureCacheHash.Add( TextureImageHashKey( image ), textureIndex );
	return textureIndex;
}

int idSoftwareRasterizer::TextureIndexForImage( const idImage *image ) {
	if ( !image ) {
		return -1;
	}

	const int existingIndex = FindTextureCacheIndex( image );
	if ( existingIndex >= 0 ) {
		return existingIndex;
	}

	return AddHybridTextureToCache( image, true );
}

bool idSoftwareRasterizer::LoadSoftwareTexture( swTexture_t &texture, const idImage *image ) const {
	texture.image = image;
	texture.name = image->imgName;
	texture.repeat = image->repeat;
	texture.width = 0;
	texture.height = 0;
	texture.mipCount = 0;
	texture.texels.Clear();
	texture.mipOffsets.Clear();
	texture.mipWidths.Clear();
	texture.mipHeights.Clear();

	if ( LoadGeneratedTexture( texture, image ) ) {
		BuildTextureMipChain( texture );
		return true;
	}

	byte *pic = NULL;
	int imageWidth = 0;
	int imageHeight = 0;
	textureDepth_t depth = TD_DEFAULT;
	R_LoadImageProgram( image->imgName.c_str(), &pic, &imageWidth, &imageHeight, NULL, &depth );
	if ( !pic || imageWidth <= 0 || imageHeight <= 0 ) {
		if ( pic ) {
			R_StaticFree( pic );
		}
		return LoadBoundTexture( texture, image );
	}

	texture.width = imageWidth;
	texture.height = imageHeight;
	texture.texels.SetNum( imageWidth * imageHeight, false );
	for ( int i = 0; i < imageWidth * imageHeight; i++ ) {
		const byte *rgba = pic + i * 4;
		texture.texels[i] = PackColor( rgba[0], rgba[1], rgba[2], rgba[3] );
	}
	R_StaticFree( pic );
	BuildTextureMipChain( texture );
	return true;
}

bool idSoftwareRasterizer::LoadBoundTexture( swTexture_t &texture, const idImage *image ) const {
	if ( !qglGetTexImage || image->type != TT_2D ) {
		return false;
	}

	idImage *mutableImage = const_cast<idImage *>( image );
	mutableImage->Bind();

	if ( image->uploadWidth <= 0 || image->uploadHeight <= 0 ) {
		return false;
	}

	texture.width = image->uploadWidth;
	texture.height = image->uploadHeight;
	texture.texels.SetNum( texture.width * texture.height, false );

	idList<byte> pixels;
	pixels.SetNum( texture.width * texture.height * 4, false );
	qglGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.Ptr() );

	for ( int i = 0; i < texture.width * texture.height; i++ ) {
		const byte *rgba = pixels.Ptr() + i * 4;
		texture.texels[i] = PackColor( rgba[0], rgba[1], rgba[2], rgba[3] );
	}
	BuildTextureMipChain( texture );
	return true;
}

bool idSoftwareRasterizer::LoadGeneratedTexture( swTexture_t &texture, const idImage *image ) const {
	if ( !globalImages ) {
		return false;
	}

	if ( image == globalImages->whiteImage || image == globalImages->noFalloffImage ) {
		texture.width = 1;
		texture.height = 1;
		texture.texels.SetNum( 1, false );
		texture.texels[0] = PackColor( 255, 255, 255, 255 );
		return true;
	}
	if ( image == globalImages->blackImage ) {
		texture.width = 1;
		texture.height = 1;
		texture.texels.SetNum( 1, false );
		texture.texels[0] = PackColor( 0, 0, 0, 255 );
		return true;
	}
	if ( image == globalImages->flatNormalMap || image == globalImages->ambientNormalMap ) {
		texture.width = 1;
		texture.height = 1;
		texture.texels.SetNum( 1, false );
		texture.texels[0] = PackColor( 128, 128, 255, 255 );
		return true;
	}
	if ( image == globalImages->fogImage ) {
		texture.width = SW_FOG_SIZE;
		texture.height = SW_FOG_SIZE;
		texture.repeat = TR_CLAMP;
		texture.texels.SetNum( texture.width * texture.height, false );

		float step[256];
		float remaining = 1.0f;
		for ( int i = 0; i < 256; i++ ) {
			step[i] = remaining;
			remaining *= 0.982f;
		}

		for ( int y = 0; y < texture.height; y++ ) {
			for ( int x = 0; x < texture.width; x++ ) {
				float d = idMath::Sqrt( static_cast<float>( ( x - SW_FOG_SIZE / 2 ) * ( x - SW_FOG_SIZE / 2 ) +
					( y - SW_FOG_SIZE / 2 ) * ( y - SW_FOG_SIZE / 2 ) ) );
				d /= static_cast<float>( SW_FOG_SIZE / 2 - 1 );
				int b = Max( 0, Min( 255, static_cast<int>( d * 255.0f ) ) );
				b = ByteFromFloat( 1.0f - step[b] );
				if ( x == 0 || x == texture.width - 1 || y == 0 || y == texture.height - 1 ) {
					b = 255;
				}
				texture.texels[y * texture.width + x] = PackColor( 255, 255, 255, b );
			}
		}
		return true;
	}
	if ( image == globalImages->fogEnterImage ) {
		texture.width = FOG_ENTER_SIZE;
		texture.height = FOG_ENTER_SIZE;
		texture.repeat = TR_CLAMP;
		texture.texels.SetNum( texture.width * texture.height, false );
		for ( int y = 0; y < texture.height; y++ ) {
			for ( int x = 0; x < texture.width; x++ ) {
				const float d = SWFogFraction( static_cast<float>( x - FOG_ENTER_SIZE / 2 ), static_cast<float>( y - FOG_ENTER_SIZE / 2 ) );
				texture.texels[y * texture.width + x] = PackColor( 255, 255, 255, ByteFromFloat( d ) );
			}
		}
		return true;
	}
	if ( image != globalImages->defaultImage ) {
		return false;
	}

	LoadDefaultTexture( texture );
	return true;
}

void idSoftwareRasterizer::LoadDefaultTexture( swTexture_t &texture ) const {
	texture.width = 16;
	texture.height = 16;
	texture.mipCount = 0;
	texture.repeat = TR_REPEAT;
	texture.texels.SetNum( texture.width * texture.height, false );
	texture.mipOffsets.Clear();
	texture.mipWidths.Clear();
	texture.mipHeights.Clear();
	for ( int y = 0; y < texture.height; y++ ) {
		for ( int x = 0; x < texture.width; x++ ) {
			const bool bright = ( ( x >> 2 ) ^ ( y >> 2 ) ) & 1;
			texture.texels[y * texture.width + x] = bright ? PackColor( 255, 0, 255, 255 ) : PackColor( 32, 32, 32, 255 );
		}
	}
}

void idSoftwareRasterizer::BuildTextureMipChain( swTexture_t &texture ) const {
	const int baseTexelCount = texture.width * texture.height;
	if ( texture.width <= 0 || texture.height <= 0 || texture.texels.Num() < baseTexelCount ) {
		texture.mipCount = 0;
		texture.mipOffsets.Clear();
		texture.mipWidths.Clear();
		texture.mipHeights.Clear();
		return;
	}

	texture.texels.SetNum( baseTexelCount, false );
	texture.mipOffsets.Clear();
	texture.mipWidths.Clear();
	texture.mipHeights.Clear();

	int currentOffset = 0;
	int currentWidth = texture.width;
	int currentHeight = texture.height;
	texture.mipOffsets.Append( currentOffset );
	texture.mipWidths.Append( currentWidth );
	texture.mipHeights.Append( currentHeight );

	while ( currentWidth > 1 || currentHeight > 1 ) {
		const int nextWidth = Max( 1, ( currentWidth + 1 ) >> 1 );
		const int nextHeight = Max( 1, ( currentHeight + 1 ) >> 1 );
		const int nextOffset = texture.texels.Num();
		texture.texels.SetNum( nextOffset + nextWidth * nextHeight, false );

		for ( int y = 0; y < nextHeight; y++ ) {
			const int srcY0 = Min( currentHeight - 1, y * 2 );
			const int srcY1 = Min( currentHeight - 1, srcY0 + 1 );
			for ( int x = 0; x < nextWidth; x++ ) {
				const int srcX0 = Min( currentWidth - 1, x * 2 );
				const int srcX1 = Min( currentWidth - 1, srcX0 + 1 );
				const unsigned int c00 = texture.texels[currentOffset + srcY0 * currentWidth + srcX0];
				const unsigned int c10 = texture.texels[currentOffset + srcY0 * currentWidth + srcX1];
				const unsigned int c01 = texture.texels[currentOffset + srcY1 * currentWidth + srcX0];
				const unsigned int c11 = texture.texels[currentOffset + srcY1 * currentWidth + srcX1];
				texture.texels[nextOffset + y * nextWidth + x] = AverageTexels( c00, c10, c01, c11 );
			}
		}

		currentOffset = nextOffset;
		currentWidth = nextWidth;
		currentHeight = nextHeight;
		texture.mipOffsets.Append( currentOffset );
		texture.mipWidths.Append( currentWidth );
		texture.mipHeights.Append( currentHeight );
	}

	texture.mipCount = texture.mipOffsets.Num();
}

unsigned int idSoftwareRasterizer::SampleTexture( int textureIndex, float s, float t ) const {
	if ( textureIndex < 0 || textureIndex >= textureCache.Num() ) {
		return PackColor( 255, 255, 255, 255 );
	}

	const swTexture_t &texture = textureCache[textureIndex];
	if ( texture.width <= 0 || texture.height <= 0 || texture.texels.Num() == 0 || texture.mipCount <= 0 ) {
		return PackColor( 255, 255, 255, 255 );
	}

	if ( ( texture.repeat == TR_CLAMP_TO_ZERO || texture.repeat == TR_CLAMP_TO_ZERO_ALPHA ) &&
		 ( s < 0.0f || s > 1.0f || t < 0.0f || t > 1.0f ) ) {
		return texture.repeat == TR_CLAMP_TO_ZERO_ALPHA ? PackColor( 0, 0, 0, 0 ) : PackColor( 0, 0, 0, 255 );
	}

	s = WrapTextureCoordinate( s, texture.repeat );
	t = WrapTextureCoordinate( t, texture.repeat );

	const int level = 0;
	const int mipWidth = texture.mipWidths[level];
	const int mipHeight = texture.mipHeights[level];
	int x = idMath::Ftoi( s * static_cast<float>( mipWidth ) );
	int y = idMath::Ftoi( t * static_cast<float>( mipHeight ) );
	x = Max( 0, Min( mipWidth - 1, x ) );
	y = Max( 0, Min( mipHeight - 1, y ) );
	return TextureTexel( texture, level, x, y );
}

unsigned int idSoftwareRasterizer::TextureTexel( const swTexture_t &texture, int level, int x, int y ) const {
	if ( texture.mipCount <= 0 || texture.texels.Num() <= 0 ) {
		return PackColor( 255, 255, 255, 255 );
	}
	level = Max( 0, Min( texture.mipCount - 1, level ) );
	const int mipWidth = texture.mipWidths[level];
	const int mipHeight = texture.mipHeights[level];
	const int mipOffset = texture.mipOffsets[level];
	if ( mipWidth <= 0 || mipHeight <= 0 || mipOffset < 0 || mipOffset >= texture.texels.Num() ) {
		return PackColor( 255, 255, 255, 255 );
	}
	if ( texture.repeat == TR_REPEAT ) {
		x %= mipWidth;
		y %= mipHeight;
		if ( x < 0 ) {
			x += mipWidth;
		}
		if ( y < 0 ) {
			y += mipHeight;
		}
	} else {
		x = Max( 0, Min( mipWidth - 1, x ) );
		y = Max( 0, Min( mipHeight - 1, y ) );
	}
	return texture.texels[mipOffset + y * mipWidth + x];
}

unsigned int idSoftwareRasterizer::SampleTextureLinear( int textureIndex, float s, float t ) const {
	return SampleTextureLinearMip( textureIndex, s, t, 0.0f );
}

unsigned int idSoftwareRasterizer::SampleTextureMip( int textureIndex, float s, float t, float lod ) const {
	return SampleTextureLinearMip( textureIndex, s, t, lod );
}

unsigned int idSoftwareRasterizer::SampleTextureLinearMip( int textureIndex, float s, float t, float lod ) const {
	if ( textureIndex < 0 || textureIndex >= textureCache.Num() ) {
		return PackColor( 255, 255, 255, 255 );
	}

	const swTexture_t &texture = textureCache[textureIndex];
	if ( texture.width <= 0 || texture.height <= 0 || texture.texels.Num() == 0 || texture.mipCount <= 0 ) {
		return PackColor( 255, 255, 255, 255 );
	}

	if ( ( texture.repeat == TR_CLAMP_TO_ZERO || texture.repeat == TR_CLAMP_TO_ZERO_ALPHA ) &&
		 ( s < 0.0f || s > 1.0f || t < 0.0f || t > 1.0f ) ) {
		return texture.repeat == TR_CLAMP_TO_ZERO_ALPHA ? PackColor( 0, 0, 0, 0 ) : PackColor( 0, 0, 0, 255 );
	}

	s = WrapTextureCoordinate( s, texture.repeat );
	t = WrapTextureCoordinate( t, texture.repeat );

	const float clampedLod = idMath::ClampFloat( 0.0f, static_cast<float>( texture.mipCount - 1 ), lod );
	const int level0 = Max( 0, Min( texture.mipCount - 1, idMath::Ftoi( idMath::Floor( clampedLod ) ) ) );
	const int level1 = Min( texture.mipCount - 1, level0 + 1 );
	const unsigned int c0 = SampleTextureLevelLinear( texture, level0, s, t );
	if ( level1 == level0 ) {
		return c0;
	}
	const unsigned int c1 = SampleTextureLevelLinear( texture, level1, s, t );
	return LerpTexels( c0, c1, clampedLod - static_cast<float>( level0 ) );
}

unsigned int idSoftwareRasterizer::SampleTextureLevelLinear( const swTexture_t &texture, int level, float s, float t ) const {
	if ( texture.mipCount <= 0 ) {
		return PackColor( 255, 255, 255, 255 );
	}
	level = Max( 0, Min( texture.mipCount - 1, level ) );
	const int mipWidth = texture.mipWidths[level];
	const int mipHeight = texture.mipHeights[level];
	if ( mipWidth <= 0 || mipHeight <= 0 ) {
		return PackColor( 255, 255, 255, 255 );
	}

	const float texelS = s * static_cast<float>( mipWidth ) - 0.5f;
	const float texelT = t * static_cast<float>( mipHeight ) - 0.5f;
	const int x0 = idMath::Ftoi( idMath::Floor( texelS ) );
	const int y0 = idMath::Ftoi( idMath::Floor( texelT ) );
	const float fracS = texelS - static_cast<float>( x0 );
	const float fracT = texelT - static_cast<float>( y0 );

	const unsigned int c00 = TextureTexel( texture, level, x0, y0 );
	const unsigned int c10 = TextureTexel( texture, level, x0 + 1, y0 );
	const unsigned int c01 = TextureTexel( texture, level, x0, y0 + 1 );
	const unsigned int c11 = TextureTexel( texture, level, x0 + 1, y0 + 1 );
	int out[4];
	const int shifts[4] = { 16, 8, 0, 24 };
	for ( int i = 0; i < 4; i++ ) {
		const int shift = shifts[i];
		const float v00 = static_cast<float>( ( c00 >> shift ) & 255 );
		const float v10 = static_cast<float>( ( c10 >> shift ) & 255 );
		const float v01 = static_cast<float>( ( c01 >> shift ) & 255 );
		const float v11 = static_cast<float>( ( c11 >> shift ) & 255 );
		const float v0 = v00 + ( v10 - v00 ) * fracS;
		const float v1 = v01 + ( v11 - v01 ) * fracS;
		out[i] = idMath::Ftoi( v0 + ( v1 - v0 ) * fracT + 0.5f );
	}
	return PackColor( out[0], out[1], out[2], out[3] );
}

float idSoftwareRasterizer::TextureLodForGradients( int textureIndex, float invW, float sOverW, float tOverW, float dsOverWdx, float dtOverWdx, float dinvWdx, float dsOverWdy, float dtOverWdy, float dinvWdy ) const {
	if ( textureIndex < 0 || textureIndex >= textureCache.Num() || invW == 0.0f ) {
		return 0.0f;
	}
	const float invW2 = invW * invW;
	if ( invW2 <= 0.000000001f ) {
		return 0.0f;
	}
	const float dsdx = ( dsOverWdx * invW - sOverW * dinvWdx ) / invW2;
	const float dtdx = ( dtOverWdx * invW - tOverW * dinvWdx ) / invW2;
	const float dsdy = ( dsOverWdy * invW - sOverW * dinvWdy ) / invW2;
	const float dtdy = ( dtOverWdy * invW - tOverW * dinvWdy ) / invW2;
	return TextureLodForDerivatives( textureIndex, dsdx, dtdx, dsdy, dtdy );
}

float idSoftwareRasterizer::TextureLodForDerivatives( int textureIndex, float dsdx, float dtdx, float dsdy, float dtdy ) const {
	if ( textureIndex < 0 || textureIndex >= textureCache.Num() ) {
		return 0.0f;
	}
	const swTexture_t &texture = textureCache[textureIndex];
	if ( texture.width <= 1 && texture.height <= 1 ) {
		return 0.0f;
	}

	const float widthScale = static_cast<float>( Max( texture.width, 1 ) );
	const float heightScale = static_cast<float>( Max( texture.height, 1 ) );
	const float rhoX = idMath::Sqrt( dsdx * dsdx * widthScale * widthScale + dtdx * dtdx * heightScale * heightScale );
	const float rhoY = idMath::Sqrt( dsdy * dsdy * widthScale * widthScale + dtdy * dtdy * heightScale * heightScale );
	const float rho = Max( rhoX, rhoY );
	if ( rho <= 1.0f ) {
		return 0.0f;
	}
	return idMath::Log( rho ) * 1.4426950408889634f;
}

unsigned int idSoftwareRasterizer::SurfaceColor( int surfIndex, const drawSurf_t *surf ) const {
	const float hue = static_cast<float>( ( surfIndex * 53 ) & 255 ) / 255.0f;
	const float r = 0.35f + 0.55f * idMath::Fabs( idMath::Sin( hue * idMath::TWO_PI + 0.0f ) );
	const float g = 0.35f + 0.55f * idMath::Fabs( idMath::Sin( hue * idMath::TWO_PI + 2.1f ) );
	const float b = 0.35f + 0.55f * idMath::Fabs( idMath::Sin( hue * idMath::TWO_PI + 4.2f ) );

	int ri = idMath::FtoiFast( r * 255.0f );
	int gi = idMath::FtoiFast( g * 255.0f );
	int bi = idMath::FtoiFast( b * 255.0f );

	if ( surf->shaderRegisters ) {
		const idMaterial *material = surf->material;
		for ( int i = 0; i < material->GetNumStages(); i++ ) {
			const shaderStage_t *stage = material->GetStage( i );
			if ( !stage || !surf->shaderRegisters[stage->conditionRegister] ) {
				continue;
			}
			ri = idMath::FtoiFast( idMath::ClampFloat( 0.0f, 1.0f, surf->shaderRegisters[stage->color.registers[0]] ) * 255.0f );
			gi = idMath::FtoiFast( idMath::ClampFloat( 0.0f, 1.0f, surf->shaderRegisters[stage->color.registers[1]] ) * 255.0f );
			bi = idMath::FtoiFast( idMath::ClampFloat( 0.0f, 1.0f, surf->shaderRegisters[stage->color.registers[2]] ) * 255.0f );
			break;
		}
	}

	return 0xff000000u | ( static_cast<unsigned int>( ri ) << 16 ) | ( static_cast<unsigned int>( gi ) << 8 ) | static_cast<unsigned int>( bi );
}

unsigned int idSoftwareRasterizer::DebugTextureColor( int mode, float s, float t ) {
	const float wrappedS = WrapTextureCoordinate( s, TR_REPEAT );
	const float wrappedT = WrapTextureCoordinate( t, TR_REPEAT );
	if ( mode == 1 ) {
		return PackColor( ByteFromFloat( wrappedS ), ByteFromFloat( wrappedT ), 64, 255 );
	}

	const int checkS = idMath::Ftoi( idMath::Floor( wrappedS * 16.0f ) );
	const int checkT = idMath::Ftoi( idMath::Floor( wrappedT * 16.0f ) );
	const bool bright = ( ( checkS ^ checkT ) & 1 ) != 0;
	return bright ? PackColor( 255, 255, 255, 255 ) : PackColor( 32, 32, 32, 255 );
}

unsigned int idSoftwareRasterizer::BlendSourceOver( unsigned int src, unsigned int dst ) {
	const int srcA = ( src >> 24 ) & 255;
	if ( srcA <= 0 ) {
		return dst;
	}
	if ( srcA >= 255 ) {
		return src;
	}

	const int invA = 255 - srcA;
	const int srcR = ( src >> 16 ) & 255;
	const int srcG = ( src >> 8 ) & 255;
	const int srcB = src & 255;
	const int dstA = ( dst >> 24 ) & 255;
	const int dstR = ( dst >> 16 ) & 255;
	const int dstG = ( dst >> 8 ) & 255;
	const int dstB = dst & 255;

	const int outA = srcA + ( dstA * invA + 127 ) / 255;
	const int outR = ( srcR * srcA + dstR * invA + 127 ) / 255;
	const int outG = ( srcG * srcA + dstG * invA + 127 ) / 255;
	const int outB = ( srcB * srcA + dstB * invA + 127 ) / 255;
	return PackColor( outR, outG, outB, outA );
}

int idSoftwareRasterizer::BlendFactor( int blend, unsigned int src, unsigned int dst, int channel, bool sourceBlend ) {
	const int srcA = ( src >> 24 ) & 255;
	const int dstA = ( dst >> 24 ) & 255;
	const int srcC = ( src >> channel ) & 255;
	const int dstC = ( dst >> channel ) & 255;

	if ( sourceBlend ) {
		switch ( blend ) {
		case GLS_SRCBLEND_ZERO:
			return 0;
		case GLS_SRCBLEND_ONE:
			return 255;
		case GLS_SRCBLEND_DST_COLOR:
			return dstC;
		case GLS_SRCBLEND_ONE_MINUS_DST_COLOR:
			return 255 - dstC;
		case GLS_SRCBLEND_SRC_ALPHA:
			return srcA;
		case GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA:
			return 255 - srcA;
		case GLS_SRCBLEND_DST_ALPHA:
			return dstA;
		case GLS_SRCBLEND_ONE_MINUS_DST_ALPHA:
			return 255 - dstA;
		case GLS_SRCBLEND_ALPHA_SATURATE:
			return channel == 24 ? 255 : Min( srcA, 255 - dstA );
		default:
			return 255;
		}
	}

	switch ( blend ) {
	case GLS_DSTBLEND_ZERO:
		return 0;
	case GLS_DSTBLEND_ONE:
		return 255;
	case GLS_DSTBLEND_SRC_COLOR:
		return srcC;
	case GLS_DSTBLEND_ONE_MINUS_SRC_COLOR:
		return 255 - srcC;
	case GLS_DSTBLEND_SRC_ALPHA:
		return srcA;
	case GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA:
		return 255 - srcA;
	case GLS_DSTBLEND_DST_ALPHA:
		return dstA;
	case GLS_DSTBLEND_ONE_MINUS_DST_ALPHA:
		return 255 - dstA;
	default:
		return 255;
	}
}

int idSoftwareRasterizer::BlendModeForBits( int srcBlend, int dstBlend ) {
	if ( srcBlend == GLS_SRCBLEND_ONE && dstBlend == GLS_DSTBLEND_ZERO ) {
		return SW_BLEND_REPLACE;
	}
	if ( srcBlend == GLS_SRCBLEND_SRC_ALPHA && dstBlend == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA ) {
		return SW_BLEND_ALPHA;
	}
	if ( srcBlend == GLS_SRCBLEND_ONE && dstBlend == GLS_DSTBLEND_ONE ) {
		return SW_BLEND_ADD;
	}
	if ( srcBlend == GLS_SRCBLEND_DST_COLOR && dstBlend == GLS_DSTBLEND_ZERO ) {
		return SW_BLEND_FILTER;
	}
	if ( srcBlend == GLS_SRCBLEND_DST_ALPHA && dstBlend == GLS_DSTBLEND_ONE_MINUS_DST_ALPHA ) {
		return SW_BLEND_DST_ALPHA;
	}
	return SW_BLEND_GENERIC;
}

unsigned int idSoftwareRasterizer::ApplyWriteMask( unsigned int src, unsigned int dst, int writeMask ) {
	if ( writeMask == SW_WRITE_COLOR ) {
		return src;
	}

	unsigned int out = dst;
	if ( writeMask & SW_WRITE_RED ) {
		out = ( out & 0xff00ffffu ) | ( src & 0x00ff0000u );
	}
	if ( writeMask & SW_WRITE_GREEN ) {
		out = ( out & 0xffff00ffu ) | ( src & 0x0000ff00u );
	}
	if ( writeMask & SW_WRITE_BLUE ) {
		out = ( out & 0xffffff00u ) | ( src & 0x000000ffu );
	}
	if ( writeMask & SW_WRITE_ALPHA ) {
		out = ( out & 0x00ffffffu ) | ( src & 0xff000000u );
	}
	return out;
}

unsigned int idSoftwareRasterizer::BlendColor( unsigned int src, unsigned int dst, int srcBlend, int dstBlend, int blendMode ) {
	if ( blendMode == SW_BLEND_ALPHA ) {
		return BlendSourceOver( src, dst );
	}
	if ( blendMode == SW_BLEND_ADD ) {
		const int r = Min( 255, static_cast<int>( ( src >> 16 ) & 255 ) + static_cast<int>( ( dst >> 16 ) & 255 ) );
		const int g = Min( 255, static_cast<int>( ( src >> 8 ) & 255 ) + static_cast<int>( ( dst >> 8 ) & 255 ) );
		const int b = Min( 255, static_cast<int>( src & 255 ) + static_cast<int>( dst & 255 ) );
		const int a = Min( 255, static_cast<int>( ( src >> 24 ) & 255 ) + static_cast<int>( ( dst >> 24 ) & 255 ) );
		return PackColor( r, g, b, a );
	}
	if ( blendMode == SW_BLEND_FILTER ) {
		const int r = ( static_cast<int>( ( src >> 16 ) & 255 ) * static_cast<int>( ( dst >> 16 ) & 255 ) + 127 ) / 255;
		const int g = ( static_cast<int>( ( src >> 8 ) & 255 ) * static_cast<int>( ( dst >> 8 ) & 255 ) + 127 ) / 255;
		const int b = ( static_cast<int>( src & 255 ) * static_cast<int>( dst & 255 ) + 127 ) / 255;
		const int a = ( static_cast<int>( ( src >> 24 ) & 255 ) * static_cast<int>( ( dst >> 24 ) & 255 ) + 127 ) / 255;
		return PackColor( r, g, b, a );
	}
	if ( blendMode == SW_BLEND_DST_ALPHA ) {
		const int dstA = ( dst >> 24 ) & 255;
		const int invA = 255 - dstA;
		const int r = ( static_cast<int>( ( src >> 16 ) & 255 ) * dstA + static_cast<int>( ( dst >> 16 ) & 255 ) * invA + 127 ) / 255;
		const int g = ( static_cast<int>( ( src >> 8 ) & 255 ) * dstA + static_cast<int>( ( dst >> 8 ) & 255 ) * invA + 127 ) / 255;
		const int b = ( static_cast<int>( src & 255 ) * dstA + static_cast<int>( dst & 255 ) * invA + 127 ) / 255;
		const int a = ( static_cast<int>( ( src >> 24 ) & 255 ) * dstA + static_cast<int>( ( dst >> 24 ) & 255 ) * invA + 127 ) / 255;
		return PackColor( r, g, b, a );
	}

	int out[4];
	const int shifts[4] = { 16, 8, 0, 24 };
	for ( int i = 0; i < 4; i++ ) {
		const int shift = shifts[i];
		const int srcC = ( src >> shift ) & 255;
		const int dstC = ( dst >> shift ) & 255;
		const int sf = BlendFactor( srcBlend, src, dst, shift, true );
		const int df = BlendFactor( dstBlend, src, dst, shift, false );
		out[i] = Min( 255, ( srcC * sf + dstC * df + 127 ) / 255 );
	}
	return PackColor( out[0], out[1], out[2], out[3] );
}

void idSoftwareRasterizer::SetupFloatPlane( float v0, float v1, float v2, float fx0, float fy0, float fx1, float fy1, float fx2, float fy2, float denom, float &base, float &dx, float &dy ) {
	dx = ( ( v1 - v0 ) * ( fy2 - fy0 ) - ( v2 - v0 ) * ( fy1 - fy0 ) ) / denom;
	dy = ( ( fx1 - fx0 ) * ( v2 - v0 ) - ( fx2 - fx0 ) * ( v1 - v0 ) ) / denom;
	base = v0 - dx * fx0 - dy * fy0;
}

unsigned int idSoftwareRasterizer::PackColor( int r, int g, int b, int a ) {
	r = Max( 0, Min( 255, r ) );
	g = Max( 0, Min( 255, g ) );
	b = Max( 0, Min( 255, b ) );
	a = Max( 0, Min( 255, a ) );
	return ( static_cast<unsigned int>( a ) << 24 ) | ( static_cast<unsigned int>( r ) << 16 ) | ( static_cast<unsigned int>( g ) << 8 ) | static_cast<unsigned int>( b );
}

ID_INLINE int idSoftwareRasterizer::PackSnorm10( float value ) {
	const float clamped = idMath::ClampFloat( -1.0f, 1.0f, value );
	const int quantized = Max( -511, Min( 511, idMath::FtoiFast( clamped * 511.0f ) ) );
	return quantized & 1023;
}

float idSoftwareRasterizer::UnpackSnorm10( unsigned int packed, int shift ) {
	int value = static_cast<int>( ( packed >> shift ) & 1023u );
	if ( value & 512 ) {
		value |= ~1023;
	}
	return idMath::ClampFloat( -1.0f, 1.0f, static_cast<float>( value ) * ( 1.0f / 511.0f ) );
}

ID_INLINE unsigned int idSoftwareRasterizer::PackNormal10( const idVec3 &normal ) {
	return PackNormal10( normal[0], normal[1], normal[2] );
}

ID_INLINE unsigned int idSoftwareRasterizer::PackNormal10( float x, float y, float z ) {
	return static_cast<unsigned int>( PackSnorm10( x ) ) |
		( static_cast<unsigned int>( PackSnorm10( y ) ) << 10 ) |
		( static_cast<unsigned int>( PackSnorm10( z ) ) << 20 );
}

ID_INLINE unsigned int idSoftwareRasterizer::PackUV16( float s, float t ) {
	float wrappedS = s - idMath::Floor( s );
	if ( wrappedS < 0.0f ) {
		wrappedS += 1.0f;
	}
	float wrappedT = t - idMath::Floor( t );
	if ( wrappedT < 0.0f ) {
		wrappedT += 1.0f;
	}
	const unsigned int us = static_cast<unsigned int>( Max( 0, Min( 65535, idMath::FtoiFast( wrappedS * 65535.0f ) ) ) );
	const unsigned int vt = static_cast<unsigned int>( Max( 0, Min( 65535, idMath::FtoiFast( wrappedT * 65535.0f ) ) ) );
	return us | ( vt << 16 );
}

unsigned int idSoftwareRasterizer::PackOptionalTextureId16( int textureIndex ) {
	if ( textureIndex < 0 ) {
		return 0;
	}
	return static_cast<unsigned int>( Max( 1, Min( 65535, textureIndex + 1 ) ) );
}

unsigned int idSoftwareRasterizer::IdDebugColor( unsigned int value ) {
	if ( value == 0xffffffffu ) {
		return PackColor( 255, 0, 255, 255 );
	}
	value ^= value >> 16;
	value *= 0x7feb352du;
	value ^= value >> 15;
	value *= 0x846ca68bu;
	value ^= value >> 16;
	return PackColor( 64 + static_cast<int>( value & 127u ),
		64 + static_cast<int>( ( value >> 8 ) & 127u ),
		64 + static_cast<int>( ( value >> 16 ) & 127u ),
		255 );
}

unsigned int idSoftwareRasterizer::ModulateColor( unsigned int color, const float scale[4] ) {
	const int b = ByteFromFloat( static_cast<float>( color & 255 ) / 255.0f * scale[2] );
	const int g = ByteFromFloat( static_cast<float>( ( color >> 8 ) & 255 ) / 255.0f * scale[1] );
	const int r = ByteFromFloat( static_cast<float>( ( color >> 16 ) & 255 ) / 255.0f * scale[0] );
	const int a = ByteFromFloat( static_cast<float>( ( color >> 24 ) & 255 ) / 255.0f * scale[3] );
	return PackColor( r, g, b, a );
}

unsigned int idSoftwareRasterizer::AverageTexels( unsigned int c00, unsigned int c10, unsigned int c01, unsigned int c11 ) {
	const int r = ( static_cast<int>( ( c00 >> 16 ) & 255 ) + static_cast<int>( ( c10 >> 16 ) & 255 ) +
		static_cast<int>( ( c01 >> 16 ) & 255 ) + static_cast<int>( ( c11 >> 16 ) & 255 ) + 2 ) >> 2;
	const int g = ( static_cast<int>( ( c00 >> 8 ) & 255 ) + static_cast<int>( ( c10 >> 8 ) & 255 ) +
		static_cast<int>( ( c01 >> 8 ) & 255 ) + static_cast<int>( ( c11 >> 8 ) & 255 ) + 2 ) >> 2;
	const int b = ( static_cast<int>( c00 & 255 ) + static_cast<int>( c10 & 255 ) +
		static_cast<int>( c01 & 255 ) + static_cast<int>( c11 & 255 ) + 2 ) >> 2;
	const int a = ( static_cast<int>( ( c00 >> 24 ) & 255 ) + static_cast<int>( ( c10 >> 24 ) & 255 ) +
		static_cast<int>( ( c01 >> 24 ) & 255 ) + static_cast<int>( ( c11 >> 24 ) & 255 ) + 2 ) >> 2;
	return PackColor( r, g, b, a );
}

unsigned int idSoftwareRasterizer::LerpTexels( unsigned int c0, unsigned int c1, float fraction ) {
	fraction = idMath::ClampFloat( 0.0f, 1.0f, fraction );
	const int shifts[4] = { 16, 8, 0, 24 };
	int out[4];
	for ( int i = 0; i < 4; i++ ) {
		const int shift = shifts[i];
		const float v0 = static_cast<float>( ( c0 >> shift ) & 255 );
		const float v1 = static_cast<float>( ( c1 >> shift ) & 255 );
		out[i] = idMath::Ftoi( v0 + ( v1 - v0 ) * fraction + 0.5f );
	}
	return PackColor( out[0], out[1], out[2], out[3] );
}

unsigned int idSoftwareRasterizer::AdditiveColor( unsigned int src, unsigned int dst ) {
	const int r = Min( 255, static_cast<int>( ( src >> 16 ) & 255 ) + static_cast<int>( ( dst >> 16 ) & 255 ) );
	const int g = Min( 255, static_cast<int>( ( src >> 8 ) & 255 ) + static_cast<int>( ( dst >> 8 ) & 255 ) );
	const int b = Min( 255, static_cast<int>( src & 255 ) + static_cast<int>( dst & 255 ) );
	const int a = Max( static_cast<int>( ( src >> 24 ) & 255 ), static_cast<int>( ( dst >> 24 ) & 255 ) );
	return PackColor( r, g, b, a );
}

float idSoftwareRasterizer::ColorChannel( unsigned int color, int shift ) {
	return static_cast<float>( ( color >> shift ) & 255 ) * ( 1.0f / 255.0f );
}

idVec3 idSoftwareRasterizer::DecodeNormal( unsigned int color ) {
	return idVec3(
		ColorChannel( color, 16 ) * 2.0f - 1.0f,
		ColorChannel( color, 8 ) * 2.0f - 1.0f,
		ColorChannel( color, 0 ) * 2.0f - 1.0f );
}

float idSoftwareRasterizer::DotPlanePoint( const idVec4 &plane, const idVec3 &point ) {
	return point[0] * plane[0] + point[1] * plane[1] + point[2] * plane[2] + plane[3];
}

int idSoftwareRasterizer::ByteFromFloat( float value ) {
	return Max( 0, Min( 255, idMath::FtoiFast( idMath::ClampFloat( 0.0f, 1.0f, value ) * 255.0f ) ) );
}

float idSoftwareRasterizer::ClipPlaneDistance( const swClipVert_t &v, int plane ) {
	switch ( plane ) {
		case 0: return v.clip[0] + v.clip[3];
		case 1: return v.clip[3] - v.clip[0];
		case 2: return v.clip[1] + v.clip[3];
		case 3: return v.clip[3] - v.clip[1];
		case 4: return v.clip[2] + v.clip[3];
		case 5: return v.clip[3] - v.clip[2];
	}
	return 0.0f;
}

void idSoftwareRasterizer::LerpClipVertex( const swClipVert_t &a, const swClipVert_t &b, float fraction, swClipVert_t &out ) {
	for ( int i = 0; i < 4; i++ ) {
		out.clip[i] = a.clip[i] + ( b.clip[i] - a.clip[i] ) * fraction;
		out.color[i] = a.color[i] + ( b.color[i] - a.color[i] ) * fraction;
	}
	for ( int i = 0; i < 3; i++ ) {
		out.global[i] = a.global[i] + ( b.global[i] - a.global[i] ) * fraction;
		out.normal[i] = a.normal[i] + ( b.normal[i] - a.normal[i] ) * fraction;
		out.tangent[0][i] = a.tangent[0][i] + ( b.tangent[0][i] - a.tangent[0][i] ) * fraction;
		out.tangent[1][i] = a.tangent[1][i] + ( b.tangent[1][i] - a.tangent[1][i] ) * fraction;
	}
	for ( int i = 0; i < SW_INTERACTION_ATTR_COUNT; i++ ) {
		out.attr[i] = a.attr[i] + ( b.attr[i] - a.attr[i] ) * fraction;
	}
	out.s = a.s + ( b.s - a.s ) * fraction;
	out.t = a.t + ( b.t - a.t ) * fraction;
}

float idSoftwareRasterizer::WrapTextureCoordinate( float value, textureRepeat_t repeat ) {
	if ( repeat == TR_REPEAT ) {
		value -= idMath::Floor( value );
		if ( value < 0.0f ) {
			value += 1.0f;
		}
		return value;
	}
	return idMath::ClampFloat( 0.0f, 1.0f, value );
}

bool idSoftwareRasterizer::FitsInt32( long long value ) {
	return value >= SW_INT32_MIN && value <= SW_INT32_MAX;
}

bool idSoftwareRasterizer::FitsEdgePacket32( long long start, long long step, int lanes ) {
	if ( !FitsInt32( step ) ) {
		return false;
	}
	return FitsInt32( start ) && FitsInt32( start + step * ( lanes - 1 ) );
}

long long idSoftwareRasterizer::EdgeValue( int ax, int ay, int bx, int by, int px, int py ) {
	return static_cast<long long>( bx - ax ) * ( py - ay ) - static_cast<long long>( by - ay ) * ( px - ax );
}

int idSoftwareRasterizer::TopLeftBias( int ax, int ay, int bx, int by ) {
	const int dx = bx - ax;
	const int dy = by - ay;
	const bool topLeft = ( dy > 0 ) || ( dy == 0 && dx < 0 );
	return topLeft ? 0 : -1;
}

#if defined( _WIN32 ) && !defined( _D3SDK )
DWORD WINAPI idSoftwareRasterizer::RasterizeWorker( LPVOID data ) {
	swRasterWorkerJob_t *job = static_cast<swRasterWorkerJob_t *>( data );
	for ( ;; ) {
		WaitForSingleObject( job->startEvent, INFINITE );
		if ( job->shutdown ) {
			SetEvent( job->doneEvent );
			break;
		}
		job->rasterizer->RasterizeTileRange( job->firstTile, job->endTile );
		SetEvent( job->doneEvent );
	}
	return 0;
}
#endif

void RB_SW_DrawView( void ) {
	swRasterizer.DrawView( backEnd.viewDef );
}
