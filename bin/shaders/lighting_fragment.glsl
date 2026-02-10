#version 410 core

// G-buffer samplers
uniform sampler2D uAlbedo;
uniform sampler2D uLightmap;

in vec2 vTexCoord;

out vec4 oFragColor;

void main()
{
	// Sample g-buffer
	vec4 albedo = texture(uAlbedo, vTexCoord);
	vec4 lightmap = texture(uLightmap, vTexCoord);

	// Debug: show lightmap only (set to 1 to enable)
#if 0
	oFragColor = albedo;
#else
	// Combine albedo with lightmap
	oFragColor = lightmap;
#endif
}
