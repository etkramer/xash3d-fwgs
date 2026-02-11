/*
gl_deferred.c - deferred rendering infrastructure
Copyright (C) 2024

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "gl_local.h"

// Debug mode cvar: 0 = off, 1 = show g-buffer debug view
static cvar_t *gl_deferred_debug;
// Enable deferred rendering
static cvar_t *gl_deferred;

// G-buffer state
static struct
{
	GLuint fbo;
	GLuint albedoTex;
	GLuint normalTex;
	GLuint lightmapTex;
	GLuint depthTex;
	int width;
	int height;
	qboolean initialized;
} gbuffer;

// Track if we're currently rendering to the g-buffer
static qboolean inGBufferPass = false;

// Track if deferred rendering is available
static qboolean deferredAvailable = false;

// Lighting shader state
static struct
{
	GLuint program;
	GLuint vertShader;
	GLuint fragShader;
	GLint uAlbedo;
	GLint uNormal;
	GLint uLightmap;
	GLint uDebugMode;
	GLuint vao;
	qboolean initialized;
} lightingShader;

static const char *lightingVertSrc = NULL;
static const char *lightingFragSrc = NULL;

/*
================
R_LoadLightingShaderSources
================
*/
static void R_LoadLightingShaderSources( void )
{
	fs_offset_t size;
	byte *data;

	if( lightingVertSrc && lightingFragSrc )
		return;

	data = gEngfuncs.fsapi->LoadFile( "shaders/lighting_vertex.glsl", &size, false );
	if( !data || !size )
	{
		gEngfuncs.Con_Printf( S_ERROR "R_LoadLightingShaderSources: missing shader shaders/lighting_vertex.glsl\n" );
		return;
	}
	lightingVertSrc = (const char *)data;

	data = gEngfuncs.fsapi->LoadFile( "shaders/lighting_fragment.glsl", &size, false );
	if( !data || !size )
	{
		gEngfuncs.Con_Printf( S_ERROR "R_LoadLightingShaderSources: missing shader shaders/lighting_fragment.glsl\n" );
		Mem_Free( (void *)lightingVertSrc );
		lightingVertSrc = NULL;
		return;
	}
	lightingFragSrc = (const char *)data;
}

/*
================
R_FreeLightingShaderSources
================
*/
static void R_FreeLightingShaderSources( void )
{
	if( lightingVertSrc )
	{
		Mem_Free( (void *)lightingVertSrc );
		lightingVertSrc = NULL;
	}
	if( lightingFragSrc )
	{
		Mem_Free( (void *)lightingFragSrc );
		lightingFragSrc = NULL;
	}
}

/*
================
R_CompileShader
================
*/
static GLuint R_CompileShader( GLenum type, const char *source )
{
	GLuint shader;
	GLint status, len;
	char infoLog[1024];

	shader = pglCreateShader( type );
	len = Q_strlen( source );
	pglShaderSource( shader, 1, &source, &len );
	pglCompileShader( shader );

	pglGetShaderiv( shader, GL_COMPILE_STATUS, &status );
	if( status == GL_FALSE )
	{
		pglGetShaderInfoLog( shader, sizeof( infoLog ), NULL, infoLog );
		gEngfuncs.Con_Printf( S_ERROR "R_CompileShader: compile failed:\n%s\n", infoLog );
		pglDeleteShader( shader );
		return 0;
	}

	return shader;
}

