// sniff-equivalence - R2's evidence: a bounded sample must give the same answer
//
// R2 replaces `Content(a, e)` + `resize(512)` in pfstool with a new
// `artemis::Sample(a, e, 512)`.  A speed-up that changes what the tool prints is
// worthless, so this walks every member of both real archives and compares:
//
//   1. the bytes     - Sample() 512 must be the exact prefix of Content()
//   2. the verdict   - SniffBytes() of that prefix must report the same kind
//
// It links against the *patched* pfs.cpp (given on the command line), so the
// comparison is against the engine's real implementation, not a copy.
//
//   sniff-equivalence <archive> [<archive> ...]

#include "pfs.h"
#include "sniff.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		fprintf(stderr, "usage: sniff-equivalence <archive> [<archive> ...]\n");
		return 2;
	}
	size_t members = 0;
	size_t byteMismatch = 0;
	size_t kindMismatch = 0;
	std::vector<std::string> kinds;

	for(int a = 1; a < argc; a++)
	{
		artemis::Archive archive;
		try
		{
			archive = artemis::Open(argv[a]);
		}
		catch(const std::exception& e)
		{
			fprintf(stderr, "sniff-equivalence: %s: %s\n", argv[a], e.what());
			return 1;
		}
		for(const artemis::Entry& e : archive.entries)
		{
			if(e.size == 0)
				continue;
			members++;

			// what the tool does today
			std::vector<uint8_t> whole = artemis::Content(archive, e);
			if(whole.size() > 512)
				whole.resize(512);
			const std::string oldKind =
				artemis::SniffBytes(whole.data(), whole.size()).kind;

			// what R2 does
			const std::vector<uint8_t> sample = artemis::Sample(archive, e, 512);
			const std::string newKind =
				artemis::SniffBytes(sample.data(), sample.size()).kind;

			if(sample.size() != whole.size() || memcmp(sample.data(), whole.data(), sample.size()) != 0)
			{
				if(byteMismatch < 5)
					fprintf(stderr, "bytes differ: %s (%zu vs %zu)\n",
						e.name.c_str(), sample.size(), whole.size());
				byteMismatch++;
			}
			if(oldKind != newKind)
			{
				if(kindMismatch < 5)
					fprintf(stderr, "kind differs: %s: '%s' vs '%s'\n",
						e.name.c_str(), oldKind.c_str(), newKind.c_str());
				kindMismatch++;
			}
			kinds.push_back(newKind);
		}
	}

	// a short census, so the number above is visibly about real content
	const char* names[] = { "png", "jpeg", "ogg", "ttf", "text", "ipt", "asb", "ast" };
	printf("members\t%zu\n", members);
	printf("byte_mismatches\t%zu\n", byteMismatch);
	printf("kind_mismatches\t%zu\n", kindMismatch);
	for(const char* name : names)
	{
		size_t n = 0;
		for(const std::string& k : kinds)
			if(k == name)
				n++;
		if(n > 0)
			printf("kind\t%s\t%zu\n", name, n);
	}
	return (byteMismatch == 0 && kindMismatch == 0) ? 0 : 1;
}
