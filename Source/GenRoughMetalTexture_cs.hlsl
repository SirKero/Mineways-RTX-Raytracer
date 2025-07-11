#include "sharedShaderData.h"

// ---[ Constant Buffers ]---
ConstantBuffer<CBMetalRoughTexGen> g_CB : register(b0);

// ---[ Resources ]---
RWTexture2D<float2> g_MetalRoughOut : register(u0);
Texture2D<float> g_RoughnessTexture : register(t0);
Texture2D<float> g_MetallicTexture : register(t1);

[numthreads(16, 16, 1)] 
void main(uint2 dTid : SV_DispatchThreadID)
{
    if(any(dTid > g_CB.texDimensions))
        return;
        
    float roughness = g_RoughnessTexture[dTid];
    float metallic = 0;
    if(g_CB.metallicValid)
        metallic = g_MetallicTexture[dTid];
    
    if(g_CB.convertShininessToRoughness)
        roughness = 1.0 - roughness;
        
    g_MetalRoughOut[dTid] = float2(metallic, roughness);
}