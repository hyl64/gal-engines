// CCTexture2D.h - the little of cocos2d's texture the software renderer names
//
// visual/RenderManager.cpp is the engine's own software render manager: it
// implements every render method the layers ask for (FillARGB, Copy,
// AlphaBlend, the colour and mask variants, the stretches) on top of the C
// blitters in tvpgl.cpp.  It only mentions cocos2d in the texture adapter the
// GL back ends need - GetAdapterTexture, which a software manager never calls -
// so all it asks for here is the type and its constructors.
//
// This header is not cocos2d: it declares exactly the names that file uses, so
// the software manager can be built without the framework.  Nothing else in the
// port includes it, and nothing calls the adapter.
#ifndef KRKR_COMPAT_CC_TEXTURE_2D_H
#define KRKR_COMPAT_CC_TEXTURE_2D_H

#include <cstddef>

namespace cocos2d
{
	struct Size
	{
		float width;
		float height;
		Size() : width(0), height(0) {}
		Size(float w, float h) : width(w), height(h) {}
		static const Size ZERO;
	};

	class Texture2D
	{
	public:
		enum class PixelFormat
		{
			NONE = 0,
			RGBA8888,
			RGB888,
			RGB565,
		};

		Texture2D() {}
		bool initWithData(const void*, std::size_t, PixelFormat, unsigned, unsigned, Size)
		{
			return false; // there is no GL context to load a texture into
		}
		// the adapter path also updates and queries the texture it made; with
		// no GL context there is nothing to update, and the size stays zero so
		// a caller never takes the adapter as usable
		void updateWithData(const void*, int, int, int, int) {}
		int getPixelsWide() const { return 0; }
		int getPixelsHigh() const { return 0; }
		void autorelease() {}
	};
}

#endif // KRKR_COMPAT_CC_TEXTURE_2D_H
