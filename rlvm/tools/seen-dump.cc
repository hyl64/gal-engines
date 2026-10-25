// seen-dump: print a SEEN file the way reallive sees it.
//
// The Rust rewrite in rlvm-rust is differential-tested against this: both
// tools must print identical bytes for identical input. Only libreallive is
// linked in -- no SDL, no GUI, no machine -- so the tool runs headless.
//
//   usage: seen-dump [--xor little_busters|...] <file.seen>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <iomanip>
#include <sstream>

#include "libreallive/archive.h"
#include "libreallive/compression.h"
#include "libreallive/scenario.h"

namespace {

const unsigned char* bytes(const std::string& s) {
  return reinterpret_cast<const unsigned char*>(s.data());
}

std::string hex(const unsigned char* data, size_t n) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) {
    out.push_back(digits[data[i] >> 4]);
    out.push_back(digits[data[i] & 0xf]);
  }
  return out;
}

// FNV-1a, 64 bit: small, dependency free, and plenty to catch a divergence.
uint64_t fnv1a(const unsigned char* data, size_t n) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; ++i) {
    h ^= data[i];
    h *= 1099511628211ULL;
  }
  return h;
}

const libreallive::compression::XorKey* key_by_name(const std::string& name) {
  using namespace libreallive::compression;
  if (name == "little_busters") return little_busters_xor_mask;
  if (name == "little_busters_ex") return little_busters_ex_xor_mask;
  if (name == "clannad_full_voice") return clannad_full_voice_xor_mask;
  if (name == "snow_standard_edition") return snow_standard_edition_xor_mask;
  if (name == "kud_wafter") return kud_wafter_xor_mask;
  if (name == "kud_wafter_all_ages") return kud_wafter_all_ages_xor_mask;
  return NULL;
}

}  // namespace

namespace {

uint32_t word_at(const std::string& data, size_t at) {
  return static_cast<uint8_t>(data[at]) |
         (static_cast<uint8_t>(data[at + 1]) << 8) |
         (static_cast<uint8_t>(data[at + 2]) << 16) |
         (static_cast<uint8_t>(data[at + 3]) << 24);
}

// Decompressed bytecode length, read the way Scenario reads it.
size_t DecompressedLength(const std::string& data,
                          const libreallive::compression::XorKey* key) {
  // A second-XOR game without its key cannot be read at all; the Scenario
  // constructor above has already thrown in that case.
  (void)key;
  return word_at(data, 0x24);
}

// Number of entries in the kidoku (seen-text) table.
size_t KidokuCount(const std::string& data) { return word_at(data, 0x0c); }

}  // namespace

