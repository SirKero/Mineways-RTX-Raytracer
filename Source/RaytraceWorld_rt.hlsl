#pragma pack_matrix(row_major)

#include <donut/shaders/material_cb.h>
#include <donut/shaders/binding_helpers.hlsli>
#include <donut/shaders/brdf.hlsli>
#include "sharedShaderData.h"

// ---[ Structures ]---

struct HitInfo
{
    float3 normal : PAYLOAD_NORMAL;
    int matID : PAYLOAD_MATID;
    
    float2 uv : PAYLOAD_UV;
    float hitT : PAYLOAD_HITT;
    int hitType : PAYLOAD_HITTYPE;
   
};

struct Attributes
{
    float2 uv;
};

struct AttributesAABB
{
    float2 uv;
    uint hitSide; //Normal Orientation: 0:-x ; 1:+x, 2:-z ; 3:+z, 4:-y ; 5:+y
};

// ---[ Constant Buffers ]---
ConstantBuffer<ConstBuffer> g_CB : register(b0);

VK_BINDING(0, 1) Texture2D t_BindlessTextures[] : register(t0, space1);

// ---[ Resources ]---
RWTexture2D<float4> RTOutput : register(u0);
RaytracingAccelerationStructure SceneBVH : register(t0);
StructuredBuffer<uint> g_IndexData : register(t1);
StructuredBuffer<VertexData> g_VertexData : register(t2);
StructuredBuffer<float2> g_AABBData : register(t3);
StructuredBuffer<uint> g_TriMaterialID : register(t4);
StructuredBuffer<AABBMaterials> g_AABBMaterialID : register(t5);
StructuredBuffer<MaterialConstants> g_Material : register(t6);

SamplerState s_MaterialSampler : register(s0);

// ---[ Constants ]---
static const int kHitTypeMiss = 0;
static const int kHitTypeTriangle = 1;
static const int kHitTypeAABB = 2;
static const float k_DielectricSpecular = 0.04;
static const float3 kEnviromentColor = float3(0.68, 0.85, 0.9); //Light Blue

// ---[ Functions ]---
RayDesc SetupPrimaryRay(uint2 pixelPosition, PlanarViewConstants view)
{
    float2 uv = (float2(pixelPosition) + 0.5) * view.viewportSizeInv;
    float4 clipPos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1);
    float4 worldPos = mul(clipPos, view.matClipToWorld);
    worldPos.xyz /= worldPos.w;

    RayDesc ray;
    ray.Origin = view.cameraDirectionOrPosition.xyz;
    ray.Direction = normalize(worldPos.xyz - ray.Origin);
    ray.TMin = g_CB.cameraNear;
    ray.TMax = g_CB.cameraFar;
    return ray;
}

AABB ReadAABBFromDataBuffer(uint primIndex)
{
    AABB aabb;
    aabb.min.xy = g_AABBData[primIndex];
    float2 data = g_AABBData[primIndex + 1];
    aabb.min.z = data.x;
    aabb.max.x = data.y;
    aabb.max.yz = g_AABBData[primIndex + 2];
    return aabb;
}

float MaxElement(float3 v)
{
    return max(max(v.x, v.y), v.z);
}

/* Ray box itersection from "A Ray-Box Intersection Algorithm and Efficient Dynamic Voxel Rendering" by Majercik et al. (2018)
   https://jcgt.org/published/0007/03/04/ Listing 5
   canStartInBox and oriented is omitted. canStartInBox = true, oriented = false
*/
bool RayBoxIntersection(AABB aabb, float3 rayOrigin, float3 rayDirection, out float distance, out float3 normal)
{
    const float3 invRayDir = 1.0 / rayDirection;
    const float3 boxCenter = (aabb.min + aabb.max) / 2.0;
    const float3 boxRadius = boxCenter - aabb.min;  //Component wise radius
    const float3 invBoxRadius = 1.0 / boxRadius;
    
    rayOrigin -= boxCenter;
    float3 winding = MaxElement(abs(rayOrigin) * invBoxRadius) < 1.0 ? -1.0 : 1.0;
    float3 sgn = -sign(rayDirection);
    //Distance to plane
    float3 d = boxRadius * winding * sgn - rayOrigin;
    d *= invRayDir;
    
#define TEST(U, VW) (d.U >= 0.0) && all(abs(rayOrigin.VW + rayDirection.VW * d.U) < boxRadius.VW)
    bool3 test = bool3(TEST(x, yz), TEST(y, zx), TEST(z, xy));
    sgn = test.x ? float3(sgn.x, 0, 0) : (test.y ? float3(0, sgn.y, 0) : float3(0, 0, test.z ? sgn.z : 0));
#undef TEST
    
    distance = (sgn.x != 0) ? d.x : ((sgn.y != 0) ? d.y : d.z);
    normal = sgn;
    
    return (sgn.x != 0) || (sgn.y != 0) || (sgn.z != 0);
}

