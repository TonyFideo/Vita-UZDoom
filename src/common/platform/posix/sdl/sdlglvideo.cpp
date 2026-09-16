/*
** sdlglvideo.cpp
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2005-2016 Marisa Heit
** Copyright 2005-2016 Christoph Oelckers
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

// HEADER FILES ------------------------------------------------------------

#include <SDL2/SDL.h>

#include <algorithm>
#include <cmath>

#ifdef HAVE_VULKAN
#include <SDL2/SDL_vulkan.h>
#include <zvulkan/vulkanbuilders.h>
#include <zvulkan/vulkandevice.h>
#include <zvulkan/vulkaninstance.h>
#include <zvulkan/vulkansurface.h>
#endif

#include "basics.h"
#include "c_dispatch.h"
#include "gl_framebuffer.h"
#include "gl_sysfb.h"
#include "i_soundinternal.h"
#include "i_video.h"
#include "m_argv.h"
#include "printf.h"
#include "bitmap.h"
#include "palettecontainer.h"
#include "v_draw.h"
#include "v_video.h"
#include "version.h"

#ifdef HAVE_GLES2
#include "gles_buffers.h"
#include "gles_framebuffer.h"
#include "gles_renderstate.h"
#include "gles_system.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "hwrenderer/data/hw_renderstate.h"
#endif

#ifdef HAVE_VULKAN
#include "vulkan/system/vk_renderdevice.h"
#endif

// MACROS ------------------------------------------------------------------

// TYPES -------------------------------------------------------------------

// PUBLIC FUNCTION PROTOTYPES ----------------------------------------------

// PRIVATE FUNCTION PROTOTYPES ---------------------------------------------

// EXTERNAL DATA DECLARATIONS ----------------------------------------------

extern IVideo *Video;

EXTERN_CVAR (Int, vid_adapter)
EXTERN_CVAR (Int, vid_displaybits)
EXTERN_CVAR (Int, vid_defwidth)
EXTERN_CVAR (Int, vid_defheight)
EXTERN_CVAR (Bool, cl_capfps)
EXTERN_CVAR(Bool, vk_debug)
EXTERN_FARG(glversion);
#if defined(VITA)
EXTERN_CVAR(Int, vid_rendermode)
#endif

// PUBLIC DATA DEFINITIONS -------------------------------------------------

CUSTOM_CVAR(Bool, gl_debug, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}
CUSTOM_CVAR(Bool, gl_es, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

CUSTOM_CVAR(String, vid_sdl_render_driver, "", CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

CCMD(vid_list_sdl_render_drivers)
{
	for (int i = 0; i < SDL_GetNumRenderDrivers(); ++i)
	{
		SDL_RendererInfo info;
		if (SDL_GetRenderDriverInfo(i, &info) == 0)
			Printf("%s\n", info.name);
	}
}

// PRIVATE DATA DEFINITIONS ------------------------------------------------

namespace Priv
{
	SDL_Window *window;
	bool vulkanEnabled;
	bool softpolyEnabled;
	bool fullscreenSwitch;
	int numberOfDisplays;
	SDL_Rect* displayBounds = nullptr;

	void updateDisplayInfo()
	{
		Priv::numberOfDisplays = SDL_GetNumVideoDisplays();
		if (Priv::numberOfDisplays <= 0) {
			Printf("%sWrong number of displays detected.\n", TEXTCOLOR_BOLD);
			return;
		}
		Printf("Number of detected displays %d .\n", Priv::numberOfDisplays);

		if (Priv::displayBounds != nullptr) {
			free(Priv::displayBounds);
		}
		Priv::displayBounds = (SDL_Rect*) calloc(Priv::numberOfDisplays, sizeof(SDL_Rect));

		for (int i=0; i < Priv::numberOfDisplays; i++) {
			if (0 != SDL_GetDisplayBounds(i, &Priv::displayBounds[i])) {
				Printf("%sError getting display %d size: %s\n", TEXTCOLOR_BOLD, i, SDL_GetError());
				if (i == 0) {
					free(Priv::displayBounds);
					displayBounds = nullptr;
				}
				Priv::numberOfDisplays = i;
				return;
			}
		}
	}

	void CreateWindow(uint32_t extraFlags)
	{
		assert(Priv::window == nullptr);

		// Get displays and default display size
		updateDisplayInfo();

		// TODO control better when updateDisplayInfo fails
		SDL_Rect* bounds = &displayBounds[vid_adapter % numberOfDisplays];

		if (win_w <= 0 || win_h <= 0)
		{
			win_w = bounds->w * 8 / 10;
			win_h = bounds->h * 8 / 10;
		}

		int xWindowPos = (win_x <= 0) ? SDL_WINDOWPOS_CENTERED_DISPLAY(vid_adapter) : win_x;
		int yWindowPos = (win_y <= 0) ? SDL_WINDOWPOS_CENTERED_DISPLAY(vid_adapter) : win_y;
		Printf("Creating window [%dx%d] on adapter %d\n", (*win_w), (*win_h), (*vid_adapter));

		FString caption;
		caption.Format(GAMENAME " %s (%s)", GetVersionString(), GetGitTime());

		const uint32_t windowFlags = (win_maximized ? SDL_WINDOW_MAXIMIZED : 0) | SDL_WINDOW_RESIZABLE | extraFlags;
		Priv::window = SDL_CreateWindow(caption.GetChars(), xWindowPos, yWindowPos, win_w, win_h, windowFlags);

		if (Priv::window != nullptr)
		{
			SDL_version sdlver;
			SDL_GetVersion(&sdlver);
			// Enforce minimum size limit
			SDL_SetWindowMinimumSize(Priv::window, VID_MIN_WIDTH, VID_MIN_HEIGHT);
			// Tell SDL to start sending text input on Wayland if it's on affected versions.
			if (strncasecmp(SDL_GetCurrentVideoDriver(), "wayland", 7) == 0 && sdlver.major == 2 && sdlver.minor == 0 && sdlver.patch < 18)
				SDL_StartTextInput();
		}
	}

	void DestroyWindow()
	{
		assert(Priv::window != nullptr);

		SDL_DestroyWindow(Priv::window);
		Priv::window = nullptr;

		if (Priv::displayBounds != nullptr) {
			free(Priv::displayBounds);
			Priv::displayBounds = nullptr;
		}
	}

	void SetupPixelFormat(int multisample, const int *glver)
	{
		SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
		SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
		SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
		if (multisample > 0) {
			SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
			SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, multisample);
		}
		if (gl_debug)
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);

		if (gl_es)
		{
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
		}
		else if (glver[0] > 2)
		{
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, glver[0]);
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, glver[1]);
		}
		else
		{
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
		}
	}
}

CUSTOM_CVAR(Int, vid_adapter, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
  if (Priv::window != nullptr) {
		// Get displays and default display size
		Priv::updateDisplayInfo();

	int display = (*self) % Priv::numberOfDisplays;

		// TODO control better when updateDisplayInfo fails
		SDL_Rect* bounds = &Priv::displayBounds[vid_adapter % Priv::numberOfDisplays];

		if (win_w <= 0 || win_h <= 0)
		{
			win_w = bounds->w * 8 / 10;
			win_h = bounds->h * 8 / 10;
		}
		// Forces to set to the ini this vars to -1, so +vid_adapter keeps working the next time that the game it's launched
		win_x = -1;
		win_y = -1;

		if ((SDL_GetWindowFlags(Priv::window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0) {

			// TODO This not works. For some reason keeps stuck on the previus screen
			/*
			SDL_DisplayMode currentDisplayMode;
			SDL_GetWindowDisplayMode(Priv::window, &currentDisplayMode);
			currentDisplayMode.w = win_w;
			currentDisplayMode.h = win_h;
			if ( 0 != SDL_SetWindowDisplayMode(Priv::window, &currentDisplayMode)) {
				Printf("A problem occured trying to change of display %s\n", SDL_GetError());
			}
			*/

			// TODO This workaround also isn't working
			/*
			SDL_SetWindowFullscreen(Priv::window, 0);
			SDL_SetWindowSize(Priv::window, win_w, win_h);
			SDL_SetWindowPosition(Priv::window, bounds->x , bounds->y);
			SDL_SetWindowFullscreen(Priv::window, SDL_WINDOW_FULLSCREEN_DESKTOP);
			*/
			Printf("Changing adapter on fullscreen, isn't full supported by SDL. Instead try to switch to windowed mode, change the adapter and then switch again to fullscreen.\n");

		} else {
			SDL_SetWindowSize(Priv::window, win_w, win_h);
			SDL_SetWindowPosition(Priv::window, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
		}

	display = SDL_GetWindowDisplayIndex(Priv::window);
	if (display >= 0) {
			Printf("New display is %d\n", display );
		} else {
			Printf("A problem occured trying to change of display %s\n", SDL_GetError());
		}
  }
}

