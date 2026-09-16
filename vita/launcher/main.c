#include "input.h"
#include "screen.h"

#include <stdio.h>

#include <vitasdk.h>

#define COLOR_WHITE RGBA8(235, 238, 245, 255)
#define COLOR_GREY RGBA8(145, 151, 164, 255)
#define COLOR_BLUE RGBA8(88, 170, 255, 255)
#define COLOR_GREEN RGBA8(110, 220, 145, 255)
#define COLOR_RED RGBA8(245, 100, 100, 255)
#define COLOR_PANEL RGBA8(20, 25, 35, 235)
#define COLOR_SELECTED RGBA8(35, 70, 105, 255)

static const char *const MenuItems[] =
{
	"Doom II - Software renderer",
	"Doom II - GLES2 / VitaGL (experimental)",
	"Exit"
};

static void DrawCenteredText(int y, float scale, unsigned int color,
	const char *text)
{
	// The default PGF is small enough that the fixed center is sufficient for
	// this menu.  The menu remains legible on the native 960x544 display.
	VitaLauncherScreenText(100, y, scale, color, text);
}

static void DrawMenu(int selection, int error_code, int launch_pending)
{
	VitaLauncherScreenBegin();

	VitaLauncherScreenRect(70, 55, 820, 430, COLOR_PANEL);
	DrawCenteredText(115, 2.0f, COLOR_BLUE, "UZDOOM VITA");
	DrawCenteredText(146, 1.0f, COLOR_GREY,
		"Select a renderer and launch directly into Doom II MAP01");

	for (int i = 0; i < (int)(sizeof(MenuItems) / sizeof(MenuItems[0])); ++i)
	{
		const int y = 205 + i * 58;
		const unsigned int color = (selection == i) ? COLOR_WHITE : COLOR_GREY;
		if (selection == i)
			VitaLauncherScreenRect(130, y - 26, 700, 38, COLOR_SELECTED);
		VitaLauncherScreenText(155, y, 1.25f, color, MenuItems[i]);
	}

	if (launch_pending)
	{
		DrawCenteredText(410, 1.0f, COLOR_GREEN,
			"Launching UZDoom...");
	}
	else if (error_code < 0)
	{
		char error_text[96];
		snprintf(error_text, sizeof(error_text),
			"sceAppMgrLoadExec failed: 0x%08X", (unsigned int)error_code);
		DrawCenteredText(410, 1.0f, COLOR_RED, error_text);
	}
	else
	{
		DrawCenteredText(410, 1.0f, COLOR_GREY,
			"Cross: launch    Circle/Start: exit    D-pad: select");
	}

	VitaLauncherScreenEnd();
}

static int LaunchUZDoom(int hardware_renderer)
{
	// Keep the argument storage static.  sceAppMgrLoadExec consumes it while
	// the current process is being replaced, just as in PrBoom+'s launcher.
	static char renderer_mode[2];
	static char *argv[11];

	renderer_mode[0] = hardware_renderer ? '4' : '0';
	renderer_mode[1] = '\0';

	argv[0] = (char *)"app0:/uzdoom.bin";
	argv[1] = (char *)"-iwad";
	argv[2] = (char *)"app0:/doom2.wad";
	argv[3] = (char *)"+map";
	argv[4] = (char *)"MAP01";
	argv[5] = (char *)"+set";
	argv[6] = (char *)"vid_rendermode";
	argv[7] = renderer_mode;
	argv[8] = NULL;

	printf("Vita launcher: starting %s renderer\n",
		hardware_renderer ? "GLES2/VitaGL" : "software");
	fflush(stdout);

	// Do not free vita2d/GXM state here.  The working PrBoom+ launcher showed
	// that Vita3K may dereference the old renderer while handling the pending
	// process replacement; on hardware LoadExec does not return.
	return sceAppMgrLoadExec("app0:/uzdoom.bin", argv, NULL);
}

int main(void)
{
	// Start on the GLES2/VitaGL entry so a plain Cross press exercises the
	// hardware renderer. Software remains available as the first menu item.
	int selection = 1;
	int error_code = 0;
	int launch_pending = 0;

	if (VitaLauncherScreenInit() < 0)
		sceKernelExitProcess(0);
	if (VitaLauncherInputInit() < 0)
	{
		VitaLauncherScreenFree();
		sceKernelExitProcess(0);
	}

	for (;;)
	{
		VitaLauncherInputUpdate();

		if (!launch_pending)
		{
			if (VitaLauncherButtonPressed(SCE_CTRL_DOWN))
				selection = (selection + 1) % 3;
			else if (VitaLauncherButtonPressed(SCE_CTRL_UP))
				selection = (selection + 2) % 3;

			if (VitaLauncherButtonPressed(SCE_CTRL_CROSS))
			{
				if (selection == 2)
					break;

				error_code = LaunchUZDoom(selection == 1);
				if (error_code >= 0)
					launch_pending = 1;
			}
			else if (VitaLauncherButtonPressed(SCE_CTRL_CIRCLE) ||
				VitaLauncherButtonPressed(SCE_CTRL_START))
			{
				break;
			}
		}

		DrawMenu(selection, error_code, launch_pending);
		sceKernelDelayThread(16000);
	}

	VitaLauncherScreenFree();
	sceKernelExitProcess(0);
	return 0;
}
