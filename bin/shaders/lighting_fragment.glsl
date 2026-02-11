#version 410 core

// G-buffer samplers
uniform sampler2D uAlbedo;
uniform sampler2D uNormal;
uniform sampler2D uLightmap;
uniform sampler2D uDepth;

// VXGI uniforms
uniform sampler3D uVoxelTex;
uniform vec3 uGridMin;
uniform vec3 uGridMax;
uniform float uGridSize;
uniform float uVoxelSize;
uniform mat4 uInvViewProj;
uniform vec3 uCameraPos;
uniform float uVXGIIntensity;
uniform int uVXGIEnabled;

// Debug mode: 0 = normal rendering, 1 = show g-buffer debug view, 2 = indirect only, 3 = voxels
uniform int uDebugMode;

in vec2 vTexCoord;

out vec4 oFragColor;

// Reconstruct world position from depth
vec3 ReconstructWorldPos( vec2 uv, float depth )
{
	vec4 clipPos = vec4( uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0 );
	vec4 worldPos = clipPos * uInvViewProj;  // Row-vector multiplication for row-major matrix
	return worldPos.xyz / worldPos.w;
}

// Convert world position to voxel UV [0,1]
vec3 WorldToVoxelUV( vec3 worldPos )
{
	return ( worldPos - uGridMin ) / ( uGridMax - uGridMin );
}

// Cone trace through voxel grid (simplified, no mipmaps)
vec4 ConeTrace( vec3 origin, vec3 direction, float coneAngle )
{
	vec3 color = vec3( 0.0 );
	float alpha = 0.0;

	float startDist = uVoxelSize * 2.0; // Start offset to avoid self-occlusion
	float dist = startDist;
	float maxDist = 1000.0;
	float stepSize = uVoxelSize * 1.5;

	for( int i = 0; i < 32 && dist < maxDist && alpha < 0.95; i++ )
	{
		vec3 samplePos = origin + direction * dist;
		vec3 voxelUV = WorldToVoxelUV( samplePos );

		// Check bounds
		if( any( lessThan( voxelUV, vec3( 0.0 ) ) ) || any( greaterThan( voxelUV, vec3( 1.0 ) ) ) )
			break;

		vec4 voxelSample = texture( uVoxelTex, voxelUV );

		// Front-to-back compositing
		float sampleAlpha = 1.0 - alpha;
		color += sampleAlpha * voxelSample.a * voxelSample.rgb;
		alpha += sampleAlpha * voxelSample.a;

		// Step forward with increasing step size
		dist += stepSize;
		stepSize *= 1.1;
	}

	return vec4( color, alpha );
}

// Diffuse cone tracing using 6 cones in hemisphere
vec3 IndirectDiffuse( vec3 worldPos, vec3 normal )
{
	vec3 indirect = vec3( 0.0 );
	float coneAngle = 0.577; // ~33 degrees

	// Build tangent space
	vec3 up = abs( normal.y ) < 0.999 ? vec3( 0.0, 1.0, 0.0 ) : vec3( 1.0, 0.0, 0.0 );
	vec3 tangent = normalize( cross( up, normal ) );
	vec3 bitangent = cross( normal, tangent );

	// Six cone directions in hemisphere
	vec3 coneDirections[6];
	coneDirections[0] = normal;
	coneDirections[1] = normalize( normal + tangent );
	coneDirections[2] = normalize( normal - tangent );
	coneDirections[3] = normalize( normal + bitangent );
	coneDirections[4] = normalize( normal - bitangent );
	coneDirections[5] = normalize( normal + tangent + bitangent );

	// Weights for each cone
	float coneWeights[6];
	coneWeights[0] = 0.25;
	coneWeights[1] = 0.15;
	coneWeights[2] = 0.15;
	coneWeights[3] = 0.15;
	coneWeights[4] = 0.15;
	coneWeights[5] = 0.15;

	// Trace each cone
	for( int i = 0; i < 6; i++ )
	{
		indirect += coneWeights[i] * ConeTrace( worldPos, coneDirections[i], coneAngle ).rgb;
	}

	return indirect;
}

