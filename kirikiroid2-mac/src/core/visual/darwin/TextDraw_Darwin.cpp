/*
 * This file is part of the macOS port of Kirikiroid2 (zeas2/Kirikiroid2).
 *
 * TextDraw_Darwin.cpp - drawing text onto a bitmap
 *
 * The Windows back end's text drawing turns out to carry no GDI call: it builds
 * a texture from the character's coverage, asks the render manager for the
 * colour-map method the blend mode calls for, and composites it with
 * OperateRect.  That is exactly what a software back end can do, so this is the
 * Windows code moved over, with the two scratch textures kept per file instead
 * of per bitmap object.
 *
 * visual/RenderManager.cpp registers the methods it asks for: ApplyColorMap and
 * its _d and _a variants, RemoveOpacity and AlphaBlend_a.
 */

#include "tjsCommHead.h"
#include "tjsString.h"
#include "LayerBitmapIntf.h"
#include "RenderManager.h"
#include "PrerenderedFont.h"
#include "CharacterData.h"
#include "tvpgl.h"
#include "LayerBitmapIntf.h"
#include "FontRasterizer.h"
#include "IndividualConfigManager.h"
#include "MsgIntf.h"
#include "EventIntf.h"

extern FontRasterizer* GetCurrentRasterizer();

#include <cstring>

#define TVP_CH_MAX_CACHE_COUNT 1300
#define TVP_CH_MAX_CACHE_COUNT_LOW 100
#define TVP_CH_MAX_CACHE_HASH_SIZE 512


// what one run of text drawing needs to know, and the scratch textures the
// Windows back end keeps as file statics
typedef tTJSRefHolder<tTVPCharacterData> tTVPCharacterDataHolder;

typedef
tTJSHashCache<tTVPFontAndCharacterData, tTVPCharacterDataHolder,
	tTVPFontHashFunc, TVP_CH_MAX_CACHE_HASH_SIZE> tTVPFontCache;
tTVPFontCache TVPFontCache(TVP_CH_MAX_CACHE_COUNT);
//---------------------------------------------------------------------------
void TVPSetFontCacheForLowMem()
{
	// set character cache limit
	TVPFontCache.SetMaxCount(TVP_CH_MAX_CACHE_COUNT_LOW);
}
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
static tTVPCharacterData * TVPGetCharacter(const tTVPFontAndCharacterData & font,
	tTVPNativeBaseBitmap *bmp, tTVPPrerenderedFont *pfont, tjs_int aofsx, tjs_int aofsy)
{
	// returns specified character data.
	// draw a character if needed.

	// make hash and search over cache
	tjs_uint32 hash = tTVPFontCache::MakeHash(font);

	tTVPCharacterDataHolder * ptr = TVPFontCache.FindAndTouchWithHash(font, hash);
	if(ptr)
	{
		// found in the cache
		return ptr->GetObject();
	}

	// not found in the cache

	// look prerendered font
	const tTVPPrerenderedCharacterItem *pitem = NULL;
	if(pfont)
		pitem = pfont->Find(font.Character);

	if(pitem)
	{
		// prerendered font
		tTVPCharacterData *data = new tTVPCharacterData();
		data->BlackBoxX = pitem->Width;
		data->BlackBoxY = pitem->Height;
		data->Metrics.CellIncX = pitem->IncX;
		data->Metrics.CellIncY = pitem->IncY;
		data->OriginX = pitem->OriginX + aofsx;
		data->OriginY = -pitem->OriginY + aofsy;

		data->Antialiased = font.Antialiased;

		data->FullColored = false;

		data->Blured = font.Blured;
		data->BlurWidth = font.BlurWidth;
		data->BlurLevel = font.BlurLevel;

		try
		{
			if(data->BlackBoxX && data->BlackBoxY)
			{
				// render
				tjs_int newpitch =  (((pitem->Width -1)>>2)+1)<<2;
				data->Pitch = newpitch;

				data->Alloc(newpitch * data->BlackBoxY);

				pfont->Retrieve(pitem, data->GetData(), newpitch);
				data->Gray = 256;
				// apply blur
				if(font.Blured) data->Blur(); // nasty ...

				// add to hash table
				tTVPCharacterDataHolder holder(data);
				TVPFontCache.AddWithHash(font, hash, holder);
			}
		}
		catch(...)
		{
			data->Release();
			throw;
		}

		return data;
	}
	else
	{
		// render font
		tTVPCharacterData *data = GetCurrentRasterizer()->GetBitmap( font, aofsx, aofsy );

		// add to hash table
		tTVPCharacterDataHolder holder(data);
		TVPFontCache.AddWithHash(font, hash, holder);
		return data;
	}
}

