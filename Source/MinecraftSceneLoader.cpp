#define TINYOBJLOADER_IMPLEMENTATION // define this in only *one* .cc
#define TINYOBJLOADER_USE_MAPBOX_EARCUT // Optional. define TINYOBJLOADER_USE_MAPBOX_EARCUT gives robust triangulation. Requires C++11
#include "MinecraftSceneLoader.h"
#include <nvrhi/utils.h>
#include <donut/core/log.h>
#include <limits>
#include <donut/shaders/material_cb.h>

using namespace donut;

//Hash defines needed for optimized Vertices
inline void hash_combine(size_t& seed, size_t hash) {
    hash += 0x9e3779b9 + (seed << 6) + (seed >> 2);
    seed ^= hash;
}

namespace std {
    template<> struct hash<float3>
    {
        size_t operator()(float3 const& f) const {
            size_t seed = 0;
            hash_combine(seed, hash<float>()(f.x));
            hash_combine(seed, hash<float>()(f.y));
            hash_combine(seed, hash<float>()(f.z));
            return seed;
        }
    };

    template<> struct hash<MinecraftSceneLoader::SceneVertex>
    {
        size_t operator()(MinecraftSceneLoader::SceneVertex const& vertex) const {
            size_t seed = 0;
            hash_combine(seed, hash<float3>()(vertex.position));
            hash_combine(seed, hash<float3>()(vertex.normal));
            hash_combine(seed, hash<float>()(vertex.uv.x));
            hash_combine(seed, hash<float>()(vertex.uv.y));
            return seed;
        }
    };
}

bool MinecraftSceneLoader::LoadScene(std::filesystem::path scenePath, std::string sceneName, nvrhi::IDevice* device, nvrhi::CommandListHandle commandList, 
    std::shared_ptr<TextureCache>& pTextureCache, std::shared_ptr<DescriptorTableManager>& descriptorTable)
{
    //Init TinyObj and load scene
	tinyobj::ObjReaderConfig reader_config;
	reader_config.mtl_search_path = ""; // Path to material files


	tinyobj::ObjReader reader;
	if (!reader.ParseFromFile((scenePath / sceneName).string(), reader_config)) {
		if (!reader.Error().empty()) {
			std::string err = "TinyObjReader: " + reader.Error();
			log::warning(err.c_str());
		}
		return false;
	}

	if (!reader.Warning().empty()) {
		std::string err = "TinyObjReader: " + reader.Warning();
		log::warning(err.c_str());
	}

	auto& attribs = reader.GetAttrib();
	auto& shapes = reader.GetShapes();
	auto& materials = reader.GetMaterials();

    if (materials.empty()) {
        log::warning("No Materials found. Abort scene loading");
        return false;
    }

    AddGeometryToScene(attribs, shapes);

    AddMaterialsToScene(materials, device, commandList, pTextureCache, descriptorTable);

    CreateMaterialsBuffers(device, commandList);
    
    CreateGeometryBuffers(device, commandList);
    
    CreateAccelerationStructure(device, commandList);

	m_sceneIsLoaded = true;
	return true;
}

bool MinecraftSceneLoader::UnloadScene(std::shared_ptr<engine::TextureCache>& pTextureCache) {
    //Clear the scene
    m_sceneStats = { };

    //CPU Buffer
    m_Materials.clear();
    m_AABBs.clear();
    m_AABBMaterials.clear();
    m_Indices.clear();
    m_Vertices.clear();
    m_TriPerFaceMatID.clear();

    //Acceleration Structures
    m_TopLevelAS = nullptr;
    m_BlasAABBs = nullptr;
    m_BlasTriangles = nullptr;

    //Buffer
    m_AABBBuffer = nullptr;
    m_VertexBuffer = nullptr;
    m_IndexBuffer = nullptr;

    m_MaterialBuffer = nullptr;
    m_TriangleMaterialIDBuffer = nullptr;
    m_AABBMaterialIDBuffer = nullptr;

    //Textures
    pTextureCache->Reset();

    m_sceneIsLoaded = false;

    return true;
}

