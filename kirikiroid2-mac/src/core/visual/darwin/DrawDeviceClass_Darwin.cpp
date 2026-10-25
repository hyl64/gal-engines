/*
 * This file is part of the macOS port of Kirikiroid2 (zeas2/Kirikiroid2).
 *
 * DrawDeviceClass_Darwin.cpp - the script-visible draw device
 *
 * A script's point of entry is `new Window()`, and tTJSNI_BaseWindow::Construct
 * gives that window a draw device object.  Upstream that object is
 * tTJSNC_BasicDrawDevice, which wraps a Direct3D device - the desktop has no
 * Direct3D, so this is the same wrapper around the SDL device in
 * DrawDevice_Darwin.cpp, registered under the name the scripts and the window
 * ask for.
 *
 * The window asks for it through TVPCreateDrawDeviceClass(), which
 * visual/WindowIntf.cpp calls (one line changed there, marked in the source).
 */

#include "tjsCommHead.h"
#include "tjsNative.h"
#include "tjsVariant.h"
#include "DrawDevice.h"

// the SDL device (DrawDevice_Darwin.cpp)
class tTVPDrawDevice_Darwin;
tTVPDrawDevice* TVPCreateDrawDeviceDarwin();

// ---------------------------------------------------------------------------
// the native instance: it owns the device for as long as the script object lives
// ---------------------------------------------------------------------------

class tTJSNI_DarwinDrawDevice : public tTJSNativeInstance
{
	typedef tTJSNativeInstance inherited;

	tTVPDrawDevice* Device;

public:
	tTJSNI_DarwinDrawDevice();
	~tTJSNI_DarwinDrawDevice();

	tjs_error TJS_INTF_METHOD Construct(tjs_int numparams, tTJSVariant** param, iTJSDispatch2* tjs_obj);
	void TJS_INTF_METHOD Invalidate();

	// the device comes up on first use: the window takes it from the interface
	// property right after constructing the class, and a host may create the
	// object without running the constructor body
	tTVPDrawDevice* GetDevice()
	{
		if(Device == nullptr)
			Device = TVPCreateDrawDeviceDarwin();
		return Device;
	}
};

tTJSNI_DarwinDrawDevice::tTJSNI_DarwinDrawDevice()
	: Device(nullptr)
{
}

tTJSNI_DarwinDrawDevice::~tTJSNI_DarwinDrawDevice()
{
	if(Device != nullptr)
		Device->Destruct();
}

tjs_error TJS_INTF_METHOD tTJSNI_DarwinDrawDevice::Construct(tjs_int numparams,
	tTJSVariant** param, iTJSDispatch2* tjs_obj)
{
	(void)numparams;
	(void)param;
	(void)tjs_obj;
	if(Device == nullptr)
		Device = TVPCreateDrawDeviceDarwin();
	return TJS_S_OK;
}

void TJS_INTF_METHOD tTJSNI_DarwinDrawDevice::Invalidate()
{
	if(Device != nullptr)
	{
		Device->Destruct();
		Device = nullptr;
	}
}

// ---------------------------------------------------------------------------
// the class the window constructs
// ---------------------------------------------------------------------------

class tTJSNC_DrawDeviceDarwin : public tTJSNativeClass
{
	typedef tTJSNativeClass inherited;

public:
	tTJSNC_DrawDeviceDarwin();
	static tjs_uint32 ClassID;

private:
	iTJSNativeInstance* CreateNativeInstance() override;
};

tjs_uint32 tTJSNC_DrawDeviceDarwin::ClassID = (tjs_uint32)-1;

tTJSNC_DrawDeviceDarwin::tTJSNC_DrawDeviceDarwin()
	: inherited(TJS_W("BasicDrawDevice"))
{
	// the name is the one the engine's own scripts and the window look for
	TJS_BEGIN_NATIVE_MEMBERS(DrawDeviceDarwin)
	TJS_DECL_EMPTY_FINALIZE_METHOD

	TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL(/*var.name*/ _this,
		/*var.type*/ tTJSNI_DarwinDrawDevice,
		/*TJS class name*/ DrawDeviceDarwin)
	{
		return TJS_S_OK;
	}
	TJS_END_NATIVE_CONSTRUCTOR_DECL(/*TJS class name*/ DrawDeviceDarwin)

	// a software device never has to be recreated, but the script may ask
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/ recreate)
	{
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/ recreate)

	TJS_BEGIN_NATIVE_PROP_DECL(/*{*/ interface /*}*/)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
				/*var. type*/ tTJSNI_DarwinDrawDevice);
			*result = reinterpret_cast<tjs_int64>(_this->GetDevice());
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER

		TJS_DENY_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(/*{*/ interface /*}*/)

	TJS_END_NATIVE_MEMBERS
}

iTJSNativeInstance* tTJSNC_DrawDeviceDarwin::CreateNativeInstance()
{
	return new tTJSNI_DarwinDrawDevice();
}

// ---------------------------------------------------------------------------
// the hook the window uses
// ---------------------------------------------------------------------------

// visual/WindowIntf.cpp asks for the class of the device to give a new window;
// upstream that is tTJSNC_BasicDrawDevice (Direct3D), here it is this one.
tTJSNativeClass* TVPCreateDrawDeviceClass()
{
	return new tTJSNC_DrawDeviceDarwin();
}
