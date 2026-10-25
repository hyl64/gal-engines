/*
 * This file is part of the macOS port of Kirikiroid2 (zeas2/Kirikiroid2).
 *
 * VideoOverlay_Darwin.cpp - the script-visible video overlay
 *
 * KAG's Movie class derives from this one (class Movie extends VideoOverlay) and
 * builds its players from it, so the class has to answer even with no video path:
 * it keeps the rectangle, the flags and the audio-stream settings a script sets,
 * opens nothing and plays nothing.
 */

#include "tjsCommHead.h"
#include "tjsNative.h"
#include "tjsVariant.h"

namespace
{
	class OverlayInstance : public tTJSNativeInstance
	{
	public:
		tjs_int Mode = 0;
		bool Visible = false;
		bool Looping = false;
		tjs_int Frame = 0;
		tjs_int Left = 0;
		tjs_int Top = 0;
		tjs_int Width = 0;
		tjs_int Height = 0;
		tjs_int AudioBalance = 0;
		tjs_int AudioVolume = 0;
		tjs_int EnabledAudioStream = 0;
		tjs_int NumberOfAudioStream = 0;
		double Position = 0.0;
		double TotalTime = 0.0;
		bool Playing = false;
	};
}

class tTJSNC_VideoOverlay_Darwin : public tTJSNativeClass
{
	typedef tTJSNativeClass inherited;

public:
	tTJSNC_VideoOverlay_Darwin();
	static tjs_uint32 ClassID;

private:
	iTJSNativeInstance* CreateNativeInstance() override;
};

tjs_uint32 tTJSNC_VideoOverlay_Darwin::ClassID = (tjs_uint32)-1;

tTJSNC_VideoOverlay_Darwin::tTJSNC_VideoOverlay_Darwin()
	: inherited(TJS_W("VideoOverlay"))
{
	TJS_BEGIN_NATIVE_MEMBERS(VideoOverlay)
	TJS_DECL_EMPTY_FINALIZE_METHOD

	TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL(/*var.name*/ _this,
		/*var.type*/ OverlayInstance, /*TJS class name*/ VideoOverlay)
	{
		return TJS_S_OK;
	}
	TJS_END_NATIVE_CONSTRUCTOR_DECL(/*TJS class name*/ VideoOverlay)

#define OVERLAY_SIMPLE(name, action)                                           \
	TJS_BEGIN_NATIVE_METHOD_DECL(name)                                          \
	{                                                                          \
		TJS_GET_NATIVE_INSTANCE(_this, OverlayInstance);                        \
		action                                                                  \
		return TJS_S_OK;                                                        \
	}                                                                          \
	TJS_END_NATIVE_METHOD_DECL(name)

	OVERLAY_SIMPLE(/*func. name*/ open, (void)numparams; (void)param; _this->Visible = true;)
	OVERLAY_SIMPLE(/*func. name*/ close, _this->Visible = false; _this->Playing = false;)
	OVERLAY_SIMPLE(/*func. name*/ play, _this->Playing = true;)
	OVERLAY_SIMPLE(/*func. name*/ stop, _this->Playing = false;)
	OVERLAY_SIMPLE(/*func. name*/ pause, _this->Playing = false;)
#undef OVERLAY_SIMPLE

#define OVERLAY_INT_PROP(name, field)                                          \
	TJS_BEGIN_NATIVE_PROP_DECL(name)                                           \
	{                                                                          \
		TJS_BEGIN_NATIVE_PROP_GETTER                                           \
		{                                                                      \
			TJS_GET_NATIVE_INSTANCE(_this, OverlayInstance);                    \
			*result = (tjs_int)_this->field;                                    \
			return TJS_S_OK;                                                    \
		}                                                                      \
		TJS_END_NATIVE_PROP_GETTER                                             \
		TJS_BEGIN_NATIVE_PROP_SETTER                                           \
		{                                                                      \
			TJS_GET_NATIVE_INSTANCE(_this, OverlayInstance);                    \
			_this->field = (tjs_int)*param;                                     \
			return TJS_S_OK;                                                    \
		}                                                                      \
		TJS_END_NATIVE_PROP_SETTER                                             \
	}                                                                          \
	TJS_END_NATIVE_PROP_DECL(name)

	OVERLAY_INT_PROP(mode, Mode)
	OVERLAY_INT_PROP(frame, Frame)
	OVERLAY_INT_PROP(left, Left)
	OVERLAY_INT_PROP(top, Top)
	OVERLAY_INT_PROP(width, Width)
	OVERLAY_INT_PROP(height, Height)
	OVERLAY_INT_PROP(audioBalance, AudioBalance)
	OVERLAY_INT_PROP(audioVolume, AudioVolume)
	OVERLAY_INT_PROP(enabledAudioStream, EnabledAudioStream)
#undef OVERLAY_INT_PROP

	TJS_BEGIN_NATIVE_PROP_DECL(visible)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, OverlayInstance);
			*result = (tjs_int)(_this->Visible ? 1 : 0);
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_BEGIN_NATIVE_PROP_SETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, OverlayInstance);
			_this->Visible = (tjs_int)*param != 0;
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(visible)

	TJS_BEGIN_NATIVE_PROP_DECL(loop)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, OverlayInstance);
			*result = (tjs_int)(_this->Looping ? 1 : 0);
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_BEGIN_NATIVE_PROP_SETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, OverlayInstance);
			_this->Looping = (tjs_int)*param != 0;
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(loop)

	TJS_BEGIN_NATIVE_PROP_DECL(numberOfAudioStream)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, OverlayInstance);
			*result = (tjs_int)0; // nothing to decode here yet
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_DENY_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(numberOfAudioStream)

	TJS_BEGIN_NATIVE_PROP_DECL(position)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, OverlayInstance);
			*result = (tjs_real)_this->Position;
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_DENY_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(position)

	TJS_BEGIN_NATIVE_PROP_DECL(totalTime)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, OverlayInstance);
			*result = (tjs_real)_this->TotalTime;
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_DENY_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(totalTime)

	TJS_END_NATIVE_MEMBERS
}

iTJSNativeInstance* tTJSNC_VideoOverlay_Darwin::CreateNativeInstance()
{
	return new OverlayInstance();
}

tTJSNativeClass* TVPCreateNativeClass_VideoOverlay()
{
	return new tTJSNC_VideoOverlay_Darwin();
}