void MinecraftSceneLoader::AddMaterialsToScene(const std::vector<tinyobj::material_t>& materials, nvrhi::IDevice* device, nvrhi::CommandListHandle commandList, 
    std::shared_ptr<TextureCache>& pTextureCache, std::shared_ptr<DescriptorTableManager>& descriptorTable)
{
    //Handle Materials
    //Mineways stores roughness and metallic textures separately, therefore a compute shader that creates a metalRough Texture is needed.
    InitMetalRoughTexGenCS(device);
    const std::filesystem::path modelFolderName = "/MinecraftModels/";
    for (uint i = 0; i < materials.size(); i++) {
        auto& material = materials[i];
        Material sceneMat{};
        sceneMat.modelFileName = "MinecraftSceneLoader";
        sceneMat.name = material.name;
        sceneMat.materialID = i;
        sceneMat.baseOrDiffuseColor = float3(material.diffuse[0], material.diffuse[1], material.diffuse[2]);
        sceneMat.emissiveColor = float3(material.emission[0], material.emission[1], material.emission[2]);
        sceneMat.roughness = 1.f;
        sceneMat.metalness = 0.f;
        sceneMat.alphaCutoff = 0.1;  //Force opaque on transmissive materials

        if (!material.diffuse_texname.empty()) {
            std::filesystem::path texPath = modelFolderName / material.diffuse_texname;
            sceneMat.baseOrDiffuseTexture = pTextureCache->LoadTextureFromFile(texPath, true, nullptr, commandList);

            //Check if the material is alpha tested
            if (!material.alpha_texname.empty()) {
                sceneMat.domain = MaterialDomain::AlphaTested;
                sceneMat.doubleSided = true;
            }
        }
        //Normal
        if (!material.normal_texname.empty()) {
            std::filesystem::path texPath = modelFolderName / material.normal_texname;
            sceneMat.normalTexture = pTextureCache->LoadTextureFromFile(texPath, false, nullptr, commandList);
        }
        //Emissive
        if (!material.emissive_texname.empty()) {
            std::filesystem::path texPath = modelFolderName / material.emissive_texname;
            sceneMat.emissiveTexture = pTextureCache->LoadTextureFromFile(texPath, false, nullptr, commandList);
        }
        //Roughness, metal
        if (!material.specular_highlight_texname.empty() || !material.roughness_texname.empty()) {
            sceneMat.metalnessInRedChannel = true;
            bool convertShininessToRoughness = material.roughness_texname.empty();
            std::string roughTexName = convertShininessToRoughness ? material.specular_highlight_texname : material.roughness_texname;
            std::filesystem::path roughTexPath = modelFolderName / roughTexName;
            std::shared_ptr<LoadedTexture> roughnessTexture = pTextureCache->LoadTextureFromFile(roughTexPath, false, nullptr, commandList);
            std::shared_ptr<LoadedTexture> metallicTexture;
            bool hasMetallic = !material.metallic_texname.empty();
            if (hasMetallic) {
                std::filesystem::path metallicTexPath = modelFolderName / material.metallic_texname;
                metallicTexture = pTextureCache->LoadTextureFromFile(metallicTexPath, false, nullptr, commandList);
            }

            sceneMat.metalRoughOrSpecularTexture = CreateMetalRoughTextures(device, commandList, descriptorTable, roughnessTexture->texture,
                hasMetallic ? metallicTexture->texture : nullptr, convertShininessToRoughness);

            //Remove loaded single channel metallic and roughness textures from the cache
            pTextureCache->UnloadTexture(roughnessTexture);
            if (hasMetallic)
                pTextureCache->UnloadTexture(metallicTexture);
        }
        m_Materials.push_back(sceneMat);
    }

    //Remove Compute shader resources, as they are not needed anymore
    RemoveMetalRoughTexGenCS();
}

