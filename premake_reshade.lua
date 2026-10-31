workspace "VYNE_BakeFX"
    configurations { "Release", "Debug" }
    platforms { "x64" }
    location "build-addon"

project "VYNE_BakeFX"
    kind "SharedLib"
    language "C++"
    cppdialect "C++17"
    architecture "x86_64"
    targetextension ".addon64"
    staticruntime "On"
    files {
        "addon/**.cpp",
        "src/NativeBakeBridge.h",
        "_deps/reshade/examples/utils/state_tracking.cpp"
    }
    includedirs {
        "_deps/reshade/include",
        "_deps/reshade/examples/utils",
        "src"
    }
    defines { "WIN32_LEAN_AND_MEAN", "NOMINMAX", "UNICODE", "_UNICODE" }
    buildoptions { "/utf-8" }

    filter "configurations:Debug"
        symbols "On"
    filter "configurations:Release"
        optimize "On"
