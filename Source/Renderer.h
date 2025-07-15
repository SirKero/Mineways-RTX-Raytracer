#pragma once
#include <donut/app/ApplicationBase.h>
#include <donut/app/DeviceManager.h>
#include <donut/app/Camera.h>
#include <donut/core/math/math.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/BindingCache.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/DescriptorTableManager.h>
#include <donut/engine/Scene.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/TextureCache.h>
#include <donut/engine/View.h>
#include <nvrhi/utils.h>

#include "MinecraftSceneLoader.h"

using namespace donut;
using namespace donut::math;

#define RENDERER_VERSION "0.1"

static const char* g_WindowTitle = "Mineways Renderer V" RENDERER_VERSION;

//UI data
struct UIData {
	//Light
	float3 lightDirection = float3(-0.340f , -0.841f , 0.421);
	float lightIntensity = 5.f;

	//Camera
	float cameraSpeed = 3.f;
	float cameraFov = 0.78f;
	float cameraNear = 0.1f;
	float cameraFar = 1000.f;

	//Rendering settings
	float ambient = 0.1f;
	float emissiveStrength = 1.0f;
	float ambientSpecularStrength = 1.f;
	
	//Shadow
	float shadowRayBias = 0.03;
	
	//Scene selection
	int selectedScene = -1;
};

class Renderer : public app::IRenderPass 
{
public:
	using IRenderPass::IRenderPass;

	Renderer(app::DeviceManager* deviceManager, UIData* ui)
		: IRenderPass(deviceManager)
		, m_ui(ui)
	{ }

	//Called upon window resizing
	void BackBufferResizing() override;

	bool KeyboardUpdate(int key, int scancode, int action, int mods) override;

	bool MousePosUpdate(double xpos, double ypos) override;

	bool MouseButtonUpdate(int button, int action, int mods) override;

	//Called once per frame for updating time depended classes
	void Animate(float fElapsedTimeSeconds) override;

	//Initialize resources for the renderer
	bool Init();

	//Main render loop
	void Render(nvrhi::IFramebuffer* framebuffer) override;
	
	//Return resolution info as a string
	const std::string GetResolutionInfo();

	//Resets the camera position to float3(0)
	void ResetCameraPosition();

	//Return render time infos
	const std::string GetFPSInfo() { return m_fpsInfo; }

	const std::vector<std::string>& GetAvailableScenes() { return m_AvailableScenes; }
	std::shared_ptr<engine::ShaderFactory> GetShaderFactory() const { return m_ShaderFactory; }
private:
	//Helper for loading the scene
	bool LoadMinecraftScene(std::string sceneName);

	//Initializes both binding and bindless layouts and Description table
	void InitBindingLayouts();

	//Initialize the ray tracing pipeline and shader table
	void InitRayTracingPipeline();

	void FindAvailableScenes();

	int m_selectedScene = -1;							//Active index in m_AvailableScenes (-1 = no scene loaded)
	std::vector<std::string> m_AvailableScenes;			//List of available Scenes
	uint2 m_Resolution = uint2(500, 500);				//Display and Render resolution
	std::string m_fpsInfo = "";							//Render Time info in ms and FPS

	nvrhi::CommandListHandle m_CommandList;				//(Graphics) Command List
	nvrhi::ShaderLibraryHandle m_ShaderLibrary;			//Shader Library
	std::shared_ptr<vfs::RootFileSystem> m_RootFS;		//Root file system
	std::filesystem::path m_ScenePath;					//Path to the scene folder
	
	std::shared_ptr<engine::ShaderFactory> m_ShaderFactory;				//Handles all shaders
	std::shared_ptr<engine::DescriptorTableManager> m_DescriptorTable;	//Descriptor for bindless textures
	std::shared_ptr<engine::TextureCache> m_TextureCache;				//Cache for texture loading

	nvrhi::rt::PipelineHandle m_Pipeline;				//Raytracing pipeline
	nvrhi::rt::ShaderTableHandle m_ShaderTable;			//Shader Table for the rt pipeline
	nvrhi::BindingLayoutHandle m_BindingLayout;			//Layout for shader bindings
	nvrhi::BindingLayoutHandle m_BindlessLayout;		//Bindless layouts for model textures
	nvrhi::BindingSetHandle m_BindingSet;				//Binding set
		
	std::shared_ptr<engine::CommonRenderPasses> m_CommonPasses;	//Helper passes
	std::unique_ptr<engine::BindingCache> m_BindingCache;		//Cache for binding used in the helper passes

	nvrhi::BufferHandle m_ConstantBuffer;			//Constant buffer used in shader
	nvrhi::TextureHandle m_RenderTarget;			//UAV Render Target
	
	engine::PlanarView m_View;				//Camera matrix helper
	app::FirstPersonCamera m_Camera;		//Camera

	std::shared_ptr<engine::SceneGraph> m_LightGraph;		//Scene Graph needed for the Directional light
	std::shared_ptr<engine::SceneGraphNode> m_LightNode;	//Scene Node needed for the Directional light
	std::shared_ptr<engine::DirectionalLight> m_DirLight;	//Directional Light helper

	std::unique_ptr<MinecraftSceneLoader> m_MinecraftSceneLoader; //Scene loader

	UIData* m_ui;	//Pointer to UI data. Is shared with the UI
};
