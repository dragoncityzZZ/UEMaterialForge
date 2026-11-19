cmake_minimum_required(VERSION 3.21)

project(MaterialForge VERSION 2.6.5 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MF_STATIC_RUNTIME "Link the MSVC runtime statically (/MT)" OFF)
option(MF_WIN32_GUI "Build a Windows GUI-subsystem executable" OFF)

if(MSVC)
    add_compile_options(/utf-8 /MP)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MF_STATIC_RUNTIME)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/bin")
endforeach()

# ---------------------------------------------------------------------------
# Third-party dependencies (vendored, no network required)
# ---------------------------------------------------------------------------
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/glfw EXCLUDE_FROM_ALL)

add_library(glad STATIC third_party/glad/src/gl.c)
target_include_directories(glad PUBLIC third_party/glad/include)

add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/misc/imgui_stdlib.cpp
    third_party/imgui/backends/imgui_impl_glfw.cpp
    third_party/imgui/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    third_party/imgui
    third_party/imgui/backends
    third_party/imgui/misc
)
target_link_libraries(imgui PUBLIC glfw)

add_library(imgui_node_editor STATIC
    third_party/imgui-node-editor/crude_json.cpp
    third_party/imgui-node-editor/imgui_canvas.cpp
    third_party/imgui-node-editor/imgui_node_editor.cpp
    third_party/imgui-node-editor/imgui_node_editor_api.cpp
)
target_include_directories(imgui_node_editor PUBLIC third_party/imgui-node-editor)
target_link_libraries(imgui_node_editor PUBLIC imgui)

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE third_party/json/single_include)

add_library(stb INTERFACE)
target_include_directories(stb INTERFACE third_party/stb)

find_package(OpenGL REQUIRED)

# ---------------------------------------------------------------------------
# Core (no OpenGL / ImGui dependency): graph, types, codegen, JSON
# ---------------------------------------------------------------------------
add_library(material_core STATIC
    src/core/Types.cpp
    src/core/Property.cpp
    src/core/Node.cpp
    src/core/Graph.cpp
    src/core/NodeRegistry.cpp
    src/core/TypeResolver.cpp
    src/core/MaterialParameters.cpp
    src/core/MaterialInstance.cpp
    src/core/TextureSettings.cpp
    src/core/DefaultGraph.cpp
    src/codegen/GlslGenerator.cpp
    src/io/GraphSerializer.cpp
    src/io/Ue5Clipboard.cpp
    src/project/Project.cpp
    src/project/UeSync.cpp
)
target_include_directories(material_core PUBLIC src)
target_link_libraries(material_core PUBLIC nlohmann_json)
target_compile_features(material_core PUBLIC cxx_std_17)

# ---------------------------------------------------------------------------
# Renderer: OpenGL preview, meshes, shaders, textures
# ---------------------------------------------------------------------------
add_library(material_render STATIC
    src/render/ShaderProgram.cpp
    src/render/Mesh.cpp
    src/render/OpenGLPreviewRenderer.cpp
    src/render/PreviewRenderer.cpp
    src/render/MaterialThumbnailRenderer.cpp
    src/render/HlslPreviewShader.cpp
    src/io/TextureLoader.cpp
)
target_include_directories(material_render PUBLIC src)
target_compile_definitions(material_render PUBLIC GLFW_INCLUDE_NONE)
target_link_libraries(material_render
    PUBLIC material_core
    PRIVATE glad glfw OpenGL::GL stb
)
if(WIN32)
    target_sources(material_render PRIVATE
        src/render/D3D12PreviewRenderer.cpp
        src/render/VulkanPreviewRenderer.cpp
    )
    target_include_directories(material_render PRIVATE third_party/vulkan/include)
    target_link_libraries(material_render PRIVATE d3d12 dxgi d3dcompiler)