AttributesAABB GetAABBAttributes(AABB aabb, float3 hitPos, float3 normal)
{
    AttributesAABB attribs;
    bool negative = any(normal < 0);
    normal = abs(normal);
        
    //TODO better method without if/else?
    if (normal.x > 0)
    {
        attribs.uv = (hitPos.zy - aabb.min.zy) / (aabb.max.zy - aabb.min.zy);
        attribs.uv = attribs.uv = negative ? float2(1.0, 1.0) - attribs.uv : float2(attribs.uv.x, 1.0 - attribs.uv.y);
        attribs.hitSide = negative ? 0 : 1;
    }
    else if (normal.z > 0)
    {
        attribs.uv = (hitPos.xy - aabb.min.xy) / (aabb.max.xy - aabb.min.xy);
        attribs.uv = negative ? float2(1.0, 1.0) - attribs.uv : float2(attribs.uv.x, 1.0 - attribs.uv.y);
        attribs.hitSide = negative ? 2 : 3;
    }
    else
    {
        attribs.uv = float2(1.0, 1.0) - ((hitPos.xz - aabb.min.xz) / (aabb.max.xz - aabb.min.xz));
        attribs.hitSide = negative ? 4 : 5;
    }
    
    return attribs;
}

void GetTriangleVertices(uint primitiveIndex,out Vertex vertices[3]){
    uint indicesBufferIndex = primitiveIndex * 3;
    uint3 indices = uint3(g_IndexData[indicesBufferIndex], g_IndexData[indicesBufferIndex + 1], g_IndexData[indicesBufferIndex + 2]);
        
    [unroll]
    for (uint i = 0; i < 3; i++)
    {
        vertices[i] = getVertex(g_VertexData[indices[i]]);
    }
}

int GetAABBMaterialID(AABBMaterials aabbMat, int side)
{
    [branch]
    switch (side)
    {
        case 0:
            return aabbMat.negXMatID;
        case 1:
            return aabbMat.posXMatID;
        case 2:
            return aabbMat.negZMatID;
        case 3:
            return aabbMat.posZMatID;
        case 4:
            return aabbMat.negYMatID;
        default:
            return aabbMat.posYMatID;
    }
}

float3 GetAABBNormalFromHitSide(int hitSide)
{
    [branch]
    switch (hitSide)
    {
        case 0: //-X
            return float3(-1, 0 , 0);
        case 1: //+X
            return float3(1, 0 , 0);
        case 2: //-Z
            return float3(0, 0 , -1);
        case 3: //+Z
            return float3(0, 0 , 1);
        case 4: //-Y
            return float3(0, -1 , 0);
        default: //+Y
            return float3(0, 1 , 0);
    }
}

//Alpha Test for Triangles. Returns true if an opaque surface was hit
bool TriangleAlphaTest(uint primitiveIndex, float2 triBarycentrics)
{
    uint triMaterialID = g_TriMaterialID[primitiveIndex];
    MaterialConstants material = g_Material[triMaterialID];
    
    bool opaqueHit = true;
    
    //Perform alpha test
    if (material.domain > 0)
    {
        float3 barycentrics = float3((1.0f - triBarycentrics.x - triBarycentrics.y), triBarycentrics.x, triBarycentrics.y);
        Vertex verts[3];
        GetTriangleVertices(primitiveIndex,verts);
        
        float2 uv = verts[0].uv * barycentrics.x + verts[1].uv * barycentrics.y + verts[2].uv * barycentrics.z;
    
        Texture2D diffuseTexture = t_BindlessTextures[NonUniformResourceIndex(material.baseOrDiffuseTextureIndex)];
        float4 baseColor = diffuseTexture.SampleLevel(s_MaterialSampler, uv, 0);
        
        opaqueHit = baseColor.a >= material.alphaCutoff;
    }
    
    return opaqueHit;
}

