//---------------------------------------------------------------------------
/*
	OverlayArchive.cpp - see OverlayArchive.h
*/
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "StorageIntf.h"
#include "OverlayArchive.h"

#include <algorithm>
#include <map>
#include <string>

//---------------------------------------------------------------------------
tTVPOverlayArchive::tTVPOverlayArchive(const ttstr &name,
	const std::vector<tTVPArchive*> &archives)
	: tTVPArchive(name)
{
	// later archives override earlier ones, so walk them in order and let
	// each later entry replace the name it shares with an earlier one
	std::map<ttstr, tEntry> merged;
	for(tTVPArchive *arc : archives)
	{
		if(arc == nullptr) continue;
		arc->AddRef();
		Sources.push_back(arc);
		const tjs_uint count = arc->GetCount();
		for(tjs_uint i = 0; i < count; i++)
		{
			tEntry e;
			e.Name = arc->GetName(i);
			e.Archive = arc;
			e.Index = i;
			merged[e.Name] = e; // a later archive wins
		}
	}
	// the archive contract wants names sorted
	Entries.reserve(merged.size());
	for(auto &kv : merged)
		Entries.push_back(kv.second);
	std::sort(Entries.begin(), Entries.end(),
		[](const tEntry &a, const tEntry &b) { return a.Name < b.Name; });
}
//---------------------------------------------------------------------------
tTVPOverlayArchive::~tTVPOverlayArchive()
{
	for(tTVPArchive *arc : Sources)
		arc->Release();
}
//---------------------------------------------------------------------------
tTJSBinaryStream *tTVPOverlayArchive::CreateStreamByIndex(tjs_uint idx)
{
	if(idx >= Entries.size()) return nullptr;
	return Entries[idx].Archive->CreateStreamByIndex(Entries[idx].Index);
}
//---------------------------------------------------------------------------
bool TVPCreateOverlayArchive(const ttstr &name,
	const std::vector<ttstr> &archives, tTVPArchive **out)
{
	std::vector<tTVPArchive*> opened;
	for(const ttstr &a : archives)
	{
		if(a.IsEmpty()) continue;
		tTVPArchive *arc = nullptr;
		try
		{
			arc = TVPOpenArchive(a, true);
		}
		catch(...)
		{
			for(tTVPArchive *o : opened) o->Release();
			throw;
		}
		opened.push_back(arc);
	}
	if(opened.empty())
	{
		*out = nullptr;
		return false;
	}
	*out = new tTVPOverlayArchive(name, opened);
	for(tTVPArchive *o : opened) o->Release(); // the overlay holds its own refs
	return true;
}
//---------------------------------------------------------------------------
