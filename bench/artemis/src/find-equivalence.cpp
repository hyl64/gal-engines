// find-equivalence - R3's evidence: a precomputed name must find exactly the same entry
//
// R3 stores `NormalizeName(entry.name)` once, when the index is parsed, so that
// Find() and FindSuffix() stop allocating a string per candidate.  A lookup that
// gets faster but answers differently is worse than a slow one, so this prints a
// canonical answer for a deterministic query set:
//
//   <index into archive.entries, or -1 for "not found"> TAB <query>
//
// Build it twice - once against the engine as it is, once against the patched
// `pfs.cpp` - and diff the two outputs.  The query set is built to hit the parts
// where a lookup can quietly change its mind:
//
//   * every member's own name                      (all the hits)
//   * case variants, slash variants                (must still hit)
//   * truncated / extended / extension-swapped     (must still miss)
//   * the empty name, and a name made by gluing
//     two real names together                      (must miss)
//   * duplicates that normalise to the same thing  (first match must not move)
//
// The duplicate case needs a crafted archive: `find-equivalence --dupes` builds
// one in memory with names that differ only in case or slash direction, so the
// "which one wins" rule is pinned down rather than assumed.
//
//   find-equivalence <archive> [<archive> ...]     real archives
//   find-equivalence --dupes                       crafted duplicate archive

#include "pfs.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	struct Query
	{
		std::string text;
		const char* family;
	};

	// ASCII case flip, one of the two things NormalizeName() folds
	std::string Upper(std::string s)
	{
		for(char& c : s)
			if(c >= 'a' && c <= 'z')
				c = (char)(c - 'a' + 'A');
		return s;
	}

	std::string SwapSlashes(std::string s)
	{
		for(char& c : s)
			c = c == '\\' ? '/' : (c == '/' ? '\\' : c);
		return s;
	}

	std::vector<Query> QueriesFor(const artemis::Archive& a)
	{
		std::vector<Query> out;
		out.reserve(a.entries.size() * 6 + 16);
		for(const artemis::Entry& e : a.entries)
			out.push_back({ e.name, "own-name" });

		// variants of a spread of members, not just the first few: names differ in
		// shape (depth, extension, Japanese) and a per-entry normalisation could in
		// principle disagree with a per-query one on any of them
		const size_t stride = a.entries.size() > 240 ? a.entries.size() / 120 : 1;
		for(size_t i = 0; i < a.entries.size(); i += stride)
		{
			const std::string& n = a.entries[i].name;
			out.push_back({ Upper(n), "upper-case" });
			out.push_back({ SwapSlashes(n), "slash-swapped" });
			out.push_back({ n + ".bak", "extended" });
			out.push_back({ "x/" + n, "prefixed" });
			if(n.size() > 1)
				out.push_back({ n.substr(0, n.size() - 1), "truncated" });
		}

		// names that must not exist
		out.push_back({ "", "empty" });
		out.push_back({ "no\\such\\member", "made-up" });
		out.push_back({ "\\", "slash" });
		out.push_back({ "image\\cg\\SYUURAKU.png.bak", "made-up" });
		if(a.entries.size() > 1)
			out.push_back({ a.entries[0].name + a.entries[1].name, "glued" });

		std::sort(out.begin(), out.end(), [](const Query& x, const Query& y) {
			if(x.text != y.text)
				return x.text < y.text;
			return std::strcmp(x.family, y.family) < 0;
		});
		return out;
	}

	long IndexOf(const artemis::Archive& a, const artemis::Entry* e)
	{
		if(e == nullptr)
			return -1;
		return (long)(e - a.entries.data());
	}

	int RunArchive(const std::string& path, const char* label)
	{
		artemis::Archive a;
		try
		{
			a = artemis::Open(path);
		}
		catch(const std::exception& e)
		{
			fprintf(stderr, "find-equivalence: %s: %s\n", path.c_str(), e.what());
			return 1;
		}
		const std::vector<Query> queries = QueriesFor(a);
		printf("# %s: %zu entries, %zu queries\n", label, a.entries.size(), queries.size());
		for(const Query& q : queries)
		{
			const artemis::Entry* hit = artemis::Find(a, q.text);
			// the suffix path matters too: it walks every entry without early exit,
			// so it is the one that always paid the per-candidate allocation
			const std::vector<const artemis::Entry*> suffix = artemis::FindSuffix(a, q.text);
			printf("%ld\t%zu\t%s\t%s\n", IndexOf(a, hit), suffix.size(), q.family, q.text.c_str());
		}
		return 0;
	}

	// an archive whose names collide after normalisation: which one Find() returns
	// is part of the contract, so it is measured, not assumed
	int RunDupes()
	{
		std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
		const std::vector<std::string> names = {
			"a\\b\\dup.png",
			"A\\B\\DUP.PNG",   // same as the first after normalisation
			"a/b/dup.png",     // and again, with forward slashes
			"a\\b\\unique.png",
			"a\\b\\dup.png.bak",
		};
		for(size_t i = 0; i < names.size(); i++)
			files.push_back({ names[i], std::vector<uint8_t>(16, (uint8_t)i) });

		// pf6: no key, so the bytes are stable and the test only looks at names
		const std::vector<uint8_t> blob = artemis::Build(files, "pf6", false,
			artemis::KeyPhase::EntryRelative);
		artemis::Archive a = artemis::Read(blob, artemis::KeyPhase::None);
		printf("# dupes: %zu entries\n", a.entries.size());

		const std::vector<std::string> queries = {
			"a\\b\\dup.png", "A/B/DUP.PNG", "a/b/dup.png", "a\\B\\Dup.Png",
			"a\\b\\unique.png", "a\\b\\dup.png.bak", "a\\b\\dup", "b\\dup.png",
		};
		for(const std::string& q : queries)
		{
			const artemis::Entry* hit = artemis::Find(a, q);
			const std::vector<const artemis::Entry*> suffix = artemis::FindSuffix(a, q);
			printf("%ld\t%zu\tdupes\t%s\n", IndexOf(a, hit), suffix.size(), q.c_str());
		}
		return 0;
	}
}

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		fprintf(stderr, "usage: find-equivalence <archive> [<archive> ...] | --dupes\n");
		return 2;
	}
	int rc = 0;
	if(std::strcmp(argv[1], "--dupes") == 0)
		return RunDupes();
	for(int i = 1; i < argc; i++)
		rc |= RunArchive(argv[i], argv[i]);
	return rc;
}
