-- include subprojects
includes("lib/commonlibf4")

-- set project constants
set_project("commonlibf4-template")
set_version("0.0.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- define targets
target("commonlibf4-template")
    add_rules("commonlibf4.plugin", {
        name = "commonlibf4-template",
        author = "libxse",
        description = "F4SE plugin template using CommonLibF4"
    })

    -- add src files
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

    -- Direct3D 11 for the overlay and blocks, DbgHelp for the crash log
    add_syslinks("d3d11", "d3dcompiler", "dxgi", "dbghelp")

    -- a .pdb next to the build, so SkyCraft_crash.log can name our functions and lines
    set_symbols("debug")
