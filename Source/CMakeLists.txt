include(../external/donut/compileshaders.cmake)
file(GLOB shaders "*.hlsl")
file(GLOB sources "*.cpp" "*.h")

set(project MinewaysRenderer)
set(folder "Source")

donut_compile_shaders(
    TARGET ${project}_shaders
    PROJECT_NAME "MinewaysRenderer"
    CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/shaders.cfg
    SOURCES ${shaders}
    FOLDER ${folder}
    DXIL ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/${project}/dxil
    SPIRV_DXC ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/${project}/spirv
)

add_executable(${project} WIN32 ${sources})
target_link_libraries(${project} donut_app donut_engine tinyobjloader)
add_dependencies(${project} ${project}_shaders)
set_target_properties(${project} PROPERTIES FOLDER ${folder})