#include <donut/app/imgui_renderer.h>
#include "Renderer.h"


class UserInterface : public app::ImGui_Renderer
{
public:
	UserInterface(app::DeviceManager* deviceManager, UIData* ui, Renderer* renderer)
		: ImGui_Renderer(deviceManager)
		, m_ui(ui), m_renderer(renderer)
	{
		ImGui::GetIO().IniFilename = nullptr;
	}

	void buildUI() override;

private:
	UIData* m_ui;
	Renderer* m_renderer;
};