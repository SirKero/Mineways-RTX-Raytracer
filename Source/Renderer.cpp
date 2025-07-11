#include "Renderer.h"
#include "sharedShaderData.h"

template<typename ... Args> std::string StringFormat(const std::string& format, Args ... args)
{
	int size_s = std::snprintf(nullptr, 0, format.c_str(), args ...) + 1; // Extra space for '\0'
	if (size_s <= 0) { throw std::runtime_error("Error during formatting."); }
	auto size = static_cast<size_t>(size_s);
	std::unique_ptr<char[]> buf(new char[size]);
	std::snprintf(buf.get(), size, format.c_str(), args ...);
	return std::string(buf.get(), buf.get() + size - 1); // We don't want the '\0' inside
}

void Renderer::BackBufferResizing()
{
	m_RenderTarget = nullptr;
	m_BindingCache->Clear();
}

bool Renderer::KeyboardUpdate(int key, int scancode, int action, int mods)
{
	m_Camera.KeyboardUpdate(key, scancode, action, mods);
	return true;
}

bool Renderer::MousePosUpdate(double xpos, double ypos)
{
	m_Camera.MousePosUpdate(xpos, ypos);
	return true;
}

bool Renderer::MouseButtonUpdate(int button, int action, int mods)
{
	m_Camera.MouseButtonUpdate(button, action, mods);
	return true;
}

void Renderer::Animate(float fElapsedTimeSeconds)
{
	m_Camera.Animate(fElapsedTimeSeconds);

	double frameTime = GetDeviceManager()->GetAverageFrameTimeSeconds();
	
	m_fpsInfo = StringFormat("%.3f ms/frame (%.1f FPS)", frameTime * 1e3, 1.0 / frameTime);

	GetDeviceManager()->SetInformativeWindowTitle(g_WindowTitle, false, m_fpsInfo.c_str());
}

bool Renderer::LoadMinecraftScene(std::string sceneName) {
	m_CommandList->open();

	bool valid = m_MinecraftSceneLoader->LoadScene(m_ScenePath, sceneName, GetDevice(), m_CommandList, m_TextureCache, m_DescriptorTable);

	m_CommandList->close();
	GetDevice()->executeCommandList(m_CommandList);

	return valid;
}

void Renderer::ResetCameraPosition() {
	m_Camera.LookAt(float3(0, 0, 0), float3(0, 0, -1));
}

const std::string Renderer::GetResolutionInfo() {
	std::string info = "";
	info += std::to_string(m_Resolution.x);
	info += ",";
	info += std::to_string(m_Resolution.y);
	return info;
}

void Renderer::InitBindingLayouts() {
	//Bindless layout used for object textures
	nvrhi::BindlessLayoutDesc bindlessLayoutDesc;
	bindlessLayoutDesc.visibility = nvrhi::ShaderType::All;
	bindlessLayoutDesc.firstSlot = 0;
	bindlessLayoutDesc.maxCapacity = 4096;
	bindlessLayoutDesc.registerSpaces = {
		nvrhi::BindingLayoutItem::Texture_SRV(1)
	};
	m_BindlessLayout = GetDevice()->createBindlessLayout(bindlessLayoutDesc);

	//Binding layout used in shader
	nvrhi::BindingLayoutDesc globalBindingLayoutDesc;
	globalBindingLayoutDesc.visibility = nvrhi::ShaderType::All;
	globalBindingLayoutDesc.bindings = {
		nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
		nvrhi::BindingLayoutItem::RayTracingAccelStruct(0),
		nvrhi::BindingLayoutItem::Texture_UAV(0),
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2),
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3),
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4),
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5),
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6),
		nvrhi::BindingLayoutItem::Sampler(0)
	};

	m_BindingLayout = GetDevice()->createBindingLayout(globalBindingLayoutDesc);
	m_DescriptorTable = std::make_shared<engine::DescriptorTableManager>(GetDevice(), m_BindlessLayout);
}

