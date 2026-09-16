#include "input.h"

#include <string.h>

#include <vitasdk.h>

static SceCtrlData CurrentPad;
static SceCtrlData PreviousPad;

int VitaLauncherInputInit(void)
{
	memset(&CurrentPad, 0, sizeof(CurrentPad));
	memset(&PreviousPad, 0, sizeof(PreviousPad));

	// The launcher only needs the digital buttons.  Keep the SDK's normal
	// positive sampling path used by the PrBoom+ Vita launcher.
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
	sceCtrlPeekBufferPositive(0, &CurrentPad, 1);
	PreviousPad = CurrentPad;
	return 0;
}

void VitaLauncherInputUpdate(void)
{
	PreviousPad = CurrentPad;
	sceCtrlPeekBufferPositive(0, &CurrentPad, 1);
}

int VitaLauncherButtonPressed(unsigned int button)
{
	return (CurrentPad.buttons & button) != 0 &&
		!(PreviousPad.buttons & button);
}

int VitaLauncherButtonHeld(unsigned int button)
{
	return (CurrentPad.buttons & button) != 0;
}