class SDLVideo : public IVideo
{
public:
	SDLVideo ();
	~SDLVideo ();

	void DumpAdapters();

	DFrameBuffer *CreateFrameBuffer ();

private:
#ifdef HAVE_VULKAN
	std::shared_ptr<VulkanSurface> surface;
#endif
};

#if defined(VITA)
// The software scene does not need UZDoom's full GLES renderer.  That
// renderer allocates its complete pipeline/shader/resource graph even when
// V_IsHardwareRenderer() is false, which is too large for the Vita user heap.
// Keep the SDL2/VitaGL context and use VitaGL only as a very small presenter
// for the CPU-rendered BGRA canvas instead.
class VitaSoftwareFrameBuffer final : public SystemGLFrameBuffer
{
	using Super = SystemGLFrameBuffer;

	struct Vita2DVertex
	{
		float x, y, u, v;
		PalEntry color;
	};

	DCanvas *Canvas = nullptr;
	TArray<uint8_t> UploadPixels;
	TArray<uint8_t> Stencil;
	bool StencilEnabled = false;
	bool ColorWrites = true;
	int StencilReference = 0;
	int StencilOperation = SOP_Keep;
	GLuint CanvasTexture = 0;
	GLuint PresentProgram = 0;
	GLint PresentPosition = 0;
	GLint PresentTexCoord = 1;
	GLint PresentTexture = -1;

	static int ClampColor(int value)
	{
		return std::max(0, std::min(255, value));
	}

	static uint8_t ClampStencil(int value)
	{
		return uint8_t(std::max(0, std::min(255, value)));
	}

	static int BlendFactor(uint8_t factor, int srcAlpha, int dstAlpha, int srcColor, int dstColor)
	{
		switch (factor)
		{
		case STYLEALPHA_Zero: return 0;
		case STYLEALPHA_One: return 255;
		case STYLEALPHA_Src: return srcAlpha;
		case STYLEALPHA_InvSrc: return 255 - srcAlpha;
		case STYLEALPHA_SrcCol: return srcColor;
		case STYLEALPHA_InvSrcCol: return 255 - srcColor;
		case STYLEALPHA_DstCol: return dstColor;
		case STYLEALPHA_InvDstCol: return 255 - dstColor;
		case STYLEALPHA_Dst: return dstAlpha;
		case STYLEALPHA_InvDst: return 255 - dstAlpha;
		default: return 255;
		}
	}

	static void ApplyTransform(const F2DDrawer::RenderCommand &command, const F2DDrawer::TwoDVertex &source, Vita2DVertex &dest)
	{
		dest.x = source.x;
		dest.y = source.y;
		if (command.useTransform)
		{
			auto transformed = command.transform * DVector3(source.x, source.y, 1.0);
			dest.x = float(transformed.X);
			dest.y = float(transformed.Y);
		}
		dest.u = source.u;
		dest.v = source.v;
		dest.color = source.color0;
	}

	static bool InsideScissor(const F2DDrawer::RenderCommand &command, int x, int y)
	{
		if (!(command.mFlags & F2DDrawer::DTF_Scissor)) return true;
		return x >= command.mScissor[0] && y >= command.mScissor[1] &&
			x < command.mScissor[2] && y < command.mScissor[3];
	}

	// Match the half-open edge rule used by hardware rasterizers. Without it,
	// both triangles of a screen quad shade their shared diagonal. That is
	// visible when a translucent full-screen flash is blended twice on the CPU.
	static bool IsTopLeftEdge(const Vita2DVertex &from, const Vita2DVertex &to)
	{
		const float dy = to.y - from.y;
		const float dx = to.x - from.x;
		return dy < 0.f || (dy == 0.f && dx > 0.f);
	}

