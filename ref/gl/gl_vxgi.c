/*
gl_vxgi.c - voxel-based global illumination
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

#define VXGI_GRID_SIZE 64
#define VXGI_MAX_LIGHTS 256

// VXGI cvars
static cvar_t *gl_vxgi;
static cvar_t *gl_vxgi_intensity;
static cvar_t *gl_vxgi_debug;

// Static light parsed from BSP entities
typedef struct
{
	vec3_t origin;
	vec3_t color;      // RGB normalized 0-1
	float intensity;   // Light intensity/radius
	qboolean valid;
} vxgi_light_t;

// VXGI state
static struct
{
	GLuint voxelTex;           // 3D RGBA texture for radiance
	vec3_t gridMins;           // World-space bounds min
	vec3_t gridMaxs;           // World-space bounds max
	float voxelSize;           // Size of one voxel in world units
	qboolean initialized;
	qboolean available;

	// Parsed static lights from BSP
	vxgi_light_t lights[VXGI_MAX_LIGHTS];
	int numLights;
	qboolean lightsParsed;
} vxgi;


/*
================
R_VXGICreateVoxelTexture
================
*/
static qboolean R_VXGICreateVoxelTexture( void )
{
	byte *initialData;

	if( vxgi.voxelTex )
		return true;

	// Allocate and zero-fill initial data to ensure texture is complete
	initialData = Mem_Calloc( r_temppool, VXGI_GRID_SIZE * VXGI_GRID_SIZE * VXGI_GRID_SIZE * 4 );

	pglGenTextures( 1, &vxgi.voxelTex );
	pglBindTexture( GL_TEXTURE_3D, vxgi.voxelTex );

	// Allocate 3D texture with initial data (not NULL - ensures texture is complete)
	pglTexImage3D( GL_TEXTURE_3D, 0, GL_RGBA8, VXGI_GRID_SIZE, VXGI_GRID_SIZE, VXGI_GRID_SIZE,
	               0, GL_RGBA, GL_UNSIGNED_BYTE, initialData );

	Mem_Free( initialData );

	// Set filtering - use LINEAR for now (mipmaps generated after data upload)
	pglTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	pglTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	pglTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	pglTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	pglTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE );

	pglBindTexture( GL_TEXTURE_3D, 0 );

	gEngfuncs.Con_Printf( "VXGI voxel texture created: %dx%dx%d (id=%u)\n", VXGI_GRID_SIZE, VXGI_GRID_SIZE, VXGI_GRID_SIZE, vxgi.voxelTex );
	return true;
}

/*
================
R_VXGIDestroyVoxelTexture
================
*/
static void R_VXGIDestroyVoxelTexture( void )
{
	if( vxgi.voxelTex )
	{
		pglDeleteTextures( 1, &vxgi.voxelTex );
		vxgi.voxelTex = 0;
	}
}

/*
================
R_InitVXGI
================
*/
qboolean R_InitVXGI( void )
{
	// Register cvars
	gl_vxgi = gEngfuncs.Cvar_Get( "gl_vxgi", "1", FCVAR_GLCONFIG, "enable voxel global illumination (0=off, 1=on)" );
	gl_vxgi_intensity = gEngfuncs.Cvar_Get( "gl_vxgi_intensity", "1.0", FCVAR_GLCONFIG, "indirect lighting intensity multiplier" );
	gl_vxgi_debug = gEngfuncs.Cvar_Get( "gl_vxgi_debug", "0", FCVAR_GLCONFIG, "vxgi debug mode (0=off, 1=voxels, 2=indirect only)" );

	// Check for required GL features - need 3D texture support
	if( !GL_Support( GL_TEXTURE_3D_EXT ))
	{
		gEngfuncs.Con_Printf( S_WARN "R_InitVXGI: 3D textures not supported\n" );
		vxgi.available = false;
		return false;
	}

	if( !pglTexImage3D || !pglTexSubImage3D )
	{
		gEngfuncs.Con_Printf( S_WARN "R_InitVXGI: required GL functions not available\n" );
		vxgi.available = false;
		return false;
	}

	// Create voxel texture
	if( !R_VXGICreateVoxelTexture() )
	{
		gEngfuncs.Con_Printf( S_ERROR "R_InitVXGI: failed to create voxel texture\n" );
		vxgi.available = false;
		return false;
	}

	vxgi.available = true;
	vxgi.initialized = true;
	gEngfuncs.Con_Printf( "VXGI initialized\n" );
	return true;
}

