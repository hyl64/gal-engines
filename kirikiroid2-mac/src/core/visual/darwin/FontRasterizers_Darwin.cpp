/*
 * This file is part of the macOS port of Kirikiroid2 (zeas2/Kirikiroid2).
 *
 * darwin_font_rasterizers.cpp - the font rasterizers and the prerendered font map
 *
 * The Windows back end keeps this in visual/win32/LayerBitmapImpl.cpp, which is
 * GDI code; the part that decides which rasterizer draws text and remembers the
 * glyphs is not: it creates the engine's FreeType rasterizer, holds the font
 * system, and maps a game's prerendered fonts (bitmap fonts shipped with the
 * game) onto the faces that stand in for them.  That part is here, moved rather
 * than rewritten, because the same code serves the software blitter.
 */

#include "tjsCommHead.h"
#include "tjsString.h"
#include "FreeType.h"
#include "FreeTypeFontRasterizer.h"
#include "PrerenderedFont.h"
#include "FontSystem.h"
#include "MsgIntf.h"
#include "SysInitIntf.h"

// the state counter ApplyFont compares against
tjs_int TVPGlobalFontStateMagic = 0;


// the lookup of a game's prerendered font for a face
tTVPPrerenderedFont* TVPGetPrerenderedMappedFont(const tTVPFont& font);

void TVPClearFontCache(); // clears the glyph cache after a rasterizer change

//---------------------------------------------------------------------------
// TVPFontSystem is defined in darwin_font.cpp alongside the default face
extern FontSystem* TVPFontSystem;

	// this is for checking global font status' change


enum {
	FONT_RASTER_FREE_TYPE,
//	FONT_RASTER_GDI,
	FONT_RASTER_EOT
};
static FontRasterizer* TVPFontRasterizers[FONT_RASTER_EOT];
static bool TVPFontRasterizersInit = false;
static tjs_int TVPCurrentFontRasterizers = FONT_RASTER_FREE_TYPE;
//static tjs_int TVPCurrentFontRasterizers = FONT_RASTER_GDI;
void TVPInializeFontRasterizers() {
	if( TVPFontRasterizersInit == false ) {
		TVPFontRasterizers[FONT_RASTER_FREE_TYPE] = new FreeTypeFontRasterizer();

		TVPFontSystem = new FontSystem();
		TVPFontRasterizersInit = true;
	}
}
void TVPUninitializeFontRasterizers() {
	for( tjs_int i = 0; i < FONT_RASTER_EOT; i++ ) {
		if( TVPFontRasterizers[i] ) {
			TVPFontRasterizers[i]->Release();
			TVPFontRasterizers[i] = NULL;
		}
	}
	if( TVPFontSystem ) {
		delete TVPFontSystem;
		TVPFontSystem = NULL;
	}
}
static tTVPAtExit
	TVPUninitializeFontRaster(TVP_ATEXIT_PRI_RELEASE, TVPUninitializeFontRasterizers);

void TVPSetFontRasterizer( tjs_int index ) {
	if( TVPCurrentFontRasterizers != index && index >= 0 && index < FONT_RASTER_EOT ) {
		TVPCurrentFontRasterizers = index;
		TVPClearFontCache(); // ラスタライザが切り替わる時、キャッシュはクリアしてしまう
		TVPGlobalFontStateMagic++; // ApplyFont が走るようにする
	}
}
tjs_int TVPGetFontRasterizer() {
	return TVPCurrentFontRasterizers;
}
FontRasterizer* GetCurrentRasterizer() {
	extern void TVPInializeFontRasterizers();
	if(!TVPFontRasterizersInit)
		TVPInializeFontRasterizers();
	return TVPFontRasterizers[TVPCurrentFontRasterizers];
}

//---------------------------------------------------------------------------
#define TVP_CH_MAX_CACHE_COUNT 1300
#define TVP_CH_MAX_CACHE_COUNT_LOW 100
#define TVP_CH_MAX_CACHE_HASH_SIZE 512
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
// Pre-rendered font management
//---------------------------------------------------------------------------
tTJSHashTable<ttstr, tTVPPrerenderedFont *> TVPPrerenderedFonts;