	static bool AcceptTriangleEdge(float edge, const Vita2DVertex &from,
		const Vita2DVertex &to)
	{
		constexpr float epsilon = 0.0001f;
		if (edge > epsilon) return true;
		if (edge < -epsilon) return false;
		return IsTopLeftEdge(from, to);
	}

	static PalEntry InterpolateColor(const Vita2DVertex &v0, const Vita2DVertex &v1, const Vita2DVertex &v2,
		float w0, float w1, float w2)
	{
		return PalEntry(
			uint8_t(ClampColor(int(v0.color.a * w0 + v1.color.a * w1 + v2.color.a * w2 + 0.5f))),
			uint8_t(ClampColor(int(v0.color.r * w0 + v1.color.r * w1 + v2.color.r * w2 + 0.5f))),
			uint8_t(ClampColor(int(v0.color.g * w0 + v1.color.g * w1 + v2.color.g * w2 + 0.5f))),
			uint8_t(ClampColor(int(v0.color.b * w0 + v1.color.b * w1 + v2.color.b * w2 + 0.5f))));
	}

	bool BlendPixel(uint8_t *destination, PalEntry source, const F2DDrawer::RenderCommand &command)
	{
		if (source.a == 0) return false;

		PalEntry dest(destination[3], destination[2], destination[1], destination[0]);
		const auto &style = command.mRenderStyle;
		int sourceAlpha = source.a;
		if (style.Flags & STYLEF_Alpha1) sourceAlpha = 255;

		int sourceR = source.r;
		int sourceG = source.g;
		int sourceB = source.b;
		int destR = dest.r;
		int destG = dest.g;
		int destB = dest.b;

		int srcFactorR = BlendFactor(style.SrcAlpha, sourceAlpha, dest.a, sourceR, destR);
		int srcFactorG = BlendFactor(style.SrcAlpha, sourceAlpha, dest.a, sourceG, destG);
		int srcFactorB = BlendFactor(style.SrcAlpha, sourceAlpha, dest.a, sourceB, destB);
		int dstFactorR = BlendFactor(style.DestAlpha, sourceAlpha, dest.a, sourceR, destR);
		int dstFactorG = BlendFactor(style.DestAlpha, sourceAlpha, dest.a, sourceG, destG);
		int dstFactorB = BlendFactor(style.DestAlpha, sourceAlpha, dest.a, sourceB, destB);

		int outR, outG, outB;
		if (style.BlendOp == STYLEOP_Sub)
		{
			outR = (sourceR * srcFactorR - destR * dstFactorR) / 255;
			outG = (sourceG * srcFactorG - destG * dstFactorG) / 255;
			outB = (sourceB * srcFactorB - destB * dstFactorB) / 255;
		}
		else if (style.BlendOp == STYLEOP_RevSub)
		{
			outR = (destR * dstFactorR - sourceR * srcFactorR) / 255;
			outG = (destG * dstFactorG - sourceG * srcFactorG) / 255;
			outB = (destB * dstFactorB - sourceB * srcFactorB) / 255;
		}
		else
		{
			outR = (sourceR * srcFactorR + destR * dstFactorR) / 255;
			outG = (sourceG * srcFactorG + destG * dstFactorG) / 255;
			outB = (sourceB * srcFactorB + destB * dstFactorB) / 255;
		}

		destination[0] = uint8_t(ClampColor(outB));
		destination[1] = uint8_t(ClampColor(outG));
		destination[2] = uint8_t(ClampColor(outR));
		destination[3] = 255;
		return true;
	}

	bool PassStencil(int x, int y) const
	{
		if (!StencilEnabled) return true;
		return Stencil[y * Canvas->GetWidth() + x] == ClampStencil(StencilReference);
	}

	void UpdateStencil(int x, int y)
	{
		if (!StencilEnabled) return;
		auto &value = Stencil[y * Canvas->GetWidth() + x];
		if (StencilOperation == SOP_Increment)
			value = uint8_t(std::min(255, int(value) + 1));
		else if (StencilOperation == SOP_Decrement)
			value = uint8_t(std::max(0, int(value) - 1));
	}

	void DrawPixel(const F2DDrawer::RenderCommand &command, int x, int y, PalEntry color)
	{
		if (!PassStencil(x, y)) return;
		bool visible = color.a != 0;
		if (visible && ColorWrites)
			visible = BlendPixel(Canvas->GetPixels() + (y * Canvas->GetPitch() + x) * 4, color, command);
		if (visible) UpdateStencil(x, y);
	}

	void Reset2DState()
	{
		const int pixels = Canvas->GetWidth() * Canvas->GetHeight();
		if (Stencil.Size() != pixels) Stencil.Resize(pixels);
		if (Stencil.Size() != 0) memset(Stencil.Data(), 0, Stencil.Size());
		StencilEnabled = false;
		ColorWrites = true;
		StencilReference = screen != nullptr ? screen->stencilValue : 0;
		StencilOperation = SOP_Keep;
	}