/*
================
R_ShutdownVXGI
================
*/
void R_ShutdownVXGI( void )
{
	R_VXGIDestroyVoxelTexture();
	memset( &vxgi, 0, sizeof( vxgi ) );
}

/*
================
R_VXGIActive
================
*/
qboolean R_VXGIActive( void )
{
	return vxgi.available && vxgi.initialized && gl_vxgi && gl_vxgi->value;
}

/*
================
R_VXGIComputeBounds

Computes the voxel grid bounds from the world model
================
*/
static void R_VXGIComputeBounds( void )
{
	vec3_t size, center;
	float maxExtent;

	if( !WORLDMODEL )
		return;

	// Get world bounds and compute center
	VectorAdd( WORLDMODEL->mins, WORLDMODEL->maxs, center );
	VectorScale( center, 0.5f, center );

	// Compute max extent for cubic grid
	VectorSubtract( WORLDMODEL->maxs, WORLDMODEL->mins, size );
	maxExtent = Q_max( size[0], Q_max( size[1], size[2] ) );

	// Make grid cubic - use maxExtent for all axes to ensure uniform voxel size
	// This fixes anisotropic sampling where cone tracing would step at different
	// rates along different axes, causing position-dependent shiny artifacts
	vxgi.gridMins[0] = center[0] - maxExtent * 0.5f;
	vxgi.gridMins[1] = center[1] - maxExtent * 0.5f;
	vxgi.gridMins[2] = center[2] - maxExtent * 0.5f;
	vxgi.gridMaxs[0] = center[0] + maxExtent * 0.5f;
	vxgi.gridMaxs[1] = center[1] + maxExtent * 0.5f;
	vxgi.gridMaxs[2] = center[2] + maxExtent * 0.5f;

	vxgi.voxelSize = maxExtent / VXGI_GRID_SIZE;
}

/*
================
R_VXGIClearVoxels

Clears the voxel grid to black
================
*/
static void R_VXGIClearVoxels( void )
{
	static byte clearData[VXGI_GRID_SIZE * VXGI_GRID_SIZE * VXGI_GRID_SIZE * 4];
	static qboolean cleared = false;

	if( !cleared )
	{
		memset( clearData, 0, sizeof( clearData ) );
		cleared = true;
	}

	pglBindTexture( GL_TEXTURE_3D, vxgi.voxelTex );
	pglTexSubImage3D( GL_TEXTURE_3D, 0, 0, 0, 0,
	                  VXGI_GRID_SIZE, VXGI_GRID_SIZE, VXGI_GRID_SIZE,
	                  GL_RGBA, GL_UNSIGNED_BYTE, clearData );
	pglBindTexture( GL_TEXTURE_3D, 0 );
}

