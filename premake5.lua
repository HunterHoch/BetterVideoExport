workspace "BetterVideoExport"
    configurations { "Release", "Debug" }
    platforms { "x64" }
    location "build"

project "BetterVideoExport"
    kind "SharedLib"
    language "C++"
    cppdialect "C++17"
    architecture "x86_64"
    targetextension ".asi"
    staticruntime "On"   -- avoid requiring the target FiveM process VC++ DLL versions

    files { "src/**.cpp", "src/**.c", "src/**.h", "fx_asi_build.rc" }
    includedirs { "src", "src/MinHook", "src/spdlog", "src/hooking" }
    buildoptions { "/utf-8" }   -- spdlog/fmt requires it

    filter "configurations:Debug"
        symbols "On"
    filter "configurations:Release"
        optimize "On"
