#version 450 core

#include "globalubos.glsl"

// Vertex input.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexcoord;

// Output = input for fragment shader.
out vec3 Normal;
out vec2 Texcoord;

void main(void)
{
	gl_Position = vec4(inPosition, 1.0) * World * ViewProjection;

	// Simple pass through
	Normal = (vec4(inNormal, 0.0) * World).xyz;
	Texcoord = inTexcoord;
}  