bool AABBAlphaTest(uint primitiveIndex, AttributesAABB attribs)
{
    AABBMaterials aabbMaterialIDs = g_AABBMaterialID[primitiveIndex];
    int materialID = GetAABBMaterialID(aabbMaterialIDs, attribs.hitSide);
    MaterialConstants material = g_Material[materialID];
    
    bool opaqueHit = true;
    
    //Perform alpha test
    if (material.domain > 0)
    {
        Texture2D diffuseTexture = t_BindlessTextures[NonUniformResourceIndex(material.baseOrDiffuseTextureIndex)];
        float4 baseColor = diffuseTexture.SampleLevel(s_MaterialSampler, attribs.uv, 0);
        opaqueHit = baseColor.a >= material.alphaCutoff;
    }
    
    return opaqueHit;
}

void EvaluateMaterialTextures(inout MaterialConstants material, float2 uv , inout float3 normal, uint mipLevel = 0)
{
    if((material.flags & MaterialFlags_UseBaseOrDiffuseTexture) > 0)
    {
        Texture2D diffuseTexture = t_BindlessTextures[NonUniformResourceIndex(material.baseOrDiffuseTextureIndex)];
        material.baseOrDiffuseColor = diffuseTexture.SampleLevel(s_MaterialSampler, uv, mipLevel).xyz;     
    }
    
    if((material.flags & MaterialFlags_UseNormalTexture) > 0)
    {
        float3 N = normal;
        Texture2D normalTexture = t_BindlessTextures[NonUniformResourceIndex(material.normalTextureIndex)];
        float3 texNormal = normalTexture.SampleLevel(s_MaterialSampler, uv, mipLevel).xyz;
        texNormal = (texNormal * 2.0) - 1.0;
        float3 T,B;
        ConstructONB(N,T,B);
        
        normal = normalize(T * texNormal.x + B * texNormal.y + N * texNormal.z);
    }
    
    if((material.flags & MaterialFlags_UseMetalRoughOrSpecularTexture) > 0)
    {
        Texture2D metalRoughTexture = t_BindlessTextures[NonUniformResourceIndex(material.metalRoughOrSpecularTextureIndex)];
        float2 metalRough = metalRoughTexture.SampleLevel(s_MaterialSampler, uv, mipLevel).xy;
        material.metalness = metalRough.x;
        material.roughness = metalRough.y;
    }
    
    if((material.flags & MaterialFlags_UseEmissiveTexture) > 0)
    {
        Texture2D emissiveTexture = t_BindlessTextures[NonUniformResourceIndex(material.emissiveTextureIndex)];
        material.emissiveColor = emissiveTexture.SampleLevel(s_MaterialSampler, uv, mipLevel).xyz;
    }
    
    // Compute the BRDF inputs for the metal-rough model (gltf-spec is used as donut was developed with it)
    // https://github.com/KhronosGroup/glTF/tree/master/specification/2.0#metal-brdf-and-dielectric-brdf
    float3 diffuseColor = material.baseOrDiffuseColor;
    material.baseOrDiffuseColor = lerp(material.baseOrDiffuseColor * (1.0 - k_DielectricSpecular), 0.0, material.metalness);
    material.specularColor = lerp(k_DielectricSpecular,  diffuseColor, material.metalness); //F0Spectular for GGX
}

