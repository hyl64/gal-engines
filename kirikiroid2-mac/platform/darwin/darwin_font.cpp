/*
 * This file is part of the macOS port of Kirikiroid2 (zeas2/Kirikiroid2).
 *
 * darwin_font.cpp - the font hooks the portable font stack asks the platform for
 *
 * visual/FreeType.cpp, FreeTypeFontRasterizer.cpp, FontSystem.cpp and
 * PrerenderedFont.cpp hold the whole text pipeline and are platform
 * independent; what they need from a back end is small and is here:
 *
 *   TVPGetDefaultFontName  - the face a game gets when it asks for none
 *   TVPCreateFontStream    - open a face by name through the storage layer
 *   TVPGetAllFontList      - the faces the system offers (the font dialog)
 *   TVPFontSystem          - the one FontSystem instance
 *
 * macOS ships no MS Gothic and no IPA fonts, so the default is the Hiragino
 * family (kana and kanji) with Arial Unicode as the fallback; ONS_FONT or
 * KRKR_FONT pick another face by path.
 */

#include "tjsCommHead.h"
#include "tjsString.h"
#include "tjsError.h"
#include "FontImpl.h"
#include "FontSystem.h"
#include "StorageIntf.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>

// the one font system the engine's text code reaches for
FontSystem* TVPFontSystem = nullptr;

static const char* FindSystemFont()
{
	static const char* const kCandidates[] = {
		"/System/Library/Fonts/Hiragino Sans GB.ttc",
		"/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
		"/System/Library/Fonts/AppleSDGothicNeo.ttc",
		"/System/Library/Fonts/Hiragino Sans W3.ttc",
		"/Library/Fonts/Arial Unicode.ttf",
		nullptr
	};
	const char* env = getenv("KRKR_FONT");
	if(env != nullptr && *env != '\0')
		return env;
	env = getenv("ONS_FONT");
	if(env != nullptr && *env != '\0')
		return env;
	for(int i = 0; kCandidates[i] != nullptr; i++)
	{
		FILE* fp = fopen(kCandidates[i], "rb");
		if(fp != nullptr)
		{
			fclose(fp);
			return kCandidates[i];
		}
	}
	return nullptr;
}

const ttstr& TVPGetDefaultFontName()
{
	static ttstr name;
	if(name.IsEmpty())
	{
		const char* found = FindSystemFont();
		if(found != nullptr)
		{
			std::string storage("file://");
			storage += found;
			name = ttstr(storage.c_str());
		}
		else
		{
			// nothing installed: a name the rasterizer will fail on, but the
			// engine keeps running and its text is empty rather than crashing
			name = ttstr("file:///System/Library/Fonts/Hiragino Sans GB.ttc");
		}
		if(TVPFontSystem == nullptr)
			TVPFontSystem = new FontSystem();
	}
	return name;
}

tTJSBinaryStream* TVPCreateFontStream(const ttstr& fontname)
{
	return TVPCreateStream(fontname);
}

// the faces a font dialog may offer: the system font folders' files
static void CollectFonts(const char* dir, std::vector<ttstr>& list, int depth)
{
	DIR* d = opendir(dir);
	if(d == nullptr)
		return;
	struct dirent* e;
	while((e = readdir(d)) != nullptr)
	{
		if(e->d_name[0] == '.')
			continue;
		std::string path = std::string(dir) + "/" + e->d_name;
		struct stat st;
		if(stat(path.c_str(), &st) != 0)
			continue;
		if(S_ISDIR(st.st_mode))
		{
			if(depth < 2)
				CollectFonts(path.c_str(), list, depth + 1);
			continue;
		}
		size_t len = strlen(e->d_name);
		if(len > 4)
		{
			const char* ext = e->d_name + len - 4;
			if(strcasecmp(ext, ".ttf") == 0 || strcasecmp(ext, ".ttc") == 0
				|| strcasecmp(ext, ".otf") == 0)
				list.push_back(ttstr(path.c_str()));
		}
	}
	closedir(d);
}

void TVPGetAllFontList(std::vector<ttstr>& list)
{
	CollectFonts("/System/Library/Fonts", list, 0);
	CollectFonts("/Library/Fonts", list, 0);
	const char* home = getenv("HOME");
	if(home != nullptr)
	{
		std::string userFonts = std::string(home) + "/Library/Fonts";
		CollectFonts(userFonts.c_str(), list, 0);
	}
}
