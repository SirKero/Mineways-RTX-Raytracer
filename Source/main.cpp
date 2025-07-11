#include <nvrhi/nvrhi.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include "Renderer.h"
#include "RendererUI.h"

#ifdef WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
#else
int main(int __argc, const char** __argv)
#endif //WIN32
{
	nvrhi::GraphicsAPI api = app::GetGraphicsAPIFromCommandLine(__argc, __argv);
	app::DeviceManager* deviceManager = app::DeviceManager::Create(api);

	app::DeviceCreationParameters deviceParams;
	deviceParams.enableRayTracingExtensions = true;

#ifdef _DEBUG
	deviceParams.enableDebugRuntime = true;
	deviceParams.enableNvrhiValidationLayer = true;
#endif // _DEBUG

	if (!deviceManager->CreateWindowDeviceAndSwapChain(deviceParams, g_WindowTitle))
	{
		log::fatal("Cannot initialize a graphics device with the requested parameters");
		return 1;
	}

	if (!deviceManager->GetDevice()->queryFeatureSupport(nvrhi::Feature::RayTracingPipeline))
	{
		log::fatal("The graphics device does not support Ray Tracing Pipelines");
		return 1;
	}

	{
		UIData uiData;

		Renderer voxelTest(deviceManager, &uiData);
		UserInterface gui(deviceManager, &uiData, &voxelTest);

		if (voxelTest.Init() && gui.Init(voxelTest.GetShaderFactory()))
		{
			deviceManager->AddRenderPassToBack(&voxelTest);
			deviceManager->AddRenderPassToBack(&gui);
			deviceManager->RunMessageLoop();
			deviceManager->RemoveRenderPass(&gui);
			deviceManager->RemoveRenderPass(&voxelTest);
		}
	}

	deviceManager->Shutdown();

	delete deviceManager;

	return 0;
}