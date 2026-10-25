//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Text read/write stream
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include <vector>

#include <zlib.h>
#include "TextStream.h"
#include "MsgIntf.h"
#include "DebugIntf.h"
#include "EventIntf.h"
#include "UtilStreams.h"
#include "tjsError.h"


/*
	Text stream is used by TJS's Array.saveStruct, Dictionary.saveStruct etc.
	to input/output binary files.
*/

//---------------------------------------------------------------------------
tTJSBinaryStream * TVPCreateBinaryStreamForRead(const ttstr & name,
	const ttstr & modestr)
{
	// check o mode
	tTJSBinaryStream* stream = TVPCreateStream(name, TJS_BS_READ);

	const tjs_char* o_ofs = TJS_strchr(modestr.c_str(), TJS_W('o'));
	if(o_ofs != NULL) {
		// seek to offset
		o_ofs++;
		tjs_char buf[256];
		int i;
		for(i = 0; i < 255; i++) {
			if(o_ofs[i] >= TJS_W('0') && o_ofs[i] <= TJS_W('9'))
				buf[i] = o_ofs[i];
			else break;
		}
		buf[i] = 0;
		tjs_uint64 ofs = ttstr(buf).AsInteger();
		stream->SetPosition(ofs);
	}
	return stream;
}
//---------------------------------------------------------------------------
tTJSBinaryStream * TVPCreateBinaryStreamForWrite(const ttstr & name,
	const ttstr & modestr)
{
	tTJSBinaryStream* stream;
	// check o mode
	const tjs_char * o_ofs;
	o_ofs = TJS_strchr(modestr.c_str(), TJS_W('o'));
	if(o_ofs != NULL) {
		// seek to offset
		o_ofs++;
		tjs_char buf[256];
		int i;
		for(i = 0; i < 255; i++) {
			if(o_ofs[i] >= TJS_W('0') && o_ofs[i] <= TJS_W('9'))
				buf[i] = o_ofs[i];
			else break;
		}
		buf[i] = 0;
		tjs_uint64 ofs = ttstr(buf).AsInteger();
		// An offset write ("o<ofs>", how KAG stores a bookmark behind its
		// thumbnail) is an in-place update: keep what is already there, then
		// continue at the offset.  Opening the storage in update mode is not
		// reliable here - the write then failed with "Write error" and every
		// KAG save died on it - so the existing bytes are carried over through
		// a plain write stream instead.
		std::vector<tjs_uint8> existing;
		if(TVPIsExistentStorage(name))
		{
			tTJSBinaryStream *in = TVPCreateStream(name, TJS_BS_READ);
			if(in)
			{
				const tjs_uint64 size = in->GetSize();
				existing.resize((size_t)size);
				if(size) in->Read(&existing[0], (tjs_uint)size);
				delete in;
			}
		}
		stream = TVPCreateStream(name, TJS_BS_WRITE);
		if(!existing.empty())
			stream->Write(&existing[0], (tjs_uint)existing.size());
		stream->SetPosition(ofs);
	} else {
		stream = TVPCreateStream(name, TJS_BS_WRITE);
	}
	return stream;
}
//---------------------------------------------------------------------------