/*
================
R_InitLightingShader
================
*/
static qboolean R_InitLightingShader( void )
{
	GLint status;
	char infoLog[1024];

	if( lightingShader.initialized )
		return true;

	gEngfuncs.Con_Printf( "R_InitLightingShader: loading shader sources...\n" );

	R_LoadLightingShaderSources();

	if( !lightingVertSrc || !lightingFragSrc )
	{
		gEngfuncs.Con_Printf( S_ERROR "R_InitLightingShader: shader sources not loaded\n" );
		return false;
	}

	gEngfuncs.Con_Printf( "R_InitLightingShader: compiling shaders...\n" );

	// Compile vertex shader
	lightingShader.vertShader = R_CompileShader( GL_VERTEX_SHADER_ARB, lightingVertSrc );
	if( !lightingShader.vertShader )
		return false;

	// Compile fragment shader
	lightingShader.fragShader = R_CompileShader( GL_FRAGMENT_SHADER_ARB, lightingFragSrc );
	if( !lightingShader.fragShader )
	{
		pglDeleteShader( lightingShader.vertShader );
		return false;
	}

	// Link program
	lightingShader.program = pglCreateProgram();
	pglAttachShader( lightingShader.program, lightingShader.vertShader );
	pglAttachShader( lightingShader.program, lightingShader.fragShader );
	pglLinkProgram( lightingShader.program );

	pglGetProgramiv( lightingShader.program, GL_LINK_STATUS, &status );
	if( status == GL_FALSE )
	{
		pglGetProgramInfoLog( lightingShader.program, sizeof( infoLog ), NULL, infoLog );
		gEngfuncs.Con_Printf( S_ERROR "R_InitLightingShader: link failed:\n%s\n", infoLog );
		pglDeleteShader( lightingShader.vertShader );
		pglDeleteShader( lightingShader.fragShader );
		pglDeleteProgram( lightingShader.program );
		return false;
	}

	// Get uniform locations
	lightingShader.uAlbedo = pglGetUniformLocation( lightingShader.program, "uAlbedo" );
	lightingShader.uNormal = pglGetUniformLocation( lightingShader.program, "uNormal" );
	lightingShader.uLightmap = pglGetUniformLocation( lightingShader.program, "uLightmap" );
	lightingShader.uDebugMode = pglGetUniformLocation( lightingShader.program, "uDebugMode" );

	// Set texture units (these don't change)
	pglUseProgram( lightingShader.program );
	if( lightingShader.uAlbedo >= 0 )
		pglUniform1i( lightingShader.uAlbedo, 0 );
	if( lightingShader.uNormal >= 0 )
		pglUniform1i( lightingShader.uNormal, 1 );
	if( lightingShader.uLightmap >= 0 )
		pglUniform1i( lightingShader.uLightmap, 2 );
	pglUseProgram( 0 );

	// Create VAO for fullscreen triangle (required for core profile)
	pglGenVertexArrays( 1, &lightingShader.vao );

	lightingShader.initialized = true;
	gEngfuncs.Con_Printf( "Deferred lighting shader initialized\n" );

	return true;
}

/*
================
R_ShutdownLightingShader
================
*/
static void R_ShutdownLightingShader( void )
{
	if( !lightingShader.initialized )
		return;

	if( lightingShader.vao )
		pglDeleteVertexArrays( 1, &lightingShader.vao );

	if( lightingShader.program )
		pglDeleteProgram( lightingShader.program );

	if( lightingShader.vertShader )
		pglDeleteShader( lightingShader.vertShader );

	if( lightingShader.fragShader )
		pglDeleteShader( lightingShader.fragShader );

	memset( &lightingShader, 0, sizeof( lightingShader ));

	R_FreeLightingShaderSources();
}

/*
================
R_CreateGBuffer
================
*/
static qboolean R_CreateGBuffer( int width, int height )
{
	GLenum drawBuffers[3] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
	GLenum status;

	if( gbuffer.initialized && gbuffer.width == width && gbuffer.height == height )
		return true;

	gEngfuncs.Con_DPrintf( "R_CreateGBuffer: creating %dx%d g-buffer\n", width, height );

	// Clean up existing resources if resizing
	if( gbuffer.initialized )
	{
		pglDeleteTextures( 1, &gbuffer.albedoTex );
		pglDeleteTextures( 1, &gbuffer.normalTex );
		pglDeleteTextures( 1, &gbuffer.lightmapTex );
		pglDeleteTextures( 1, &gbuffer.depthTex );
		pglDeleteFramebuffers( 1, &gbuffer.fbo );
		gbuffer.initialized = false;
	}

	gbuffer.width = width;
	gbuffer.height = height;

	// Create FBO
	pglGenFramebuffers( 1, &gbuffer.fbo );
	pglBindFramebuffer( GL_FRAMEBUFFER, gbuffer.fbo );

	// Create albedo texture (RGB = diffuse color, A = unused)
	pglGenTextures( 1, &gbuffer.albedoTex );
	pglBindTexture( GL_TEXTURE_2D, gbuffer.albedoTex );
	pglTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	pglFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gbuffer.albedoTex, 0 );

	// Create normal texture (RGB = world-space normal, A = unused)
	pglGenTextures( 1, &gbuffer.normalTex );
	pglBindTexture( GL_TEXTURE_2D, gbuffer.normalTex );
	pglTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	pglFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, gbuffer.normalTex, 0 );

	// Create lightmap texture
	pglGenTextures( 1, &gbuffer.lightmapTex );
	pglBindTexture( GL_TEXTURE_2D, gbuffer.lightmapTex );
	pglTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	pglFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, gbuffer.lightmapTex, 0 );

	// Create depth texture
	pglGenTextures( 1, &gbuffer.depthTex );
	pglBindTexture( GL_TEXTURE_2D, gbuffer.depthTex );
	pglTexImage2D( GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	pglFramebufferTexture2D( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, gbuffer.depthTex, 0 );

	// Set draw buffers
	pglDrawBuffers( 3, drawBuffers );

	// Check framebuffer status
	status = pglCheckFramebufferStatus( GL_FRAMEBUFFER );
	if( status != GL_FRAMEBUFFER_COMPLETE )
	{
		gEngfuncs.Con_Printf( S_ERROR "R_CreateGBuffer: framebuffer incomplete (status 0x%x)\n", status );
		pglBindFramebuffer( GL_FRAMEBUFFER, 0 );
		return false;
	}

	pglBindFramebuffer( GL_FRAMEBUFFER, 0 );

	gbuffer.initialized = true;
	gEngfuncs.Con_Printf( "G-buffer created: %dx%d\n", width, height );

	return true;
}

