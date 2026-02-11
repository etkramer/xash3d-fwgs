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

// VXGI cvars
static cvar_t *gl_vxgi;
static cvar_t *gl_vxgi_intensity;
static cvar_t *gl_vxgi_debug;

// VXGI state
static struct
{
	GLuint voxelTex;           // 3D RGBA texture for radiance
	vec3_t gridMins;           // World-space bounds min
	vec3_t gridMaxs;           // World-space bounds max
	float voxelSize;           // Size of one voxel in world units
	qboolean initialized;
	qboolean available;
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
	vec3_t size;
	float maxExtent;

	if( !WORLDMODEL )
		return;

	// Get world bounds
	VectorCopy( WORLDMODEL->mins, vxgi.gridMins );
	VectorCopy( WORLDMODEL->maxs, vxgi.gridMaxs );

	// Compute voxel size
	VectorSubtract( vxgi.gridMaxs, vxgi.gridMins, size );
	maxExtent = Q_max( size[0], Q_max( size[1], size[2] ) );
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
R_VXGIInjectLights

Injects dynamic lights into the voxel grid
================
*/
static void R_VXGIInjectLights( void )
{
	int i;
	byte *voxelData;
	vec3_t gridSize;
	int numLightsInjected = 0;

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
			numLightsInjected++;
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
	if( !R_VXGIActive() )
		return;

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