	PalEntry Sample2D(const F2DDrawer::RenderCommand &command, const FBitmap *bitmap, float u, float v,
		const PalEntry &vertexColor) const
	{
		PalEntry result = vertexColor;
		uint8_t textureAlpha = 255;
		const bool outsideY = v < 0.f || v > 1.f;

		if (bitmap != nullptr && bitmap->GetPixels() != nullptr && bitmap->GetWidth() > 0 && bitmap->GetHeight() > 0)
		{
			if (command.mFlags & F2DDrawer::DTF_Wrap)
			{
				u -= std::floor(u);
				v -= std::floor(v);
			}
			u = std::max(0.f, std::min(0.999999f, u));
			v = std::max(0.f, std::min(0.999999f, v));
			int x = std::min(bitmap->GetWidth() - 1, std::max(0, int(u * bitmap->GetWidth())));
			int y = std::min(bitmap->GetHeight() - 1, std::max(0, int(v * bitmap->GetHeight())));
			const auto *pixel = bitmap->GetPixels() + y * bitmap->GetPitch() + x * 4;
			PalEntry tex(pixel[3], pixel[2], pixel[1], pixel[0]);
			textureAlpha = tex.a;

			switch (command.mDrawMode)
			{
			case TM_STENCIL:
				result.r = vertexColor.r;
				result.g = vertexColor.g;
				result.b = vertexColor.b;
				result.a = uint8_t(textureAlpha * vertexColor.a / 255);
				break;
			case TM_ALPHATEXTURE:
				result.r = vertexColor.r;
				result.g = vertexColor.g;
				result.b = vertexColor.b;
				result.a = uint8_t(pixel[2] * vertexColor.a / 255);
				break;
			case TM_INVERSE:
				result.r = 255 - tex.r;
				result.g = 255 - tex.g;
				result.b = 255 - tex.b;
				result.a = uint8_t(textureAlpha * vertexColor.a / 255);
				break;
			case TM_INVERTOPAQUE:
				result.r = 255 - tex.r;
				result.g = 255 - tex.g;
				result.b = 255 - tex.b;
				result.a = 255;
				break;
			case TM_OPAQUE:
				result.r = tex.r;
				result.g = tex.g;
				result.b = tex.b;
				result.a = 255;
				break;
			case TM_CLAMPY:
				result.r = uint8_t(tex.r * vertexColor.r / 255);
				result.g = uint8_t(tex.g * vertexColor.g / 255);
				result.b = uint8_t(tex.b * vertexColor.b / 255);
				result.a = outsideY ? 0 : uint8_t(textureAlpha * vertexColor.a / 255);
				break;
			default:
				result.r = uint8_t(tex.r * vertexColor.r / 255);
				result.g = uint8_t(tex.g * vertexColor.g / 255);
				result.b = uint8_t(tex.b * vertexColor.b / 255);
				result.a = uint8_t(textureAlpha * vertexColor.a / 255);
				break;
			}
		}
		else if (command.mDrawMode == TM_ALPHATEXTURE || command.mDrawMode == TM_STENCIL)
		{
			result.a = vertexColor.a;
		}

		if (command.mRenderStyle.Flags & STYLEF_InvertSource)
		{
			result.r = 255 - result.r;
			result.g = 255 - result.g;
			result.b = 255 - result.b;
		}

		if (command.mDesaturate > 0)
		{
			int gray = (result.r * 77 + result.g * 143 + result.b * 37) >> 8;
			int amount = std::min(255, command.mDesaturate);
			result.r = uint8_t((result.r * (255 - amount) + gray * amount) / 255);
			result.g = uint8_t((result.g * (255 - amount) + gray * amount) / 255);
			result.b = uint8_t((result.b * (255 - amount) + gray * amount) / 255);
		}

		if (command.mSpecialColormap[0].a != 0)
		{
			// F2DDrawer stores the fixed-colormap endpoints at half range because
			// the GLES shader multiplies the resulting RGB by two. Reproduce that
			// conversion here instead of making flash/translation colors too dark.
			float gray = (result.r * 0.30f + result.g * 0.56f + result.b * 0.14f) / 255.f;
			const auto &start = command.mSpecialColormap[0];
			const auto &end = command.mSpecialColormap[1];
			result.r = uint8_t(ClampColor(int((start.r + (end.r - start.r) * gray) * 2.f + 0.5f)));
			result.g = uint8_t(ClampColor(int((start.g + (end.g - start.g) * gray) * 2.f + 0.5f)));
			result.b = uint8_t(ClampColor(int((start.b + (end.b - start.b) * gray) * 2.f + 0.5f)));
		}

		// mColor1 is the premultiplied overlay used by the normal GLES2 drawer.
		// It has an unused alpha component in this path, so add its RGB here.
		result.r = uint8_t(ClampColor(result.r + command.mColor1.r));
		result.g = uint8_t(ClampColor(result.g + command.mColor1.g));
		result.b = uint8_t(ClampColor(result.b + command.mColor1.b));
		if (command.mScreenFade < 1.f)
		{
			const float fade = std::max(0.f, std::min(1.f, command.mScreenFade));
			result.r = uint8_t(result.r * fade + 0.5f);
			result.g = uint8_t(result.g * fade + 0.5f);
			result.b = uint8_t(result.b * fade + 0.5f);
		}
		return result;
	}

	void Draw2DTriangle(const F2DDrawer::RenderCommand &command, const Vita2DVertex &v0,
		const Vita2DVertex &v1, const Vita2DVertex &v2, const FBitmap *bitmap)
	{
		// Normalize the winding so the edge rule below can use one orientation.
		Vita2DVertex a = v0, b = v1, c = v2;
		float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
		if (std::fabs(area) < 0.001f) return;
		if (area < 0.f)
		{
			std::swap(b, c);
			area = -area;
		}

		int left = std::max(0, int(std::floor(std::min({a.x, b.x, c.x}))));
		int right = std::min(Canvas->GetWidth() - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
		int top = std::max(0, int(std::floor(std::min({a.y, b.y, c.y}))));
		int bottom = std::min(Canvas->GetHeight() - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));

		for (int y = top; y <= bottom; ++y)
		{
			for (int x = left; x <= right; ++x)
			{
				if (!InsideScissor(command, x, y)) continue;
				float px = x + 0.5f;
				float py = y + 0.5f;
				float edge0 = (b.x - px) * (c.y - py) - (b.y - py) * (c.x - px);
				float edge1 = (c.x - px) * (a.y - py) - (c.y - py) * (a.x - px);
				float edge2 = (a.x - px) * (b.y - py) - (a.y - py) * (b.x - px);
				if (!AcceptTriangleEdge(edge0, b, c) ||
					!AcceptTriangleEdge(edge1, c, a) ||
					!AcceptTriangleEdge(edge2, a, b)) continue;

				float w0 = edge0 / area;
				float w1 = edge1 / area;
				float w2 = edge2 / area;

				float u = a.u * w0 + b.u * w1 + c.u * w2;
				float v = a.v * w0 + b.v * w1 + c.v * w2;
				PalEntry color = InterpolateColor(a, b, c, w0, w1, w2);
				color = Sample2D(command, bitmap, u, v, color);
				DrawPixel(command, x, y, color);
			}
		}
	}

