#version 410 core

// G-buffer samplers
uniform sampler2D uAlbedo;
uniform sampler2D uNormal;
uniform sampler2D uLightmap;

// Debug mode: 0 = normal rendering, 1 = show g-buffer debug view
uniform int uDebugMode;

in vec2 vTexCoord;

out vec4 oFragColor;

void main()
{
	// Sample g-buffer
	vec4 albedo = texture(uAlbedo, vTexCoord);
	vec4 normal = texture(uNormal, vTexCoord);
	vec4 lightmap = texture(uLightmap, vTexCoord);

	if (uDebugMode == 1)
	{
		// Debug view: show different g-buffer channels in quadrants
		// Top-left: Albedo
		// Top-right: Normals
		// Bottom-left: Lightmap
		// Bottom-right: Final composed result

		vec4 finalColor = albedo * lightmap;

		if (vTexCoord.x < 0.5 && vTexCoord.y >= 0.5)
		{
			// Top-left: Albedo
			oFragColor = albedo;
		}
		else if (vTexCoord.x >= 0.5 && vTexCoord.y >= 0.5)
		{
			// Top-right: Normals (remap from [-1,1] to [0,1] for visualization)
			oFragColor = normal;
		}
		else if (vTexCoord.x < 0.5 && vTexCoord.y < 0.5)
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
	else
	{
		// Normal rendering: combine albedo with lightmap
		oFragColor = albedo * lightmap;
	}
}
