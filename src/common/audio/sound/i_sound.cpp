/*
** i_sound.cpp
**
** Stubs for sound interfaces.
**
**---------------------------------------------------------------------------
**
** Copyright 1998-2016 Marisa Heit
** Copyright 2008-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Code written prior to 2026 is also licensed under:
**
** SPDX-License-Identifier: BSD-3-Clause
**
**---------------------------------------------------------------------------
**
*/

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <zmusic.h>

#if defined(VITA)
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#endif

#include "c_cvars.h"
#include "cmdlib.h"
#include "i_module.h"
#include "m_argv.h"
#include "oalsound.h"
#include "printf.h"

EXTERN_CVAR (Float, snd_sfxvolume)
EXTERN_CVAR(Float, snd_musicvolume)
CUSTOM_CVAR(Int, snd_samplerate, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self != 0 && self != 8000 && self != 11025 && self != 22050 && self != 32000 && self != 44100 && self != 48000)
	{
		self = 0;
		return;
	}
}
CVAR(Int, snd_buffersize, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, snd_hrtf, -1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

FARG(nomusic, "Configuration", "Turns off in-game music playback.", "",
	"Prevents the playback of music.");
FARG(nosound, "Configuration", "Turns off all in-game sound/music.", "",
	"Disables both music and sound effects.");
FARG(nosfx, "Configuration", "Turns off in-game sound effects.", "",
	"Prevents the playback of sound effects.");

#if defined(VITA)
#define DEF_BACKEND "sdl"
#elif !defined(NO_OPENAL)
#define DEF_BACKEND "openal"
#else
#define DEF_BACKEND "null"
#endif

CVAR(String, snd_backend, DEF_BACKEND, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

SoundRenderer *GSnd;
bool nosound;
bool nosfx;

#if defined(VITA)
namespace
{
struct VitaSfxData
{
	Mix_Chunk *Chunk = nullptr;
	Uint8 *RawBuffer = nullptr;
	unsigned int Samples = 0;
	int Frequency = 0;
};

struct VitaChannelState
{
	FISoundChannel *Channel = nullptr;
	VitaSfxData *Sound = nullptr;
	Uint32 StartTicks = 0;
};

class VitaSoundStream;

// SDL_mixer exposes one custom music hook.  UZDoom also creates one active
// music stream at a time, so keeping the hook global lets the same path serve
// ZMusic, cutscene audio and custom streams without opening a second Vita
// audio device.
static std::atomic<VitaSoundStream *> VitaMusicStream{ nullptr };
static std::atomic<unsigned int> VitaMusicCallbacks{ 0 };
static std::atomic<bool> VitaMixerReady{ false };

static void SDLCALL VitaMusicHook(void *userdata, Uint8 *stream, int len);

class VitaSoundStream final : public SoundStream
{
	SoundStreamCallback Callback = nullptr;
	void *UserData = nullptr;
	SDL_AudioStream *Converter = nullptr;
	std::vector<Uint8> SourceBuffer;

	Uint16 OutputFormat = AUDIO_S16SYS;
	int OutputRate = 0;
	int OutputChannels = 0;
	int OutputFrameBytes = 0;
	int SourceRate = 0;
	int SourceFrameBytes = 0;
	std::atomic<float> StreamVolume{ 1.f };
	std::atomic<float> *RendererMusicVolume = nullptr;
	std::atomic<bool> *RendererMuted = nullptr;
	std::atomic<bool> Playing{ false };
	std::atomic<bool> Paused{ false };
	std::atomic<bool> Ended{ false };
	std::atomic<uint64_t> OutputFramesPlayed{ 0 };

	static Uint16 GetSDLFormat(SampleType type)
	{
		switch (type)
		{
		case SampleType_UInt8: return AUDIO_U8;
		case SampleType_Int16: return AUDIO_S16SYS;
		case SampleType_Float32: return AUDIO_F32SYS;
		default: return 0;
		}
	}

	static int GetChannelCount(ChannelConfig channels)
	{
		switch (channels)
		{
		case ChannelConfig_Mono: return 1;
		case ChannelConfig_Stereo: return 2;
		default: return 0;
		}
	}

	static int GetFormatBytes(Uint16 format)
	{
		const int bits = SDL_AUDIO_BITSIZE(format);
		return bits > 0 ? bits / 8 : 0;
	}

	void ApplyVolume(Uint8 *buffer, int bytes)
	{
		float volume = StreamVolume.load(std::memory_order_relaxed);
		if (RendererMusicVolume != nullptr)
			volume *= RendererMusicVolume->load(std::memory_order_relaxed);
		if (RendererMuted != nullptr && RendererMuted->load(std::memory_order_relaxed))
			volume = 0.f;
		volume = std::max(0.f, std::min(1.f, volume));
		if (volume >= 0.999f) return;

		if (OutputFormat == AUDIO_S16SYS)
		{
			const int count = bytes / int(sizeof(int16_t));
			auto *samples = reinterpret_cast<int16_t *>(buffer);
			for (int i = 0; i < count; ++i)
				samples[i] = int16_t(std::max(-32768.f, std::min(32767.f, samples[i] * volume)));
		}
		else if (OutputFormat == AUDIO_F32SYS)
		{
			const int count = bytes / int(sizeof(float));
			auto *samples = reinterpret_cast<float *>(buffer);
			for (int i = 0; i < count; ++i) samples[i] *= volume;
		}
		else if (OutputFormat == AUDIO_U8)
		{
			const int count = bytes;
			for (int i = 0; i < count; ++i)
				buffer[i] = Uint8(128 + (int(buffer[i]) - 128) * volume);
		}
	}

	void Fill(Uint8 *output, int bytes)
	{
		std::memset(output, 0, size_t(bytes));
		if (!Playing.load(std::memory_order_acquire) || Paused.load(std::memory_order_relaxed) || Converter == nullptr)
			return;

		while (!Ended.load(std::memory_order_relaxed) && SDL_AudioStreamAvailable(Converter) < bytes)
		{
			if (Callback == nullptr || !Callback(this, SourceBuffer.data(), int(SourceBuffer.size()), UserData))
			{
				Ended.store(true, std::memory_order_release);
				SDL_AudioStreamFlush(Converter);
				break;
			}
			if (SDL_AudioStreamPut(Converter, SourceBuffer.data(), int(SourceBuffer.size())) < 0)
			{
				Ended.store(true, std::memory_order_release);
				break;
			}
		}

		int received = SDL_AudioStreamGet(Converter, output, bytes);
		if (received < 0) received = 0;
		if (received < bytes) std::memset(output + received, 0, size_t(bytes - received));
		if (received > 0)
		{
			ApplyVolume(output, received);
			if (OutputFrameBytes > 0 && SourceRate > 0 && OutputRate > 0)
				OutputFramesPlayed.fetch_add(uint64_t(received / OutputFrameBytes), std::memory_order_relaxed);
		}

		if (Ended.load(std::memory_order_acquire) && SDL_AudioStreamAvailable(Converter) == 0 && received < bytes)
			Playing.store(false, std::memory_order_release);
	}

public:
	VitaSoundStream(SoundStreamCallback callback, int buffbytes, SampleType stype, ChannelConfig chans,
		int samplerate, void *userdata, int outputRate, Uint16 outputFormat, int outputChannels,
		std::atomic<float> *musicVolume, std::atomic<bool> *muted)
		: Callback(callback), UserData(userdata), OutputFormat(outputFormat), OutputRate(outputRate),
		  OutputChannels(outputChannels), SourceRate(samplerate), RendererMusicVolume(musicVolume), RendererMuted(muted)
	{
		const Uint16 sourceFormat = GetSDLFormat(stype);
		const int sourceChannels = GetChannelCount(chans);
		const int sourceBytes = GetFormatBytes(sourceFormat);
		const int outputBytes = GetFormatBytes(OutputFormat);
		if (sourceFormat == 0 || sourceChannels <= 0 || sourceBytes <= 0 || outputChannels <= 0 ||
			outputBytes <= 0 || SourceRate <= 0 || OutputRate <= 0)
			return;

		SourceFrameBytes = sourceBytes * sourceChannels;
		OutputFrameBytes = outputBytes * OutputChannels;
		if (buffbytes <= 0) buffbytes = 4096;
		buffbytes = std::max(SourceFrameBytes, buffbytes - (buffbytes % SourceFrameBytes));
		SourceBuffer.resize(size_t(buffbytes));
		Converter = SDL_NewAudioStream(sourceFormat, Uint8(sourceChannels), SourceRate,
			OutputFormat, Uint8(OutputChannels), OutputRate);
		if (Converter == nullptr)
			SourceBuffer.clear();
	}

	~VitaSoundStream() override
	{
		Stop();
		if (Converter != nullptr) SDL_FreeAudioStream(Converter);
	}

	bool IsValid() const { return Converter != nullptr && !SourceBuffer.empty(); }

	bool Play(bool, float volume) override
	{
		if (!IsValid()) return false;
		VitaSoundStream *other = VitaMusicStream.load(std::memory_order_acquire);
		if (other != nullptr && other != this) other->Stop();

		SDL_AudioStreamClear(Converter);
		StreamVolume.store(std::max(0.f, std::min(1.f, volume)), std::memory_order_relaxed);
		OutputFramesPlayed.store(0, std::memory_order_relaxed);
		Ended.store(false, std::memory_order_release);
		Paused.store(false, std::memory_order_release);
		Playing.store(true, std::memory_order_release);
		VitaMusicStream.store(this, std::memory_order_release);
		Mix_HookMusic(VitaMusicHook, this);
		return true;
	}

	void Stop() override
	{
		Playing.store(false, std::memory_order_release);
		Ended.store(true, std::memory_order_release);
		VitaSoundStream *expected = this;
		if (VitaMusicStream.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel) &&
			VitaMixerReady.load(std::memory_order_acquire))
			Mix_HookMusic(nullptr, nullptr);

		while (VitaMusicCallbacks.load(std::memory_order_acquire) != 0)
			SDL_Delay(1);
		if (Converter != nullptr) SDL_AudioStreamClear(Converter);
	}

	void SetVolume(float volume) override
	{
		StreamVolume.store(std::max(0.f, std::min(1.f, volume)), std::memory_order_relaxed);
	}

	bool SetPaused(bool paused) override
	{
		Paused.store(paused, std::memory_order_release);
		return true;
	}

	bool IsEnded() override { return Ended.load(std::memory_order_acquire); }

	Position GetPlayPosition() override
	{
		uint64_t frames = OutputFramesPlayed.load(std::memory_order_relaxed);
		uint64_t sourceSamples = OutputRate > 0 ? frames * uint64_t(SourceRate) / uint64_t(OutputRate) : 0;
		return Position{ sourceSamples, std::chrono::nanoseconds(0) };
	}

	FString GetStats() override
	{
		return FStringf("Vita SDL stream: %d Hz, %s", SourceRate,
			Playing.load(std::memory_order_relaxed) ? (Paused.load(std::memory_order_relaxed) ? "paused" : "playing") : "stopped");
	}

	void MixAudio(Uint8 *output, int bytes) { Fill(output, bytes); }
};

static void SDLCALL VitaMusicHook(void *, Uint8 *stream, int len)
{
	std::memset(stream, 0, size_t(std::max(0, len)));
	VitaSoundStream *soundStream = VitaMusicStream.load(std::memory_order_acquire);
	if (soundStream == nullptr) return;
	VitaMusicCallbacks.fetch_add(1, std::memory_order_acq_rel);
	if (VitaMusicStream.load(std::memory_order_acquire) == soundStream)
		soundStream->MixAudio(stream, len);
	VitaMusicCallbacks.fetch_sub(1, std::memory_order_acq_rel);
}

class VitaSoundRenderer final : public SoundRenderer
{
	static constexpr int MaxChannels = 32;

	bool AudioOpened = false;
	std::atomic<bool> Muted{ false };
	float SfxVolume = 1.f;
	std::atomic<float> MusicVolume{ 1.f };
	int OutputRate = 0;
	Uint16 OutputFormat = AUDIO_S16SYS;
	int OutputChannels = 2;
	std::array<VitaChannelState, MaxChannels> ChannelStates{};

	static int MixerChannel(const FISoundChannel *channel)
	{
		if (channel == nullptr || channel->SysChannel == nullptr) return -1;
		return int(reinterpret_cast<intptr_t>(channel->SysChannel)) - 1;
	}

	static void *SystemChannel(int channel)
	{
		return reinterpret_cast<void *>(static_cast<intptr_t>(channel + 1));
	}

	int MixerVolume(float volume) const
	{
		volume = std::max(0.f, std::min(1.f, volume * SfxVolume));
		return int(volume * MIX_MAX_VOLUME + 0.5f);
	}

	unsigned int SampleCount(const Mix_Chunk *chunk) const
	{
		if (chunk == nullptr || OutputChannels <= 0) return 0;
		int bits = SDL_AUDIO_BITSIZE(OutputFormat);
		int bytesPerFrame = (bits / 8) * OutputChannels;
		return bytesPerFrame > 0 ? unsigned(chunk->alen / bytesPerFrame) : 0;
	}

	SoundHandle MakeHandle(Mix_Chunk *chunk, Uint8 *rawBuffer, int frequency, unsigned int samples)
	{
		if (chunk == nullptr)
		{
			delete[] rawBuffer;
			return { nullptr };
		}

		auto *sound = new VitaSfxData;
		sound->Chunk = chunk;
		sound->RawBuffer = rawBuffer;
		sound->Frequency = frequency > 0 ? frequency : OutputRate;
		sound->Samples = samples != 0 ? samples : SampleCount(chunk);
		return { sound };
	}

	void SetChannelPosition(int channel, const SoundListener *listener, const FVector3 &position)
	{
		if (channel < 0 || listener == nullptr) return;
		const double dx = position.X - listener->position.X;
		const double dy = position.Y - listener->position.Y;
		int angle = int(std::atan2(dx, dy) * 180.0 / 3.14159265358979323846);
		if (angle < 0) angle += 360;
		// Keep distance at zero for the first Vita backend. This preserves
		// audible SFX while still providing left/right positioning.
		Mix_SetPosition(channel, Sint16(angle), 0);
	}

public:
	VitaSoundRenderer()
	{
		if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
		{
			Printf(TEXTCOLOR_RED "Vita SDL audio init failed: %s\n", SDL_GetError());
			return;
		}

		int requestedRate = snd_samplerate != 0 ? snd_samplerate : 22050;
		if (Mix_OpenAudio(requestedRate, AUDIO_S16SYS, 2, 1024) < 0)
		{
			Printf(TEXTCOLOR_RED "Vita SDL mixer open failed: %s\n", Mix_GetError());
			return;
		}

		Mix_QuerySpec(&OutputRate, &OutputFormat, &OutputChannels);
		if (OutputRate <= 0) OutputRate = requestedRate;
		if (OutputChannels <= 0) OutputChannels = 2;
		Mix_AllocateChannels(MaxChannels);
		AudioOpened = true;
		VitaMixerReady.store(true, std::memory_order_release);
		Printf("Vita SDL audio: %d Hz, %d channels\n", OutputRate, OutputChannels);
	}

	~VitaSoundRenderer() override
	{
		if (AudioOpened)
		{
			if (VitaMusicStream.load(std::memory_order_acquire) != nullptr)
				VitaMusicStream.load(std::memory_order_acquire)->Stop();
			VitaMixerReady.store(false, std::memory_order_release);
			Mix_HaltChannel(-1);
			Mix_CloseAudio();
		}
	}

	void SetSfxVolume(float volume) override
	{
		SfxVolume = std::max(0.f, std::min(1.f, volume));
		if (AudioOpened) Mix_Volume(-1, Muted.load(std::memory_order_relaxed) ? 0 : MixerVolume(1.f));
	}

	void SetMusicVolume(float volume) override
	{
		MusicVolume.store(std::max(0.f, std::min(1.f, volume)), std::memory_order_relaxed);
	}

	void UpdateMusicParams() override {}

	SoundHandle LoadSound(uint8_t *sfxdata, int length, int, int) override
	{
		if (!AudioOpened || sfxdata == nullptr || length <= 0) return { nullptr };
		SDL_RWops *rw = SDL_RWFromConstMem(sfxdata, length);
		if (rw == nullptr) return { nullptr };
		Mix_Chunk *chunk = Mix_LoadWAV_RW(rw, 1);
		return MakeHandle(chunk, nullptr, OutputRate, 0);
	}

	SoundHandle LoadSoundRaw(uint8_t *sfxdata, int length, int frequency, int channels, int bits, int, int = -1) override
	{
		if (!AudioOpened || sfxdata == nullptr || length <= 0 || frequency <= 0 || channels <= 0) return { nullptr };

		Uint16 sourceFormat;
		if (bits == 8) sourceFormat = AUDIO_U8;
		else if (bits == 16 || bits == -16) sourceFormat = AUDIO_S16SYS;
		else return { nullptr };

		SDL_AudioCVT converter{};
		if (SDL_BuildAudioCVT(&converter, sourceFormat, Uint8(channels), frequency,
			OutputFormat, Uint8(OutputChannels), OutputRate) < 0)
		{
			return { nullptr };
		}

		int multiplier = converter.needed ? converter.len_mult : 1;
		if (multiplier < 1) multiplier = 1;
		auto *converted = new Uint8[size_t(length) * size_t(multiplier)];
		std::memcpy(converted, sfxdata, size_t(length));
		converter.buf = converted;
		converter.len = length;
		if (converter.needed && SDL_ConvertAudio(&converter) < 0)
		{
			delete[] converted;
			return { nullptr };
		}

		Mix_Chunk *chunk = Mix_QuickLoad_RAW(converted, Uint32(converter.len));
		unsigned int samples = SampleCount(chunk);
		return MakeHandle(chunk, converted, OutputRate, samples);
	}

	void UnloadSound(SoundHandle handle) override
	{
		auto *sound = static_cast<VitaSfxData *>(handle.data);
		if (sound == nullptr) return;
		if (sound->Chunk != nullptr) Mix_FreeChunk(sound->Chunk);
		delete[] sound->RawBuffer;
		delete sound;
	}

	unsigned int GetMSLength(SoundHandle handle) override
	{
		auto *sound = static_cast<VitaSfxData *>(handle.data);
		if (sound == nullptr || sound->Frequency <= 0 || sound->Samples == 0) return 250;
		return unsigned((uint64_t(sound->Samples) * 1000) / uint64_t(sound->Frequency));
	}

	unsigned int GetSampleLength(SoundHandle handle) override
	{
		auto *sound = static_cast<VitaSfxData *>(handle.data);
		return sound != nullptr ? sound->Samples : 0;
	}

	float GetOutputRate() override
	{
		return float(OutputRate > 0 ? OutputRate : 22050);
	}

	SoundStream *CreateStream(SoundStreamCallback callback, int buffbytes, SampleType stype, ChannelConfig chans,
		int samplerate, void *userdata) override
	{
		if (!AudioOpened || callback == nullptr) return nullptr;
		auto *stream = new VitaSoundStream(callback, buffbytes, stype, chans, samplerate, userdata,
			OutputRate, OutputFormat, OutputChannels, &MusicVolume, &Muted);
		if (!stream->IsValid())
		{
			delete stream;
			Printf(TEXTCOLOR_RED "Vita SDL audio: cannot create stream converter\n");
			return nullptr;
		}
		return stream;
	}

	FISoundChannel *StartSound(SoundHandle handle, float volume, float, int chanflags,
		FISoundChannel *reuseChannel, float) override
	{
		auto *sound = static_cast<VitaSfxData *>(handle.data);
		if (!AudioOpened || sound == nullptr || sound->Chunk == nullptr) return nullptr;

		if (reuseChannel != nullptr)
		{
			int oldChannel = MixerChannel(reuseChannel);
			if (oldChannel >= 0) Mix_HaltChannel(oldChannel);
		}

		int mixerChannel = Mix_PlayChannel(-1, sound->Chunk, (chanflags & SNDF_LOOP) ? -1 : 0);
		if (mixerChannel < 0 || mixerChannel >= MaxChannels) return nullptr;
		Mix_Volume(mixerChannel, Muted.load(std::memory_order_relaxed) ? 0 : MixerVolume(volume));

		FISoundChannel *channel = reuseChannel;
		if (channel == nullptr) channel = soundEngine->GetChannel(SystemChannel(mixerChannel));
		else channel->SysChannel = SystemChannel(mixerChannel);
		ChannelStates[mixerChannel].Channel = channel;
		ChannelStates[mixerChannel].Sound = sound;
		ChannelStates[mixerChannel].StartTicks = SDL_GetTicks();
		return channel;
	}

	FISoundChannel *StartSound3D(SoundHandle handle, SoundListener *listener, float volume,
		FRolloffInfo *, float, float pitch, int, const FVector3 &position, const FVector3 &, int,
		int chanflags, FISoundChannel *reuseChannel, float startTime) override
	{
		FISoundChannel *channel = StartSound(handle, volume, pitch, chanflags, reuseChannel, startTime);
		if (channel != nullptr) SetChannelPosition(MixerChannel(channel), listener, position);
		return channel;
	}

	void StopChannel(FISoundChannel *channel) override
	{
		int mixerChannel = MixerChannel(channel);
		if (mixerChannel < 0 || mixerChannel >= MaxChannels) return;
		soundEngine->ChannelEnded(channel);
		Mix_HaltChannel(mixerChannel);
		if (!(channel->ChanFlags & CHANF_EVICTED)) soundEngine->SoundDone(channel);
		ChannelStates[mixerChannel] = {};
	}

	void ChannelVolume(FISoundChannel *channel, float volume) override
	{
		int mixerChannel = MixerChannel(channel);
		if (mixerChannel >= 0 && mixerChannel < MaxChannels)
			Mix_Volume(mixerChannel, Muted.load(std::memory_order_relaxed) ? 0 : MixerVolume(volume));
	}

	void ChannelPitch(FISoundChannel *, float) override {}

	void MarkStartTime(FISoundChannel *channel, float) override
	{
		int mixerChannel = MixerChannel(channel);
		if (mixerChannel >= 0 && mixerChannel < MaxChannels) ChannelStates[mixerChannel].StartTicks = SDL_GetTicks();
	}

	unsigned int GetPosition(FISoundChannel *channel) override
	{
		int mixerChannel = MixerChannel(channel);
		if (mixerChannel < 0 || mixerChannel >= MaxChannels) return 0;
		auto &state = ChannelStates[mixerChannel];
		if (state.Sound == nullptr) return 0;
		if (!Mix_Playing(mixerChannel)) return state.Sound->Samples;
		uint32_t elapsed = SDL_GetTicks() - state.StartTicks;
		uint64_t samples = uint64_t(elapsed) * uint64_t(OutputRate) / 1000;
		if (state.Sound->Samples != 0 && (channel->ChanFlags & CHANF_LOOP)) samples %= state.Sound->Samples;
		return unsigned(std::min<uint64_t>(samples, state.Sound->Samples));
	}

	float GetAudibility(FISoundChannel *channel) override
	{
		return channel != nullptr && channel->SysChannel != nullptr && !Muted.load(std::memory_order_relaxed) ? SfxVolume : 0.f;
	}

	void Sync(bool) override {}

	void SetSfxPaused(bool paused, int) override
	{
		if (AudioOpened)
		{
			if (paused) Mix_Pause(-1);
			else Mix_Resume(-1);
		}
	}

	void SetInactive(EInactiveState inactive) override
	{
		if (!AudioOpened) return;
		if (inactive == INACTIVE_Complete) Mix_Pause(-1);
		else if (inactive == INACTIVE_Mute)
		{
			Muted.store(true, std::memory_order_release);
			Mix_Volume(-1, 0);
		}
		else
		{
			Muted.store(false, std::memory_order_release);
			Mix_Resume(-1);
			Mix_Volume(-1, MixerVolume(1.f));
		}
	}

	void UpdateSoundParams3D(SoundListener *listener, FISoundChannel *channel, bool, const FVector3 &position, const FVector3 &) override
	{
		SetChannelPosition(MixerChannel(channel), listener, position);
	}

	void UpdateListener(SoundListener *) override {}

	void UpdateSounds() override
	{
		if (!AudioOpened || soundEngine == nullptr) return;
		for (FSoundChan *channel = soundEngine->GetChannels(); channel != nullptr; )
		{
			FSoundChan *next = channel->NextChan;
			int mixerChannel = MixerChannel(channel);
			if (mixerChannel >= 0 && mixerChannel < MaxChannels && !Mix_Playing(mixerChannel))
				StopChannel(channel);
			channel = next;
		}
	}

	bool IsValid() override { return AudioOpened; }

	void PrintStatus() override
	{
		Printf("Vita SDL mixer: %d Hz, %d channels\n", OutputRate, OutputChannels);
	}

	void PrintDriversList() override
	{
		Printf("Vita SDL mixer backend\n");
	}

	FString GatherStats() override
	{
		return FStringf("Vita SDL mixer: %d Hz, %d channels", OutputRate, OutputChannels);
	}
};
}
#endif

void I_CloseSound ();


//
// SFX API
//

//==========================================================================
//
// CVAR snd_mastervolume
//
// Maximum volume of all audio
//==========================================================================

CUSTOM_CVAR(Float, snd_mastervolume, 0.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	if (self < 0.f)
		self = 0.f;
	else if (self > 1.f)
		self = 1.f;

	ChangeMusicSetting(zmusic_snd_mastervolume, nullptr, self);
	snd_sfxvolume->Callback();
	snd_musicvolume->Callback();
}

//==========================================================================
//
// CVAR snd_sfxvolume
//
// Maximum volume of a sound effect.
//==========================================================================

CUSTOM_CVAR (Float, snd_sfxvolume, 1.f, CVAR_ARCHIVE|CVAR_GLOBALCONFIG|CVAR_NOINITCALL)
{
	if (self < 0.f)
		self = 0.f;
	else if (self > 1.f)
		self = 1.f;
	else if (GSnd != NULL)
	{
		GSnd->SetSfxVolume (self * snd_mastervolume);
	}
}

class MIDIStreamer;

class NullSoundRenderer : public SoundRenderer
{
public:
	virtual bool IsNull() { return true; }
	void SetSfxVolume (float volume)
	{
	}
	void SetMusicVolume (float volume)
	{
	}
	virtual void UpdateMusicParams()
	{
	}
	SoundHandle LoadSound(uint8_t *sfxdata, int length, int def_loop_start, int def_loop_end)
	{
		SoundHandle retval = { NULL };
		return retval;
	}
	SoundHandle LoadSoundRaw(uint8_t *sfxdata, int length, int frequency, int channels, int bits, int loopstart, int loopend)
	{
		SoundHandle retval = { NULL };
		return retval;
	}
	void UnloadSound (SoundHandle sfx)
	{
	}
	unsigned int GetMSLength(SoundHandle sfx)
	{
		// Return something that isn't 0. This is only used by some
		// ambient sounds to specify a default minimum period.
		return 250;
	}
	unsigned int GetSampleLength(SoundHandle sfx)
	{
		return 0;
	}
	float GetOutputRate()
	{
		return 11025;	// Lies!
	}
	void StopChannel(FISoundChannel *chan)
	{
	}
	void ChannelVolume(FISoundChannel *, float)
	{
	}
	void ChannelPitch(FISoundChannel *, float)
	{
	}

	// Streaming sounds.
	SoundStream *CreateStream (SoundStreamCallback callback, int buffbytes, SampleType stype, ChannelConfig chans, int samplerate, void *userdata)
	{
		return NULL;
	}

	// Starts a sound.
	FISoundChannel *StartSound (SoundHandle sfx, float vol, float pitch, int chanflags, FISoundChannel *reuse_chan, float startTime)
	{
		return NULL;
	}
	FISoundChannel *StartSound3D (SoundHandle sfx, SoundListener *listener, float vol, FRolloffInfo *rolloff, float distscale, float pitch, int priority, const FVector3 &pos, const FVector3 &vel, int channum, int chanflags, FISoundChannel *reuse_chan, float startTime)
	{
		return NULL;
	}

	// Marks a channel's start time without actually playing it.
	void MarkStartTime (FISoundChannel *chan, float startTime)
	{
	}

	// Returns position of sound on this channel, in samples.
	unsigned int GetPosition(FISoundChannel *chan)
	{
		return 0;
	}

	// Gets a channel's audibility (real volume).
	float GetAudibility(FISoundChannel *chan)
	{
		return 0;
	}

	// Synchronizes following sound startups.
	void Sync (bool sync)
	{
	}

	// Pauses or resumes all sound effect channels.
	void SetSfxPaused (bool paused, int slot)
	{
	}

	// Pauses or resumes *every* channel, including environmental reverb.
	void SetInactive(SoundRenderer::EInactiveState inactive)
	{
	}

	// Updates the volume, separation, and pitch of a sound channel.
	void UpdateSoundParams3D (SoundListener *listener, FISoundChannel *chan, bool areasound, const FVector3 &pos, const FVector3 &vel)
	{
	}

	void UpdateListener (SoundListener *)
	{
	}
	void UpdateSounds ()
	{
	}

	bool IsValid ()
	{
		return true;
	}
	void PrintStatus ()
	{
		Printf("Null sound module active.\n");
	}
	void PrintDriversList ()
	{
		Printf("Null sound module uses no drivers.\n");
	}
	FString GatherStats ()
	{
		return "Null sound module has no stats.";
	}
};

void I_InitSound ()
{
	FModule_SetProgDir(progdir.GetChars());
	/* Get command line options: */
	nosound = !!Args->CheckParm (FArg_nosound);
	nosfx = !!Args->CheckParm (FArg_nosfx);

	GSnd = NULL;
	if (nosound)
	{
		GSnd = new NullSoundRenderer;
		return;
	}

	// Keep it simple: let everything except "null" init the sound.
	if (stricmp(snd_backend, "null") == 0)
	{
		GSnd = new NullSoundRenderer;
	}
	#if defined(VITA)
	else if (stricmp(snd_backend, "sdl") == 0)
	{
		GSnd = new VitaSoundRenderer;
	}
	#endif
	else
	{
		#ifndef NO_OPENAL
			if (IsOpenALPresent())
			{
				GSnd = new OpenALSoundRenderer;
			}
		#endif
	}
	if (!GSnd || !GSnd->IsValid ())
	{
		I_CloseSound();
		GSnd = new NullSoundRenderer;
		Printf (TEXTCOLOR_RED"Sound init failed. Using nosound.\n");
	}
	snd_sfxvolume->Callback ();
}


void I_CloseSound ()
{
	// Free all loaded samples. Beware that the sound engine may already have been deleted.
	if (soundEngine) soundEngine->UnloadAllSounds();

	delete GSnd;
	GSnd = NULL;
}

const char *GetSampleTypeName(SampleType type)
{
	switch(type)
	{
		case SampleType_UInt8: return "Unsigned 8-bit";
		case SampleType_Int16: return "Signed 16-bit";
		case SampleType_Float32: return "32-bit float";
		default: break;
	}
	return "(invalid sample type)";
}

const char *GetChannelConfigName(ChannelConfig chan)
{
	switch(chan)
	{
		case ChannelConfig_Mono: return "Mono";
		case ChannelConfig_Stereo: return "Stereo";
	}
	return "(invalid channel config)";
}

SoundRenderer::SoundRenderer ()
{
}

SoundRenderer::~SoundRenderer ()
{
}

FString SoundRenderer::GatherStats ()
{
	return "No stats for this sound renderer.";
}

void SoundRenderer::DrawWaveDebug(int mode)
{
}

FString SoundStream::GetStats()
{
	return "No stream stats available.";
}

//==========================================================================
//
// SoundRenderer :: LoadSoundVoc
//
//==========================================================================

SoundHandle SoundRenderer::LoadSoundVoc(uint8_t *sfxdata, int length)
{
	uint8_t * data = NULL;
	int len, frequency, channels, bits, loopstart, loopend;
	len = frequency = channels = bits = 0;
	loopstart = loopend = -1;
	do if (length > 26)
	{
		// First pass to parse data and validate the file
		if (strncmp ((const char *)sfxdata, "Creative Voice File", 19))
			break;
		int i = 26, blocktype = 0, blocksize = 0, codec = -1;
		bool noextra = true, okay = true;
		while (i < length)
		{
			// Read block header
			blocktype = sfxdata[i];
			if (blocktype == 0)
				break;
			blocksize = sfxdata[i+1] + (sfxdata[i+2]<<8) + (sfxdata[i+3]<<16);
			i += 4;
			if (i + blocksize > length)
			{
				//okay = false;
				break;
			}

			// Read block data
			switch (blocktype)
			{
			case 1: // Sound data
				if (/*noextra &*/ (codec == -1 || codec == sfxdata[i + 1])) // NAM contains a VOC where a valid data block follows an extra block.
				{
					frequency = 1000000 / (256 - sfxdata[i]);
					channels = 1;
					codec = sfxdata[i + 1];
					if (codec == 0)
						bits = 8;
					else if (codec == 4)
						bits = 16;
					else okay = false;
					len += blocksize - 2;
				}
				else okay = false;
				break;
			case 2: // Sound data continuation
				if (codec == -1)
					okay = false;
				len += blocksize;
				break;
			case 3: // Silence
				if (frequency == 1000000/(256 - sfxdata[i+2]))
				{
					int silength = 1 + sfxdata[i] + (sfxdata[i+1]<<8);
					if (codec == 0) // 8-bit unsigned PCM
						len += silength;
					else if (codec == 4) // 16-bit signed PCM
						len += silength<<1;
					else okay = false;
				} else okay = false;
				break;
			case 4: // Mark (ignored)
			case 5: // Text (ignored)
				break;
			case 6: // Repeat start
				loopstart = len;
				break;
			case 7: // Repeat end
				loopend = len;
				if (loopend < loopstart)
					okay = false;
				break;
			case 8: // Extra info
				noextra = false;
				if (codec == -1)
				{
					codec = sfxdata[i+2];
					channels = 1+sfxdata[i+3];
					frequency = 256000000/(channels * (65536 - (sfxdata[i]+(sfxdata[i+1]<<8))));
				} else okay = false;
				break;
			case 9: // Sound data in new format
				if (codec == -1)
				{
					frequency = sfxdata[i] + (sfxdata[i+1]<<8) + (sfxdata[i+2]<<16) + (sfxdata[i+3]<<24);
					bits = sfxdata[i+4];
					channels = sfxdata[i+5];
					codec = sfxdata[i+6] + (sfxdata[i+7]<<8);
					if (codec == 0)
						bits = 8;
					else if (codec == 4)
						bits = 16;
					else okay = false;
					len += blocksize - 12;
				} else okay = false;
				break;
			default: // Unknown block type
				okay = false;
				DPrintf (DMSG_ERROR, "Unknown VOC block type %i\n", blocktype);
				break;
			}
			// Move to next block
			i += blocksize;
		}

		// Second pass to write the data
		if (okay && len > 0)
		{
			data = new uint8_t[len];
			i = 26;
			int j = 0;
			while (i < length)
			{
				// Read block header again
				blocktype = sfxdata[i];
				if (blocktype == 0) break;
				blocksize = sfxdata[i+1] + (sfxdata[i+2]<<8) + (sfxdata[i+3]<<16);
				i += 4;
				switch (blocktype)
				{
				case 1: memcpy(data+j, sfxdata+i+2,  blocksize-2 ); j += blocksize-2;	break;
				case 2: memcpy(data+j, sfxdata+i,    blocksize   ); j += blocksize;		break;
				case 9: memcpy(data+j, sfxdata+i+12, blocksize-12); j += blocksize-12;	break;
				case 3:
					{
						int silength = 1 + sfxdata[i] + (sfxdata[i+1]<<8);
						if (bits == 8)
						{
							memset(data+j, 128, silength);
							j += silength;
						}
						else if (bits == -16)
						{
							memset(data+j, 0, silength<<1);
							j += silength<<1;
						}
					}
					break;
				default: break;
				}
				i += blocksize;
			}
		}

	} while (false);
	SoundHandle retval = LoadSoundRaw(data, len, frequency, channels, bits, loopstart, loopend);
	if (data) delete[] data;
	return retval;
}
