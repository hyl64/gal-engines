//---------------------------------------------------------------------------
/*
	OverlayArchive.h - one archive made of several, for patch layering.

	A released KiriKiri game is data.xp3 plus optional patch.xp3, patch2.xp3,
	...  The engine's own path resolution reads the archive named by the
	current directory first, so patches mounted merely through the auto path
	never win.  This archive presents the merged view instead: names from a
	later (higher priority) archive replace the same name in an earlier one,
	which is exactly how KAG documents patch priority.
*/
//---------------------------------------------------------------------------
#ifndef OverlayArchiveH
#define OverlayArchiveH

#include "tjsCommHead.h"
#include "StorageIntf.h"

#include <vector>

class tTVPOverlayArchive : public tTVPArchive
{
public:
	// archives are taken in ascending priority: the LAST one wins on a clash
	tTVPOverlayArchive(const ttstr &name,
		const std::vector<tTVPArchive*> &archives);
	~tTVPOverlayArchive() override;

	tjs_uint GetCount() override { return (tjs_uint)Entries.size(); }
	ttstr GetName(tjs_uint idx) override { return Entries[idx].Name; }
	tTJSBinaryStream *CreateStreamByIndex(tjs_uint idx) override;

private:
	struct tEntry
	{
		ttstr Name;
		tTVPArchive *Archive;
		tjs_uint Index;
	};
	std::vector<tEntry> Entries;
	std::vector<tTVPArchive*> Sources; // addref'd
};

// Builds the overlay in `out` and hands its ownership over (AddRef'd, the
// caller is expected to Release it).  Returns false when `archives` is empty.
bool TVPCreateOverlayArchive(const ttstr &name,
	const std::vector<ttstr> &archives, tTVPArchive **out);

#endif
