#include <donut/shaders/view_cb.h>
#include <donut/shaders/light_cb.h>

#ifndef SHARED_SHADER_DATA
#define SHARED_SHADER_DATA

//Constatn Buffer used in RaytraceWorld
struct ConstBuffer {
	PlanarViewConstants viewConstants;
	LightConstants directionalLightConstants;

	float ambient;
	float emissiveStrength;
	float cameraNear;
	float cameraFar;

	float ambientSpecular;
	float shadowRayOffset;
	float2 padding;
};

struct CBMetalRoughTexGen {
	int convertShininessToRoughness;
	int metallicValid;
	uint2 texDimensions;
};

//Vertex format used in the buffer for alignment
struct VertexData
{
	float3 position;
	float uvX;
	float3 normal;
	float uvY;
};

//Vertex format used in shader
struct Vertex {
	float3 position;
	float3 normal;
	float2 uv;

	VertexData getData() {
		VertexData data;
		data.position = position;
		data.normal = normal;
		data.uvX = uv.x;
		data.uvY = uv.y;
		return data;
	}
};

static Vertex getVertex(VertexData data) {
	Vertex vert;
	vert.position = data.position;
	vert.normal = data.normal;
	vert.uv = float2(data.uvX, data.uvY);
	return vert;
}

//AABB buffer as defined by API
struct AABB
{
	float3 min;
	float3 max;
};

struct AABBMaterials {
	int negXMatID;
	int posXMatID;
	int negZMatID;
	int posZMatID;

	int negYMatID;
	int posYMatID;
	int2 padding;
};

#endif // !USE_SHARED_SHADER_DATA

