#include "RendererUI.h"
#include <cfloat>
#include <donut/core/math/basics.h>
#include <cmath>

static const float k_PI_2 = dm::PI_f / 2.f;

//Helper functions for indenting imgui
void IndentFloat(const char* name, const char* tag, float* data, float v_speed = 1.f, float v_min = 0.f, float v_max = 1.f, const char* format = "%.3f") {
	ImGui::Text(name);
	ImGui::Indent();
	ImGui::DragFloat(tag, data, v_speed, v_min, v_max, format);
	ImGui::Unindent();
}

void IndentFloat2(const char* name, const char* tag, float* data, float v_speed = 1.f, float v_min = 0.f, float v_max = 1.f, const char* format = "%.3f") {
	ImGui::Text(name);
	ImGui::Indent();
	ImGui::DragFloat2(tag, data, v_speed, v_min, v_max, format);
	ImGui::Unindent();
}

void IndentFloat3(const char* name, const char* tag, float* data, float v_speed = 1.f, float v_min = 0.f, float v_max = 1.f, const char* format = "%.3f") {
	ImGui::Text(name);
	ImGui::Indent();
	ImGui::DragFloat3(tag, data, v_speed, v_min, v_max, format);
	ImGui::Unindent();
}

//Polar coordinates helper (direction is normalized -> r==1)
inline float2 CartesianToPolar(float3 direction) {
	direction = normalize(direction);
	float tan = acos(direction.y) - k_PI_2;
	float phi = acos(direction.x / (sqrt(direction.x * direction.x + direction.z * direction.z) + 1e-7f));
	if (direction.y > 0) phi *= -1;
	
	return float2(tan, phi);
}

inline float3 PolarToCartesian(float2 polar) {
	polar.x += k_PI_2;
	return float3(sin(polar.x) * cos(polar.y), cos(polar.x), sin(polar.x) * sin(polar.y));
}

void UserInterface::buildUI()
{
	ImGui::SetNextWindowPos(ImVec2(10.f, 10.f), 0);
	ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

	ImGui::Text("%s, %s", GetDeviceManager()->GetRendererString(), m_renderer->GetResolutionInfo().c_str());
	ImGui::Text(m_renderer->GetFPSInfo().c_str());

	//Select Scene
	const std::vector<std::string>& availableScenes = m_renderer->GetAvailableScenes();
	static int selectedScene = 0;
	if (!availableScenes.empty()) {
		ImGui::Text("Currently selected scene:");
		ImGui::Indent();
		if (m_ui->selectedScene >= 0)
			ImGui::Text(availableScenes[m_ui->selectedScene].c_str());
		else
			ImGui::Text("No Scene selected");
		ImGui::Unindent();

		if (availableScenes.size() > 1) {
			if (ImGui::BeginCombo("Scene", availableScenes[selectedScene].c_str())) {
				for (int i = 0; i < availableScenes.size(); i++) {
					bool is_selected = i == selectedScene;
					if (ImGui::Selectable(availableScenes[i].c_str(), is_selected))
						selectedScene = i;
					if (is_selected)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
		}

	}
	else {
		ImGui::Text("WARNING: No Scene found");
		ImGui::Text("Please add a \"Mineways\" .obj scene");
		ImGui::Text("to the \"MinecraftModels\" folder.");
		ImGui::End();
		return;
	}

	if (availableScenes.size() > 1) {
		if (ImGui::Button("Load Scene"))
			m_ui->selectedScene = selectedScene;
	}

	//Settings
	
	if (ImGui::CollapsingHeader("Directional Light")) //, ImGuiTreeNodeFlags_DefaultOpen))
	{
		static bool asPolar = true;
		if (asPolar) {
			static float2 polar = CartesianToPolar(m_ui->lightDirection);
			ImGui::Text("Light Direction (Polar):");
			ImGui::Indent();
			ImGui::DragFloat("##LightDirectionPolarTheta", &polar.x, 0.0001f, 0.f, k_PI_2, " % .4f");
			ImGui::SameLine();
			ImGui::DragFloat("##LightDirectionPolarPhi", &polar.y, 0.0001f, -FLT_MAX, FLT_MAX, " % .4f");
			ImGui::Unindent();
			m_ui->lightDirection = PolarToCartesian(polar);
		}
		else {
			IndentFloat3("Light Direction:", "##LightDirection", &m_ui->lightDirection.x, 0.001f, -FLT_MAX, FLT_MAX, " % .3f");
		}
		ImGui::Checkbox("Polar Light Direction Control", &asPolar);
		
		
		IndentFloat("Light Intensity:", "##LightIntensity", &m_ui->lightIntensity, 0.01f, 0.f, FLT_MAX, " % .2f");
	}

	if (ImGui::CollapsingHeader("Camera")) //, ImGuiTreeNodeFlags_DefaultOpen))
	{
		IndentFloat("Speed:", "##CameraSpeed", &m_ui->cameraSpeed, 0.01f, 0.f, FLT_MAX, " % .3f");
		IndentFloat("FOV:", "##CameraFOV", &m_ui->cameraFov, 0.0001f, 0.f, dm::PI_f, " % .4f");
		IndentFloat("Near:", "##CameraNear", &m_ui->cameraNear, 0.001f, 0.f, FLT_MAX, " % .3f");
		IndentFloat("Far:", "##CameraFar", &m_ui->cameraFar, 0.001f, 0.f, FLT_MAX, " % .3f");
		if (ImGui::Button("Reset Camera Position")) {
			m_renderer->ResetCameraPosition();
		}
	}

	if (ImGui::CollapsingHeader("Shading")) //, ImGuiTreeNodeFlags_DefaultOpen))
	{
		IndentFloat("Ambient:", "##Ambient", &m_ui->ambient, 0.0001f, 0.f, 1.f, " % .4f");

		IndentFloat("Emissive Strength:", "##EmissiveStrength", &m_ui->emissiveStrength, 0.01f, 0.f, FLT_MAX, " % .2f");

		IndentFloat("Specular Ambient", "##SpecularAmbient", &m_ui->ambientSpecularStrength, 0.0001f, 0.f, FLT_MAX, " % .4f");
	}

	if (ImGui::CollapsingHeader("Shadow")) //, ImGuiTreeNodeFlags_DefaultOpen))
	{
		IndentFloat("Ray Shadow Offset", "##RayShadowOffset", &m_ui->shadowRayBias, 0.0000001f, 0.f, FLT_MAX, " % .8f");
	}

	// End of window
	ImGui::End();
}