/*
================
R_VXGIParseLights

Parses light entities from the BSP entity lump
================
*/
static void R_VXGIParseLights( void )
{
	char *data;
	char token[2048];
	char key[256], value[256];

	vxgi.numLights = 0;
	vxgi.lightsParsed = true;

	if( !WORLDMODEL || !WORLDMODEL->entities )
		return;

	data = (char *)WORLDMODEL->entities;

	// Parse entities
	while( data )
	{
		// Parse opening brace
		data = COM_ParseFile( data, token, sizeof( token ));
		if( !data )
			break;
		if( token[0] != '{' )
			continue;

		// Initialize temporary light data
		vec3_t origin = { 0, 0, 0 };
		vec3_t color = { 1.0f, 1.0f, 1.0f };
		float intensity = 300.0f;  // Default light radius
		qboolean isLight = false;
		qboolean hasOrigin = false;

		// Parse key-value pairs
		while( data )
		{
			data = COM_ParseFile( data, token, sizeof( token ));
			if( !data || token[0] == '}' )
				break;

			Q_strncpy( key, token, sizeof( key ));

			data = COM_ParseFile( data, token, sizeof( token ));
			if( !data )
				break;

			Q_strncpy( value, token, sizeof( value ));

			// Check classname
			if( !Q_stricmp( key, "classname" ))
			{
				if( !Q_strnicmp( value, "light", 5 ))
					isLight = true;
			}
			// Parse origin
			else if( !Q_stricmp( key, "origin" ))
			{
				if( sscanf( value, "%f %f %f", &origin[0], &origin[1], &origin[2] ) == 3 )
					hasOrigin = true;
			}
			// Parse _light (RGBA or RGB + intensity)
			else if( !Q_stricmp( key, "_light" ))
			{
				int r, g, b, i;
				if( sscanf( value, "%d %d %d %d", &r, &g, &b, &i ) == 4 )
				{
					color[0] = r / 255.0f;
					color[1] = g / 255.0f;
					color[2] = b / 255.0f;
					intensity = (float)i;
				}
				else if( sscanf( value, "%d %d %d", &r, &g, &b ) == 3 )
				{
					color[0] = r / 255.0f;
					color[1] = g / 255.0f;
					color[2] = b / 255.0f;
				}
			}
			// Parse light (intensity only, older format)
			else if( !Q_stricmp( key, "light" ))
			{
				intensity = Q_atof( value );
			}
		}

		// Add light if valid
		if( isLight && hasOrigin && vxgi.numLights < VXGI_MAX_LIGHTS )
		{
			vxgi_light_t *light = &vxgi.lights[vxgi.numLights];
			VectorCopy( origin, light->origin );
			VectorCopy( color, light->color );
			light->intensity = intensity;
			light->valid = true;
			vxgi.numLights++;
		}
	}
}

