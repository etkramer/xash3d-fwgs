#version 410 core

// Fullscreen triangle vertex shader
// Generates a triangle that covers the entire screen using vertex ID
// No vertex buffer needed - just draw 3 vertices

out vec2 vTexCoord;

void main()
{
	// Generate fullscreen triangle vertices from vertex ID
	// Vertex 0: (-1, -1), Vertex 1: (3, -1), Vertex 2: (-1, 3)
	float x = float((gl_VertexID & 1) << 2) - 1.0;
	float y = float((gl_VertexID & 2) << 1) - 1.0;

	gl_Position = vec4(x, y, 0.0, 1.0);
	vTexCoord = vec2((x + 1.0) * 0.5, (y + 1.0) * 0.5);
}
