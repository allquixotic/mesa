/* SPDX-License-Identifier: MIT */
#version 450
layout(set=0,binding=0,std430) buffer Data { uint value[]; } data;
layout(location=0) out vec4 color;
void main(){atomicAdd(data.value[6],1);color=vec4(.25,.5,.75,1);}
