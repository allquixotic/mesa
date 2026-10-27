/* SPDX-License-Identifier: MIT */
#version 450
layout(set=0,binding=0,std430) buffer Data { uint value[]; } data;
void main() { atomicAdd(data.value[0],1); gl_Position=vec4(0,0,0,1); gl_PointSize=1; }