//Shadow test using ray queries. True if lit, false if shadowed
bool RayShadowTest(float3 posW, float3 faceN, float3 toLight)
{
    RayDesc shadowRay;
    shadowRay.Origin = posW + faceN * 1e-4;
    shadowRay.Direction = toLight;
    shadowRay.TMin = 1e-4;
    shadowRay.TMax = g_CB.cameraFar; //Approximate with camera far
    
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> rayQuery;
    rayQuery.TraceRayInline(SceneBVH, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, shadowRay);
    
    while(rayQuery.Proceed())
    {
        if(rayQuery.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE)
        {
            uint bufferIndex = rayQuery.CandidatePrimitiveIndex() * 3;
            AABB aabb = ReadAABBFromDataBuffer(bufferIndex);
    
            float distance = -1;
            float3 normal = float3(0,0,0);
    
            if (RayBoxIntersection(aabb, shadowRay.Origin, shadowRay.Direction, distance, normal))
            {
                float3 hitPos = shadowRay.Origin +  shadowRay.Direction * distance;
                AttributesAABB attribs = GetAABBAttributes(aabb, hitPos, normal);
                if(AABBAlphaTest(rayQuery.CandidatePrimitiveIndex(), attribs))
                {
                    rayQuery.CommitProceduralPrimitiveHit(distance);
                    rayQuery.Abort();
                    break;
                }
                //If first hit was rejected (alpha test) calculate the second hit by moving the ray inside of the box
                float oldDistance = distance;
                hitPos += shadowRay.Direction * 1e-2;
                if (RayBoxIntersection(aabb, hitPos,  shadowRay.Direction, distance, normal))
                {
                    hitPos = hitPos + shadowRay.Direction * distance;
                    attribs = GetAABBAttributes(aabb, hitPos, normal);
                    distance += oldDistance;
                    if(AABBAlphaTest(rayQuery.CandidatePrimitiveIndex(), attribs))
                    {
                        rayQuery.CommitProceduralPrimitiveHit(distance);
                        rayQuery.Abort();
                        break;
                    }
                }
            }
        }
        else if(rayQuery.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            if(rayQuery.CommittedInstanceID()!=0)
                    continue;
            if(TriangleAlphaTest(rayQuery.CandidatePrimitiveIndex(), rayQuery.CandidateTriangleBarycentrics()))
            {
                rayQuery.CommitNonOpaqueTriangleHit();
                rayQuery.Abort();
                break;
            }
        }        
    }
    
    return rayQuery.CommittedStatus() == COMMITTED_NOTHING;
}

// ---[ Miss Shader ]---

[shader("miss")]
void Miss(inout HitInfo payload : SV_RayPayload)
{
    payload.hitType = kHitTypeMiss;
}

// ---[ Any Hit Shader ]---
[shader("anyhit")]
void AnyHitTriangle(inout HitInfo payload : SV_RayPayload,
    Attributes attrib : SV_IntersectionAttributes)
{
    if(!TriangleAlphaTest(PrimitiveIndex(), attrib.uv))
        IgnoreHit();   
}

[shader("anyhit")]
void AnyHitAABB(inout HitInfo payload : SV_RayPayload,
    AttributesAABB attrib : SV_IntersectionAttributes)
{
    if(!AABBAlphaTest(PrimitiveIndex(), attrib))
        IgnoreHit();
}

// ---[ Closest Hit Shader ]---

[shader("closesthit")]
void ClosestHitTriangle(inout HitInfo payload : SV_RayPayload,
    Attributes attrib : SV_IntersectionAttributes)
{
    float3 barycentrics = float3((1.0f - attrib.uv.x - attrib.uv.y), attrib.uv.x, attrib.uv.y);
    Vertex verts[3];
    GetTriangleVertices(PrimitiveIndex(),verts);
    uint triMaterialID = g_TriMaterialID[PrimitiveIndex()];
        
    //Fill payload hit data
    payload.hitType = kHitTypeTriangle;
    payload.normal = normalize(verts[0].normal * barycentrics.x + verts[1].normal * barycentrics.y + verts[2].normal * barycentrics.z);
    payload.uv = verts[0].uv * barycentrics.x + verts[1].uv * barycentrics.y + verts[2].uv * barycentrics.z;
    payload.hitT = RayTCurrent();
    payload.matID = triMaterialID;
}

[shader("closesthit")]
void ClosestHitAABB(inout HitInfo payload : SV_RayPayload,
    AttributesAABB attrib : SV_IntersectionAttributes)
{    
    AABBMaterials aabbMaterialIDs = g_AABBMaterialID[PrimitiveIndex()];
        
    payload.hitType = kHitTypeAABB;
    payload.normal = GetAABBNormalFromHitSide(attrib.hitSide);
    payload.uv = attrib.uv;
    payload.hitT = RayTCurrent();
    payload.matID = GetAABBMaterialID(aabbMaterialIDs, attrib.hitSide);;
}