	void Draw2DLine(const F2DDrawer::RenderCommand &command, const Vita2DVertex &v0, const Vita2DVertex &v1)
	{
		int x0 = int(std::lround(v0.x)), y0 = int(std::lround(v0.y));
		int x1 = int(std::lround(v1.x)), y1 = int(std::lround(v1.y));
		int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
		int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
		int error = dx + dy;
		int length = std::max(dx, -dy);
		int step = 0;

		for (;;)
		{
			if (x0 >= 0 && x0 < Canvas->GetWidth() && y0 >= 0 && y0 < Canvas->GetHeight() && InsideScissor(command, x0, y0))
			{
				float t = length == 0 ? 0.f : float(step) / length;
				PalEntry color(
					uint8_t(v0.color.a + (v1.color.a - v0.color.a) * t),
					uint8_t(v0.color.r + (v1.color.r - v0.color.r) * t),
					uint8_t(v0.color.g + (v1.color.g - v0.color.g) * t),
					uint8_t(v0.color.b + (v1.color.b - v0.color.b) * t));
				color = Sample2D(command, nullptr, 0, 0, color);
				DrawPixel(command, x0, y0, color);
			}
			if (x0 == x1 && y0 == y1) break;
			int e2 = 2 * error;
			if (e2 >= dy) error += dy, x0 += sx;
			if (e2 <= dx) error += dx, y0 += sy;
			++step;
		}
	}

	void Composite2D()
	{
		if (Canvas == nullptr || twod == nullptr || twod->mData.Size() == 0) return;
		Reset2DState();

		for (const auto &command : twod->mData)
		{
			if (command.isSpecial != SpecialDrawCommand::NotSpecial)
			{
				switch (command.isSpecial)
				{
				case SpecialDrawCommand::EnableStencil:
					StencilEnabled = command.stencilOn;
					break;
				case SpecialDrawCommand::SetStencil:
					StencilReference = (screen != nullptr ? screen->stencilValue : 0) + command.stencilOffs;
					StencilOperation = command.stencilOp;
					if (command.stencilFlags != -1)
						ColorWrites = !(command.stencilFlags & SF_ColorMaskOff);
					break;
				case SpecialDrawCommand::ClearStencil:
					if (Stencil.Size() != 0) memset(Stencil.Data(), 0, Stencil.Size());
					break;
				default:
					break;
				}
				continue;
			}

			if (command.mRenderStyle.BlendOp == STYLEOP_None)
				continue;

			const FBitmap *bitmap = nullptr;
			FBitmap textureBitmap;
			if (command.mTexture != nullptr && command.mTexture->isValid() && command.mTexture->GetTexture() != nullptr)
			{
				const PalEntry *translation = nullptr;
				if (command.mTranslationId.isvalid() && !IsLuminosityTranslation(command.mTranslationId))
				{
					auto remap = GPalette.TranslationToTable(command.mTranslationId);
					if (remap != nullptr && !remap->Inactive) translation = remap->Palette;
				}
				textureBitmap = command.mTexture->GetTexture()->GetBgraBitmap(translation);
				bitmap = &textureBitmap;
			}

			if (command.shape2DBufInfo != nullptr)
			{
				const auto &vertices = command.shape2DBufInfo->cpuVertices;
				const auto &indices = command.shape2DBufInfo->cpuIndices;
				for (int i = 0; i + 2 < command.shape2DIndexCount && i + 2 < indices.Size(); i += 3)
				{
					int i0 = indices[i];
					int i1 = indices[i + 1];
					int i2 = indices[i + 2];
					if (i0 < 0 || i1 < 0 || i2 < 0 || i0 >= vertices.Size() ||
						i1 >= vertices.Size() || i2 >= vertices.Size()) continue;
					Vita2DVertex v0, v1, v2;
					ApplyTransform(command, vertices[i0], v0);
					ApplyTransform(command, vertices[i1], v1);
					ApplyTransform(command, vertices[i2], v2);
					Draw2DTriangle(command, v0, v1, v2, bitmap);
				}
			}
			else if (command.mType == F2DDrawer::DrawTypeTriangles)
			{
				for (int i = 0; i + 2 < command.mIndexCount; i += 3)
				{
					int i0 = twod->mIndices[command.mIndexIndex + i];
					int i1 = twod->mIndices[command.mIndexIndex + i + 1];
					int i2 = twod->mIndices[command.mIndexIndex + i + 2];
					if (i0 < 0 || i1 < 0 || i2 < 0 || i0 >= twod->mVertices.Size() ||
						i1 >= twod->mVertices.Size() || i2 >= twod->mVertices.Size()) continue;
					Vita2DVertex v0, v1, v2;
					ApplyTransform(command, twod->mVertices[i0], v0);
					ApplyTransform(command, twod->mVertices[i1], v1);
					ApplyTransform(command, twod->mVertices[i2], v2);
					Draw2DTriangle(command, v0, v1, v2, bitmap);
				}
			}
			else if (command.mType == F2DDrawer::DrawTypeLines)
			{
				for (int i = 0; i + 1 < command.mVertCount; i += 2)
				{
					int i0 = command.mVertIndex + i;
					int i1 = command.mVertIndex + i + 1;
					if (i0 < 0 || i1 < 0 || i0 >= twod->mVertices.Size() || i1 >= twod->mVertices.Size()) continue;
					Vita2DVertex v0, v1;
					ApplyTransform(command, twod->mVertices[i0], v0);
					ApplyTransform(command, twod->mVertices[i1], v1);
					Draw2DLine(command, v0, v1);
				}
			}
			else if (command.mType == F2DDrawer::DrawTypePoints)
			{
				for (int i = 0; i < command.mVertCount; ++i)
				{
					int index = command.mVertIndex + i;
					if (index < 0 || index >= twod->mVertices.Size()) continue;
					Vita2DVertex vertex;
					ApplyTransform(command, twod->mVertices[index], vertex);
					int x = int(std::lround(vertex.x)), y = int(std::lround(vertex.y));
					if (x < 0 || x >= Canvas->GetWidth() || y < 0 || y >= Canvas->GetHeight() || !InsideScissor(command, x, y)) continue;
					PalEntry color = Sample2D(command, bitmap, vertex.u, vertex.v, vertex.color);
					DrawPixel(command, x, y, color);
				}
			}
		}
	}

	GLuint CompilePresentShader(GLenum type, const char *source)
	{
		GLuint shader = glCreateShader(type);
		glShaderSource(shader, 1, &source, nullptr);
		glCompileShader(shader);

		GLint status = GL_FALSE;
		glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
		if (status == GL_FALSE)
		{
			GLchar log[512] = {};
			glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
			I_FatalError("VitaGL presenter shader compilation failed:\n%s\n", log);
		}
		return shader;
	}