void MinecraftSceneLoader::CreateMaterialsBuffers(nvrhi::IDevice* device, nvrhi::CommandListHandle commandList)
{
    //Fill the material buffer construct
    std::vector<MaterialConstants> materialConstants;
    for (uint i = 0; i < m_Materials.size(); i++) {
        MaterialConstants constant;
        m_Materials[i].FillConstantBuffer(constant);
        materialConstants.push_back(constant);
    }

    //Create GPU Buffers
    nvrhi::BufferDesc bufferDesc;
    bufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;

    //Material Data Buffer
    bufferDesc.byteSize = sizeof(MaterialConstants) * materialConstants.size();
    bufferDesc.debugName = "MaterialBuffer";
    bufferDesc.structStride = sizeof(MaterialConstants);
    bufferDesc.keepInitialState = true;
    m_MaterialBuffer = device->createBuffer(bufferDesc);
    commandList->writeBuffer(m_MaterialBuffer, materialConstants.data(), sizeof(MaterialConstants) * materialConstants.size());

    //Material ID Buffers for Triangles and AABBs
    if (!m_TriPerFaceMatID.empty())
    {
        bufferDesc.byteSize = sizeof(uint) * m_TriPerFaceMatID.size();
        bufferDesc.debugName = "TrianglePerFaceMatID";
        bufferDesc.structStride = sizeof(uint);
        m_TriangleMaterialIDBuffer = device->createBuffer(bufferDesc);
        commandList->writeBuffer(m_TriangleMaterialIDBuffer, m_TriPerFaceMatID.data(), sizeof(uint) * m_TriPerFaceMatID.size());
    }
    if (!m_AABBMaterials.empty())
    {
        bufferDesc.byteSize = sizeof(AABBMaterials) * m_AABBMaterials.size();
        bufferDesc.debugName = "AABBMatID";
        bufferDesc.structStride = sizeof(AABBMaterials);
        m_AABBMaterialIDBuffer = device->createBuffer(bufferDesc);
        commandList->writeBuffer(m_AABBMaterialIDBuffer, m_AABBMaterials.data(), sizeof(AABBMaterials) * m_AABBMaterials.size());
    }
}

void MinecraftSceneLoader::AddGeometryToScene(const tinyobj::attrib_t& attribs, const std::vector<tinyobj::shape_t>& shapes)
{
    std::unordered_map<SceneVertex, uint32_t> uniqueVertices{};

    // Loop over shapes
    for (size_t s = 0; s < shapes.size(); s++) {
        size_t index_offset = 0;
        //Check if Block (AABB) or more billboard/complex (stored as Triangle)
        if (shapes[s].mesh.num_face_vertices.size() == 12) //Case AABB
        {
            AABB aabb;
            aabb.min = float3(std::numeric_limits<float>::max());
            aabb.max = float3(std::numeric_limits<float>::max()) * -1.f;
            //Get box extends with min/max
            for (size_t f = 0; f < shapes[s].mesh.num_face_vertices.size(); f++) {
                size_t fv = size_t(shapes[s].mesh.num_face_vertices[f]);
                // Loop over vertices in the face.
                for (size_t v = 0; v < fv; v++) {
                    tinyobj::index_t idx = shapes[s].mesh.indices[index_offset + v];
                    float vx = static_cast<float>(attribs.vertices[3 * size_t(idx.vertex_index) + 0]);
                    float vy = static_cast<float>(attribs.vertices[3 * size_t(idx.vertex_index) + 1]);
                    float vz = static_cast<float>(attribs.vertices[3 * size_t(idx.vertex_index) + 2]);

                    aabb.min.x = min(vx, aabb.min.x);
                    aabb.min.y = min(vy, aabb.min.y);
                    aabb.min.z = min(vz, aabb.min.z);
                    aabb.max.x = max(vx, aabb.max.x);
                    aabb.max.y = max(vy, aabb.max.y);
                    aabb.max.z = max(vz, aabb.max.z);
                }
                index_offset += fv;
            }

            //Get per face material (sorted from negative x,y,z to positive x,y,z
            //TODO Calculated front orientation to correctly recreate uv's for blocks with special orientations
            AABBMaterials aabbMaterials;
            aabbMaterials.negXMatID = shapes[s].mesh.material_ids[0];
            aabbMaterials.negYMatID = shapes[s].mesh.material_ids[2];
            aabbMaterials.negZMatID = shapes[s].mesh.material_ids[4];
            aabbMaterials.posXMatID = shapes[s].mesh.material_ids[6];
            aabbMaterials.posYMatID = shapes[s].mesh.material_ids[8];
            aabbMaterials.posZMatID = shapes[s].mesh.material_ids[10];
            aabbMaterials.padding = 0;

            m_AABBs.push_back(aabb);
            m_AABBMaterials.push_back(aabbMaterials);
            m_sceneStats.numAABBs++;
        }
        else //Case Triangle
        {
            for (size_t f = 0; f < shapes[s].mesh.num_face_vertices.size(); f++) {
                size_t fv = size_t(shapes[s].mesh.num_face_vertices[f]);
                //Skip non-triangles (not-supported)
                if (fv != 3) {
                    log::warning("MinecraftSceneLoader::Load() encountered a non-triangle");
                    continue;
                }

                // Loop over vertices in the face.
                for (size_t v = 0; v < fv; v++) {
                    SceneVertex vertex{};
                    // access to vertex
                    tinyobj::index_t idx = shapes[s].mesh.indices[index_offset + v];
                    vertex.position.x = static_cast<float>(attribs.vertices[3 * size_t(idx.vertex_index) + 0]);
                    vertex.position.y = static_cast<float>(attribs.vertices[3 * size_t(idx.vertex_index) + 1]);
                    vertex.position.z = static_cast<float>(attribs.vertices[3 * size_t(idx.vertex_index) + 2]);

                    // Check if `normal_index` is zero or positive. negative = no normal data
                    if (idx.normal_index >= 0) {
                        vertex.normal.x = static_cast<float>(attribs.normals[3 * size_t(idx.normal_index) + 0]);
                        vertex.normal.y = static_cast<float>(attribs.normals[3 * size_t(idx.normal_index) + 1]);
                        vertex.normal.z = static_cast<float>(attribs.normals[3 * size_t(idx.normal_index) + 2]);
                    }

                    // Check if `texcoord_index` is zero or positive. negative = no texcoord data
                    if (idx.texcoord_index >= 0) {
                        vertex.uv.x = static_cast<float>(attribs.texcoords[2 * size_t(idx.texcoord_index) + 0]);
                        vertex.uv.y = 1.0f - static_cast<float>(attribs.texcoords[2 * size_t(idx.texcoord_index) + 1]); //Y needs to be flipped for Vulkan and D12
                    }

                    //Check if vertex is duplicate
                    if (uniqueVertices.count(vertex) == 0) {
                        uniqueVertices[vertex] = static_cast<uint32_t>(m_Vertices.size()); //Add new index to map
                        m_Vertices.push_back(vertex.toVertexData()); //Store vertex data
                        m_sceneStats.numUniqueVertices++;
                    }

                    m_Indices.push_back(uniqueVertices[vertex]);
                    m_sceneStats.numIndices++;
                }
                index_offset += fv;
                m_sceneStats.numTriangles++;

                m_TriPerFaceMatID.push_back(shapes[s].mesh.material_ids[f]);
            }
        }
    }
}

