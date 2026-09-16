#pragma once

#include <vita2d.h>

#define VITA_LAUNCHER_WIDTH 960
#define VITA_LAUNCHER_HEIGHT 544

int VitaLauncherScreenInit(void);
void VitaLauncherScreenFree(void);
void VitaLauncherScreenBegin(void);
void VitaLauncherScreenEnd(void);
void VitaLauncherScreenText(int x, int y, float scale, unsigned int color,
	const char *text);
void VitaLauncherScreenRect(int x, int y, int width, int height,
	unsigned int color);
