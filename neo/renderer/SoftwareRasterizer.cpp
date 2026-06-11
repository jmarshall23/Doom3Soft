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
static const int SW_FP_SHIFT = 4;
static const int SW_FP_ONE = 1 << SW_FP_SHIFT;
static const int SW_FP_HALF = SW_FP_ONE >> 1;
static const int SW_WRITE_RED = BIT(0);
static const int SW_WRITE_GREEN = BIT(1);
static const int SW_WRITE_BLUE = BIT(2);
static const int SW_WRITE_ALPHA = BIT(3);
static const int SW_WRITE_COLOR = SW_WRITE_RED | SW_WRITE_GREEN | SW_WRITE_BLUE | SW_WRITE_ALPHA;
static const int SW_WRITE_DYNAMIC = -1;

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
};

struct swClipVert_t {
	float clip[4];
	float s;
	float t;
	float color[4];
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
	float alphaTestValue;
	int alphaTestByte;
	unsigned int fallbackColor;
};

struct swTexture_t {
	swTexture_t() {
		image = NULL;
		width = 0;
		height = 0;
		repeat = TR_REPEAT;
	}

	const idImage *image;
	idStr name;
	int width;
	int height;
	textureRepeat_t repeat;
	idList<unsigned int> texels;
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
	float alphaTestValue;
	int alphaTestByte;
	unsigned int fallbackColor;
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
	void BeginSurfacePass();
	void BeginInteractionPass();
	void ReadCurrentFramebuffer( const viewDef_t *viewDef );
	void SetupTriangles( const viewDef_t *viewDef );
	void SetupDepthPrepass( const viewDef_t *viewDef );
	void SetupAmbientTriangles( const viewDef_t *viewDef );
	void SetupDrawSurf( const viewDef_t *viewDef, const drawSurf_t *surf );
	void SetupDepthDrawSurf( const viewDef_t *viewDef, const drawSurf_t *surf );
	void SetupDrawSurfStage( const viewDef_t *viewDef, const drawSurf_t *surf, const swSurfaceStage_t &stage );
	void ConfigureStageForView( const viewDef_t *viewDef, const drawSurf_t *surf, swSurfaceStage_t &stage ) const;
	void ConfigureDepthStage( const drawSurf_t *surf, swSurfaceStage_t &stage, const shaderStage_t *sourceStage );
	bool TransformVertex( const idDrawVert &src, const float *modelViewMatrix, const float *projectionMatrix, swScreenVert_t &dst ) const;
	void ApplyTextureCoordinates( const idDrawVert &src, const swSurfaceStage_t &stage, swScreenVert_t &dst ) const;
	bool BuildClipVertex( const idDrawVert &src, const float *modelViewMatrix, const float *projectionMatrix, const swSurfaceStage_t &stage, swClipVert_t &dst ) const;
	bool BuildInteractionClipVertex( const idDrawVert &src, const float *modelViewMatrix, const float *projectionMatrix, swClipVert_t &dst ) const;
	int ClipTriangleToFrustum( const swClipVert_t &v0, const swClipVert_t &v1, const swClipVert_t &v2, swClipVert_t clipped[SW_MAX_CLIP_VERTS] ) const;
	bool ProjectClipVertex( const swClipVert_t &src, swScreenVert_t &dst ) const;
	bool ProjectInteractionClipVertex( const swClipVert_t &src, swInteractionVert_t &dst ) const;
	bool SetupTriangle( const swScreenVert_t &v0, const swScreenVert_t &v1, const swScreenVert_t &v2, int cullType, const swSurfaceStage_t &stage, swTriSetup_t &tri ) const;
	bool SetupInteractionTriangle( const swInteractionVert_t &v0, const swInteractionVert_t &v1, const swInteractionVert_t &v2, int cullType, const drawInteraction_t &interaction, swInteractionTri_t &tri );
	void BinTriangle( int triIndex );
	void BinInteractionTriangle( int triIndex );
	void DrawLights( const viewDef_t *viewDef );
	void RasterizeTiles();
	void RasterizeTileRange( int firstTile, int endTile );
	void RasterizeTile( int tileX, int tileY );
	void RasterizeTriangleInTile( const swTriSetup_t &tri, int tileX, int tileY );
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
	unsigned int ShadeInteractionPixel( const swInteractionTri_t &tri, float invW, const float attrOverW[SW_INTERACTION_ATTR_COUNT] ) const;
	void ApplyLightScale();
	void Present() const;

