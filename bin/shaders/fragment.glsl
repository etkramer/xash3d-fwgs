#if ATTR_TEXCOORD0
uniform sampler2D uTex0;
#endif
#if ATTR_TEXCOORD1
uniform sampler2D uTex1;
#endif
#if FEAT_ALPHA_TEST
uniform float uAlphaTest;
#endif
#if FEAT_FOG
uniform vec4 uFog;
#endif
uniform vec4 uColor;

#if ATTR_COLOR
in vec4 vColor;
#endif
#if ATTR_TEXCOORD0
in vec2 vTexCoord0;
#endif
#if ATTR_TEXCOORD1
in vec2 vTexCoord1;
#endif

#if FEAT_GBUFFER
// G-buffer outputs
layout(location = 0) out vec4 oAlbedo;
layout(location = 1) out vec4 oLightmap;
#else
out vec4 oFragColor;
#endif

void main()
{
#if ATTR_COLOR
	vec4 c = vColor;
#else
	vec4 c = uColor;
#endif
#if ATTR_TEXCOORD0
	c *= texture(uTex0, vTexCoord0);
#endif

#if FEAT_ALPHA_TEST
	if( c.a <= uAlphaTest )
		discard;
#endif

#if FEAT_GBUFFER
	// G-buffer mode: output albedo and lightmap separately
	#if ATTR_TEXCOORD1
	vec4 lightmap = texture(uTex1, vTexCoord1);
	#else
	vec4 lightmap = vec4(1.0);
	#endif
	oAlbedo = c;
	oLightmap = lightmap;
#else
	// Forward mode: combine albedo and lightmap
	#if ATTR_TEXCOORD1
	c *= texture(uTex1, vTexCoord1);
	#endif
	#if FEAT_FOG
	float fogDist = gl_FragCoord.z / gl_FragCoord.w;
	float fogRate = clamp(exp(-uFog.w * fogDist), 0.0, 1.0);
	c.rgb = mix(uFog.rgb, c.rgb, fogRate);
	#endif
	oFragColor = c;
#endif
}
