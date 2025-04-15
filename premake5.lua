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

    files
    {
        "Source/**.h",
        "Source/**.cpp",
        "Source/**.cppm",
        "Source/**.hlsl",
        "*.lua",
    }
    
    defines
    {
         "HE_CORE_IMPORT_SHAREDLIB",
         "NVRHI_SHARED_LIBRARY_INCLUDE",
         "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
         "GLFW_DLL",
    }
    
    includedirs
    {
       "Source",
       "%{IncludeDir.HydraEngine}",
       "%{IncludeDir.imgui}",
       "%{IncludeDir.glfw}",
    }
    
    links
    {
       "HydraEngine",
       "glfw",
       "nvrhi",
       "imgui",
    }

    buildoptions 
    {
        AddCppm("std"),
        AddCppm("HydraEngine"),
        AddCppm("nvrhi")
    }

     filter { "files:**.hlsl" }
        buildcommands {
            BuildShaders(
                { D3D11 = true, D3D12 = true, VULKAN = true },  -- api
                "%{prj.location}/Source/Shaders",               -- sourceDir
                "%{prj.location}/Source/HEImGui/Embeded",       -- cacheDir
                "--header",                                     -- args
                {}
            ),
        }
        buildoutputs { "%{wks.location}/dumy" }
    filter {}
    
    filter "system:windows"
        systemversion "latest"
        defines 
        {
        	"NVRHI_HAS_D3D11",
        	"NVRHI_HAS_D3D12",
        	"NVRHI_HAS_VULKAN",
        }

    filter "system:linux"
		pic "On"
		systemversion "latest"
        defines
        {
        	"NVRHI_HAS_VULKAN",
        }
     
    filter "configurations:Debug"
     	defines "HE_DEBUG"
     	runtime "Debug"
     	symbols "On"
     
    filter "configurations:Release"
     	defines "HE_RELEASE"
     	runtime "Release"
     	optimize "On"
     
    filter "configurations:Profile"
        includedirs { "%{IncludeDir.tracy}" }
        defines { "HE_PROFILE", "TRACY_IMPORTS" }
        runtime "Release"
        optimize "On"

    filter "configurations:Dist"
     	defines "HE_DIST"
     	runtime "Release"
     	optimize "Speed"
        symbols "Off"

group "Plugins"