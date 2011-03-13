project "3DBox"
	kind "WindowedApp"
	language "C++"

	files { "**.h", "**.cpp" }

	objdir "Build/"
	targetdir "./"

	flags { "StaticRuntime", "NoExceptions", "NoRTTI", "ExtraWarnings" }

	links { "Fuji" }

	dofile "../../Project/fujiconfig.lua"