	void SelectSurfaceStage( int surfIndex, const drawSurf_t *surf, swSurfaceStage_t &stage, int stageIndex );
	int TextureIndexForImage( const idImage *image );
	bool LoadSoftwareTexture( swTexture_t &texture, const idImage *image ) const;
	bool LoadBoundTexture( swTexture_t &texture, const idImage *image ) const;
	bool LoadGeneratedTexture( swTexture_t &texture, const idImage *image ) const;
	void LoadDefaultTexture( swTexture_t &texture ) const;
	unsigned int SampleTexture( int textureIndex, float s, float t ) const;
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
	static unsigned int ModulateColor( unsigned int color, const float scale[4] );
	static unsigned int AdditiveColor( unsigned int src, unsigned int dst );
	static float ColorChannel( unsigned int color, int shift );
	static idVec3 DecodeNormal( unsigned int color );
	static float DotPlanePoint( const idVec4 &plane, const idVec3 &point );
	static int ByteFromFloat( float value );
	static float ClipPlaneDistance( const swClipVert_t &v, int plane );
	static void LerpClipVertex( const swClipVert_t &a, const swClipVert_t &b, float fraction, swClipVert_t &out );
	static float WrapTextureCoordinate( float value, textureRepeat_t repeat );
	static long long EdgeValue( int ax, int ay, int bx, int by, int px, int py );
	static int TopLeftBias( int ax, int ay, int bx, int by );
#if defined( _WIN32 ) && !defined( _D3SDK )
	void StartWorkers();
	void ShutdownWorkers();
	static DWORD WINAPI RasterizeWorker( LPVOID data );
#endif

	int width;
	int height;
	int tileCountX;
	int tileCountY;

	idList<float> depthBuffer;
	idList<unsigned int> colorBuffer;
	idList<swTriSetup_t> triangles;
	idList<swInteractionTri_t> interactionTriangles;
	idList<swTileBin_t> tileBins;
	idList<swTexture_t> textureCache;
	swRasterPass_t rasterPass;

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
	tileCountX = 0;
	tileCountY = 0;
	rasterPass = SW_RASTER_SURFACE;

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
	tileBins.SetNum( tileCountX * tileCountY, false );
}

void idSoftwareRasterizer::Clear( bool clearColor ) {
	for ( int i = 0; i < depthBuffer.Num(); i++ ) {
		depthBuffer[i] = 1.0f;
	}
	if ( clearColor ) {
		for ( int i = 0; i < colorBuffer.Num(); i++ ) {
			colorBuffer[i] = 0xff000000u;
		}
	}

	triangles.SetNum( 0, false );
	interactionTriangles.SetNum( 0, false );
	ClearTileBins();
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
	qglReadPixels( viewDef->viewport.x1, viewDef->viewport.y1, width, height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, colorBuffer.Ptr() );
}