struct tTVPCharacterDrawData
{
	tTVPCharacterData * Data; // main character data
	tTVPCharacterData * Shadow; // shadow character data
	tjs_int X, Y;
	tTVPRect ShadowRect;
	bool ShadowDrawn;

	tTVPCharacterDrawData(
		tTVPCharacterData * data,
		tTVPCharacterData * shadow,
		tjs_int x, tjs_int y)
	{
		Data = data;
		Shadow = shadow;
		X = x;
		Y = y;
		ShadowDrawn = false;

		if(Data) Data->AddRef();
		if(Shadow) Shadow->AddRef();
	}

	~tTVPCharacterDrawData()
	{
		if(Data) Data->Release();
		if(Shadow) Shadow->Release();
	}

	tTVPCharacterDrawData(const tTVPCharacterDrawData & rhs)
	{
		Data = Shadow = NULL;
		*this = rhs;
	}

	void operator = (const tTVPCharacterDrawData & rhs)
	{
		X = rhs.X;
		Y = rhs.Y;
		ShadowRect = rhs.ShadowRect;
		ShadowDrawn = rhs.ShadowDrawn;

		if(Data != rhs.Data)
		{
			if(Data) Data->Release();
			Data = rhs.Data;
			if(Data) Data->AddRef();
		}
		if(Shadow != rhs.Shadow)
		{
			if(Shadow) Shadow->Release();
			Shadow = rhs.Shadow;
			if(Shadow) Shadow->AddRef();
		}
	}
};


struct tTVPDrawTextData
{
	tTVPRect rect;
	tjs_int bmppitch;
	tjs_int opa;
	bool holdalpha;
	tTVPBBBltMethod bltmode;
};

static iTVPTexture2D* gCharacterTexture = nullptr;
static iTVPTexture2D* gCharacterTextureRGBA = nullptr;



extern FontRasterizer* GetCurrentRasterizer();
void TVPClearFontCache();
bool TVPIsSoftwareRenderManager();

