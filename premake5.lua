IncludeDir["ImGui"] = "%{HE}/Plugins/HEImGui/imgui"

function Link.Plugin.ImGui()

    buildoptions {

        AddProjCppm(HE, "imgui"),
    }

    includedirs {

        "%{IncludeDir.ImGui}",
    }

    links {

        "ImGui",
    }
end

group "Plugins/imgui"
    include "imgui"

    project "HEImGui"
        kind "SharedLib"
        language "C++"
        cppdialect  "C++latest"
        staticruntime "Off"
        targetdir ("Binaries/" .. outputdir)
        objdir ("Binaries/Intermediates/" .. outputdir)
   
        Link.Runtime.Core()

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
           "%{IncludeDir.ImGui}",
           "%{IncludeDir.glfw}",
        }

        links
        {
           "imgui",
           "glfw",
        }

        SetupShaders(
            { D3D11 = true, D3D12 = true, VULKAN = true },  -- api
            "%{prj.location}/Source/Shaders",               -- sourceDir
            "%{prj.location}/Source/HEImGui/Embeded",       -- cacheDir
            "--header"                                      -- args
        )


group "Plugins"