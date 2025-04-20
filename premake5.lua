IncludeDir["imgui"] = path.getabsolute(".") .. "/imgui"

group "Plugins/imgui"
include "imgui"

project "HEImGui"
    kind "SharedLib"
    language "C++"
    cppdialect  "C++latest"
    staticruntime "Off"
    targetdir ("Binaries/" .. outputdir)
    objdir ("Binaries/Intermediates/" .. outputdir)
   
    LinkHydra(includSourceCode, { "glfw" })

    files
    {
        "Source/**.h",
        "Source/**.cpp",
        "Source/**.cppm",
        "Source/**.hlsl",
        "*.lua",
    }
    
    includedirs
    {
       "Source",
       "%{IncludeDir.imgui}",
       "%{IncludeDir.glfw}",
    }

    links
    {
       "imgui",
    }

    SetupShaders(
        { D3D11 = true, D3D12 = true, VULKAN = true },  -- api
        "%{prj.location}/Source/Shaders",               -- sourceDir
        "%{prj.location}/Source/HEImGui/Embeded",       -- cacheDir
        "--header"                                      -- args
    )

    SetHydraFilters()

group "Plugins"