void Renderer::InitRayTracingPipeline() {
	
	//Add Raygen and miss
	nvrhi::rt::PipelineDesc pipelineDesc;
	pipelineDesc.globalBindingLayouts = { m_BindingLayout, m_BindlessLayout };
	pipelineDesc.shaders = {
		{ "", m_ShaderLibrary->getShader("RayGen", nvrhi::ShaderType::RayGeneration), nullptr },
		{ "", m_ShaderLibrary->getShader("Miss", nvrhi::ShaderType::Miss), nullptr }
	};

	//Add both Hit Groups
	pipelineDesc.hitGroups = { 
		//Hit Group for the Triangles (0)
		{
			"TriangleHitGroup",
			m_ShaderLibrary->getShader("ClosestHitTriangle", nvrhi::ShaderType::ClosestHit),
			m_ShaderLibrary->getShader("AnyHitTriangle", nvrhi::ShaderType::AnyHit), // anyHitShader
			nullptr, // intersectionShader
			nullptr, // bindingLayout
			false  // isProceduralPrimitive
		},
		//Hit Group for the AABBs(1)
		{
			"AABBHitGroup",
			m_ShaderLibrary->getShader("ClosestHitAABB", nvrhi::ShaderType::ClosestHit),
			m_ShaderLibrary->getShader("AnyHitAABB", nvrhi::ShaderType::AnyHit), // anyHitShader
			m_ShaderLibrary->getShader("IntersectionAABB", nvrhi::ShaderType::Intersection), // intersectionShader
			nullptr, // bindingLayout
			true  // isProceduralPrimitive
		}
	};

	//Set max payload and attribute size
	pipelineDesc.maxPayloadSize = sizeof(float4) * 2;
	pipelineDesc.maxAttributeSize = sizeof(float4);

	m_Pipeline = GetDevice()->createRayTracingPipeline(pipelineDesc);

	//Init shader table
	m_ShaderTable = m_Pipeline->createShaderTable();
	m_ShaderTable->setRayGenerationShader("RayGen");
	m_ShaderTable->addHitGroup("TriangleHitGroup"); //0
	m_ShaderTable->addHitGroup("AABBHitGroup");	 //1
	m_ShaderTable->addMissShader("Miss");
}

void Renderer::FindAvailableScenes() {
	//Get all available scenes
	const std::string sceneNameExtension = ".obj";
	for (const auto& file : std::filesystem::directory_iterator(m_ScenePath))
	{
		if (!file.is_regular_file()) continue;
		std::string fileName = file.path().filename().string();
		std::string extension = file.path().extension().string();
		if (sceneNameExtension == extension) {
			m_AvailableScenes.push_back(fileName);
		}
	}

	if (m_AvailableScenes.size() == 1) {
		LoadMinecraftScene(m_AvailableScenes[0]);
		m_selectedScene = 0;
		m_ui->selectedScene = 0;
	}
}

bool Renderer::Init() {
	//Get file system paths
	auto nativeFS = std::make_shared<vfs::NativeFileSystem>();
	std::filesystem::path frameworkShaderPath = app::GetDirectoryWithExecutable() / "shaders/framework" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());
	std::filesystem::path appShaderPath = app::GetDirectoryWithExecutable() / "shaders/MinewaysRenderer" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());
	m_ScenePath = app::GetDirectoryWithExecutable().parent_path() / "MinecraftModels";

	m_RootFS = std::make_shared<vfs::RootFileSystem>();
	m_RootFS->mount("/shaders/donut", frameworkShaderPath);
	m_RootFS->mount("/shaders/app", appShaderPath);
	m_RootFS->mount("/MinecraftModels", m_ScenePath);

	m_ShaderFactory = std::make_shared<engine::ShaderFactory>(GetDevice(), m_RootFS, "/shaders");
	m_ShaderLibrary = m_ShaderFactory->CreateShaderLibrary("app/RaytraceWorld_rt.hlsl", nullptr);

	//Loading shader failed
	if (!m_ShaderLibrary)
		return false;

	//Init Camera and Light
	m_Camera.LookAt(float3(0.f, 1.8f, 0.f), float3(1.f, 1.8f, 0.f));
	m_Camera.SetMoveSpeed(3.f);
	//Using Donuts light helpers is a bit overcomplicated for our purposes, but needs to be done this way for "SetDirection" to work
	//TODO use a simplified custom struct
	m_LightGraph = std::make_shared<engine::SceneGraph>();
	m_LightNode = std::make_shared<engine::SceneGraphNode>();
	m_DirLight = std::make_shared<engine::DirectionalLight>();
	m_LightGraph->SetRootNode(m_LightNode);
	m_LightNode->SetLeaf(m_DirLight);
	m_DirLight->irradiance = m_ui->lightIntensity;
	m_DirLight->angularSize = 0.53f;
	m_DirLight->SetDirection(double3(m_ui->lightDirection));
	m_LightGraph->Refresh(0);

	//Init Common passes helper
	m_BindingCache = std::make_unique<engine::BindingCache>(GetDevice());
	m_CommonPasses = std::make_unique<engine::CommonRenderPasses>(GetDevice(), m_ShaderFactory);

	InitBindingLayouts();
	m_TextureCache = std::make_shared<engine::TextureCache>(GetDevice(), m_RootFS, m_DescriptorTable);

	m_CommandList = GetDevice()->createCommandList();

	InitRayTracingPipeline();

	m_ConstantBuffer = GetDevice()->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(ConstBuffer), "ConstantBuffer", engine::c_MaxRenderPassConstantBufferVersions));

	//Init Scene
	m_MinecraftSceneLoader = std::make_unique<MinecraftSceneLoader>(m_ShaderFactory);

	FindAvailableScenes();

	return true;
}

