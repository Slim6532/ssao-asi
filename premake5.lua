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
	-- compile the SSAO shaders into headers (fxc comes with the Windows SDK)
	prebuildcommands {
		'fxc /nologo /O3 /T vs_3_0 /E VSMain /Vn g_vsQuad /Fh ..\\src\\ssao_vs.h ..\\src\\ssao.hlsl',
		'fxc /nologo /O3 /T ps_3_0 /E PSAO /Vn g_psAO0 /D NDIR=3 /D NSTEP=3 /Fh ..\\src\\ssao_ps_ao0.h ..\\src\\ssao.hlsl',
		'fxc /nologo /O3 /T ps_3_0 /E PSAO /Vn g_psAO1 /D NDIR=4 /D NSTEP=4 /Fh ..\\src\\ssao_ps_ao1.h ..\\src\\ssao.hlsl',
		'fxc /nologo /O3 /T ps_3_0 /E PSAO /Vn g_psAO2 /D NDIR=6 /D NSTEP=5 /Fh ..\\src\\ssao_ps_ao2.h ..\\src\\ssao.hlsl',
		'fxc /nologo /O3 /T ps_3_0 /E PSBlur /Vn g_psBlur /Fh ..\\src\\ssao_ps_blur.h ..\\src\\ssao.hlsl',
		'fxc /nologo /O3 /T ps_3_0 /E PSComp /Vn g_psComp /Fh ..\\src\\ssao_ps_comp.h ..\\src\\ssao.hlsl',
		'fxc /nologo /O3 /T vs_3_0 /E VSMain /Vn g_vsPost /Fh ..\\src\\post_vs.h ..\\src\\post.hlsl',
		'fxc /nologo /O3 /T ps_3_0 /E PSPost /Vn g_psPost /Fh ..\\src\\post_ps.h ..\\src\\post.hlsl'
	}

	filter "configurations:Release"
		defines { "NDEBUG" }
		optimize "On"
	filter "configurations:Debug"
		defines { "DEBUG" }
		symbols "On"