/*
================
R_DestroyGBuffer
================
*/
static void R_DestroyGBuffer( void )
{
	if( !gbuffer.initialized )
		return;

	pglDeleteTextures( 1, &gbuffer.albedoTex );
	pglDeleteTextures( 1, &gbuffer.normalTex );
	pglDeleteTextures( 1, &gbuffer.lightmapTex );
	pglDeleteTextures( 1, &gbuffer.depthTex );
	pglDeleteFramebuffers( 1, &gbuffer.fbo );

	memset( &gbuffer, 0, sizeof( gbuffer ));
}

/*
================
R_InitDeferred
================
*/
qboolean R_InitDeferred( void )
{
	// Register enable cvar
	gl_deferred = gEngfuncs.Cvar_Get( "gl_deferred", "1", FCVAR_GLCONFIG, "enable deferred rendering (0=off, 1=on)" );

	// Register debug cvar
	gl_deferred_debug = gEngfuncs.Cvar_Get( "gl_deferred_debug", "0", FCVAR_GLCONFIG, "show deferred rendering debug view (0=off, 1=show g-buffer)" );

	// Check if FBO functions are available
	if( !pglGenFramebuffers || !pglBindFramebuffer || !pglFramebufferTexture2D )
	{
		gEngfuncs.Con_Printf( S_WARN "R_InitDeferred: FBO functions not available\n" );
		deferredAvailable = false;
		return false;
	}

	if( !R_InitLightingShader() )
	{
		gEngfuncs.Con_Printf( S_ERROR "R_InitDeferred: failed to initialize lighting shader\n" );
		deferredAvailable = false;
		return false;
	}

	deferredAvailable = true;
	gEngfuncs.Con_Printf( "Deferred rendering initialized\n" );
	return true;
}

/*
================
R_ShutdownDeferred
================
*/
void R_ShutdownDeferred( void )
{
	R_DestroyGBuffer();
	R_ShutdownLightingShader();
	deferredAvailable = false;
	inGBufferPass = false;
}

/*
================
R_BeginGBufferPass

Binds the G-buffer FBO for geometry rendering
================
*/
void R_BeginGBufferPass( void )
{
	int width, height;

	// Skip if deferred rendering not available
	if( !deferredAvailable || !gl_deferred )
		return;

	width = RI.viewport[2];
	height = RI.viewport[3];

	// Sanity check viewport dimensions
	if( width <= 0 || height <= 0 )
		return;

	// Ensure G-buffer is the right size
	if( !R_CreateGBuffer( width, height ))
	{
		gEngfuncs.Con_Printf( S_ERROR "R_BeginGBufferPass: failed to create g-buffer\n" );
		return;
	}

	// Bind G-buffer FBO
	pglBindFramebuffer( GL_FRAMEBUFFER, gbuffer.fbo );
	inGBufferPass = true;

#if !XASH_GL_STATIC
	// Enable g-buffer shader mode
	GL2_SetGBufferMode( true );
#endif

	// Clear the G-buffer
	pglClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	pglClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
}

