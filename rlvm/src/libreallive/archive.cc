// -*- Mode: C++; tab-width:2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
// vi:tw=80:et:ts=2:sts=2
//
// -----------------------------------------------------------------------
//
// This file is part of libreallive, a dependency of RLVM.
//
// -----------------------------------------------------------------------
//
// Copyright (c) 2006, 2007 Peter Jolly
//
// Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use, copy,
// modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
// BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
// ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// -----------------------------------------------------------------------

#include "libreallive/archive.h"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <cstring>
#include <string>

#include "encodings/codepage.h"
#include "libreallive/compression.h"

using boost::istarts_with;
using boost::iends_with;
namespace fs = boost::filesystem;

namespace libreallive {

Archive::Archive(const std::string& filename)
    : name_(filename),
      info_(filename, Read),
      second_level_xor_key_(NULL),
      text_encoding_(-1) {
  ReadTOC();
  ReadOverrides();
}

Archive::Archive(const std::string& filename, const std::string& regname)
    : name_(filename),
      info_(filename, Read),
      second_level_xor_key_(NULL),
      regname_(regname),
      text_encoding_(-1) {
  ReadTOC();
  ReadOverrides();

  if (regname == "KEY\\CLANNAD_FV") {
    second_level_xor_key_ =
        libreallive::compression::clannad_full_voice_xor_mask;
  } else if (regname ==
             "\x4b\x45\x59\x5c\x83\x8a\x83\x67\x83\x8b\x83"
             "\x6f\x83\x58\x83\x5e\x81\x5b\x83\x59\x81\x49") {
    second_level_xor_key_ = libreallive::compression::little_busters_xor_mask;
  } else if (regname ==
             "\x4b\x45\x59\x5c\x83\x8a\x83\x67\x83\x8b\x83\x6f\x83\x58\x83\x5e"
             "\x81\x5b\x83\x59\x81\x49\x82\x64\x82\x77") {
    // "KEY\<little busters in katakana>!EX", with all fullwidth latin
    // characters.
    second_level_xor_key_ =
        libreallive::compression::little_busters_ex_xor_mask;
  } else if (regname == "StudioMebius\\SNOWSE") {
    second_level_xor_key_ =
        libreallive::compression::snow_standard_edition_xor_mask;
  } else if (regname ==
             "\x4b\x45\x59\x5c\x83\x4e\x83\x68\x82\xed\x82\xd3\x82"
             "\xbd\x81\x5b") {
    // "KEY\<Kud Wafter in hiragana>"
    second_level_xor_key_ =
        libreallive::compression::kud_wafter_xor_mask;
  } else if (regname ==
             "\x4b\x45\x59\x5c\x83\x4e\x83\x68\x82\xed\x82\xd3\x82"
             "\xbd\x81\x5b\x81\x79\x91\x53\x94\x4e\x97\xee\x91\xce"
             "\x8f\xdb\x94\xc5\x81\x7a") {
    second_level_xor_key_ =
        libreallive::compression::kud_wafter_all_ages_xor_mask;
  }
}

Archive::~Archive() {}

int Archive::text_encoding() {
  if (text_encoding_ != -1)
    return text_encoding_;

  // One scenario is not enough to answer: a file can happen to contain no
  // byte that the narrow reader mistakes for a delimiter. Sample a handful
  // spread across the archive and let the reader that frames more of them
  // win.
  std::vector<const scenarios_t::value_type*> sample;
  const size_t available = scenarios_.size();
  const size_t wanted = available < 8 ? available : 8;
  for (size_t k = 0; k < wanted; ++k) {
    scenarios_t::const_iterator it = scenarios_.begin();
    std::advance(it, k * available / wanted);
    sample.push_back(&*it);
  }

  // Ask the narrow reader first. Most games are Shift-JIS and it will
  // frame every sample, in which case there is nothing left to ask.
  size_t readable[2] = {0, 0};
  Cp::instance(0);
  for (const scenarios_t::value_type* entry : sample) {
    try {
      Scenario probe(entry->second, entry->first, regname_,
                     second_level_xor_key_, 0);
      ++readable[0];
    } catch (const Error&) {
      // Counted by omission.
    }
  }

  if (readable[0] < sample.size()) {
    Cp::instance(1);
    for (const scenarios_t::value_type* entry : sample) {
      try {
        Scenario probe(entry->second, entry->first, regname_,
                       second_level_xor_key_, 1);
        ++readable[1];
      } catch (const Error&) {
        // Counted by omission.
      }
    }
  } else {
    readable[1] = 0;
  }

  text_encoding_ = readable[1] > readable[0] ? 1 : 0;
  Cp::instance(text_encoding_);
  return text_encoding_;
}

Scenario* Archive::GetScenario(int index) {
  accessed_t::const_iterator at = accessed_.find(index);
  if (at != accessed_.end())
    return at->second.get();
  scenarios_t::const_iterator st = scenarios_.find(index);
  if (st != scenarios_.end()) {
    // The bytecode reader asks the codepage in force how wide a character
    // is, so it must agree with the archive before parsing starts.
    int encoding = text_encoding();
    Cp::instance(encoding);
    Scenario* scene = new Scenario(st->second, index, regname_,
                                   second_level_xor_key_, encoding);
    accessed_[index].reset(scene);
    return scene;
  }
  return NULL;
}

int Archive::GetProbableEncodingType() const {
  // Directly create Header objects instead of Scenarios. We don't want to
  // parse the entire SEEN file here.
  for (auto it = scenarios_.cbegin(); it != scenarios_.cend(); ++it) {
    Header header(it->second.data, it->second.length);
    if (header.rldev_metadata_.text_encoding() != 0)
      return header.rldev_metadata_.text_encoding();
  }

  return 0;
}

void Archive::ReadTOC() {
  // The table is nominally 10000 entries of eight bytes, but a file is free
  // to be shorter than that -- a hand-built game, or a Seen.txt holding a
  // single scenario. Reading the whole nominal table walked straight off the
  // end of the mapping and took the process with it.
  const char* idx = info_.get();
  const size_t available = info_.size() / 8;
  const int entries = static_cast<int>(available < 10000 ? available : 10000);
  for (int i = 0; i < entries; ++i, idx += 8) {
    const int offs = read_i32(idx);
    if (!offs)
      continue;
    const int length = read_i32(idx + 4);
    if (offs < 0 || length < 0 ||
        static_cast<size_t>(offs) + length > info_.size())
      continue;  // Entry points outside the file; ignore rather than crash.
    scenarios_[i] = FilePos(info_.get() + offs, length);
  }
}

void Archive::ReadOverrides() {
  // Iterate over all files in the directory and override the table of contents
  // if there is a free SEENXXXX.TXT file.
  fs::path seen_dir = fs::path(name_).parent_path();
  fs::directory_iterator end;
  for (fs::directory_iterator it(seen_dir); it != end; ++it) {
    std::string filename = it->path().filename().string();
    if (filename.size() == 12 && istarts_with(filename, "seen") &&
        iends_with(filename, ".txt") && isdigit(filename[4]) &&
        isdigit(filename[5]) && isdigit(filename[6]) && isdigit(filename[7])) {
      Mapping* mapping = new Mapping((seen_dir / filename).string(), Read);
      maps_to_delete_.emplace_back(mapping);

      int index = std::stoi(filename.substr(4, 4));
      scenarios_[index] = FilePos(mapping->get(), mapping->size());
    }
  }
}

}  // namespace libreallive
