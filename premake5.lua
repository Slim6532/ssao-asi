workspace "dualpass"
	configurations { "Release", "Debug" }
	location "build"
	platforms { "x32" }

	includedirs { "src", os.getenv("RWSDK36") }
	files { "src/*.*" }
	removefiles { "src/*.hlsl" }

project "dualpass"
	kind "SharedLib"
	language "C++"
	targetdir "bin/%{cfg.buildcfg}"
	targetextension ".asi"
	characterset "MBCS"
	buildoptions { "/Zc:threadSafeInit-" }
	flags { "StaticRuntime" }
	-- compile the AO shaders into headers (fxc comes with the Windows SDK)
	prebuildcommands {
		'fxc /nologo /O3 /T vs_3_0 /E VSMain /Vn g_vsQuad /Fh ..\\src\\ssao_vs.h ..\\src\\ssao.hlsl',
		'fxc /nologo /O3 /T ps_3_0 /E PSAO /Vn g_psAO0 /D NDIR=4 /D NSTEP=4 /Fh ..\\src\\ssao_ps_ao0.h ..\\src\\ssao.hlsl',
		'fxc /nologo /O3 /T ps_3_0 /E PSAO /Vn g_psAO1 /D NDIR=8 /D NSTEP=4 /Fh ..\\src\\ssao_ps_ao1.h ..\\src\\ssao.hlsl',
		'fxc /nologo /O3 /T ps_3_0 /E PSAO /Vn g_psAO2 /D NDIR=8 /D NSTEP=4 /Fh ..\\src\\ssao_ps_ao2.h ..\\src\\ssao.hlsl',
		'fxc /nologo /O3 /T ps_3_0 /E PSSao /Vn g_psSao /Fh ..\\src\\ssao_ps_sao.h ..\\src\\ssao.hlsl',
		'fxc /nologo /O3 /T ps_3_0 /E PSBlur /Vn g_psBlur /Fh ..\\src\\ssao_ps_blur.h ..\\src\\ssao.hlsl',
		'fxc /nologo /O3 /T ps_3_0 /E PSComp /Vn g_psComp /Fh ..\\src\\ssao_ps_comp.h ..\\src\\ssao.hlsl',
	}

	filter "configurations:Release"
		defines { "NDEBUG" }
		optimize "On"
	filter "configurations:Debug"
		defines { "DEBUG" }
		symbols "On"
