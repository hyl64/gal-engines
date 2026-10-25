// darwin_system.cpp - the System object for the desktop host
//
// TJS2 scripts print through System.inform ("show this to the user") and
// System.println / print; the engine itself has no console.  Upstream the
// application layer registers that object (MainScene.cpp on Android,
// Application.cpp on Windows); the desktop host registers this one, which
// writes to the terminal.  The members live inside the constructor's
// TJS_BEGIN_NATIVE_MEMBERS block because that is where the macros both
// define and register them.

#include "darwin_platform.h"

#include "tjsNative.h"
#include "tjsVariant.h"
#include "tjsString.h"

class tTJSNC_System : public tTJSNativeClass
{
	typedef tTJSNativeClass inherited;

public:
	tTJSNC_System();
	~tTJSNC_System();

	static tjs_uint32 ClassID;
};

tjs_uint32 tTJSNC_System::ClassID = (tjs_uint32)-1;

tTJSNC_System::tTJSNC_System() : inherited(TJS_W("System"))
{
	TJS_BEGIN_NATIVE_MEMBERS(/*TJS class name*/ System)
	TJS_DECL_EMPTY_FINALIZE_METHOD

	TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL_NO_INSTANCE(/*TJS class name*/ System)
	{
		return TJS_S_OK;
	}
	TJS_END_NATIVE_CONSTRUCTOR_DECL(/*TJS class name*/ System)

	// System.print(text): the text, as it is
	TJS_BEGIN_NATIVE_METHOD_DECL(/*{*/ print /*}*/)
	{
		if(numparams < 1)
			return TJS_E_BADPARAMCOUNT;
		if(param[0]->Type() != tvtVoid)
			KrkrOut(ttstr(*param[0]).c_str());
		return TJS_S_OK;
	}
	TJS_END_NATIVE_STATIC_METHOD_DECL(/*{*/ print /*}*/)

	// System.println(text): the text and a line break
	TJS_BEGIN_NATIVE_METHOD_DECL(/*{*/ println /*}*/)
	{
		if(numparams < 1)
			return TJS_E_BADPARAMCOUNT;
		if(param[0]->Type() != tvtVoid)
			KrkrOut(ttstr(*param[0]).c_str());
		KrkrOut(TJS_W("\n"));
		return TJS_S_OK;
	}
	TJS_END_NATIVE_STATIC_METHOD_DECL(/*{*/ println /*}*/)

	// System.inform(text): the engine's "tell the user" call; a terminal
	// run shows it on its own line
	TJS_BEGIN_NATIVE_METHOD_DECL(/*{*/ inform /*}*/)
	{
		if(numparams < 1)
			return TJS_E_BADPARAMCOUNT;
		if(param[0]->Type() != tvtVoid)
			KrkrOut(ttstr(*param[0]).c_str());
		KrkrOut(TJS_W("\n"));
		return TJS_S_OK;
	}
	TJS_END_NATIVE_STATIC_METHOD_DECL(/*{*/ inform /*}*/)

	// System.platform: what the host runs on, which a script may branch on
	TJS_BEGIN_NATIVE_PROP_DECL(/*{*/ platform /*}*/)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			*result = TJS_W("macOS");
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER

		TJS_DENY_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_STATIC_PROP_DECL(/*{*/ platform /*}*/)

	TJS_END_NATIVE_MEMBERS
}

tTJSNC_System::~tTJSNC_System()
{
}

iTJSDispatch2* KrkrCreateSystemClass()
{
	return new tTJSNC_System();
}