//---------------------------------------------------------------------------
// tTVPPrerenderedFontMap
//---------------------------------------------------------------------------
struct tTVPPrerenderedFontMap
{
	tTVPFont Font; // mapped font
	tTVPPrerenderedFont * Object; // prerendered font object
};
static std::vector<tTVPPrerenderedFontMap> TVPPrerenderedFontMapVector;
//---------------------------------------------------------------------------
void TVPMapPrerenderedFont(const tTVPFont & font, const ttstr & storage)
{
	// map specified font to specified prerendered font
	ttstr fn = TVPSearchPlacedPath(storage);

	// search or retrieve specified storage
	tTVPPrerenderedFont * object;

	tTVPPrerenderedFont ** found = TVPPrerenderedFonts.Find(fn);
	if(!found)
	{
		// not yet exist; create
		object = new tTVPPrerenderedFont(fn);
	}
	else
	{
		// already exist
		object = *found;
		object->AddRef();
	}

	// search existing mapped font
	std::vector<tTVPPrerenderedFontMap>::iterator i;
	for(i = TVPPrerenderedFontMapVector.begin();
		i !=TVPPrerenderedFontMapVector.end(); i++)
	{
		if(i->Font == font)
		{
			// found font
			// replace existing
			i->Object->Release();
			i->Object = object;
			break;
		}
	}
	if(i == TVPPrerenderedFontMapVector.end())
	{
		// not found
		tTVPPrerenderedFontMap map;
		map.Font = font;
		map.Object = object;
		TVPPrerenderedFontMapVector.push_back(map); // add
	}

	TVPGlobalFontStateMagic ++; // increase magic number

	TVPClearFontCache(); // clear font cache
}
//---------------------------------------------------------------------------
void TVPUnmapPrerenderedFont(const tTVPFont & font)
{
	// unmap specified font
	std::vector<tTVPPrerenderedFontMap>::iterator i;
	for(i = TVPPrerenderedFontMapVector.begin();
		i !=TVPPrerenderedFontMapVector.end(); i++)
	{
		if(i->Font == font)
		{
			// found font
			// replace existing
			i->Object->Release();
			TVPPrerenderedFontMapVector.erase(i);
			TVPGlobalFontStateMagic ++; // increase magic number
			TVPClearFontCache();
			return;
		}
	}
}
//---------------------------------------------------------------------------
static void TVPUnmapAllPrerenderedFonts()
{
	// unmap all prerendered fonts
	std::vector<tTVPPrerenderedFontMap>::iterator i;
	for(i = TVPPrerenderedFontMapVector.begin();
		i !=TVPPrerenderedFontMapVector.end(); i++)
	{
		i->Object->Release();
	}
	TVPPrerenderedFontMapVector.clear();
	TVPGlobalFontStateMagic ++; // increase magic number
}
//---------------------------------------------------------------------------
static tTVPAtExit TVPUnmapAllPrerenderedFontsAtExit
	(TVP_ATEXIT_PRI_PREPARE, TVPUnmapAllPrerenderedFonts);
//---------------------------------------------------------------------------
// The glyph cache itself belongs to the same Windows file and is filled while
// drawing; this back end draws through the rasterizer directly, so there is no
// cache to clear yet.
void TVPClearFontCache()
{
}
tTVPPrerenderedFont* TVPGetPrerenderedMappedFont(const tTVPFont &font)
{
	// search mapped prerendered font
	std::vector<tTVPPrerenderedFontMap>::iterator i;
	for(i = TVPPrerenderedFontMapVector.begin();
		i !=TVPPrerenderedFontMapVector.end(); i++)
	{
		if(i->Font == font)
		{
			// found font
			// replace existing
			i->Object->AddRef();

			// note that the object is AddRefed
			return i->Object;
		}
	}
	return NULL;
}
