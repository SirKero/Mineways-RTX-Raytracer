#pragma once
#include <donut/engine/SceneTypes.h>
#include <donut/engine/TextureCache.h>
#include <donut/engine/DescriptorTableManager.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/core/math/math.h>
#include <tiny_obj_loader.h>

using namespace donut::math;
#include "sharedShaderData.h" //Needs namespace donut::math;
using namespace donut::engine;

/* Class to load Minecraft Scene from Mineways .obj with individual block export enabled
*/
class MinecraftSceneLoader {
public:
	//Helper struct to get unique vertices (as hlsl does not support ==operator redefinition)
	//Needs to be able to generate VertexData
	struct SceneVertex {
		float3 position;
		float3 normal;
		float2 uv;

		bool operator==(const SceneVertex& other) const {
			bool3 samePos = position == other.position;
			bool3 sameNormal = normal == other.normal;
			bool2 sameUV = uv == other.uv;
			return all(samePos) && all(sameNormal) && all(sameUV);
		}

		VertexData toVertexData() {
			VertexData data{};
			data.position = position;
			data.normal = normal;
			data.uvX = uv.x;
			data.uvY = uv.y;
			return data;
		}
	};

	MinecraftSceneLoader(std::shared_ptr<ShaderFactory> shaderFactory) : m_ShaderFactory(shaderFactory) {}

	//Loads a Mineways obj scene
	bool LoadScene(std::filesystem::path scenePath , std::string sceneName, nvrhi::IDevice* device, 
		nvrhi::CommandListHandle commandList, std::shared_ptr<TextureCache>& pTextureCache, std::shared_ptr<DescriptorTableManager>& descriptorTable);

	// Removes all scene resources
	bool UnloadScene(std::shared_ptr<TextureCache>& pTextureCache);

	//True if scene can be used
	bool IsLoaded() { return m_sceneIsLoaded; }

	nvrhi::rt::AccelStructHandle GetTLAS() { return m_TopLevelAS; }
	nvrhi::BufferHandle GetAABBBuffer() { return m_AABBBuffer; }
	nvrhi::BufferHandle GetVertexBuffer() { return m_VertexBuffer; }
	nvrhi::BufferHandle GetIndexBuffer() { return m_IndexBuffer; }
	nvrhi::BufferHandle GetAABBMaterialIDBuffer() { return m_AABBMaterialIDBuffer; }
	nvrhi::BufferHandle GetTriangleMaterialIDBuffer() { return m_TriangleMaterialIDBuffer; }
	nvrhi::BufferHandle GetMaterialBuffer() { return m_MaterialBuffer; }

private:
	struct SceneStats {
		int numTriangles = 0;
		int numAABBs = 0;
		int numMaterials = 0;
		int numUniqueVertices = 0;
		int numIndices = 0;
	};

	//Adds all "materials" to the scene structures (CPU) and loads textures to the GPU
	void AddMaterialsToScene(const std::vector<tinyobj::material_t>& materials, nvrhi::IDevice* device, nvrhi::CommandListHandle commandList,
		std::shared_ptr<TextureCache>& pTextureCache, std::shared_ptr<DescriptorTableManager>& descriptorTable);
	//Creates the materials ID buffers
	void CreateMaterialsBuffers(nvrhi::IDevice* device, nvrhi::CommandListHandle commandList);

	//Adds the geometry to the scene structures on the CPU
	void AddGeometryToScene(const tinyobj::attrib_t& attribs, const std::vector<tinyobj::shape_t>& shapes);
	//Creates and uploads the geometry buffers to the GPU
	void CreateGeometryBuffers(nvrhi::IDevice* device, nvrhi::CommandListHandle commandList);
	//Creates the Acceleration Structure for Ray Tracing
	void CreateAccelerationStructure(nvrhi::IDevice* device, nvrhi::CommandListHandle commandList);

	//Initializes the compute shader to generate a MetalRoughness texture from two separate textures
	void InitMetalRoughTexGenCS(nvrhi::IDevice* device);
	//Creates a MetalRoughness texture via compute shader
	std::shared_ptr<LoadedTexture> CreateMetalRoughTextures(nvrhi::IDevice* device, nvrhi::CommandListHandle& commandList, std::shared_ptr<DescriptorTableManager>& descriptorTable,
		nvrhi::TextureHandle roughnessTex, nvrhi::TextureHandle metallicTex, bool convertShininessToRoughness);
	//Removes compute shader resources
	void RemoveMetalRoughTexGenCS();

	bool m_sceneIsLoaded = false;
	SceneStats m_sceneStats = {};
	std::vector<AABB> m_AABBs;
	std::vector<AABBMaterials> m_AABBMaterials;

	std::vector<uint> m_Indices;
	std::vector<VertexData> m_Vertices;
	std::vector<int> m_TriPerFaceMatID;

	std::vector<Material> m_Materials;

	//Acceleration Structures
	nvrhi::rt::AccelStructHandle m_BlasTriangles;	//Triangle Bottom Level Acceleration Structure for all non-block geometry
	nvrhi::rt::AccelStructHandle m_BlasAABBs;		//AABB Bottom Level Acceleration Structure for all blocks
	nvrhi::rt::AccelStructHandle m_TopLevelAS;		//Top Level Acceleration Structure for the scene

	//GPU Geometry Buffers
	nvrhi::BufferHandle m_AABBBuffer;
	nvrhi::BufferHandle m_VertexBuffer;
	nvrhi::BufferHandle m_IndexBuffer;

	//GPU Material Buffers/Textures
	nvrhi::BufferHandle m_AABBMaterialIDBuffer;
	nvrhi::BufferHandle m_TriangleMaterialIDBuffer;
	nvrhi::BufferHandle m_MaterialBuffer;

	//Create and handle metal rough textures
	std::shared_ptr<ShaderFactory> m_ShaderFactory;
	nvrhi::ShaderHandle m_Shader;
	nvrhi::BufferHandle m_ConstantBuffer;
	nvrhi::BindingLayoutHandle m_BindingLayout;
	nvrhi::ComputePipelineHandle m_ComputeHandle;
};