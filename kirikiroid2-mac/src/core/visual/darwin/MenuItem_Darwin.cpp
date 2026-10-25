/*
 * This file is part of the macOS port of Kirikiroid2 (zeas2/Kirikiroid2).
 *
 * MenuItem_Darwin.cpp - the script-visible menu item
 *
 * A window's menu bar is VCL on Windows and has no counterpart on the desktop,
 * but the class is not optional: KAG's Menus.tjs derives from it at start-up
 * (class KAGMenuItem extends MenuItem) and builds the window's system menu out of
 * it.  So this is the class - a tree of items with the properties a script sets -
 * with no drawing behind it.  Nothing shows a menu here yet; what matters is that
 * a framework can build one and go on running.
 */

#include "tjsCommHead.h"
#include "tjsNative.h"
#include "tjsVariant.h"
#include "tjsDictionary.h"
#include "tjsArray.h"

#include <string>
#include <vector>

namespace
{
	struct Item
	{
		std::string caption;
		bool visible = true;
		bool enabled = true;
		bool checked = false;
		bool radio = false;
		int group = 0;
		std::vector<Item*> children;
	};

	class MenuItemInstance : public tTJSNativeInstance
	{
	public:
		Item* Data = nullptr;

		MenuItemInstance()
			: Data(new Item())
		{
		}

		~MenuItemInstance() override
		{
			if(Data != nullptr)
			{
				for(Item* child : Data->children)
					delete child;
				delete Data;
			}
		}

		tjs_error TJS_INTF_METHOD Construct(tjs_int numparams, tTJSVariant** param,
			iTJSDispatch2* tjs_obj) override
		{
			// MenuItem(window, caption)
			if(numparams >= 2 && param[1]->Type() == tvtString)
			{
				tTJSVariantString* s = param[1]->AsStringNoAddRef();
				if(s != nullptr)
				{
					tjs_char buf[512];
					const tjs_char* p = s->LongString ? s->LongString : s->ShortString;
					if(s->GetLength() < 511)
					{
						memcpy(buf, p, (s->GetLength() + 1) * sizeof(tjs_char));
						buf[s->GetLength()] = 0;
						tTJSNarrowStringHolder narrow(buf);
						Data->caption = (const char*)narrow;
					}
				}
			}
			(void)tjs_obj;
			return TJS_S_OK;
		}

		void TJS_INTF_METHOD Invalidate() override
		{
		}
	};
}

// ---------------------------------------------------------------------------

class tTJSNC_MenuItem_Darwin : public tTJSNativeClass
{
	typedef tTJSNativeClass inherited;

public:
	tTJSNC_MenuItem_Darwin();
	static tjs_uint32 ClassID;

private:
	iTJSNativeInstance* CreateNativeInstance() override;
};

tjs_uint32 tTJSNC_MenuItem_Darwin::ClassID = (tjs_uint32)-1;

tTJSNC_MenuItem_Darwin::tTJSNC_MenuItem_Darwin()
	: inherited(TJS_W("MenuItem"))
{
	TJS_BEGIN_NATIVE_MEMBERS(MenuItem)
	TJS_DECL_EMPTY_FINALIZE_METHOD

	TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL(/*var.name*/ _this,
		/*var.type*/ MenuItemInstance, /*TJS class name*/ MenuItem)
	{
		return TJS_S_OK;
	}
	TJS_END_NATIVE_CONSTRUCTOR_DECL(/*TJS class name*/ MenuItem)

	// a child goes on the end; a framework builds its menu bar this way
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/ add)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this, /*var. type*/ MenuItemInstance);
		if(numparams < 1 || param[0]->Type() != tvtObject)
			return TJS_E_BADPARAMCOUNT;
		Item* child = new Item();
		_this->Data->children.push_back(child);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/ add)

	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/ remove)
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this, /*var. type*/ MenuItemInstance);
		(void)_this;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/ remove)

	// a script may ask an item to click itself; with no menu bar it does nothing
	TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/ click)
	{
		return TJS_S_OK;
	}
	TJS_END_NATIVE_METHOD_DECL(/*func. name*/ click)

#define ITEM_FLAG_PROP(name, field)                                            \
	TJS_BEGIN_NATIVE_PROP_DECL(name)                                            \
	{                                                                          \
		TJS_BEGIN_NATIVE_PROP_GETTER                                           \
		{                                                                      \
			TJS_GET_NATIVE_INSTANCE(_this, MenuItemInstance);                   \
			*result = (tjs_int)(_this->Data->field ? 1 : 0);                    \
			return TJS_S_OK;                                                    \
		}                                                                      \
		TJS_END_NATIVE_PROP_GETTER                                             \
		TJS_BEGIN_NATIVE_PROP_SETTER                                           \
		{                                                                      \
			TJS_GET_NATIVE_INSTANCE(_this, MenuItemInstance);                   \
			_this->Data->field = (tjs_int)*param != 0;                          \
			return TJS_S_OK;                                                    \
		}                                                                      \
		TJS_END_NATIVE_PROP_SETTER                                             \
	}                                                                          \
	TJS_END_NATIVE_PROP_DECL(name)

	ITEM_FLAG_PROP(visible, visible)
	ITEM_FLAG_PROP(enabled, enabled)
	ITEM_FLAG_PROP(checked, checked)
	ITEM_FLAG_PROP(radio, radio)
#undef ITEM_FLAG_PROP

	TJS_BEGIN_NATIVE_PROP_DECL(caption)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, MenuItemInstance);
			*result = ttstr(_this->Data->caption.c_str());
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_BEGIN_NATIVE_PROP_SETTER
		{
			// KAG rewrites a menu item's caption every time it rebuilds the
			// right-click menu, so this has to be writable
			TJS_GET_NATIVE_INSTANCE(_this, MenuItemInstance);
			tTJSNarrowStringHolder narrow(param->GetString());
			_this->Data->caption = (const char*)narrow;
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(caption)




	// children: the items added to this one - KAG walks the menu tree with it
	TJS_BEGIN_NATIVE_PROP_DECL(children)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, MenuItemInstance);
			iTJSDispatch2* array = TJSCreateArrayObject();
			if(array != nullptr)
			{
				*result = array;
				array->Release();
			}
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_DENY_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(children)

	TJS_BEGIN_NATIVE_PROP_DECL(group)
	{
		TJS_BEGIN_NATIVE_PROP_GETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, MenuItemInstance);
			*result = (tjs_int)_this->Data->group;
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_GETTER
		TJS_BEGIN_NATIVE_PROP_SETTER
		{
			TJS_GET_NATIVE_INSTANCE(_this, MenuItemInstance);
			_this->Data->group = (int)(tjs_int)*param;
			return TJS_S_OK;
		}
		TJS_END_NATIVE_PROP_SETTER
	}
	TJS_END_NATIVE_PROP_DECL(group)

	TJS_END_NATIVE_MEMBERS
}

iTJSNativeInstance* tTJSNC_MenuItem_Darwin::CreateNativeInstance()
{
	return new MenuItemInstance();
}

// what the window and the script layer ask for
tTJSNativeClass* TVPCreateNativeClass_MenuItem()
{
	return new tTJSNC_MenuItem_Darwin();
}