void main()
{
	// Sample g-buffer
	vec4 albedo = texture( uAlbedo, vTexCoord );
	vec4 normalSample = texture( uNormal, vTexCoord );
	vec4 lightmap = texture( uLightmap, vTexCoord );
	float depth = texture( uDepth, vTexCoord ).r;

	// Unpack normal from [0,1] to [-1,1]
	vec3 normal = normalize( normalSample.rgb * 2.0 - 1.0 );

	// Direct lighting from lightmap
	vec3 directLight = lightmap.rgb;
	directLight = vec3(0);

	// Indirect lighting via VXGI cone tracing
	vec3 indirectLight = vec3( 0.0 );

	if( uVXGIEnabled != 0 && depth < 1.0 )
	{
		vec3 worldPos = ReconstructWorldPos( vTexCoord, depth );
		indirectLight = IndirectDiffuse( worldPos, normal ) * uVXGIIntensity;
	}

	// Combine direct and indirect lighting
	vec3 totalLight = directLight + indirectLight;
	vec4 finalColor = vec4( albedo.rgb * totalLight, albedo.a );

	if( uDebugMode == 1 )
	{
		// Debug view: show different g-buffer channels in quadrants
		// Top-left: Albedo
		// Top-right: Normals
		// Bottom-left: Lightmap
		// Bottom-right: Final composed result

		if( vTexCoord.x < 0.5 && vTexCoord.y >= 0.5 )
		{
			// Top-left: Albedo
			oFragColor = albedo;
		}
		else if( vTexCoord.x >= 0.5 && vTexCoord.y >= 0.5 )
		{
			// Top-right: Normals (remap from [-1,1] to [0,1] for visualization)
			oFragColor = normalSample;
		}
		else if( vTexCoord.x < 0.5 && vTexCoord.y < 0.5 )
		{
			// Bottom-left: Lightmap
			oFragColor = lightmap;
		}
		else
		{
			// Bottom-right: Final composed result
			oFragColor = finalColor;
		}
	}
	else if( uDebugMode == 2 )
	{
		// Show only indirect lighting
		oFragColor = vec4( indirectLight, 1.0 );
	}
	else if( uDebugMode == 3 )
	{
		// Show voxel UV coordinates as colors (RGB = XYZ)
		if( depth < 1.0 )
		{
			vec3 worldPos = ReconstructWorldPos( vTexCoord, depth );
			vec3 voxelUV = WorldToVoxelUV( worldPos );
			// Clamp to valid range for visualization
			oFragColor = vec4( clamp( voxelUV, 0.0, 1.0 ), 1.0 );
		}
		else
		{
			oFragColor = vec4( 1.0, 0.0, 1.0, 1.0 ); // Magenta = sky/no depth
		}
	}
	else if( uDebugMode == 4 )
	{
		// Show raw voxel texture sample
		if( depth < 1.0 )
		{
			vec3 worldPos = ReconstructWorldPos( vTexCoord, depth );
			vec3 voxelUV = WorldToVoxelUV( worldPos );
			oFragColor = texture( uVoxelTex, voxelUV );
		}
		else
		{
			oFragColor = vec4( 0.0 );
		}
	}
	else if( uDebugMode == 5 )
	{
		// Show depth buffer (amplified to see small differences)
		// Near objects should be dark, far objects bright
		float amplifiedDepth = 1.0 - pow( 1.0 - depth, 100.0 ); // Amplify differences near 1.0
		oFragColor = vec4( vec3( amplifiedDepth ), 1.0 );
	}
	else if( uDebugMode == 7 )
	{
		// Show raw unadjusted depth
		oFragColor = vec4( depth, depth, depth, 1.0 );
	}
	else if( uDebugMode == 8 )
	{
		// Show reconstructed world position as colors (scaled down)
		if( depth < 1.0 )
		{
			vec3 worldPos = ReconstructWorldPos( vTexCoord, depth );
			// Scale world position to visible range (assuming positions in thousands)
			vec3 scaledPos = fract( worldPos / 1000.0 );
			oFragColor = vec4( scaledPos, 1.0 );
		}
		else
		{
			oFragColor = vec4( 1.0, 0.0, 1.0, 1.0 );
		}
	}
	else if( uDebugMode == 6 )
	{
		// Show if VXGI is enabled (solid green = yes, red = no)
		if( uVXGIEnabled != 0 )
			oFragColor = vec4( 0.0, 1.0, 0.0, 1.0 );
		else
			oFragColor = vec4( 1.0, 0.0, 0.0, 1.0 );
	}
	else
	{
		// Normal rendering
		oFragColor = finalColor;
	}
}