void MinecraftSceneLoader::CreateGeometryBuffers(nvrhi::IDevice* device, nvrhi::CommandListHandle commandList)
{
    //Triangle (Vertex + Index Buffers)
    if (!m_Vertices.empty() && !m_Indices.empty())
    {
        nvrhi::BufferDesc bufferDesc;
        bufferDesc.byteSize = sizeof(uint) * m_Indices.size();
        bufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        bufferDesc.keepInitialState = true;
        bufferDesc.isAccelStructBuildInput = true;
        bufferDesc.structStride = sizeof(uint);
        bufferDesc.debugName = "MinecraftSceneLoader::Indices";
        m_IndexBuffer = device->createBuffer(bufferDesc);
        bufferDesc.canHaveRawViews = false;
        bufferDesc.byteSize = sizeof(VertexData) * m_Vertices.size();
        bufferDesc.structStride = sizeof(Vertex);
        bufferDesc.debugName = "MinecraftSceneLoader::Vertices";
        m_VertexBuffer = device->createBuffer(bufferDesc);

        commandList->writeBuffer(m_IndexBuffer, m_Indices.data(), sizeof(uint) * m_Indices.size());
        commandList->writeBuffer(m_VertexBuffer, m_Vertices.data(), sizeof(VertexData) * m_Vertices.size());
    }

    //AABB Buffer
    if (!m_AABBs.empty())
    {
        nvrhi::BufferDesc bufferDesc;
        bufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        bufferDesc.keepInitialState = true;
        bufferDesc.isAccelStructBuildInput = true;
        bufferDesc.structStride = sizeof(float2);   //Necessary due to SPIR-V HLSL alignment bug (AABB(float6) does not work properly)
        bufferDesc.byteSize = sizeof(AABB) * m_AABBs.size();
        bufferDesc.debugName = "MinecraftSceneLoader::AABBs";
        m_AABBBuffer = device->createBuffer(bufferDesc);
        commandList->writeBuffer(m_AABBBuffer, m_AABBs.data(), sizeof(AABB) * m_AABBs.size());
    }
}