int main(int argc, char** argv) {
  std::string xor_name;
  const char* path = NULL;
  bool from_table_of_contents = false;
  bool dump_all = false;
  std::string regname;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--xor") == 0 && i + 1 < argc) {
      xor_name = argv[++i];
    } else if (std::strcmp(argv[i], "--toc") == 0) {
      from_table_of_contents = true;
    } else if (std::strcmp(argv[i], "--all") == 0) {
      dump_all = true;
    } else if (std::strcmp(argv[i], "--regname") == 0 && i + 1 < argc) {
      regname = argv[++i];
    } else {
      path = argv[i];
    }
  }
  if (!path) {
    std::cerr << "usage: seen-dump [--xor <key>] [--toc] [--all --regname <name>] <file.seen>\n";
    return 2;
  }

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::cerr << "seen-dump: cannot open " << path << "\n";
    return 2;
  }
  std::string data((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());

  if (from_table_of_contents) {
    // Fixtures are one-scenario archives: a 10000-entry table of contents
    // sits at the front. Entry 0 is a placeholder -- 0,0 -- and the first
    // entry carrying a real offset names where the SEEN begins.
    if (data.size() < 8) {
      std::cerr << "seen-dump: " << path << " is too short to hold a TOC\n";
      return 2;
    }
    const auto word = [&](size_t at) -> uint32_t {
      return static_cast<uint8_t>(data[at]) |
             (static_cast<uint8_t>(data[at + 1]) << 8) |
             (static_cast<uint8_t>(data[at + 2]) << 16) |
             (static_cast<uint8_t>(data[at + 3]) << 24);
    };
    bool found = false;
    for (size_t i = 0; i < 10000 && (i + 1) * 8 <= data.size(); ++i) {
      const uint32_t offs = word(i * 8), len = word(i * 8 + 4);
      if (offs == 0) {
        continue;
      }
      if (static_cast<size_t>(offs) + len > data.size()) {
        std::cerr << "seen-dump: TOC entry " << i << " runs past the end of "
                  << path << "\n";
        return 2;
      }
      std::string slice(data, offs, len);
      data.swap(slice);
      found = true;
      break;
    }
    if (!found) {
      std::cerr << "seen-dump: " << path << " holds no scenarios\n";
      return 2;
    }
  }

  const libreallive::compression::XorKey* key = NULL;
  if (const char* dump = std::getenv("RLVM_DUMP_DECOMPRESSED")) {
    // A development aid, never part of the canonical diff: expose the exact
    // stream the C++ parser saw when tracking down a framing mismatch.
    const uint32_t offs = word_at(data, 0x20);
    const uint32_t raw_len = word_at(data, 0x28);
    const uint32_t out_len = word_at(data, 0x24);
    std::vector<char> out(out_len);
    const libreallive::compression::XorKey* dump_key = NULL;
    if (!xor_name.empty()) dump_key = key_by_name(xor_name);
    libreallive::compression::Decompress(data.data() + offs, raw_len,
                                         out.data(), out_len, dump_key);
    std::ofstream stream(dump, std::ios::binary);
    stream.write(out.data(), out.size());
  }
  if (!xor_name.empty()) {
    key = key_by_name(xor_name);
    if (!key) {
      std::cerr << "seen-dump: unknown --xor key '" << xor_name << "'\n";
      return 2;
    }
  }

  try {
    if (dump_all) {
      // Whole-game mode: walk the table of contents, parse every scenario
      // through the real Archive (which owns the second-XOR key by REGNAME),
      // and print one canonical summary line per scenario. The hash covers
      // the instruction stream exactly as the single-file mode prints it.
      libreallive::Archive arc(path, regname);
      for (int i = 0; i < 10000; ++i) {
        // One unparseable scenario must not hide the other 4000: report it
        // and keep walking, exactly as a player would want.
        libreallive::Scenario* scenario = NULL;
        try {
          scenario = arc.GetScenario(i);
        } catch (const std::exception& e) {
          std::cout << "s " << i << " error " << e.what() << "\n";
          continue;
        }
        if (!scenario)
          continue;
        uint64_t hash = 1469598103934665603ULL;
        size_t elements = 0;
        size_t blen = 0;
        std::string line;
        for (libreallive::Scenario::const_iterator it = scenario->begin();
             it != scenario->end(); ++it) {
          const size_t len = (*it)->GetBytecodeLength();
          line = "i " + std::to_string(blen) + " " + std::to_string(len);
          const libreallive::CommandElement* command =
              dynamic_cast<const libreallive::CommandElement*>(it->get());
          if (command) {
            line += " " + std::to_string(int(command->modtype())) + " " +
                    std::to_string(int(command->module())) + " " +
                    std::to_string(command->opcode()) + " " +
                    std::to_string(command->overload());
          }
          const int entrypoint = (*it)->GetEntrypoint();
          if (entrypoint != libreallive::BytecodeElement::kInvalidEntrypoint)
            line += " entrypoint " + std::to_string(entrypoint);
          line += "\n";
          for (char c : line) {
            hash ^= static_cast<unsigned char>(c);
            hash *= 1099511628211ULL;
          }
          ++elements;
          blen += len ? len : 1;
        }
        std::ostringstream hashout;
        hashout << std::hex << std::setw(16) << std::setfill('0') << hash;
        std::cout << "s " << i << " enc " << scenario->encoding()
                  << " blen " << blen << " ins " << elements
                  << " hash " << hashout.str() << "\n";
      }
      return 0;
    }

    libreallive::Scenario scenario(data.c_str(), data.size(), 0, path, key);

    // Both halves of the differential test print these lines: the header's
    // view of the file, plus the decompressed bytecode length, which is the
    // whole of what a scenario reader owes its parser.
    std::cout << "encoding " << scenario.encoding() << "\n";
    std::cout << "savepoints " << scenario.savepoint_message() << " "
              << scenario.savepoint_selcom() << " "
              << scenario.savepoint_seentop() << "\n";
    std::cout << "bytecode_len " << DecompressedLength(data, key) << "\n";
    std::cout << "kidoku " << KidokuCount(data) << "\n";

    // A canonical instruction stream: the next stage of the rewrite must
    // agree not merely on file headers but on every instruction boundary.
    size_t offset = 0;
    for (libreallive::Scenario::const_iterator it = scenario.begin();
         it != scenario.end(); ++it) {
      const size_t len = (*it)->GetBytecodeLength();
      std::cout << "i " << offset << " " << len;
      const libreallive::CommandElement* command =
          dynamic_cast<const libreallive::CommandElement*>(it->get());
      if (command) {
        std::cout << " " << int(command->modtype()) << " "
                  << int(command->module()) << " " << command->opcode()
                  << " " << command->overload();
      }
      const int entrypoint = (*it)->GetEntrypoint();
      if (entrypoint != libreallive::BytecodeElement::kInvalidEntrypoint)
        std::cout << " entrypoint " << entrypoint;
      std::cout << "\n";
      offset += len ? len : 1;
    }
  } catch (const std::exception& e) {
    std::cout << "error " << e.what() << "\n";
    return 1;
  }
  return 0;
}
