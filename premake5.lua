IncludeDir["ImGui"] = "%{HE}/Plugins/HEImGui/imgui"

function Link.Plugin.ImGui()

    includedirs {

        "%{IncludeDir.ImGui}",
    }

    links {

        "ImGui",
    }
end

function Link.Plugin.ImGuiLayer()

    includedirs {

        "%{HE}/Plugins/HEImGui/Source/HEImGui",
    }

    links {

        "HEImGui",
    }
end

group "Plugins/imgui"
    include "imgui"

    project "HEImGui"
        language "C++"
        cppdialect  "C++latest"
        implibdir "%{cfg.objdir}"

        ProjectKind("SharedLib")
        filter "kind:SharedLib"
            targetdir ("Binaries/" .. outputdir)
            objdir ("Binaries/Intermediates/" .. outputdir)
        filter "kind:StaticLib"
            targetdir (libOutputDir)
            objdir (IntermediatesOutputDir)
        filter {}
   
        Link.Runtime.Core()

        files
        {
            "Source/**.h",
            "Source/**.cpp",
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