// ---[ Intersection Shader ]---
[shader("intersection")]
void IntersectionAABB()
{
    uint bufferIndex = PrimitiveIndex() * 3;
    AABB aabb = ReadAABBFromDataBuffer(bufferIndex);
    
    float distance = -1;
    float3 normal = float3(0,0,0);
    
    if (RayBoxIntersection(aabb, WorldRayOrigin(), WorldRayDirection(), distance, normal))
    {
        
        float3 hitPos = WorldRayOrigin() + WorldRayDirection() * distance;
        AttributesAABB attribs = GetAABBAttributes(aabb, hitPos, normal);
        
        if(ReportHit(distance, 0, attribs))
            return;
        
        //If first hit was rejected (alpha test) calculate the second hit by moving the ray inside of the box
        float oldDistance = distance;
        hitPos += WorldRayDirection() * 1e-2;
        if (RayBoxIntersection(aabb, hitPos, WorldRayDirection(), distance, normal))
        {
            hitPos = hitPos + WorldRayDirection() * distance;
            attribs = GetAABBAttributes(aabb, hitPos, normal);
            
            distance += oldDistance;
            ReportHit(distance, 0, attribs);
        }
    }
}

// ---[ Ray Generation Shader ]---
[shader("raygeneration")]
void RayGen()
{
    uint2 LaunchIndex = DispatchRaysIndex().xy;
    uint2 LaunchDimensions = DispatchRaysDimensions().xy;
    
    // Setup the ray
    RayDesc ray = SetupPrimaryRay(LaunchIndex, g_CB.viewConstants);

    // Trace the ray
    HitInfo payload;
    payload.hitType = kHitTypeMiss;
    payload.matID = -1;
    payload.normal = float3(0,1,0);
    payload.uv = float2(0,0);
    payload.hitT = -1.0;

    TraceRay(
    SceneBVH,       //Acceleration Structure
    RAY_FLAG_NONE,  //Ray Flags
    0xFF,           //Instance Mask
    0,              //Offset for Hit Kind
    2,              //Mulitplier for Hit Kind offset
    0,              //Miss shader index
    ray,            //Ray Description
    payload);       //Payload

    float3 outColor = float3(0, 0, 0);
    
    //Shade if something was hit
    if(payload.hitType != kHitTypeMiss)
    {
        
        MaterialConstants material = g_Material[payload.matID];
        
        //Flip normal if material is double sided and normal is backfacing
        if ((material.flags & MaterialFlags_DoubleSided) > 0){
            if(dot(payload.normal, -ray.Direction) < 0){
                payload.normal = -payload.normal;
            }
        }
        
        float3 faceN = payload.normal;
        float3 posW = ray.Origin + ray.Direction * payload.hitT;
        
        EvaluateMaterialTextures(material, payload.uv, payload.normal, 0);
        
        //Shading
        LightConstants dirLight = g_CB.directionalLightConstants;
        float3 ambient = g_CB.ambient * material.baseOrDiffuseColor;
        float3 emission = g_CB.emissiveStrength * material.emissiveColor;
        float3 reflectDirection = normalize(reflect(ray.Direction, payload.normal));
        float3 H = normalize(-ray.Direction + reflectDirection);
        float3 reflectionAmbient = Schlick_Fresnel(material.specularColor, saturate(dot(-ray.Direction,H))) * material.specularColor * kEnviromentColor;
        
        float3 diffuseRadiance = float3(0,0,0);
        float3 specularRadiance = float3(0,0,0);
        if(RayShadowTest(posW, faceN, -dirLight.direction))
        {
            diffuseRadiance = Lambert(payload.normal, dirLight.direction) * material.baseOrDiffuseColor * dirLight.intensity;
            specularRadiance = GGX_AnalyticalLights_times_NdotL(dirLight.direction, ray.Direction, payload.normal,
                material.roughness, material.specularColor, dirLight.angularSizeOrInvRange * 0.5) * dirLight.intensity;
        }
                
        outColor = emission + ambient + reflectionAmbient + diffuseRadiance + specularRadiance;
    }
    else //Set to background color
    {
        outColor = kEnviromentColor;
    }
        
    RTOutput[LaunchIndex.xy] = float4(outColor, 1.f);
}