void idSoftwareRasterizer::DrawView( const viewDef_t *viewDef ) {
	if ( !viewDef ) {
		return;
	}

	const int viewportWidth = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	const int viewportHeight = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;
	Resize( viewportWidth, viewportHeight );
	if ( viewDef->viewEntitys ) {
		Clear( true );
	} else {
		ReadCurrentFramebuffer( viewDef );
		Clear( false );
	}

	if ( viewDef->viewEntitys ) {
		RB_DetermineLightScale();

		BeginSurfacePass();
		SetupDepthPrepass( viewDef );
		RasterizeTiles();

		DrawLights( viewDef );
		ApplyLightScale();

		BeginSurfacePass();
		SetupAmbientTriangles( viewDef );
		RasterizeTiles();
	} else {
		BeginSurfacePass();
		SetupTriangles( viewDef );
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

void idSoftwareRasterizer::SetupDepthPrepass( const viewDef_t *viewDef ) {
	for ( int i = 0; i < viewDef->numDrawSurfs; i++ ) {
		const drawSurf_t *surf = viewDef->drawSurfs[i];
		if ( !surf || !surf->geo || !surf->material ) {
			continue;
		}
		const float sort = surf->material->GetSort();
		if ( sort < SS_OPAQUE || sort == SS_PORTAL_SKY || sort >= SS_POST_PROCESS ) {
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
		const float sort = surf->material->GetSort();
		if ( sort < SS_OPAQUE || sort == SS_PORTAL_SKY || sort >= SS_POST_PROCESS ) {
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

void idSoftwareRasterizer::SetupDrawSurf( const viewDef_t *viewDef, const drawSurf_t *surf ) {
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
		SetupDrawSurfStage( viewDef, surf, stage );
	}
}

void idSoftwareRasterizer::ConfigureStageForView( const viewDef_t *viewDef, const drawSurf_t *surf, swSurfaceStage_t &stage ) const {
	const bool is2DView = viewDef->viewEntitys == NULL;
	const bool translucent = is2DView || surf->material->Coverage() == MC_TRANSLUCENT || surf->material->GetSort() > SS_OPAQUE;
	stage.depthTest = viewDef->viewEntitys != NULL;
	stage.depthWrite = viewDef->viewEntitys != NULL && !translucent;
	stage.depthEqual = false;
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
	stage.alphaTestValue = 0.0f;
	stage.alphaTestByte = 0;
	stage.fallbackColor = PackColor( 255, 255, 255, 255 );

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
		if ( !BuildClipVertex( dv0, surf->space->modelViewMatrix, viewDef->projectionMatrix, stage, cv0 ) ||
			 !BuildClipVertex( dv1, surf->space->modelViewMatrix, viewDef->projectionMatrix, stage, cv1 ) ||
			 !BuildClipVertex( dv2, surf->space->modelViewMatrix, viewDef->projectionMatrix, stage, cv2 ) ) {
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

	dst.x = idMath::FtoiFast( sx * static_cast<float>( SW_FP_ONE ) );
	dst.y = idMath::FtoiFast( sy * static_cast<float>( SW_FP_ONE ) );
	dst.z = ndcZ * 0.5f + 0.5f;
	dst.invW = 1.0f / clip[3];
	dst.sOverW = 0.0f;
	dst.tOverW = 0.0f;
	for ( int i = 0; i < 4; i++ ) {
		dst.colorOverW[i] = dst.invW;
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

bool idSoftwareRasterizer::BuildClipVertex( const idDrawVert &src, const float *modelViewMatrix, const float *projectionMatrix, const swSurfaceStage_t &stage, swClipVert_t &dst ) const {
	if ( !modelViewMatrix || !projectionMatrix ) {
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

	dst.x = idMath::FtoiFast( sx * static_cast<float>( SW_FP_ONE ) );
	dst.y = idMath::FtoiFast( sy * static_cast<float>( SW_FP_ONE ) );
	dst.z = idMath::ClampFloat( 0.0f, 1.0f, ndcZ * 0.5f + 0.5f );
	dst.invW = invW;
	dst.sOverW = src.s * invW;
	dst.tOverW = src.t * invW;
	for ( int i = 0; i < 4; i++ ) {
		dst.colorOverW[i] = src.color[i] * invW;
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

	dst.x = idMath::FtoiFast( sx * static_cast<float>( SW_FP_ONE ) );
	dst.y = idMath::FtoiFast( sy * static_cast<float>( SW_FP_ONE ) );
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
	tri.alphaTestValue = stage.alphaTestValue;
	tri.alphaTestByte = stage.alphaTestByte;
	tri.fallbackColor = stage.fallbackColor;

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

	BeginInteractionPass();

	const int oldDepthFunc = backEnd.depthFunc;
	const viewEntity_t *oldSpace = backEnd.currentSpace;
	viewLight_t *oldLight = backEnd.vLight;

	for ( viewLight_t *vLight = viewDef->viewLights; vLight; vLight = vLight->next ) {
		backEnd.vLight = vLight;
		backEnd.currentSpace = NULL;

		if ( vLight->lightShader->IsFogLight() || vLight->lightShader->IsBlendLight() ) {
			continue;
		}
		if ( !vLight->localInteractions && !vLight->globalInteractions && !vLight->translucentInteractions ) {
			continue;
		}

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
	}

	backEnd.depthFunc = oldDepthFunc;
	backEnd.currentSpace = oldSpace;
	backEnd.vLight = oldLight;

	if ( interactionTriangles.Num() > 0 ) {
		RasterizeTiles();
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
	const int textureMode = r_softwareTextureMode.GetInteger();
	if ( textureMode == 1 ) {
		RasterizeTriangleInTileTexture<1>( tri, tileX, tileY );
	} else if ( textureMode == 2 ) {
		RasterizeTriangleInTileTexture<2>( tri, tileX, tileY );
	} else {
		RasterizeTriangleInTileTexture<0>( tri, tileX, tileY );
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
		float *depth = depthBuffer.Ptr() + y * width + x0;
		unsigned int *color = colorBuffer.Ptr() + y * width + x0;

		for ( int x = x0; x <= x1; x++ ) {
			if ( ( e0 | e1 | e2 ) >= 0 ) {
				const bool depthPass = !DEPTH_TEST || ( tri.depthEqual ? idMath::Fabs( z - *depth ) <= SW_DEPTH_EQUAL_EPSILON : z < *depth );
				if ( depthPass ) {
					const unsigned int srcColor = ShadePixel<HAS_TEXTURE, TEXTURE_MODE, COLOR_MOD>( tri, invW, sOverW, tOverW, colorOverW );
					if ( !ALPHA_TEST || ( ( srcColor >> 24 ) & 255 ) >= tri.alphaTestByte ) {
						if ( DEPTH_WRITE ) {
							*depth = z;
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
			depth++;
			color++;
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
	}
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
					const unsigned int srcColor = ShadeInteractionPixel( tri, invW, attrOverW );
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

unsigned int idSoftwareRasterizer::ShadeInteractionPixel( const swInteractionTri_t &tri, float invW, const float attrOverW[SW_INTERACTION_ATTR_COUNT] ) const {
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

	const unsigned int lightTexel = SampleTexture( tri.lightTextureIndex, lightS, lightT );
	const unsigned int falloffTexel = SampleTexture( tri.falloffTextureIndex, falloffS, 0.5f );
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

	idVec3 bumpNormal = DecodeNormal( SampleTexture( tri.bumpTextureIndex, bumpS, bumpT ) );
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

	const unsigned int diffuseTexel = SampleTexture( tri.diffuseTextureIndex, diffuseS, diffuseT );
	float r = ColorChannel( diffuseTexel, 16 ) * tri.diffuseColor[0] * ColorChannel( lightTexel, 16 ) * falloff * nDotL * vertexScale[0];
	float g = ColorChannel( diffuseTexel, 8 ) * tri.diffuseColor[1] * ColorChannel( lightTexel, 8 ) * falloff * nDotL * vertexScale[1];
	float b = ColorChannel( diffuseTexel, 0 ) * tri.diffuseColor[2] * ColorChannel( lightTexel, 0 ) * falloff * nDotL * vertexScale[2];

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
						const unsigned int specularTexel = SampleTexture( tri.specularTextureIndex, specularS, specularT );
						r += ColorChannel( specularTexel, 16 ) * tri.specularColor[0] * ColorChannel( lightTexel, 16 ) * falloff * specular * vertexScale[0];
						g += ColorChannel( specularTexel, 8 ) * tri.specularColor[1] * ColorChannel( lightTexel, 8 ) * falloff * specular * vertexScale[1];
						b += ColorChannel( specularTexel, 0 ) * tri.specularColor[2] * ColorChannel( lightTexel, 0 ) * falloff * specular * vertexScale[2];
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
		srcColor = SampleTexture( tri.textureIndex, s, t );
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
		const int a = Min( 255, static_cast<int>( ( src >> 24 ) & 255 ) + static_cast<int>( ( dst >> 24 ) & 255 ) );
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
	RB_SetGL2D();
	GL_State( GLS_DEPTHFUNC_ALWAYS | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO );
	qglDisable( GL_DEPTH_TEST );
	qglDisable( GL_STENCIL_TEST );

	qglViewport( backEnd.viewDef->viewport.x1, backEnd.viewDef->viewport.y1, width, height );
	qglScissor( backEnd.viewDef->viewport.x1, backEnd.viewDef->viewport.y1, width, height );

	qglMatrixMode( GL_PROJECTION );
	qglLoadIdentity();
	qglOrtho( 0, width, 0, height, -1, 1 );
	qglMatrixMode( GL_MODELVIEW );
	qglLoadIdentity();
	qglRasterPos2i( 0, 0 );
	qglDrawPixels( width, height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, colorBuffer.Ptr() );
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
	stageInfo.alphaTestValue = 0.0f;
	stageInfo.alphaTestByte = 0;
	stageInfo.fallbackColor = SurfaceColor( surfIndex, surf );

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

int idSoftwareRasterizer::TextureIndexForImage( const idImage *image ) {
	if ( !image ) {
		return -1;
	}

	for ( int i = 0; i < textureCache.Num(); i++ ) {
		if ( textureCache[i].image == image ) {
			return i;
		}
	}

	swTexture_t &texture = textureCache.Alloc();
	if ( !LoadSoftwareTexture( texture, image ) ) {
		common->Printf( "software renderer: using default texture for '%s'\n", image->imgName.c_str() );
		texture.image = image;
		texture.name = image->imgName;
		LoadDefaultTexture( texture );
	}
	return textureCache.Num() - 1;
}

bool idSoftwareRasterizer::LoadSoftwareTexture( swTexture_t &texture, const idImage *image ) const {
	texture.image = image;
	texture.name = image->imgName;
	texture.repeat = image->repeat;
	texture.width = 0;
	texture.height = 0;
	texture.texels.Clear();

	if ( LoadGeneratedTexture( texture, image ) ) {
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
	if ( image != globalImages->defaultImage ) {
		return false;
	}

	LoadDefaultTexture( texture );
	return true;
}

void idSoftwareRasterizer::LoadDefaultTexture( swTexture_t &texture ) const {
	texture.width = 16;
	texture.height = 16;
	texture.repeat = TR_REPEAT;
	texture.texels.SetNum( texture.width * texture.height, false );
	for ( int y = 0; y < texture.height; y++ ) {
		for ( int x = 0; x < texture.width; x++ ) {
			const bool bright = ( ( x >> 2 ) ^ ( y >> 2 ) ) & 1;
			texture.texels[y * texture.width + x] = bright ? PackColor( 255, 0, 255, 255 ) : PackColor( 32, 32, 32, 255 );
		}
	}
}

unsigned int idSoftwareRasterizer::SampleTexture( int textureIndex, float s, float t ) const {
	if ( textureIndex < 0 || textureIndex >= textureCache.Num() ) {
		return PackColor( 255, 255, 255, 255 );
	}

	const swTexture_t &texture = textureCache[textureIndex];
	if ( texture.width <= 0 || texture.height <= 0 || texture.texels.Num() == 0 ) {
		return PackColor( 255, 255, 255, 255 );
	}

	if ( ( texture.repeat == TR_CLAMP_TO_ZERO || texture.repeat == TR_CLAMP_TO_ZERO_ALPHA ) &&
		 ( s < 0.0f || s > 1.0f || t < 0.0f || t > 1.0f ) ) {
		return texture.repeat == TR_CLAMP_TO_ZERO_ALPHA ? PackColor( 0, 0, 0, 0 ) : PackColor( 0, 0, 0, 255 );
	}

	s = WrapTextureCoordinate( s, texture.repeat );
	t = WrapTextureCoordinate( t, texture.repeat );

	int x = idMath::Ftoi( s * static_cast<float>( texture.width ) );
	int y = idMath::Ftoi( t * static_cast<float>( texture.height ) );
	x = Max( 0, Min( texture.width - 1, x ) );
	y = Max( 0, Min( texture.height - 1, y ) );
	return texture.texels[y * texture.width + x];
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

unsigned int idSoftwareRasterizer::ModulateColor( unsigned int color, const float scale[4] ) {
	const int b = ByteFromFloat( static_cast<float>( color & 255 ) / 255.0f * scale[2] );
	const int g = ByteFromFloat( static_cast<float>( ( color >> 8 ) & 255 ) / 255.0f * scale[1] );
	const int r = ByteFromFloat( static_cast<float>( ( color >> 16 ) & 255 ) / 255.0f * scale[0] );
	const int a = ByteFromFloat( static_cast<float>( ( color >> 24 ) & 255 ) / 255.0f * scale[3] );
	return PackColor( r, g, b, a );
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
