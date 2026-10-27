/* SPDX-License-Identifier: MIT */
#version 450
#extension GL_ARB_shader_draw_parameters : require
layout(set=0,binding=0,std430) buffer Data { uint value[]; } data;
void main() {
 atomicAdd(data.value[0],1);
 atomicAdd(data.value[1],uint(gl_DrawIDARB));
 atomicAdd(data.value[2],uint(gl_VertexIndex));
 atomicAdd(data.value[3],uint(gl_InstanceIndex));
 atomicAdd(data.value[4],uint(gl_BaseVertexARB));
 atomicAdd(data.value[5],uint(gl_BaseInstanceARB));
 gl_Position=vec4(0,0,0,1); gl_PointSize=1;
}
