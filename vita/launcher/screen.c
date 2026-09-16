#include "screen.h"

#include <stdarg.h>
#include <stdio.h>

#include <vitasdk.h>

#define LAUNCHER_BLACK RGBA8(8, 10, 14, 255)

static vita2d_pgf *MainFont;
static int Drawing;

int VitaLauncherScreenInit(void)
{
	// These are the same lightweight Vita services initialized by the
	// PrBoom+ launcher before vita2d.  No SDL or game resource is loaded here.
	sceAppUtilInit(&(SceAppUtilInitParam){}, &(SceAppUtilBootParam){});
	sceCommonDialogSetConfigParam(&(SceCommonDialogConfigParam){});

	vita2d_init();
	vita2d_set_clear_color(LAUNCHER_BLACK);
	MainFont = vita2d_load_default_pgf();
	if (MainFont == NULL)
	{
		VitaLauncherScreenFree();
		return -1;
	}
	return 0;
}

void VitaLauncherScreenFree(void)
{
	if (MainFont != NULL)
	{
		vita2d_free_pgf(MainFont);
		MainFont = NULL;
	}
	vita2d_fini();
	Drawing = 0;
}

void VitaLauncherScreenBegin(void)
{
	if (Drawing)
		VitaLauncherScreenEnd();
	vita2d_start_drawing();
	Drawing = 1;
	vita2d_set_clear_color(LAUNCHER_BLACK);
	vita2d_clear_screen();
}

void VitaLauncherScreenEnd(void)
{
	if (!Drawing)
		return;
	vita2d_end_drawing();
	vita2d_common_dialog_update();
	vita2d_wait_rendering_done();
	vita2d_swap_buffers();
	Drawing = 0;
}

void VitaLauncherScreenText(int x, int y, float scale, unsigned int color,
	const char *text)
{
	if (MainFont != NULL)
		vita2d_pgf_draw_text(MainFont, x, y, color, scale, text);
}

void VitaLauncherScreenRect(int x, int y, int width, int height,
	unsigned int color)
{
	vita2d_draw_rectangle(x, y, width, height, color);
}