void Renderer::Render(nvrhi::IFramebuffer* framebuffer) {
	//Check if scene has changed
	if (m_selectedScene != m_ui->selectedScene) {
		if (m_MinecraftSceneLoader->IsLoaded()) {
			m_BindingSet = nullptr;
			m_MinecraftSceneLoader->UnloadScene(m_TextureCache);
		}

		m_selectedScene = m_ui->selectedScene;
	}

	if (!m_MinecraftSceneLoader->IsLoaded() && m_selectedScene != -1)
	{
		LoadMinecraftScene(m_AvailableScenes[m_selectedScene]);
	}

	if (!m_RenderTarget) {
		m_BindingSet = nullptr;

		nvrhi::TextureDesc textureDesc = framebuffer->getDesc().colorAttachments[0].texture->getDesc();
		m_Resolution = uint2(textureDesc.width, textureDesc.height);
		textureDesc.isUAV = true;
		textureDesc.isRenderTarget = false;
		textureDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
		textureDesc.keepInitialState = true;
		textureDesc.format = nvrhi::Format::RGBA8_UNORM;
		m_RenderTarget = GetDevice()->createTexture(textureDesc);
	}

	//Return early
	if (!m_MinecraftSceneLoader->IsLoaded()) {
		m_CommandList->open();
		m_CommandList->clearTextureFloat(m_RenderTarget, nvrhi::AllSubresources, nvrhi::Color(0.1f, 0.6f, 0.1f, 1.f));
		m_CommonPasses->BlitTexture(m_CommandList, framebuffer, m_RenderTarget, m_BindingCache.get());
		m_CommandList->close();
		GetDevice()->executeCommandList(m_CommandList);
		return;
	}

	//Create Binding set
	if (!m_BindingSet) {
		nvrhi::BindingSetDesc bindingSetDesc;
		bindingSetDesc.bindings = {
			nvrhi::BindingSetItem::ConstantBuffer(0, m_ConstantBuffer),
			nvrhi::BindingSetItem::RayTracingAccelStruct(0, m_MinecraftSceneLoader->GetTLAS()),
			nvrhi::BindingSetItem::Texture_UAV(0, m_RenderTarget),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_MinecraftSceneLoader->GetIndexBuffer()),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(2, m_MinecraftSceneLoader->GetVertexBuffer()),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(3, m_MinecraftSceneLoader->GetAABBBuffer()),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(4, m_MinecraftSceneLoader->GetTriangleMaterialIDBuffer()),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(5, m_MinecraftSceneLoader->GetAABBMaterialIDBuffer()),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(6, m_MinecraftSceneLoader->GetMaterialBuffer()),
			nvrhi::BindingSetItem::Sampler(0, m_CommonPasses->m_PointClampSampler)
		};
		m_BindingSet = GetDevice()->createBindingSet(bindingSetDesc, m_BindingLayout);
	}

	//Update viewport and camera
	m_Camera.SetMoveSpeed(m_ui->cameraSpeed);
	nvrhi::Viewport windowViewport(float(m_Resolution.x), float(m_Resolution.y));
	m_View.SetViewport(windowViewport);
	m_View.SetMatrices(m_Camera.GetWorldToViewMatrix(), perspProjD3DStyleReverse(m_ui->cameraFov, windowViewport.width() / windowViewport.height(), m_ui->cameraFar));
	m_View.UpdateCache();

	m_DirLight->SetDirection(double3(m_ui->lightDirection));
	m_DirLight->irradiance = m_ui->lightIntensity;
	m_LightGraph->Refresh(0);

	m_CommandList->open();

	//m_CommandList->clearTextureFloat(m_RenderTarget, nvrhi::AllSubresources, nvrhi::Color(0.f, 0.f, 0.f, 1.f));
	//Fill Constant buffer
	ConstBuffer constants = {};
	m_View.FillPlanarViewConstants(constants.viewConstants);
	m_DirLight->FillLightConstants(constants.directionalLightConstants);
	constants.ambient = m_ui->ambient;
	constants.emissiveStrength = m_ui->emissiveStrength;
	constants.cameraNear = m_ui->cameraNear;
	constants.cameraFar = m_ui->cameraFar;
	m_CommandList->writeBuffer(m_ConstantBuffer, &constants, sizeof(constants));

	nvrhi::rt::State state;
	state.shaderTable = m_ShaderTable;
	state.bindings = { m_BindingSet, m_DescriptorTable->GetDescriptorTable() };
	m_CommandList->setRayTracingState(state);

	nvrhi::rt::DispatchRaysArguments args;
	args.width = m_Resolution.x;
	args.height = m_Resolution.y;
	m_CommandList->dispatchRays(args);

	m_CommonPasses->BlitTexture(m_CommandList, framebuffer, m_RenderTarget, m_BindingCache.get());

	m_CommandList->close();
	GetDevice()->executeCommandList(m_CommandList);
}