/*
================
R_VXGIInjectLights

Injects dynamic lights into the voxel grid
================
*/
static void R_VXGIInjectLights( void )
{
	int i;
	byte *voxelData;
	vec3_t gridSize;

	VectorSubtract( vxgi.gridMaxs, vxgi.gridMins, gridSize );

	// Avoid division by zero
	if( gridSize[0] < 1.0f ) gridSize[0] = 1.0f;
	if( gridSize[1] < 1.0f ) gridSize[1] = 1.0f;
	if( gridSize[2] < 1.0f ) gridSize[2] = 1.0f;

	// Allocate temporary buffer for light injection
	voxelData = Mem_Calloc( r_temppool, VXGI_GRID_SIZE * VXGI_GRID_SIZE * VXGI_GRID_SIZE * 4 );

	// Debug mode: inject a test light at camera position
	if( gl_vxgi_debug && gl_vxgi_debug->value >= 1 )
	{
		vec3_t voxelPos;
		int vx, vy, vz;
		int radius_voxels = 4;
		int x, y, z;

		voxelPos[0] = ( RI.vieworg[0] - vxgi.gridMins[0] ) / gridSize[0] * VXGI_GRID_SIZE;
		voxelPos[1] = ( RI.vieworg[1] - vxgi.gridMins[1] ) / gridSize[1] * VXGI_GRID_SIZE;
		voxelPos[2] = ( RI.vieworg[2] - vxgi.gridMins[2] ) / gridSize[2] * VXGI_GRID_SIZE;

		vx = (int)voxelPos[0];
		vy = (int)voxelPos[1];
		vz = (int)voxelPos[2];

		if( vx >= 0 && vx < VXGI_GRID_SIZE &&
		    vy >= 0 && vy < VXGI_GRID_SIZE &&
		    vz >= 0 && vz < VXGI_GRID_SIZE )
		{
			for( z = Q_max( 0, vz - radius_voxels ); z < Q_min( VXGI_GRID_SIZE, vz + radius_voxels ); z++ )
			{
				for( y = Q_max( 0, vy - radius_voxels ); y < Q_min( VXGI_GRID_SIZE, vy + radius_voxels ); y++ )
				{
					for( x = Q_max( 0, vx - radius_voxels ); x < Q_min( VXGI_GRID_SIZE, vx + radius_voxels ); x++ )
					{
						float dist = sqrt( (float)((x-vx)*(x-vx) + (y-vy)*(y-vy) + (z-vz)*(z-vz)) );
						float atten = 1.0f - ( dist / radius_voxels );
						int idx;

						if( atten <= 0.0f )
							continue;

						idx = ( z * VXGI_GRID_SIZE * VXGI_GRID_SIZE + y * VXGI_GRID_SIZE + x ) * 4;

						voxelData[idx + 0] = (byte)( 255 * atten );
						voxelData[idx + 1] = (byte)( 200 * atten );
						voxelData[idx + 2] = (byte)( 150 * atten );
						voxelData[idx + 3] = 255;
					}
				}
			}
		}
	}

	// Inject dynamic lights
	for( i = 0; i < MAX_DLIGHTS; i++ )
	{
		dlight_t *dl = &tr.dlights[i];
		vec3_t voxelPos;
		int vx, vy, vz;
		int radius_voxels;
		int x, y, z;

		if( dl->die < gp_cl->time || !dl->radius )
			continue;

		// Convert world position to voxel coordinates
		voxelPos[0] = ( dl->origin[0] - vxgi.gridMins[0] ) / gridSize[0] * VXGI_GRID_SIZE;
		voxelPos[1] = ( dl->origin[1] - vxgi.gridMins[1] ) / gridSize[1] * VXGI_GRID_SIZE;
		voxelPos[2] = ( dl->origin[2] - vxgi.gridMins[2] ) / gridSize[2] * VXGI_GRID_SIZE;

		vx = (int)voxelPos[0];
		vy = (int)voxelPos[1];
		vz = (int)voxelPos[2];

		// Skip if outside grid
		if( vx < 0 || vx >= VXGI_GRID_SIZE ||
		    vy < 0 || vy >= VXGI_GRID_SIZE ||
		    vz < 0 || vz >= VXGI_GRID_SIZE )
			continue;

		// Radius in voxels
		radius_voxels = (int)( dl->radius / vxgi.voxelSize ) + 1;
		radius_voxels = Q_min( radius_voxels, 8 ); // Cap to prevent huge fills

		// Fill voxels within radius
		for( z = Q_max( 0, vz - radius_voxels ); z < Q_min( VXGI_GRID_SIZE, vz + radius_voxels ); z++ )
		{
			for( y = Q_max( 0, vy - radius_voxels ); y < Q_min( VXGI_GRID_SIZE, vy + radius_voxels ); y++ )
			{
				for( x = Q_max( 0, vx - radius_voxels ); x < Q_min( VXGI_GRID_SIZE, vx + radius_voxels ); x++ )
				{
					float dist = sqrt( (float)((x-vx)*(x-vx) + (y-vy)*(y-vy) + (z-vz)*(z-vz)) );
					float atten = 1.0f - ( dist * vxgi.voxelSize / dl->radius );
					int idx;
					byte r, g, b;

					if( atten <= 0.0f )
						continue;

					atten = Q_min( atten, 1.0f );
					idx = ( z * VXGI_GRID_SIZE * VXGI_GRID_SIZE + y * VXGI_GRID_SIZE + x ) * 4;

					// Add light contribution (clamped)
					r = (byte)Q_min( 255, voxelData[idx + 0] + (int)( dl->color.r * atten ) );
					g = (byte)Q_min( 255, voxelData[idx + 1] + (int)( dl->color.g * atten ) );
					b = (byte)Q_min( 255, voxelData[idx + 2] + (int)( dl->color.b * atten ) );

					voxelData[idx + 0] = r;
					voxelData[idx + 1] = g;
					voxelData[idx + 2] = b;
					voxelData[idx + 3] = 255; // Alpha = 1 for occupied
				}
			}
		}
	}

	// Inject entity lights
	for( i = 0; i < MAX_ELIGHTS; i++ )
	{
		dlight_t *el = &tr.elights[i];
		vec3_t voxelPos;
		int vx, vy, vz;
		int radius_voxels;
		int x, y, z;

		if( el->die < gp_cl->time || el->radius <= 0.0f )
			continue;

		// Convert world position to voxel coordinates
		voxelPos[0] = ( el->origin[0] - vxgi.gridMins[0] ) / gridSize[0] * VXGI_GRID_SIZE;
		voxelPos[1] = ( el->origin[1] - vxgi.gridMins[1] ) / gridSize[1] * VXGI_GRID_SIZE;
		voxelPos[2] = ( el->origin[2] - vxgi.gridMins[2] ) / gridSize[2] * VXGI_GRID_SIZE;

		vx = (int)voxelPos[0];
		vy = (int)voxelPos[1];
		vz = (int)voxelPos[2];

		if( vx < 0 || vx >= VXGI_GRID_SIZE ||
		    vy < 0 || vy >= VXGI_GRID_SIZE ||
		    vz < 0 || vz >= VXGI_GRID_SIZE )
			continue;

		radius_voxels = (int)( el->radius / vxgi.voxelSize ) + 1;
		radius_voxels = Q_min( radius_voxels, 8 );

		for( z = Q_max( 0, vz - radius_voxels ); z < Q_min( VXGI_GRID_SIZE, vz + radius_voxels ); z++ )
		{
			for( y = Q_max( 0, vy - radius_voxels ); y < Q_min( VXGI_GRID_SIZE, vy + radius_voxels ); y++ )
			{
				for( x = Q_max( 0, vx - radius_voxels ); x < Q_min( VXGI_GRID_SIZE, vx + radius_voxels ); x++ )
				{
					float dist = sqrt( (float)((x-vx)*(x-vx) + (y-vy)*(y-vy) + (z-vz)*(z-vz)) );
					float atten = 1.0f - ( dist * vxgi.voxelSize / el->radius );
					int idx;
					byte r, g, b;

					if( atten <= 0.0f )
						continue;

					atten = Q_min( atten, 1.0f );
					idx = ( z * VXGI_GRID_SIZE * VXGI_GRID_SIZE + y * VXGI_GRID_SIZE + x ) * 4;

					r = (byte)Q_min( 255, voxelData[idx + 0] + (int)( el->color.r * atten ) );
					g = (byte)Q_min( 255, voxelData[idx + 1] + (int)( el->color.g * atten ) );
					b = (byte)Q_min( 255, voxelData[idx + 2] + (int)( el->color.b * atten ) );

					voxelData[idx + 0] = r;
					voxelData[idx + 1] = g;
					voxelData[idx + 2] = b;
					voxelData[idx + 3] = 255;
				}
			}
		}
	}

	// Inject static lights from BSP
	for( i = 0; i < vxgi.numLights; i++ )
	{
		vxgi_light_t *light = &vxgi.lights[i];
		vec3_t voxelPos;
		int vx, vy, vz;
		int radius_voxels;
		int x, y, z;
		float lightRadius;

		if( !light->valid )
			continue;

		// Convert world position to voxel coordinates
		voxelPos[0] = ( light->origin[0] - vxgi.gridMins[0] ) / gridSize[0] * VXGI_GRID_SIZE;
		voxelPos[1] = ( light->origin[1] - vxgi.gridMins[1] ) / gridSize[1] * VXGI_GRID_SIZE;
		voxelPos[2] = ( light->origin[2] - vxgi.gridMins[2] ) / gridSize[2] * VXGI_GRID_SIZE;

		vx = (int)voxelPos[0];
		vy = (int)voxelPos[1];
		vz = (int)voxelPos[2];

		if( vx < 0 || vx >= VXGI_GRID_SIZE ||
		    vy < 0 || vy >= VXGI_GRID_SIZE ||
		    vz < 0 || vz >= VXGI_GRID_SIZE )
			continue;

		// Light radius in world units (intensity is typically 0-255 range, scale up)
		lightRadius = light->intensity * 1.5f;
		radius_voxels = (int)( lightRadius / vxgi.voxelSize ) + 1;
		radius_voxels = Q_min( radius_voxels, 12 ); // Allow slightly larger radius for static lights

		for( z = Q_max( 0, vz - radius_voxels ); z < Q_min( VXGI_GRID_SIZE, vz + radius_voxels ); z++ )
		{
			for( y = Q_max( 0, vy - radius_voxels ); y < Q_min( VXGI_GRID_SIZE, vy + radius_voxels ); y++ )
			{
				for( x = Q_max( 0, vx - radius_voxels ); x < Q_min( VXGI_GRID_SIZE, vx + radius_voxels ); x++ )
				{
					float dist = sqrt( (float)((x-vx)*(x-vx) + (y-vy)*(y-vy) + (z-vz)*(z-vz)) );
					float atten = 1.0f - ( dist * vxgi.voxelSize / lightRadius );
					int idx;
					byte r, g, b;

					if( atten <= 0.0f )
						continue;

					atten = Q_min( atten, 1.0f );
					idx = ( z * VXGI_GRID_SIZE * VXGI_GRID_SIZE + y * VXGI_GRID_SIZE + x ) * 4;

					// Add light contribution with color
					r = (byte)Q_min( 255, voxelData[idx + 0] + (int)( light->color[0] * 255.0f * atten ) );
					g = (byte)Q_min( 255, voxelData[idx + 1] + (int)( light->color[1] * 255.0f * atten ) );
					b = (byte)Q_min( 255, voxelData[idx + 2] + (int)( light->color[2] * 255.0f * atten ) );

					voxelData[idx + 0] = r;
					voxelData[idx + 1] = g;
					voxelData[idx + 2] = b;
					voxelData[idx + 3] = 255;
				}
			}
		}
	}

	// Upload to texture
	pglBindTexture( GL_TEXTURE_3D, vxgi.voxelTex );
	pglTexSubImage3D( GL_TEXTURE_3D, 0, 0, 0, 0,
	                  VXGI_GRID_SIZE, VXGI_GRID_SIZE, VXGI_GRID_SIZE,
	                  GL_RGBA, GL_UNSIGNED_BYTE, voxelData );

	pglBindTexture( GL_TEXTURE_3D, 0 );

	Mem_Free( voxelData );
}

