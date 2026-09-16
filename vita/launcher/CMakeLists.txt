# Minimal Vita launcher adapted from the working PrBoom+ Vita launcher.
#
# The launcher is deliberately kept separate from UZDoom's SDL/VitaGL
# process.  It only draws a small menu with vita2d and replaces itself with
# the engine through sceAppMgrLoadExec().

set(VLAUNCHER_SOURCES
	input.c
	main.c
	screen.c
)

add_executable(vlauncher.elf ${VLAUNCHER_SOURCES})

target_compile_options(vlauncher.elf PRIVATE
	-std=gnu99
	-Wno-declaration-after-statement
	-Wno-format-truncation
)

# Keep the same Vita2D/SDK dependency set as the known-good PrBoom+ launcher.
# The launcher does not use SDL, so it does not inherit the much larger engine
# dependency graph or its renderer initialization cost.
target_link_libraries(vlauncher.elf PRIVATE vita2d png16 jpeg freetype z m c)
target_link_libraries(vlauncher.elf PRIVATE
	SceLibKernel_stub
	SceAppMgr_stub
	SceSysmodule_stub
	SceCtrl_stub
	SceTouch_stub
	SceAppUtil_stub
	ScePower_stub
	SceCommonDialog_stub
	SceAudio_stub
	SceGxm_stub
	SceDisplay_stub
	SceHid_stub
	ScePgf_stub
)

vita_create_self(vlauncher.self vlauncher.elf)