	void CreatePresenterProgram()
	{
		static const char *vertexSource =
			"attribute vec2 aPosition;\n"
			"attribute vec2 aTexCoord;\n"
			"varying vec2 vTexCoord;\n"
			"void main() {\n"
			"  gl_Position = vec4(aPosition, 0.0, 1.0);\n"
			"  vTexCoord = aTexCoord;\n"
			"}\n";
		static const char *fragmentSource =
			"precision mediump float;\n"
			"uniform sampler2D uCanvas;\n"
			"varying vec2 vTexCoord;\n"
			"void main() { gl_FragColor = texture2D(uCanvas, vTexCoord); }\n";

		GLuint vertexShader = CompilePresentShader(GL_VERTEX_SHADER, vertexSource);
		GLuint fragmentShader = CompilePresentShader(GL_FRAGMENT_SHADER, fragmentSource);

		PresentProgram = glCreateProgram();
		glAttachShader(PresentProgram, vertexShader);
		glAttachShader(PresentProgram, fragmentShader);
		glBindAttribLocation(PresentProgram, PresentPosition, "aPosition");
		glBindAttribLocation(PresentProgram, PresentTexCoord, "aTexCoord");
		glLinkProgram(PresentProgram);

		GLint status = GL_FALSE;
		glGetProgramiv(PresentProgram, GL_LINK_STATUS, &status);
		if (status == GL_FALSE)
		{
			GLchar log[512] = {};
			glGetProgramInfoLog(PresentProgram, sizeof(log), nullptr, log);
			I_FatalError("VitaGL presenter shader link failed:\n%s\n", log);
		}

		PresentTexture = glGetUniformLocation(PresentProgram, "uCanvas");
		glDeleteShader(vertexShader);
		glDeleteShader(fragmentShader);
	}

	void SetPresentState()
	{
		int width = GetClientWidth();
		int height = GetClientHeight();
		if (width <= 0) width = GetWidth();
		if (height <= 0) height = GetHeight();

		glViewport(0, 0, width, height);
	}

	void UploadCanvas()
	{
		const int width = GetWidth();
		const int height = GetHeight();
		const int sourcePitch = Canvas->GetPitch() * 4;
		const int uploadPitch = width * 4;
		const uint8_t *source = Canvas->GetPixels();
		uint8_t *upload = UploadPixels.Data();

		for (int y = 0; y < height; ++y)
		{
			memcpy(upload + y * uploadPitch, source + y * sourcePitch, uploadPitch);
		}

		glBindTexture(GL_TEXTURE_2D, CanvasTexture);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
			GL_BGRA, GL_UNSIGNED_BYTE, upload);
	}

public:
	VitaSoftwareFrameBuffer(void *hMonitor, bool fullscreen)
		: Super(hMonitor, fullscreen)
	{
		mPipelineNbr = 1;
		mPipelineType = 1;
	}

	~VitaSoftwareFrameBuffer() override
	{
		if (PresentProgram != 0)
			glDeleteProgram(PresentProgram);
		if (CanvasTexture != 0)
			glDeleteTextures(1, &CanvasTexture);
		delete mViewpoints;
		mViewpoints = nullptr;
		delete Canvas;
	}

	bool IsPoly() override { return true; }
	DCanvas *GetCanvas() override { return Canvas; }
	const char *DeviceName() const override { return "VitaGL software presenter"; }
	TArray<uint8_t> GetScreenshotBuffer(int &pitch, ESSType &color_type, float &gamma) override
	{
		if (Canvas == nullptr || Canvas->GetPixels() == nullptr)
			return TArray<uint8_t>();

		pitch = Canvas->GetPitch() * 4;
		color_type = SS_BGRA;
		gamma = 1.f;

		TArray<uint8_t> pixels(pitch * Canvas->GetHeight(), true);
		memcpy(pixels.Data(), Canvas->GetPixels(), pixels.Size());
		return pixels;
	}
	IVertexBuffer *CreateVertexBuffer() override { return new OpenGLESRenderer::GLVertexBuffer; }
	IIndexBuffer *CreateIndexBuffer() override { return new OpenGLESRenderer::GLIndexBuffer; }
	IDataBuffer *CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize) override
	{
		return new OpenGLESRenderer::GLDataBuffer(bindingpoint, ssbo);
	}

	void InitializeState() override
	{
		// This loads the same GLES2/VitaGL entry points used by the normal
		// OpenGLESFrameBuffer, without constructing FGLRenderer.
		OpenGLESRenderer::InitGLES();
		OpenGLESRenderer::gl_RenderState.Reset();
		mViewpoints = new HWViewpointBuffer(1);

		Canvas = new DCanvas(GetWidth(), GetHeight(), true);
		UploadPixels.Resize(GetWidth() * GetHeight() * 4);

		// Do not change depth/cull/blend state here.  The modern VitaGL backend
		// applies those states directly to the current GXM scene, and no scene
		// exists until the first glClear() in Update().  The default VitaGL
		// values already match this software presenter.
		glEnable(GL_TEXTURE_2D);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

		glGenTextures(1, &CanvasTexture);
		glBindTexture(GL_TEXTURE_2D, CanvasTexture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GetWidth(), GetHeight(), 0,
			GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
		CreatePresenterProgram();

		SetViewportRects(nullptr);
	}

	void Update() override
	{
		static const GLfloat quad[] = {
			-1.f, -1.f, 0.f, 1.f,
			 1.f, -1.f, 1.f, 1.f,
			-1.f,  1.f, 0.f, 0.f,
			 1.f,  1.f, 1.f, 0.f,
		};

		// The software scene path flushes the weapon sprites while the scene is
		// being completed.  The status bar, console and menus are queued later
		// by D_Display(), so consume the remaining 2D commands immediately
		// before uploading the final CPU canvas.
		Composite2D();
		twod->Clear();

		glClear(GL_COLOR_BUFFER_BIT);
		SetPresentState();
		UploadCanvas();
		glBindTexture(GL_TEXTURE_2D, CanvasTexture);
		glUseProgram(PresentProgram);
		glUniform1i(PresentTexture, 0);
		glEnableVertexAttribArray(PresentPosition);
		glEnableVertexAttribArray(PresentTexCoord);
		glVertexAttribPointer(PresentPosition, 2, GL_FLOAT, GL_FALSE,
			4 * sizeof(GLfloat), quad);
		glVertexAttribPointer(PresentTexCoord, 2, GL_FLOAT, GL_FALSE,
			4 * sizeof(GLfloat), quad + 2);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

		twod->Clear();
		FPSLimit();
		SwapBuffers();
	}

	void Draw2D() override
	{
		// RenderView has just queued the weapon sprites and HUD.  Composite those
		// commands into the same CPU canvas before it is uploaded by Update().
		Composite2D();
	}

	void PostProcessScene(bool, int, float, const std::function<void()> &afterScene) override
	{
		if (afterScene)
			afterScene();
	}
};