/*
================
R_EndGBufferPass

Unbinds the G-buffer FBO
================
*/
void R_EndGBufferPass( void )
{
	if( !inGBufferPass )
		return;

#if !XASH_GL_STATIC
	// Disable g-buffer shader mode
	GL2_SetGBufferMode( false );
#endif

	pglBindFramebuffer( GL_FRAMEBUFFER, 0 );
	inGBufferPass = false;

	// Reset draw buffer to default (single buffer)
	pglDrawBuffer( GL_BACK );
}

/*
================
R_DeferredActive

Returns true if deferred rendering is currently active
================
*/
qboolean R_DeferredActive( void )
{
	return deferredAvailable && gbuffer.initialized && lightingShader.initialized && gl_deferred;
}

/*
================
R_InGBufferPass

Returns true if currently rendering to the g-buffer
================
*/
qboolean R_InGBufferPass( void )
{
	return inGBufferPass;
}

/*
================
R_BlitGBufferDepth

Copies the G-buffer depth to the default framebuffer so forward-rendered
transparent objects can depth test against the deferred geometry.
================
*/
void R_BlitGBufferDepth( void )
{
	if( !gbuffer.initialized )
		return;

	// Blit depth from G-buffer FBO to default framebuffer
	pglBindFramebuffer( GL_READ_FRAMEBUFFER, gbuffer.fbo );
	pglBindFramebuffer( GL_DRAW_FRAMEBUFFER, 0 );

	pglBlitFramebuffer(
		0, 0, gbuffer.width, gbuffer.height,
		0, 0, gbuffer.width, gbuffer.height,
		GL_DEPTH_BUFFER_BIT,
		GL_NEAREST
	);

	pglBindFramebuffer( GL_FRAMEBUFFER, 0 );
}

/*
================
R_DeferredLightingPass

Renders the fullscreen lighting pass
================
*/
void R_DeferredLightingPass( void )
{
	int debugMode;

	if( !lightingShader.initialized || !gbuffer.initialized )
		return;

	debugMode = gl_deferred_debug ? (int)gl_deferred_debug->value : 0;

	// Set up viewport for full screen
	pglViewport( 0, 0, gbuffer.width, gbuffer.height );

	// Disable states that might interfere
	pglDisable( GL_DEPTH_TEST );
	pglDisable( GL_BLEND );
	pglDisable( GL_CULL_FACE );
	pglDisable( GL_ALPHA_TEST );
	pglDepthMask( GL_FALSE );

	// Use lighting shader
	pglUseProgram( lightingShader.program );

	// Set debug mode uniform
	if( lightingShader.uDebugMode >= 0 )
		pglUniform1i( lightingShader.uDebugMode, debugMode );

	// Bind G-buffer textures
	// Unit 0: Albedo
	pglActiveTexture( GL_TEXTURE0_ARB );
	pglBindTexture( GL_TEXTURE_2D, gbuffer.albedoTex );

	// Unit 1: Normal
	pglActiveTexture( GL_TEXTURE0_ARB + 1 );
	pglBindTexture( GL_TEXTURE_2D, gbuffer.normalTex );

	// Unit 2: Lightmap
	pglActiveTexture( GL_TEXTURE0_ARB + 2 );
	pglBindTexture( GL_TEXTURE_2D, gbuffer.lightmapTex );

	// Bind VAO and draw fullscreen triangle
	pglBindVertexArray( lightingShader.vao );
	pglDrawArrays( GL_TRIANGLES, 0, 3 );
	pglBindVertexArray( 0 );

	// Cleanup
	pglActiveTexture( GL_TEXTURE0_ARB + 2 );
	pglBindTexture( GL_TEXTURE_2D, 0 );
	pglActiveTexture( GL_TEXTURE0_ARB + 1 );
	pglBindTexture( GL_TEXTURE_2D, 0 );
	pglActiveTexture( GL_TEXTURE0_ARB );
	pglBindTexture( GL_TEXTURE_2D, 0 );

	pglUseProgram( 0 );

#if !XASH_GL_STATIC
	// Invalidate GL2 shim program state since we used an external shader
	GL2_InvalidateProg();
#endif

	// Restore GL state for forward rendering pass
	pglDepthMask( GL_TRUE );
	pglEnable( GL_DEPTH_TEST );
	pglEnable( GL_CULL_FACE );

	// Restore viewport
	pglViewport( RI.viewport[0], RI.viewport[1], RI.viewport[2], RI.viewport[3] );
}