/*
================
R_VXGIUpdate

Called once per frame to update the voxel grid
================
*/
void R_VXGIUpdate( void )
{
	static model_t *lastWorldModel = NULL;

	if( !R_VXGIActive() )
		return;

	// Parse lights when world model changes (new map loaded)
	if( WORLDMODEL != lastWorldModel )
	{
		lastWorldModel = WORLDMODEL;
		vxgi.lightsParsed = false;
	}

	if( !vxgi.lightsParsed )
		R_VXGIParseLights();

	// Compute grid bounds from world
	R_VXGIComputeBounds();

	// Clear voxels
	R_VXGIClearVoxels();

	// Inject lights
	R_VXGIInjectLights();
}

/*
================
R_VXGIGetVoxelTexture
================
*/
GLuint R_VXGIGetVoxelTexture( void )
{
	return vxgi.voxelTex;
}

/*
================
R_VXGIGetGridMins
================
*/
const float *R_VXGIGetGridMins( void )
{
	return vxgi.gridMins;
}

/*
================
R_VXGIGetGridMaxs
================
*/
const float *R_VXGIGetGridMaxs( void )
{
	return vxgi.gridMaxs;
}

/*
================
R_VXGIGetGridSize
================
*/
int R_VXGIGetGridSize( void )
{
	return VXGI_GRID_SIZE;
}

/*
================
R_VXGIGetVoxelSize
================
*/
float R_VXGIGetVoxelSize( void )
{
	return vxgi.voxelSize;
}

/*
================
R_VXGIGetIntensity
================
*/
float R_VXGIGetIntensity( void )
{
	return gl_vxgi_intensity ? gl_vxgi_intensity->value : 1.0f;
}

/*
================
R_VXGIGetDebugMode
================
*/
int R_VXGIGetDebugMode( void )
{
	return gl_vxgi_debug ? (int)gl_vxgi_debug->value : 0;
}