#endif

// CODE --------------------------------------------------------------------

#ifdef HAVE_VULKAN
void I_GetVulkanDrawableSize(int *width, int *height)
{
	assert(Priv::vulkanEnabled);
	assert(Priv::window != nullptr);
	SDL_Vulkan_GetDrawableSize(Priv::window, width, height);
}

bool I_GetVulkanPlatformExtensions(unsigned int *count, const char **names)
{
	assert(Priv::vulkanEnabled);
	assert(Priv::window != nullptr);
	return SDL_Vulkan_GetInstanceExtensions(Priv::window, count, names) == SDL_TRUE;
}

bool I_CreateVulkanSurface(VkInstance instance, VkSurfaceKHR *surface)
{
	assert(Priv::vulkanEnabled);
	assert(Priv::window != nullptr);
	return SDL_Vulkan_CreateSurface(Priv::window, instance, surface) == SDL_TRUE;
}
#endif


SDLVideo::SDLVideo ()
{
	if (SDL_Init(SDL_INIT_VIDEO) < 0)
	{
		fprintf(stderr, "Video initialization failed: %s\n", SDL_GetError());
		return;
	}

	// Fail gracefully if we somehow reach here after linking against a SDL2 library older than 2.0.6.
	if (!SDL_VERSION_ATLEAST(2, 0, 6))
	{
		I_FatalError("Only SDL 2.0.6 or later is supported.");
	}

#ifdef HAVE_VULKAN
	Priv::vulkanEnabled = vid_preferbackend == BACKEND_VULKAN;

	if (Priv::vulkanEnabled)
	{
		Priv::CreateWindow(SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN | (vid_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));

		if (Priv::window == nullptr)
		{
			Priv::vulkanEnabled = false;
		}
	}
#endif
}

SDLVideo::~SDLVideo ()
{
#ifdef HAVE_VULKAN
	surface.reset();
#endif
}

void SDLVideo::DumpAdapters()
{
	Priv::updateDisplayInfo();
  for (int i=0; i < Priv::numberOfDisplays; i++) {
	Printf("%s%d. [%dx%d @ (%d,%d)]\n",
		vid_adapter == i ? TEXTCOLOR_BOLD : "",
		i,
		Priv::displayBounds[i].w,
		Priv::displayBounds[i].h,
		Priv::displayBounds[i].x,
		Priv::displayBounds[i].y
	  );
  }
}


DFrameBuffer *SDLVideo::CreateFrameBuffer ()
{
	SystemBaseFrameBuffer *fb = nullptr;

#if defined(VITA)
	// A saved desktop configuration may still contain backend 0 (OpenGL).
	// VitaGL exposes the GLES2-compatible path, while the scene itself remains
	// selected independently by vid_rendermode.
	vid_preferbackend = BACKEND_OPENGLES;
	gl_es = true;
#endif

	// first try Vulkan, if that fails OpenGL
#ifdef HAVE_VULKAN
	if (Priv::vulkanEnabled)
	{
		try
		{
			unsigned int count = 64;
			const char* names[64];
			if (!I_GetVulkanPlatformExtensions(&count, names))
				VulkanError("I_GetVulkanPlatformExtensions failed");

			VulkanInstanceBuilder builder;
			builder.DebugLayer(vk_debug);
			for (unsigned int i = 0; i < count; i++)
				builder.RequireExtension(names[i]);
			auto instance = builder.Create();

			VkSurfaceKHR surfacehandle = nullptr;
			if (!I_CreateVulkanSurface(instance->Instance, &surfacehandle))
				VulkanError("I_CreateVulkanSurface failed");

			surface = std::make_shared<VulkanSurface>(instance, surfacehandle);

			fb = new VulkanRenderDevice(nullptr, vid_fullscreen, surface);
		}
		catch (CVulkanError const &error)
		{
			if (Priv::window != nullptr)
			{
				Priv::DestroyWindow();
			}

			Printf(TEXTCOLOR_RED "Initialization of Vulkan failed: %s\n", error.what());
			Priv::vulkanEnabled = false;
		}
	}
#endif

	if (fb == nullptr)
	{
#ifdef HAVE_GLES2
		if (vid_preferbackend != BACKEND_OPENGL)
		{
		#if defined(VITA)
			// The launcher selects the scene path before V_Init2(): mode 0 keeps
			// the CPU renderer and mode 4 constructs the GLES2/VitaGL renderer.
			if (vid_rendermode != 4)
				fb = new VitaSoftwareFrameBuffer(0, vid_fullscreen);
			else
				fb = new OpenGLESRenderer::OpenGLFrameBuffer(0, vid_fullscreen);
		#else
			fb = new OpenGLESRenderer::OpenGLFrameBuffer(0, vid_fullscreen);
		#endif
		}
		else
#endif
			fb = new OpenGLRenderer::OpenGLFrameBuffer(0, vid_fullscreen);
	}

	return fb;
}


IVideo *gl_CreateVideo()
{
	return new SDLVideo();
}


// FrameBuffer Implementation -----------------------------------------------

SystemBaseFrameBuffer::SystemBaseFrameBuffer (void *, bool fullscreen)
: DFrameBuffer (vid_defwidth, vid_defheight)
{
}

int SystemBaseFrameBuffer::GetClientWidth()
{
	int width = 0;


#ifdef HAVE_VULKAN
	assert(Priv::vulkanEnabled);
	SDL_Vulkan_GetDrawableSize(Priv::window, &width, nullptr);
#endif

	return width;
}

int SystemBaseFrameBuffer::GetClientHeight()
{
	int height = 0;

#ifdef HAVE_VULKAN
	assert(Priv::vulkanEnabled);
	SDL_Vulkan_GetDrawableSize(Priv::window, nullptr, &height);
#endif

	return height;
}