void MinecraftSceneLoader::CreateAccelerationStructure(nvrhi::IDevice* device, nvrhi::CommandListHandle commandList)
{
    //Create Bottom Level Acceleration Structure
    //Triangle
    if (m_IndexBuffer && m_VertexBuffer)
    {
        nvrhi::rt::AccelStructDesc blasDesc;
        blasDesc.isTopLevel = false;
        nvrhi::rt::GeometryDesc geometryDesc;
        auto& triangles = geometryDesc.geometryData.triangles;
        triangles.indexBuffer = m_IndexBuffer;
        triangles.vertexBuffer = m_VertexBuffer;
        triangles.indexFormat = nvrhi::Format::R32_UINT;
        triangles.indexCount = m_Indices.size();
        triangles.vertexFormat = nvrhi::Format::RGB32_FLOAT;
        triangles.vertexStride = sizeof(VertexData);
        triangles.vertexCount = m_Vertices.size();
        geometryDesc.geometryType = nvrhi::rt::GeometryType::Triangles;
        geometryDesc.flags = nvrhi::rt::GeometryFlags::NoDuplicateAnyHitInvocation;
        blasDesc.bottomLevelGeometries.push_back(geometryDesc);

        m_BlasTriangles = device->createAccelStruct(blasDesc);
        nvrhi::utils::BuildBottomLevelAccelStruct(commandList, m_BlasTriangles, blasDesc);
    }

    //Boxes
    if (m_AABBBuffer)
    {
        nvrhi::rt::AccelStructDesc blasDesc;
        blasDesc.isTopLevel = false;
        //Compaction and FastTrace flag is this BLAS contains static geometry
        blasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::AllowCompaction | nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;
        nvrhi::rt::GeometryDesc geometryDesc;
        auto& aabbDesc = geometryDesc.geometryData.aabbs;
        aabbDesc.buffer = m_AABBBuffer;
        aabbDesc.count = m_AABBs.size();
        aabbDesc.stride = sizeof(AABB);
        aabbDesc.offset = 0;
        geometryDesc.geometryType = nvrhi::rt::GeometryType::AABBs;
        geometryDesc.flags = nvrhi::rt::GeometryFlags::NoDuplicateAnyHitInvocation;
        blasDesc.bottomLevelGeometries.push_back(geometryDesc);
        m_BlasAABBs = device->createAccelStruct(blasDesc);
        nvrhi::utils::BuildBottomLevelAccelStruct(commandList, m_BlasAABBs, blasDesc);
    }

    //Create Instances and TLAS
    std::vector<nvrhi::rt::InstanceDesc> instances;

    //Triangle instance
    if (m_BlasTriangles)
    {
        nvrhi::rt::InstanceDesc instanceDesc;
        instanceDesc.bottomLevelAS = m_BlasTriangles;
        instanceDesc.instanceMask = 0xFF;
        instanceDesc.flags = nvrhi::rt::InstanceFlags::TriangleFrontCounterclockwise;
        instanceDesc.instanceContributionToHitGroupIndex = 0;
        float3x4 transform = float3x4::identity();
        memcpy(instanceDesc.transform, &transform, sizeof(transform));
        instances.push_back(instanceDesc);
    }

    //AABB instance
    if (m_BlasAABBs)
    {
        nvrhi::rt::InstanceDesc instanceDesc;
        instanceDesc.bottomLevelAS = m_BlasAABBs;
        instanceDesc.instanceMask = 0xFF;
        instanceDesc.flags = nvrhi::rt::InstanceFlags::None;
        instanceDesc.instanceContributionToHitGroupIndex = 1;
        float3x4 transform = float3x4::identity();
        memcpy(instanceDesc.transform, &transform, sizeof(transform));

        instances.push_back(instanceDesc);
    }

    //Build tlas with set instances
    nvrhi::rt::AccelStructDesc tlasDesc;
    tlasDesc.isTopLevel = true;
    tlasDesc.topLevelMaxInstances = instances.size();
    m_TopLevelAS = device->createAccelStruct(tlasDesc);

    commandList->buildTopLevelAccelStruct(m_TopLevelAS, instances.data(), instances.size());
}