endif()

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
add_executable(MaterialForge
    src/main.cpp
    src/app/Application.cpp
    src/app/AppSettings.cpp
    src/app/Localization.cpp
    src/app/NodeGraphView.cpp
    src/app/FileBrowser.cpp
    src/app/McpServer.cpp
    src/app/PluginManager.cpp
    src/app/UiWidgets.cpp
    src/util/PathUtil.cpp
)
target_include_directories(MaterialForge PRIVATE src)
target_compile_definitions(MaterialForge PRIVATE
    GLFW_INCLUDE_NONE
    MF_VERSION_STRING="${PROJECT_VERSION}"
)
target_link_libraries(MaterialForge PRIVATE
    material_core
    material_render
    imgui
    imgui_node_editor
    glfw
    glad
    OpenGL::GL
    stb
)
set_target_properties(MaterialForge PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

if(WIN32)
    target_sources(MaterialForge PRIVATE resources/MaterialForge.rc)
    target_include_directories(MaterialForge PRIVATE "${CMAKE_SOURCE_DIR}/resources")
    target_link_libraries(MaterialForge PRIVATE shell32 ole32 uuid ws2_32)
    if(MF_WIN32_GUI)
        set_target_properties(MaterialForge PROPERTIES WIN32_EXECUTABLE TRUE)
        target_compile_definitions(MaterialForge PRIVATE MF_WINDOWS_GUI)
    endif()
endif()

# Copy runtime content next to the executable so the app also works when
# launched by double-clicking it.
add_custom_command(TARGET MaterialForge POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:MaterialForge>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/examples" "$<TARGET_FILE_DIR:MaterialForge>/examples"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/third_party/glslang/bin"
            "$<TARGET_FILE_DIR:MaterialForge>/tools/glslang"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/third_party/vulkan/bin/vulkan-1.dll"
            "$<TARGET_FILE_DIR:MaterialForge>/vulkan-1.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/third_party/d3dcompiler/bin/d3dcompiler_47.dll"
            "$<TARGET_FILE_DIR:MaterialForge>/d3dcompiler_47.dll"
    COMMENT "Copying MaterialForge runtime content"
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
option(MF_BUILD_TESTS "Build MaterialForge unit tests" ON)
if(MF_BUILD_TESTS)
    enable_testing()

    add_executable(material_tests
        tests/test_core.cpp
    )
    target_link_libraries(material_tests PRIVATE material_core)
    target_include_directories(material_tests PRIVATE src)
    add_test(NAME material_core COMMAND material_tests)
    set_tests_properties(material_core PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    add_executable(shader_smoke_test
        tests/test_shader_compile.cpp
    )
    target_link_libraries(shader_smoke_test PRIVATE material_render glad glfw OpenGL::GL stb)
    target_compile_definitions(shader_smoke_test PRIVATE GLFW_INCLUDE_NONE)
    if(WIN32)
        target_link_libraries(shader_smoke_test PRIVATE d3dcompiler)
    endif()
    add_test(NAME shader_smoke COMMAND shader_smoke_test)
    set_tests_properties(shader_smoke PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    add_executable(shader_dump
        tests/dump_shaders.cpp
    )
    target_link_libraries(shader_dump PRIVATE material_render glad glfw OpenGL::GL stb)
    target_compile_definitions(shader_dump PRIVATE GLFW_INCLUDE_NONE)

    add_executable(app_settings_tests
        tests/test_app_settings.cpp
        src/app/AppSettings.cpp
    )
    target_include_directories(app_settings_tests PRIVATE src)
    target_link_libraries(app_settings_tests PRIVATE nlohmann_json)
    add_test(NAME app_settings COMMAND app_settings_tests)

    add_executable(ue5_clipboard_tests
        tests/test_ue5_clipboard.cpp
    )
    target_link_libraries(ue5_clipboard_tests PRIVATE material_core)
    target_include_directories(ue5_clipboard_tests PRIVATE src)
    add_test(NAME ue5_clipboard COMMAND ue5_clipboard_tests)
    set_tests_properties(ue5_clipboard PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

# Development helper: regenerates the built-in material function presets.
add_executable(generate_function_presets
    tools/generate_function_presets.cpp
    src/util/PathUtil.cpp
)
target_include_directories(generate_function_presets PRIVATE src)
target_link_libraries(generate_function_presets PRIVATE material_core)

