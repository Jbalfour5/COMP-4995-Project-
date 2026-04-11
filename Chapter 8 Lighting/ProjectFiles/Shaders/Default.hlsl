//***************************************************************************************
// Default.hlsl by Frank Luna (C) 2015 All Rights Reserved.
// Modified by Jayden Bergstrome 2026-04-10
// Default shader, currently supports lighting.
//***************************************************************************************

// Defaults for number of lights
#ifndef NUM_DIR_LIGHTS
    #define NUM_DIR_LIGHTS 3
#endif

#ifndef NUM_POINT_LIGHTS
    #define NUM_POINT_LIGHTS 0
#endif

#ifndef NUM_SPOT_LIGHTS
    #define NUM_SPOT_LIGHTS 2
#endif

// Include structures and functions for lighting
#include "LightingUtil.hlsl"

// Texture and sampler registers
Texture2D    gDiffuseMap : register(t0);
SamplerState gsamLinearWrap  : register(s0);

// Constant data that varies per object
cbuffer cbPerObject : register(b0)
{
    float4x4 gWorld;
    float4x4 gTexTransform;
};

// Constant data that varies per material
cbuffer cbMaterial : register(b1)
{
    float4 gDiffuseAlbedo;
    float3 gFresnelR0;
    float  gRoughness;
    float4x4 gMatTransform;
};

// Constant data that varies per pass
cbuffer cbPass : register(b2)
{
    float4x4 gView;
    float4x4 gInvView;
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float3 gEyePosW;
    float cbPerObjectPad1;
    float2 gRenderTargetSize;
    float2 gInvRenderTargetSize;
    float gNearZ;
    float gFarZ;
    float gTotalTime;
    float gDeltaTime;
    float4 gAmbientLight;

    // Indices 0 to NUM_DIR_LIGHTS are directional lights
    // Indices NUM_DIR_LIGHTS to NUM_DIR_LIGHTS NUM_POINT_LIGHTS are point lights
    // Indices NUM_DIR_LIGHTS NUM_POINT_LIGHTS to NUM_DIR_LIGHTS NUM_POINT_LIGHT NUM_SPOT_LIGHTS are spot lights
    Light gLights[MaxLights];
};
 
struct VertexIn
{
    float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC    : TEXCOORD;
};

struct VertexOut
{
    float4 PosH    : SV_POSITION;
    float3 PosW    : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC    : TEXCOORD;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout = (VertexOut)0.0f;
    
    // Transform to world space
    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW = posW.xyz;

    // Assumes nonuniform scaling
    vout.NormalW = mul(vin.NormalL, (float3x3)gWorld);

    // Transform to homogeneous clip space
    vout.PosH = mul(posW, gViewProj);

    // Output vertex attributes for interpolation across triangle
    float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform);
    vout.TexC = mul(texC, gMatTransform).xy;

    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    // Sample texture
    float4 diffuseAlbedo = gDiffuseMap.Sample(gsamLinearWrap, pin.TexC) * gDiffuseAlbedo;

    // Renormalize normal
    pin.NormalW = normalize(pin.NormalW);

    // Vector to eye
    float3 toEyeW = normalize(gEyePosW - pin.PosW);

    // Indirect lighting
    float4 ambient = gAmbientLight*diffuseAlbedo;

    const float shininess = 1.0f - gRoughness;
    Material mat = { diffuseAlbedo, gFresnelR0, shininess };
    float3 shadowFactor = 1.0f;
    
    // Compute direct lighting
    float4 directLight = ComputeLighting(gLights, mat, pin.PosW, 
        pin.NormalW, toEyeW, shadowFactor);

    float4 litColor = ambient + directLight;

    // Take alpha from diffuse material and texture
    litColor.a = gDiffuseAlbedo.a * diffuseAlbedo.a;

    return litColor;
}

[maxvertexcount(3)]
void MyGS(triangle VertexOut input[3], inout TriangleStream<VertexOut> TriStream)
{
    float offset = 0.1f + 0.5f * sin(gTotalTime * 2.0f);

    for (int i = 0; i < 3; ++i)
    {
        VertexOut v = input[i];

        v.PosW += v.NormalW * offset;

        v.PosH = mul(float4(v.PosW, 1.0f), gViewProj);
        
        // Pass texture coordinates through geometry shader
        v.TexC = input[i].TexC;

        TriStream.Append(v);
    }

    TriStream.RestartStrip();
}