bool tTVPNativeBaseBitmap::InternalBlendText(
    tTVPCharacterData *data, tTVPDrawTextData *dtdata, tjs_uint32 color, const tTVPRect &srect, tTVPRect &drect)
{
    // blend to the bitmap
    tjs_int pitch = data->Pitch;
    //tjs_uint8 *sl = (tjs_uint8*)GetScanLineForWrite(drect.top);
    tjs_int h = drect.bottom - drect.top;
    tjs_int w = drect.right - drect.left;
	tjs_uint8 *bp = data->GetData() + pitch * srect.top;

	iTVPRenderMethod * method = nullptr;
	int opa_id, clr_id;
#define GEMTHOD_OPA_CLR(n) \
	static iTVPRenderMethod *_method = TVPGetRenderManager()->GetRenderMethod(#n); \
	static int _opa_id = _method->EnumParameterID("opacity"); \
	static int _clr_id = _method->EnumParameterID("color"); \
	method = _method; opa_id = _opa_id; clr_id = _clr_id;

	static bool fastGPURoute = !TVPIsSoftwareRenderManager()
		&& !IndividualConfigManager::GetInstance()->GetValue<bool>("ogl_accurate_render", false);

	iTVPTexture2D *pTexSrc;
	if (fastGPURoute && dtdata->bltmode == bmAlphaOnAlpha && dtdata->opa > 0) {
		// convert to addalpha bitmap
		tTVPBitmap* tmp = new tTVPBitmap(w, h, 32);
		tjs_int spitch = pitch;
		tjs_int dpitch = tmp->GetPitch();
		tjs_uint8 *src = bp;
		tjs_uint8* dst = (tjs_uint8*)tmp->GetBits();
		for (tjs_int y = 0; y < h; ++y) {
			for (tjs_int x = 0; x < w; ++x) {
				((tjs_uint32*)dst)[x] = (color & 0xFFFFFF) | (src[x] << 24);
			}
			TVPConvertAlphaToAdditiveAlpha((tjs_uint32*)dst, w);
			dst += dpitch;
			src += spitch;
		}
		if (gCharacterTextureRGBA) {
			if (gCharacterTextureRGBA->GetFormat() != TVPTextureFormat::RGBA) {
				gCharacterTextureRGBA->Release();
				gCharacterTextureRGBA = nullptr;
			}
		}
		if (!gCharacterTextureRGBA) {
			gCharacterTextureRGBA = GetRenderManager()->CreateTexture2D(tmp->GetBits(), dpitch, w, h, TVPTextureFormat::RGBA, RENDER_CREATE_TEXTURE_FLAG_NO_COMPRESS);
		} else if (gCharacterTextureRGBA->GetInternalWidth() < w || gCharacterTextureRGBA->GetInternalHeight() < h) {
			gCharacterTextureRGBA->Release();
			gCharacterTextureRGBA = GetRenderManager()->CreateTexture2D(tmp->GetBits(), dpitch, w, h, TVPTextureFormat::RGBA, RENDER_CREATE_TEXTURE_FLAG_NO_COMPRESS);
		} else {
			gCharacterTextureRGBA->Update(tmp->GetBits(), TVPTextureFormat::RGBA, dpitch, tTVPRect(0, 0, w, h));
		}

		tmp->Release();

		GEMTHOD_OPA_CLR(AlphaBlend_a);
		method->SetParameterOpa(opa_id, dtdata->opa);
		pTexSrc = gCharacterTextureRGBA;
	} else {
		if (dtdata->bltmode == bmAlphaOnAlpha) {
			if (dtdata->opa > 0) {
				GEMTHOD_OPA_CLR(ApplyColorMap_d);
			} else {
				// opacity removal
				GEMTHOD_OPA_CLR(RemoveOpacity);
			}
		} else if (dtdata->bltmode == bmAlphaOnAddAlpha) {
			GEMTHOD_OPA_CLR(ApplyColorMap_a);
		} else {
			GEMTHOD_OPA_CLR(ApplyColorMap);
		}

		// blend to the texture
		if (!gCharacterTexture) {
			gCharacterTexture = GetRenderManager()->CreateTexture2D(nullptr, pitch, w, h, TVPTextureFormat::Gray);
		} else if (gCharacterTexture->GetInternalWidth() < w || gCharacterTexture->GetInternalHeight() < h) {
			gCharacterTexture->Release();
			gCharacterTexture = GetRenderManager()->CreateTexture2D(nullptr, pitch, w, h, TVPTextureFormat::Gray);
		}
		gCharacterTexture->Update(bp, TVPTextureFormat::Gray, pitch, tTVPRect(0, 0, w, h));

		method->SetParameterOpa(opa_id, dtdata->opa);
		method->SetParameterColor4B(clr_id, color);

		pTexSrc = gCharacterTexture;
	}
#if 0
    if (pShader->isBlendEnabled() || !IsIndependent()) {
		iTVPTexture2D *origTex = GetTexture();
		iTVPTexture2D *tex = GetTextureForRender();
        TVPRenderTexture2(pShader,
            tex, drect,
            origTex, drect,
            gCharacterTexture, texRect(0, 0, w, h));
    } else { // optimize for independent texture
		iTVPTexture2D *tmptex = TVPCreateTextureForRender(drect.get_width(), drect.get_height());
		iTVPTexture2D *tex = GetTexture();
        texRect rc(drect);
        rc.x = 0; rc.y = 0;
        TVPCopyTexture(
            tmptex, rc,
            tex, drect);
        TVPRenderTexture2(pShader,
            tex, drect,
            tmptex, rc,
            gCharacterTexture, texRect(0, 0, w, h));
        tmptex->Release();
    }
#endif
	tRenderTexRectArray::Element src_tex[] = {
		tRenderTexRectArray::Element(pTexSrc, tTVPRect(0, 0, w, h))
	};
	TVPGetRenderManager()->OperateRect(method,
		GetTextureForRender(method->IsBlendTarget(), &drect), nullptr, drect,
		tRenderTexRectArray(src_tex));
    return true;
}

bool tTVPNativeBaseBitmap::InternalDrawText(tTVPCharacterData *data, tjs_int x,
	tjs_int y, tjs_uint32 color, tTVPDrawTextData *dtdata, tTVPRect &drect)
{
	// setup destination and source rectangle
	drect.left = x + data->OriginX;
	drect.top = y + data->OriginY;
	drect.right = drect.left + data->BlackBoxX;
	drect.bottom = drect.top + data->BlackBoxY;

	tTVPRect srect;
	srect.left = srect.top = 0;
	srect.right = data->BlackBoxX;
	srect.bottom = data->BlackBoxY;

	// check boundary
	if(drect.left < dtdata->rect.left)
	{
		srect.left += (dtdata->rect.left - drect.left);
		drect.left = dtdata->rect.left;
	}

	if(drect.right > dtdata->rect.right)
	{
		srect.right -= (drect.right - dtdata->rect.right);
		drect.right = dtdata->rect.right;
	}

	if(srect.left >= srect.right) return false; // not drawable

	if(drect.top < dtdata->rect.top)
	{
		srect.top += (dtdata->rect.top - drect.top);
		drect.top = dtdata->rect.top;
	}

	if(drect.bottom > dtdata->rect.bottom)
	{
		srect.bottom -= (drect.bottom - dtdata->rect.bottom);
		drect.bottom = dtdata->rect.bottom;
	}

	if(srect.top >= srect.bottom) return false; // not drawable

    return InternalBlendText(data, dtdata, color, srect, drect);
}

void tTVPNativeBaseBitmap::DrawGlyph(iTJSDispatch2* glyph, const tTVPRect &destrect, tjs_int x, tjs_int y,
			tjs_uint32 color, tTVPBBBltMethod bltmode, tjs_int opa,
			bool holdalpha, bool aa, tjs_int shlevel,
			tjs_uint32 shadowcolor,
			tjs_int shwidth, tjs_int shofsx, tjs_int shofsy,
			tTVPComplexRect *updaterects )
{
	if(!Is32BPP()) TVPThrowExceptionMessage(TVPInvalidOperationFor8BPP);

	if(bltmode == bmAlphaOnAlpha)
	{
		if(opa < -255) opa = -255;
		if(opa > 255) opa = 255;
	}
	else
	{
		if(opa < 0) opa = 0;
		if(opa > 255 ) opa = 255;
	}

	if(opa == 0) return; // nothing to do


	tjs_int itemcount;
	tTJSVariant tmp;
	if(TJS_SUCCEEDED(glyph->PropGet(TJS_MEMBERMUSTEXIST, TJS_W("count"), 0, &tmp, glyph)))
		itemcount = tmp;
	else
		itemcount = 0;

	if( itemcount < 8 ) TVPThrowExceptionMessage( TVPFaildGlyphForDrawGlyph );

	enum {
		GLYPH_WIDTH,
		GLYPH_HEIGHT,
		GLYPH_ORIGINX,
		GLYPH_ORIGINY,
		GLYPH_INCX,
		GLYPH_INCY,
		GLYPH_INC,
		GLYPH_BITMAP,
		GLYPH_COLORS,
		GLYPH_EOT
	};
	tjs_int glyphitem[7];
	for( tjs_int i = 0; i < 7; i++ ) {
		if(TJS_FAILED(glyph->PropGetByNum(TJS_MEMBERMUSTEXIST, i, &tmp, glyph)))
			TVPThrowExceptionMessage( TVPFaildGlyphForDrawGlyph );
		glyphitem[i] = tmp;
	}

	if(TJS_FAILED(glyph->PropGetByNum(TJS_MEMBERMUSTEXIST, GLYPH_BITMAP, &tmp, glyph)))
		TVPThrowExceptionMessage( TVPFaildGlyphForDrawGlyph );

	tjs_int numcolor = 256;
	if( itemcount >= 9 ) {
		if(TJS_FAILED(glyph->PropGetByNum(TJS_MEMBERMUSTEXIST, GLYPH_COLORS, &tmp, glyph)))
			TVPThrowExceptionMessage( TVPFaildGlyphForDrawGlyph );
		numcolor = tmp;
	}
	tTJSVariantOctet *o = tmp.AsOctetNoAddRef();

	Independ();
	ApplyFont();

	tTVPDrawTextData dtdata;
	dtdata.rect = destrect;
	dtdata.bmppitch = GetPitchBytes();
	dtdata.bltmode = bltmode;
	dtdata.opa = opa;
	dtdata.holdalpha = holdalpha;

	tTVPCharacterData* data = NULL;
	tTVPCharacterData* shadow = NULL;
	try {
		tGlyphMetrics metrics;
		metrics.CellIncX = glyphitem[GLYPH_INCX];
		metrics.CellIncY = glyphitem[GLYPH_INCY];
		data = new tTVPCharacterData( o->GetData(), glyphitem[GLYPH_WIDTH],
			glyphitem[GLYPH_ORIGINX] + AscentOfsX, -glyphitem[GLYPH_ORIGINY] + AscentOfsY,
			glyphitem[GLYPH_WIDTH], glyphitem[GLYPH_HEIGHT],
			metrics, numcolor > 256 );

		data->Antialiased = aa;
		data->Blured = false;
		data->BlurWidth = shwidth;
		data->BlurLevel = shlevel;
		data->Gray = numcolor;
		if( shlevel != 0 ) {
			if( shlevel == 255 && shwidth == 0 ) {
				// normal shadow
				shadow = data;
				shadow->AddRef();
			} else {
				// blured shadow
				shadow = new tTVPCharacterData( o->GetData(), glyphitem[GLYPH_WIDTH],
					glyphitem[GLYPH_ORIGINX] + AscentOfsX, -glyphitem[GLYPH_ORIGINY] + AscentOfsY,
					glyphitem[GLYPH_WIDTH], glyphitem[GLYPH_HEIGHT],
					metrics, numcolor > 256 );
				shadow->Antialiased = aa;
				shadow->Blured = true;
				shadow->BlurWidth = shwidth;
				shadow->BlurLevel = shlevel;
				shadow->Gray = numcolor;
				if( !shadow->FullColored ) shadow->Blur();
			}
		}

		if(data)
		{

			if(data->BlackBoxX != 0 && data->BlackBoxY != 0)
			{
				tTVPRect drect;
				tTVPRect shadowdrect;

				bool shadowdrawn = false;

				if(shadow)
				{
					shadowdrawn = InternalDrawText(shadow, x + shofsx, y + shofsy,
						shadowcolor, &dtdata, shadowdrect);
				}

				bool drawn = InternalDrawText(data, x, y, color, &dtdata, drect);
				if(updaterects)
				{
					if(!shadowdrawn)
					{
						if(drawn) updaterects->Or(drect);
					}
					else
					{
						if(drawn)
						{
							tTVPRect d;
							TVPUnionRect(&d, drect, shadowdrect);
							updaterects->Or(d);
						}
						else
						{
							updaterects->Or(shadowdrect);
						}
					}
				}
			}
		}
	}
	catch(...)
	{
		if(data) data->Release();
		if(shadow) shadow->Release();
		throw;
	}

	if(data) data->Release();
	if(shadow) shadow->Release();
}

void tTVPNativeBaseBitmap::DrawTextSingle(const tTVPRect &destrect,
	tjs_int x, tjs_int y, const ttstr &text,
		tjs_uint32 color, tTVPBBBltMethod bltmode, tjs_int opa,
			bool holdalpha, bool aa, tjs_int shlevel,
			tjs_uint32 shadowcolor,
			tjs_int shwidth, tjs_int shofsx, tjs_int shofsy,
			tTVPComplexRect *updaterects)
{
	// text drawing function for single character

	if(!Is32BPP()) TVPThrowExceptionMessage(TVPInvalidOperationFor8BPP);

	if(bltmode == bmAlphaOnAlpha)
	{
		if(opa < -255) opa = -255;
		if(opa > 255) opa = 255;
	}
	else
	{
		if(opa < 0) opa = 0;
		if(opa > 255 ) opa = 255;
	}

	if(opa == 0) return; // nothing to do

	Independ();

	ApplyFont();

	const tjs_char *p = text.c_str();
	tTVPDrawTextData dtdata;
	dtdata.rect = destrect;
	dtdata.bmppitch = GetPitchBytes();
	dtdata.bltmode = bltmode;
	dtdata.opa = opa;
	dtdata.holdalpha = holdalpha;

	tTVPFontAndCharacterData font;
	font.Font = Font;
	font.Antialiased = aa;
	font.Hinting = true;
	font.BlurLevel = shlevel;
	font.BlurWidth = shwidth;
	font.FontHash = FontHash;

	font.Character = *p;

	font.Blured = false;
	tTVPCharacterData * shadow = NULL;
	tTVPCharacterData * data = NULL;

	try
	{
		data = TVPGetCharacter(font, this, PrerenderedFont, AscentOfsX, AscentOfsY);

		if(shlevel != 0)
		{
			if(shlevel == 255 && shwidth == 0)
			{
				// normal shadow
				shadow = data;
				shadow->AddRef();
			}
			else
			{
				// blured shadow
				font.Blured = true;
				shadow =
					TVPGetCharacter(font, this, PrerenderedFont, AscentOfsX, AscentOfsY);
			}
		}


		if(data)
		{

			if(data->BlackBoxX != 0 && data->BlackBoxY != 0)
			{
				tTVPRect drect;
				tTVPRect shadowdrect;

				bool shadowdrawn = false;

				if(shadow)
				{
					shadowdrawn = InternalDrawText(shadow, x + shofsx, y + shofsy,
						shadowcolor, &dtdata, shadowdrect);
				}

				bool drawn = InternalDrawText(data, x, y, color, &dtdata, drect);
				if(updaterects)
				{
					if(!shadowdrawn)
					{
						if(drawn) updaterects->Or(drect);
					}
					else
					{
						if(drawn)
						{
							tTVPRect d;
							TVPUnionRect(&d, drect, shadowdrect);
							updaterects->Or(d);
						}
						else
						{
							updaterects->Or(shadowdrect);
						}
					}
				}
			}
		}
	}
	catch(...)
	{
		if(data) data->Release();
		if(shadow) shadow->Release();
		throw;
	}

	if(data) data->Release();
	if(shadow) shadow->Release();
}

void tTVPNativeBaseBitmap::DrawTextMultiple(const tTVPRect &destrect,
	tjs_int x, tjs_int y, const ttstr &text,
		tjs_uint32 color, tTVPBBBltMethod bltmode, tjs_int opa,
			bool holdalpha, bool aa, tjs_int shlevel,
			tjs_uint32 shadowcolor,
			tjs_int shwidth, tjs_int shofsx, tjs_int shofsy,
			tTVPComplexRect *updaterects)
{
	// text drawing function for multiple characters

	if(!Is32BPP()) TVPThrowExceptionMessage(TVPInvalidOperationFor8BPP);

	if(bltmode == bmAlphaOnAlpha)
	{
		if(opa < -255) opa = -255;
		if(opa > 255) opa = 255;
	}
	else
	{
		if(opa < 0) opa = 0;
		if(opa > 255 ) opa = 255;
	}

	if(opa == 0) return; // nothing to do

	Independ();

	ApplyFont();

	const tjs_char *p = text.c_str();
	tTVPDrawTextData dtdata;
	dtdata.rect = destrect;
	dtdata.bmppitch = GetPitchBytes();
	dtdata.bltmode = bltmode;
	dtdata.opa = opa;
	dtdata.holdalpha = holdalpha;

	tTVPFontAndCharacterData font;
	font.Font = Font;
	font.Antialiased = aa;
	font.Hinting = true;
	font.BlurLevel = shlevel;
	font.BlurWidth = shwidth;
	font.FontHash = FontHash;


	std::vector<tTVPCharacterDrawData> drawdata;
	drawdata.reserve(text.GetLen());

	// prepare all drawn characters
	while(*p) // while input string is remaining
	{
		font.Character = *p;

		font.Blured = false;
		tTVPCharacterData * data = NULL;
		tTVPCharacterData * shadow = NULL;
		try
		{
			data =
				TVPGetCharacter(font, this, PrerenderedFont, AscentOfsX, AscentOfsY);

			if(data)
			{
				if(shlevel != 0)
				{
					if(shlevel == 255 && shwidth == 0)
					{
						// normal shadow
						// shadow is the same as main character data
						shadow = data;
						shadow->AddRef();
					}
					else
					{
						// blured shadow
						font.Blured = true;
						shadow =
							TVPGetCharacter(font, this, PrerenderedFont, AscentOfsX, AscentOfsY);
					}
				}


				if(data->BlackBoxX != 0 && data->BlackBoxY != 0)
				{
					// append to array
					drawdata.push_back(tTVPCharacterDrawData(data, shadow, x, y));
				}

				// step to the next character position
				x += data->Metrics.CellIncX;
				if(data->Metrics.CellIncY != 0)
				{
					// Windows 9x returns negative CellIncY.
					// so we must verify whether CellIncY is proper.
					if(Font.Angle < 1800)
					{
						if(data->Metrics.CellIncY > 0) data->Metrics.CellIncY = - data->Metrics.CellIncY;
					}
					else
					{
						if(data->Metrics.CellIncY < 0) data->Metrics.CellIncY = - data->Metrics.CellIncY;
					}
					y += data->Metrics.CellIncY;
				}
			}
		}
		catch(...)
		{
			 if(data) data->Release();
			 if(shadow) shadow->Release();
			 throw;
		}
		if(data) data->Release();
		if(shadow) shadow->Release();

		p++;
	}

	// draw shadows first
	if(shlevel != 0)
	{
		for(std::vector<tTVPCharacterDrawData>::iterator i = drawdata.begin();
			i != drawdata.end(); i++)
		{
			tTVPCharacterData * shadow = i->Shadow;

			if(shadow)
			{
				i->ShadowDrawn = InternalDrawText(shadow, i->X + shofsx, i->Y + shofsy,
					shadowcolor, &dtdata, i->ShadowRect);
			}
		}
	}

	// then draw main characters
	// and compute returning update rectangle
	for(std::vector<tTVPCharacterDrawData>::iterator i = drawdata.begin();
		i != drawdata.end(); i++)
	{
		tTVPCharacterData * data = i->Data;
		tTVPRect drect;

		bool drawn = InternalDrawText(data, i->X, i->Y, color, &dtdata, drect);
		if(updaterects)
		{
			if(!i->ShadowDrawn)
			{
				if(drawn) updaterects->Or(drect);
			}
			else
			{
				if(drawn)
				{
					tTVPRect d;
					TVPUnionRect(&d, drect, i->ShadowRect);
					updaterects->Or(d);
				}
				else
				{
					updaterects->Or(i->ShadowRect);
				}
			}
		}
	}
}