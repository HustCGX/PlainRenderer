#version 460
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec3 inPos;

layout(push_constant) uniform MatrixBlock {
	mat4 mvp;
	mat4 model;
} translation;

layout(location = 0) out vec2 passQuadPos;
layout(location = 1) out vec3 passWorldPos;

void main(){
    passQuadPos = inPos.xy;
    passWorldPos = mat3(translation.model) * inPos;
	gl_Position = translation.mvp * vec4(inPos, 0.f);
	gl_Position.z = gl_Position.w;
}