bool SystemBaseFrameBuffer::IsFullscreen ()
{
	return (SDL_GetWindowFlags(Priv::window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
}

void SystemBaseFrameBuffer::ToggleFullscreen(bool yes)
{
	SDL_ShowWindow(Priv::window);
	SDL_SetWindowFullscreen(Priv::window, yes ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
	if ( !yes )
	{
		if ( !Priv::fullscreenSwitch )
		{
			Priv::fullscreenSwitch = true;
			vid_fullscreen = false;
		}
		else
		{
			Priv::fullscreenSwitch = false;
			SetWindowSize(win_w, win_h);
		}
	}
}

void SystemBaseFrameBuffer::SetWindowSize(int w, int h)
{
	if (w < VID_MIN_WIDTH || h < VID_MIN_HEIGHT)
	{
		w = VID_MIN_WIDTH;
		h = VID_MIN_HEIGHT;
	}
	win_w = w;
	win_h = h;
	if (vid_fullscreen)
	{
		vid_fullscreen = false;
	}
	else
	{
		win_maximized = false;
		SDL_SetWindowSize(Priv::window, w, h);
		SDL_SetWindowPosition(Priv::window, SDL_WINDOWPOS_CENTERED_DISPLAY(vid_adapter), SDL_WINDOWPOS_CENTERED_DISPLAY(vid_adapter));
		SetSize(GetClientWidth(), GetClientHeight());
		int x, y;
		SDL_GetWindowPosition(Priv::window, &x, &y);
		win_x = x;
		win_y = y;

	}
}


SystemGLFrameBuffer::SystemGLFrameBuffer(void *hMonitor, bool fullscreen)
: SystemBaseFrameBuffer(hMonitor, fullscreen)
{
	// NOTE: Core profiles were added with GL 3.2, so there's no sense trying
	// to set core 3.1 or 3.0. We could try a forward-compatible context
	// instead, but that would be too restrictive (w.r.t. shaders).
	static const int glvers[][2] = {
		{ 4, 6 }, { 4, 5 }, { 4, 4 }, { 4, 3 }, { 4, 2 }, { 4, 1 }, { 4, 0 },
		{ 3, 3 }, { 3, 2 }, { 2, 0 },
		{ 0, 0 },
	};
	int glveridx = 0;
	int i;

	const char *version = Args->CheckValue(FArg_glversion);
	if (version != NULL)
	{
		double gl_version = strtod(version, NULL) + 0.01;
		int vermaj = (int)gl_version;
		int vermin = (int)(gl_version*10.0) % 10;

		while (glvers[glveridx][0] > vermaj || (glvers[glveridx][0] == vermaj &&
				glvers[glveridx][1] > vermin))
		{
			glveridx++;
			if (glvers[glveridx][0] == 0)
			{
				glveridx = 0;
				break;
			}
		}
	}

	for ( ; glvers[glveridx][0] > 0; ++glveridx)
	{
		Priv::SetupPixelFormat(0, glvers[glveridx]);
#if defined(VITA)
		// Vita has no desktop window manager.  Creating the SDL window hidden
		// can leave SDL's focus state inactive and prevent the main loop from
		// presenting after startup.
		Priv::CreateWindow(SDL_WINDOW_OPENGL | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
#else
		Priv::CreateWindow(SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
#endif

		if (Priv::window == nullptr)
		{
			continue;
		}

		GLContext = SDL_GL_CreateContext(Priv::window);
		if (GLContext == nullptr)
		{
			Priv::DestroyWindow();
		}
		else
		{
#if defined(VITA)
			SDL_ShowWindow(Priv::window);
#endif
			break;
		}
	}
	if (Priv::window == nullptr)
	{
		I_FatalError("Could not create OpenGL window:\n%s\n",SDL_GetError());
	}
}

SystemGLFrameBuffer::~SystemGLFrameBuffer ()
{
	if (Priv::window)
	{
		if (GLContext)
		{
			SDL_GL_DeleteContext(GLContext);
		}

		Priv::DestroyWindow();
	}
}

int SystemGLFrameBuffer::GetClientWidth()
{
	int width = 0;
	SDL_GL_GetDrawableSize(Priv::window, &width, nullptr);
	return width;
}

int SystemGLFrameBuffer::GetClientHeight()
{
	int height = 0;
	SDL_GL_GetDrawableSize(Priv::window, nullptr, &height);
	return height;
}

void SystemGLFrameBuffer::SetVSync( bool vsync )
{
#if defined (__APPLE__)
	const GLint value = vsync ? 1 : 0;
	CGLSetParameter( CGLGetCurrentContext(), kCGLCPSwapInterval, &value );
#else
	if (vsync)
	{
		if (SDL_GL_SetSwapInterval(-1) == -1)
			SDL_GL_SetSwapInterval(1);
	}
	else
	{
		SDL_GL_SetSwapInterval(0);
	}
#endif
}

void SystemGLFrameBuffer::SwapBuffers()
{
	SDL_GL_SwapWindow(Priv::window);
}


void ProcessSDLWindowEvent(const SDL_WindowEvent &event)
{
	switch (event.event)
	{
	extern bool AppActive;

	case SDL_WINDOWEVENT_FOCUS_GAINED:
		S_SetSoundPaused(1);
		AppActive = true;
		break;

	case SDL_WINDOWEVENT_FOCUS_LOST:
		S_SetSoundPaused(0);
		AppActive = false;
		break;

	case SDL_WINDOWEVENT_MOVED:
		if (!vid_fullscreen)
		{
			int top = 0, left = 0;
			SDL_GetWindowBordersSize(Priv::window, &top, &left, nullptr, nullptr);
			win_x = event.data1-left;
			win_y = event.data2-top;
		}
		break;

	case SDL_WINDOWEVENT_RESIZED:
		if (!vid_fullscreen && !Priv::fullscreenSwitch)
		{
			win_w = event.data1;
			win_h = event.data2;
		}
		break;

	case SDL_WINDOWEVENT_MAXIMIZED:
		win_maximized = true;
		break;

	case SDL_WINDOWEVENT_RESTORED:
		win_maximized = false;
		break;
	}
}


// each platform has its own specific version of this function.
void I_SetWindowTitle(const char* caption)
{
	if (caption)
	{
		SDL_SetWindowTitle(Priv::window, caption);
	}
	else
	{
		FString default_caption;
		default_caption.Format(GAMENAME " %s (%s)", GetVersionString(), GetGitTime());
		SDL_SetWindowTitle(Priv::window, default_caption.GetChars());
	}
}
