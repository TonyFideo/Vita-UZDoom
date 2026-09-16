/*
** Vita ZMusic compatibility.
**
** ZMusic's public configuration surface still references FluidSynth even
** when ZMUSIC_ENABLE_FLUIDSYNTH is disabled. Vita deliberately ships the
** lighter non-FluidSynth backend, so provide the two ABI symbols that the
** shared ZMusic sources expect without pulling in the FluidSynth library.
*/

#include "../../../../libraries/ZMusic/source/zmusic/midiconfig.h"
#include <stdexcept>

class MIDIDevice;

FluidConfig fluidConfig;

MIDIDevice *CreateFluidSynthMIDIDevice(int, const char *)
{
	// ZMusic's device selector treats a null result as an unfinished device
	// attempt and retries the same backend forever.  Report the intentionally
	// unavailable Vita backend as a recoverable failure so its normal fallback
	// chain can select OPL/Timidity instead.
	throw std::runtime_error("FluidSynth is not available on Vita");
}
