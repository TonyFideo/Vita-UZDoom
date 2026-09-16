/*
** i_specialpaths.cpp
**
** Vita-specific persistent paths.
*/

#include <string>

#include <vitasdk.h>

#include "cmdlib.h"
#include "i_specialpaths.h"
#include "vita_platform.h"

extern bool netgame;

namespace
{
	// sceIoMkdir returns an error when a component already exists.  That is
	// expected here, so each component is attempted without treating EEXIST as
	// fatal.  The Vita device prefix (ux0:) is not itself a directory.
	void CreateVitaPath(const char *path)
	{
		std::string current;
		for (const char *cursor = path; ; ++cursor)
		{
			if (*cursor == '/' || *cursor == '\0')
			{
				if (!current.empty() && current != "ux0:" && current != "app0:")
					sceIoMkdir(current.c_str(), 0777);

				if (*cursor == '\0') break;
				// Keep the device separator in the accumulated path. Without
				// this, the next component would turn ux0:/data into ux0:data.
				current.push_back('/');
			}
			else
			{
				current.push_back(*cursor);
			}
		}
	}

	FString VitaPath(const char *leaf = nullptr)
	{
		if (leaf == nullptr || *leaf == '\0') return FString(UZDOOM_VITA_DATA_ROOT);
		return FStringf("%s/%s", UZDOOM_VITA_DATA_ROOT, leaf);
	}
}

const char *GetConfigPath()
{
	return UZDOOM_VITA_DATA_ROOT;
}

const char *GetCachePath()
{
	return UZDOOM_VITA_DATA_ROOT "/cache";
}

const char *GetDataPath()
{
	return UZDOOM_VITA_DATA_ROOT;
}

const char *GetPicturesPath()
{
	return UZDOOM_VITA_DATA_ROOT "/screenshots";
}

FString GetUserFile(const char *file)
{
	CreateVitaPath(UZDOOM_VITA_DATA_ROOT);
	return VitaPath(file);
}

FString M_GetAppDataPath(bool create)
{
	FString path = VitaPath();
	if (create) CreateVitaPath(path.GetChars());
	return path;
}

FString M_GetCachePath(bool create, FString ns)
{
	FString path = FStringf("%s/cache/%s", UZDOOM_VITA_DATA_ROOT, ns.GetChars());
	if (create) CreateVitaPath(path.GetChars());
	return path;
}

FString M_GetAutoexecPath()
{
	return GetUserFile("autoexec.cfg");
}

FString M_GetConfigPath(bool)
{
	return GetUserFile(GAMENAMELOWERCASE ".ini");
}

FString M_GetDocumentsPath()
{
	return VitaPath();
}

FString M_GetScreenshotsPath()
{
	FString path = VitaPath("screenshots");
	CreateVitaPath(path.GetChars());
	return path;
}

FString M_GetSavegamesPath()
{
	FString path = netgame ? VitaPath("savegames/netgame") : VitaPath("savegames");
	CreateVitaPath(path.GetChars());
	return path + "/";
}

FString M_GetDemoPath()
{
	FString path = VitaPath("demo");
	CreateVitaPath(path.GetChars());
	return path + "/";
}

FString M_GetNormalizedPath(const char *path)
{
	return path != nullptr ? NicePath(path) : FString();
}