void MinecraftSceneLoader::InitMetalRoughTexGenCS(nvrhi::IDevice* device) {
    //Create Shader
    m_Shader = m_ShaderFactory->CreateAutoShader("app/GenRoughMetalTexture_cs.hlsl", "main", DONUT_MAKE_PLATFORM_SHADER(g_genRoughMetalTex_cs), nullptr, nvrhi::ShaderType::Compute);
    
    //Constant buffer
    nvrhi::BufferDesc constantBufferDesc;
    constantBufferDesc.byteSize = sizeof(CBMetalRoughTexGen);
    constantBufferDesc.isConstantBuffer = true;
    constantBufferDesc.isVolatile = true;
    constantBufferDesc.debugName = "MinecraftSceneLoader::MetalRoughCB";
    constantBufferDesc.maxVersions = 1024;
    m_ConstantBuffer = device->createBuffer(constantBufferDesc);

    // BindingLayout
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
    layoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(0));    //Roughness
    layoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(1));    //Metallic
    layoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(0));    //RoughMetalTex
    m_BindingLayout = device->createBindingLayout(layoutDesc);

    //Pipeline
    nvrhi::ComputePipelineDesc computePipelineDesc;
    computePipelineDesc.CS = m_Shader;
    computePipelineDesc.bindingLayouts = { m_BindingLayout };
    m_ComputeHandle = device->createComputePipeline(computePipelineDesc);
}

void MinecraftSceneLoader::RemoveMetalRoughTexGenCS() {
    //Free resources. nvrhi will delete them if all references are removed
    m_Shader = nullptr;
    m_ConstantBuffer = nullptr;
    m_BindingLayout = nullptr;
    m_ComputeHandle = nullptr;
}

std::shared_ptr<LoadedTexture> MinecraftSceneLoader::CreateMetalRoughTextures(nvrhi::IDevice* device, nvrhi::CommandListHandle& commandList, std::shared_ptr<DescriptorTableManager>& descriptorTable,
    nvrhi::TextureHandle roughnessTex, nvrhi::TextureHandle metallicTex, bool convertShininessToRoughness)
{
    //Create the MetalRough texture
    std::shared_ptr<LoadedTexture> loadedTexture = std::make_shared<LoadedTexture>();
    nvrhi::TextureDesc textureDesc;
    textureDesc = roughnessTex->getDesc();
    textureDesc.isUAV = true;
    textureDesc.format = nvrhi::Format::RG32_FLOAT;
    loadedTexture->texture = device->createTexture(textureDesc);

    //Set bindings
    nvrhi::BindingSetDesc setDesc;
    setDesc.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, m_ConstantBuffer),
        nvrhi::BindingSetItem::Texture_SRV(0, roughnessTex),
        nvrhi::BindingSetItem::Texture_SRV(1, metallicTex ? metallicTex : roughnessTex),
        nvrhi::BindingSetItem::Texture_UAV(0, loadedTexture->texture)
    };
    nvrhi::BindingSetHandle bindingSet = device->createBindingSet(setDesc, m_BindingLayout);

    //Fill Constant Buffer and upload
    CBMetalRoughTexGen cb;
    cb.convertShininessToRoughness = convertShininessToRoughness ? 1 : 0;
    cb.metallicValid = (metallicTex != nullptr) ? 1 : 0;
    cb.texDimensions = uint2(textureDesc.width, textureDesc.height);
    commandList->writeBuffer(m_ConstantBuffer, &cb, sizeof(cb));

    //Dispatch compute shader
    nvrhi::ComputeState state;
    state.pipeline = m_ComputeHandle;
    state.bindings = { bindingSet};
    commandList->setComputeState(state);
    commandList->dispatch(textureDesc.width, textureDesc.height);

    //Finalize texture and add to the bindless pool
    commandList->setPermanentTextureState(loadedTexture->texture, nvrhi::ResourceStates::ShaderResource);
    loadedTexture->bindlessDescriptor = descriptorTable->CreateDescriptorHandle(nvrhi::BindingSetItem::Texture_SRV(0, loadedTexture->texture));

    return(loadedTexture);
}