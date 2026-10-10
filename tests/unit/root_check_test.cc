// The checks made at open of a copy of a ROOT file (OpenPolicy.h rootHeaderVerdict, rootCheckRead;
// RootFormat.h): every rule, on whole synthetic files laid out as ROOT lays them out (32- and
// 64-bit positions, a header never completed, long names), each read made as the plug-in makes it;
// then hostile bytes: mutations of synthetic files and of the ROOT files under tests/data, which
// must never crash, read out of bounds or ask for a read outside the copy (run the ASan build for
// the memory half of that).
#include "OpenPolicy.h"
#include "RootFormat.h"

#include <gtest/gtest.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace ucache;

namespace {
using K = RootHeaderVerdict::Kind;

// A sparse file image: the bytes written at each position, zeros elsewhere, `size` bytes long.
struct Image {
  std::map<int64_t, std::vector<unsigned char>> parts;
  long long size = 0;
  void put(int64_t at, const std::vector<unsigned char>& b) { parts[at] = b; }
  // What a read of `len` bytes at `at` answers: the bytes up to the copy's end, `cut` bytes fewer
  // when the server answers short.
  std::vector<unsigned char> read(int64_t at, size_t len, size_t cut = 0) const {
    std::vector<unsigned char> out;
    if (at < 0 || at >= size)
      return out;
    const size_t n =
        static_cast<size_t>(std::min<long long>(static_cast<long long>(len), size - at));
    out.assign(n, 0);
    for (const auto& p : parts)
      for (size_t i = 0; i < p.second.size(); ++i) {
        const int64_t pos = p.first + static_cast<int64_t>(i);
        if (pos >= at && pos < at + static_cast<int64_t>(n))
          out[static_cast<size_t>(pos - at)] = p.second[i];
      }
    out.resize(n - std::min(n, cut));
    return out;
  }
};

// Runs the checks over `img` as the plug-in does: the first rootHeaderBytes(size) bytes, then each
// read the verdict asks for until it is final. `reads`: the reads made, "at+len" each; `shortAt`:
// the read (0 the first) that the server answers one byte short.
RootHeaderVerdict check(const Image& img, std::vector<std::string>* reads = nullptr,
                        int shortAt = -1) {
  std::vector<unsigned char> b = img.read(0, rootHeaderBytes(img.size), shortAt == 0 ? 1 : 0);
  if (reads)
    reads->push_back("0+" + std::to_string(rootHeaderBytes(img.size)));
  RootHeaderVerdict v = rootHeaderVerdict(b.data(), b.size(), img.size);
  for (int i = 1; v.next != RootHeaderVerdict::kNone; ++i) {
    EXPECT_LT(i, 4) << "the checks make at most three reads";
    if (i >= 4)
      break;
    EXPECT_GE(v.nextAt, 0);
    EXPECT_LE(v.nextAt + static_cast<long long>(v.nextLen), img.size) << "a read inside the copy";
    if (reads)
      reads->push_back(std::to_string(v.nextAt) + "+" + std::to_string(v.nextLen));
    b = img.read(v.nextAt, v.nextLen, shortAt == i ? 1 : 0);
    rootCheckRead(v, b.data(), b.size());
  }
  return v;
}

void be(std::vector<unsigned char>& b, uint64_t v, int k) {
  for (int i = k - 1; i >= 0; --i)
    b.push_back(static_cast<unsigned char>(v >> (8 * i)));
}
void name(std::vector<unsigned char>& b, const std::string& s) {
  if (s.size() < 255) {
    b.push_back(static_cast<unsigned char>(s.size()));
  } else {
    b.push_back(255);
    be(b, s.size(), 4);
  }
  b.insert(b.end(), s.begin(), s.end());
}
// A record's label as TKey::FillBuffer writes it.
struct Label {
  int32_t nbytes = 0;
  int16_t version = 4; // above 1000: 64-bit positions
  int64_t seekKey = 0, seekPdir = 0;
  std::string cls = "TH1F", nm = "h", title = "h";
};
std::vector<unsigned char> label(const Label& l) {
  std::vector<unsigned char> b;
  be(b, static_cast<uint32_t>(l.nbytes), 4);
  be(b, static_cast<uint16_t>(l.version), 2);
  be(b, 100, 4); // objlen
  be(b, 0x6f2a1c00, 4);
  be(b, 0, 2); // keylen (set below)
  be(b, 1, 2);
  const bool wide = l.version > 1000;
  be(b, static_cast<uint64_t>(l.seekKey), wide ? 8 : 4);
  be(b, static_cast<uint64_t>(l.seekPdir), wide ? 8 : 4);
  name(b, l.cls);
  name(b, l.nm);
  name(b, l.title);
  b[14] = static_cast<unsigned char>(b.size() >> 8);
  b[15] = static_cast<unsigned char>(b.size());
  return b;
}

// A ROOT file as ROOT lays it out: the header at 0, the top directory's name record at `begin` and
// its record right after it, the class descriptions, the objects' records, the list of top-level
// objects and the list of free space at the end. Every field can be set to what a damaged copy
// holds.
struct Synth {
  bool wide = false; // 64-bit positions in the header, the directory's record and the labels
  int64_t base = 0;  // where the records begin (past 2^32 for a 64-bit file)
  int32_t version = 63602;
  int64_t begin = 100;
  int32_t nameLen = 58;           // the header's nbytesName
  int32_t dirNameLen = INT32_MIN; // the directory record's (INT32_MIN: the header's)
  int16_t dirVersion = -1;        // -1: 5, or 1005 when wide
  int objects = 3;
  bool completed = true; // the header places a list of top-level objects
  std::string nameText = "f.root";
  // Overrides (applied after layout) of what the header and the records say.
  std::map<std::string, int64_t> set;
  Image img;
  int64_t end = 0, seekKeys = 0, seekInfo = 0, seekFree = 0, seekDir = 0;
  int32_t nbytesKeys = 0, nbytesInfo = 0, nbytesFree = 0;
  std::vector<Label> listed;

  int64_t get(const std::string& k, int64_t dflt) const {
    const auto it = set.find(k);
    return it == set.end() ? dflt : it->second;
  }
  Image build() {
    img = Image();
    const int16_t dv = dirVersion >= 0 ? dirVersion : static_cast<int16_t>(wide ? 1005 : 5);
    const int16_t kv = wide ? 1004 : 4;
    seekDir = begin;
    // The top directory: its label and name record, then its record.
    std::vector<unsigned char> top = label({nameLen, kv, begin, 0, "TFile", nameText, ""});
    top.resize(static_cast<size_t>(std::max(nameLen, 1)), 0x20);
    img.put(begin, top);
    const int64_t dirAt = begin + nameLen;
    int64_t pos = std::max<int64_t>(base, dirAt + 60);
    // The class descriptions, then the objects.
    seekInfo = pos;
    nbytesInfo = 400;
    img.put(seekInfo, label({nbytesInfo, kv, seekInfo, begin, "TList", "StreamerInfo",
                             "Doubly linked list"}));
    pos += nbytesInfo;
    listed.clear();
    for (int i = 0; i < objects; ++i) {
      Label l{1000 + 37 * i, kv, pos, begin, "TH1F", "h" + std::to_string(i), "a histogram"};
      img.put(pos, label(l));
      listed.push_back(l);
      pos += l.nbytes;
    }
    // The list of top-level objects: its own label, the count, the objects' labels.
    seekKeys = pos;
    for (int pass = 0; pass < 2; ++pass) {
      std::vector<unsigned char> list =
          label({nbytesKeys, kv, seekKeys, begin, "TFile", nameText, ""});
      be(list, static_cast<uint32_t>(get("count", objects)), 4);
      for (Label l : listed) {
        if (set.count("entrySeek") && l.nm == "h1")
          l.seekKey = get("entrySeek", l.seekKey);
        if (set.count("entryNbytes") && l.nm == "h1")
          l.nbytes = static_cast<int32_t>(get("entryNbytes", l.nbytes));
        if (set.count("entryPdir") && l.nm == "h1")
          l.seekPdir = get("entryPdir", l.seekPdir);
        const auto e = label(l);
        list.insert(list.end(), e.begin(), e.end());
      }
      nbytesKeys = static_cast<int32_t>(list.size());
      if (pass == 1) {
        // the list's own label, as a damaged copy may state it
        std::vector<unsigned char> own =
            label({static_cast<int32_t>(get("ownNbytes", nbytesKeys)), kv, get("ownSeek", seekKeys),
                   get("ownPdir", begin), "TFile", nameText, ""});
        std::copy(own.begin(), own.end(), list.begin());
        img.put(seekKeys, list);
      }
    }
    pos += nbytesKeys;
    seekFree = pos;
    nbytesFree = 60;
    img.put(seekFree, label({nbytesFree, kv, seekFree, begin, "TFile", nameText, ""}));
    pos += nbytesFree;
    end = pos;
    // The top directory's record.
    std::vector<unsigned char> dir;
    be(dir, static_cast<uint16_t>(dv), 2);
    be(dir, 0x6f2a1c00, 4);
    be(dir, 0x6f2a1c00, 4);
    be(dir, static_cast<uint32_t>(get("nbytesKeys", nbytesKeys)), 4);
    be(dir, static_cast<uint32_t>(dirNameLen != INT32_MIN ? dirNameLen : nameLen), 4);
    const int dw = dv > 1000 ? 8 : 4;
    be(dir, static_cast<uint64_t>(get("seekDir", seekDir)), dw);
    be(dir, 0, dw);
    be(dir, static_cast<uint64_t>(completed ? get("seekKeys", seekKeys) : 0), dw);
    dir.resize(60, 0);
    img.put(dirAt, dir);
    // The header.
    std::vector<unsigned char> h = {'r', 'o', 'o', 't'};
    const int32_t ver = wide && version < 1000000 ? version + 1000000 : version;
    be(h, static_cast<uint32_t>(ver), 4);
    be(h, static_cast<uint32_t>(begin), 4);
    const int w = ver >= 1000000 ? 8 : 4;
    be(h, static_cast<uint64_t>(get("end", end)), w);
    be(h, static_cast<uint64_t>(completed ? get("seekFree", seekFree) : 0), w);
    be(h, static_cast<uint32_t>(get("nbytesFree", nbytesFree)), 4);
    be(h, 1, 4);
    be(h, static_cast<uint32_t>(nameLen), 4);
    h.push_back(static_cast<unsigned char>(w));
    be(h, 505, 4);
    be(h, static_cast<uint64_t>(completed ? get("seekInfo", seekInfo) : 0), w);
    be(h, static_cast<uint32_t>(get("nbytesInfo", nbytesInfo)), 4);
    h.resize(h.size() + 18, 0x77); // the UUID
    img.put(0, h);
    img.size = get("size", get("end", end));
    return img;
  }
};
} // namespace

// The parse of the header, the directory's record and a label, with every cut of their bytes.
TEST(RootFormat, ParsesWhatRootWrites) {
  Synth s;
  const Image img = s.build();
  const auto b = img.read(0, 300);
  RootFileHeader h;
  ASSERT_TRUE(parseRootFileHeader(b.data(), b.size(), h));
  EXPECT_EQ(h.version, 63602);
  EXPECT_EQ(h.begin, 100);
  EXPECT_EQ(h.end, s.end);
  EXPECT_EQ(h.seekFree, s.seekFree);
  EXPECT_EQ(h.nbytesFree, s.nbytesFree);
  EXPECT_EQ(h.nbytesName, 58);
  EXPECT_EQ(h.units, 4);
  EXPECT_EQ(h.compress, 505);
  EXPECT_EQ(h.seekInfo, s.seekInfo);
  EXPECT_EQ(h.nbytesInfo, s.nbytesInfo);
  for (size_t n = 0; n < 45; ++n)
    EXPECT_FALSE(parseRootFileHeader(b.data(), n, h)) << n;
  EXPECT_TRUE(parseRootFileHeader(b.data(), 45, h));
  EXPECT_FALSE(parseRootFileHeader(nullptr, 300, h));
  RootDirRecord d;
  ASSERT_TRUE(parseRootDirRecord(b.data() + 158, 60, d));
  EXPECT_EQ(d.version, 5);
  EXPECT_EQ(d.nbytesKeys, s.nbytesKeys);
  EXPECT_EQ(d.nbytesName, 58);
  EXPECT_EQ(d.seekDir, 100);
  EXPECT_EQ(d.seekKeys, s.seekKeys);
  for (size_t n = 0; n < 30; ++n)
    EXPECT_FALSE(parseRootDirRecord(b.data() + 158, n, d)) << n;
  EXPECT_TRUE(parseRootDirRecord(b.data() + 158, 30, d)) << "32-bit positions end at 30 bytes";
  // 64-bit: the header's fields end at 57, the record's at 42.
  Synth w;
  w.wide = true;
  w.base = 5000000000LL;
  const Image wi = w.build();
  const auto wb = wi.read(0, 300);
  ASSERT_TRUE(parseRootFileHeader(wb.data(), wb.size(), h));
  EXPECT_TRUE(h.wide());
  EXPECT_EQ(h.end, w.end);
  EXPECT_GT(h.end, 5000000000LL);
  EXPECT_EQ(h.seekInfo, w.seekInfo);
  EXPECT_FALSE(parseRootFileHeader(wb.data(), 56, h));
  EXPECT_TRUE(parseRootFileHeader(wb.data(), 57, h));
  ASSERT_TRUE(parseRootDirRecord(wb.data() + 158, 60, d));
  EXPECT_EQ(d.version, 1005);
  EXPECT_EQ(d.seekKeys, w.seekKeys);
  EXPECT_FALSE(parseRootDirRecord(wb.data() + 158, 41, d));
  // A label: 32-bit positions are unsigned, the top 16 bits of a 64-bit directory are dropped, and
  // a name of 255 characters or more has a 32-bit length.
  auto l = label({70, 4, 0xF0000000LL, 100, "TTree", "Events", std::string(300, 'x')});
  size_t at = 0;
  RootKeyLabel k;
  ASSERT_TRUE(parseRootKeyLabel(l.data(), l.size(), at, k));
  EXPECT_EQ(at, l.size());
  EXPECT_EQ(k.seekKey, 0xF0000000LL) << "unsigned, as ROOT reads it";
  EXPECT_EQ(k.nbytes, 70);
  for (size_t n = 0; n < l.size(); ++n) {
    size_t a = 0;
    EXPECT_FALSE(parseRootKeyLabel(l.data(), n, a, k)) << n;
    EXPECT_EQ(a, 0u);
  }
  l = label({70, 1004, 6000000000LL, (int64_t(7) << 48) | 100, "TTree", "Events", "t"});
  at = 0;
  ASSERT_TRUE(parseRootKeyLabel(l.data(), l.size(), at, k));
  EXPECT_EQ(k.seekKey, 6000000000LL);
  EXPECT_EQ(k.seekPdir, 100) << "the process-id offset in the top bits is not part of the position";
  // A long name whose 32-bit length is negative, or runs past the bytes.
  l = label({70, 4, 200, 100, "TTree", "Events", "t"});
  std::vector<unsigned char> bad(l.begin(), l.begin() + 26);
  bad.push_back(255);
  be(bad, 0xFFFFFFF0u, 4);
  at = 0;
  EXPECT_FALSE(parseRootKeyLabel(bad.data(), bad.size(), at, k));
  bad.resize(27);
  be(bad, 1000, 4);
  bad.resize(bad.size() + 10, 'a');
  at = 0;
  EXPECT_FALSE(parseRootKeyLabel(bad.data(), bad.size(), at, k));
  at = 5;
  EXPECT_FALSE(parseRootKeyLabel(l.data(), 3, at, k)) << "a position past the bytes";
}

// Whole files that ROOT reads: the size confirmed when it equals the header's end, no claim when
// larger.
TEST(RootCheck, WholeFilesStand) {
  for (bool wide : {false, true}) {
    Synth s;
    s.wide = wide;
    if (wide)
      s.base = 5000000000LL;
    Image img = s.build();
    std::vector<std::string> reads;
    RootHeaderVerdict v = check(img, &reads);
    EXPECT_EQ(v.kind, K::kConfirmed) << v.why;
    EXPECT_EQ(v.next, RootHeaderVerdict::kNone);
    ASSERT_EQ(reads.size(), 2u) << "the first bytes, then the list of top-level objects";
    EXPECT_EQ(reads[1], std::to_string(s.seekKeys) + "+" + std::to_string(s.nbytesKeys));
    EXPECT_EQ(v.dir.seekKeys, s.seekKeys);
    img.size = s.end + 4096; // bytes past the end (appended, or a file updated in place)
    EXPECT_EQ(check(img).kind, K::kNoClaim) << wide;
    img.size = s.end - 1; // cut short of its end: F1
    v = check(img);
    EXPECT_EQ(v.kind, K::kBad);
    EXPECT_EQ(v.why, "size " + std::to_string(s.end - 1) + ": its header says the file is " +
                         std::to_string(s.end) + " bytes");
  }
  // No objects at all (every object deleted): an empty list stands.
  Synth e;
  e.objects = 0;
  EXPECT_EQ(check(e.build()).kind, K::kConfirmed);
  // Many objects, names of every length.
  Synth m;
  m.objects = 3000;
  EXPECT_EQ(check(m.build()).kind, K::kConfirmed);
  // A file of a version before 40000: the directory's record is 48 bytes.
  Synth o;
  o.version = 30000;
  EXPECT_EQ(check(o.build()).kind, K::kConfirmed);
}

// A header never completed (the writer never saved the file: no list of top-level objects placed
// past the header) makes no claim: no refusal for what follows, and no confirmed size.
TEST(RootCheck, NeverCompletedMakesNoClaim) {
  for (bool wide : {false, true}) {
    Synth s;
    s.wide = wide;
    s.completed = false;
    Image img = s.build();
    std::vector<std::string> reads;
    const RootHeaderVerdict v = check(img, &reads);
    EXPECT_EQ(v.kind, K::kNoClaim) << "not confirmed, though the end equals the size";
    EXPECT_EQ(reads.size(), 1u) << "no list to read";
    img.size = s.end - 10;
    EXPECT_EQ(check(img).kind, K::kBad) << "a cut copy is still refused (F1)";
  }
  // The list placed at the header's begin, or before it, is no position: never completed.
  for (int64_t at : {int64_t(100), int64_t(50), int64_t(-5)}) {
    Synth s;
    s.set["seekKeys"] = at;
    EXPECT_EQ(check(s.build()).kind, K::kNoClaim) << at;
  }
  // The file just created and never written to: its end right after the directory's record.
  Synth fresh;
  fresh.completed = false;
  fresh.set["end"] = 100 + 58 + 60;
  const Image img = fresh.build();
  EXPECT_EQ(img.size, 218);
  EXPECT_EQ(check(img).kind, K::kNoClaim);
}

// The header's own fields, where ROOT refuses a file for them.
TEST(RootCheck, HeaderFields) {
  struct Row {
    bool wide;
    int64_t begin;
    int32_t nameLen;
    K kind;
    const char* what;
  };
  const Row rows[] = {
      {false, 100, 58, K::kConfirmed, "as written"},
      {false, 45, 58, K::kConfirmed, "the first record right after the 32-bit header's fields"},
      {false, 44, 58, K::kBad, "inside the 32-bit header's fields"},
      {true, 57, 58, K::kConfirmed, "right after the 64-bit header's fields"},
      {true, 56, 58, K::kBad, "inside the 64-bit header's fields"},
      {false, 0, 58, K::kBad, "at 0"},
      {false, -100, 58, K::kBad, "negative"},
      {false, 100, 10, K::kConfirmed, "the shortest name record ROOT reads"},
      {false, 100, 9, K::kBad, "a name record of 9 bytes"},
      {false, 100, 0, K::kBad, "of 0"},
      {false, 100, -58, K::kBad, "negative"},
      {false, 100, 10000, K::kConfirmed, "the longest ROOT reads"},
      {false, 100, 10001, K::kBad, "one more"},
      {true, 100, 10001, K::kBad, "one more, 64-bit"},
  };
  for (const Row& r : rows) {
    Synth s;
    s.wide = r.wide;
    s.begin = r.begin;
    s.nameLen = r.nameLen;
    if (r.begin < 45 ||
        r.nameLen < 10) { // no layout has a place for these: the header's fields alone
      Synth t;            // are set, the records stay where they were written
      t.wide = r.wide;
      Image img = t.build();
      std::vector<unsigned char> h = img.read(0, 57);
      const auto put32 = [&h](size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i)
          h[at + static_cast<size_t>(i)] = static_cast<unsigned char>(v >> (8 * (3 - i)));
      };
      put32(8, static_cast<uint32_t>(r.begin));
      put32(r.wide ? 36 : 28, static_cast<uint32_t>(r.nameLen));
      img.put(0, h);
      const RootHeaderVerdict v = check(img);
      EXPECT_EQ(v.kind, r.kind) << r.what << ": " << v.why;
      continue;
    }
    const RootHeaderVerdict v = check(s.build());
    EXPECT_EQ(v.kind, r.kind) << r.what << ": " << v.why;
  }
  // The texts.
  Synth s;
  s.build();
  Image img = s.img;
  std::vector<unsigned char> h = img.read(0, 45);
  h[8] = h[9] = h[10] = 0;
  h[11] = 44;
  img.put(0, h);
  EXPECT_EQ(check(img).why,
            "size " + std::to_string(s.end) +
                ": its header says its first record begins at 44, inside the header");
  Synth p;
  p.set["end"] = 90;
  p.set["size"] = 6559;
  EXPECT_EQ(
      check(p.build()).why,
      "size 6559: its header says its first record begins at 100, past the end of its data at 90");
  Synth n;
  n.nameLen = 10001;
  const std::string nwhy = check(n.build()).why;
  EXPECT_EQ(
      nwhy,
      "size " + std::to_string(n.end) +
          ": its header gives the top directory's name record 10001 bytes; ROOT reads 10 to 10000");
}

// The top directory's record: inside the data, its name record in range, read again when long names
// place it past the first bytes.
TEST(RootCheck, DirectoryRecord) {
  // Its end past the end of the data (the end placed right before it ends): refused as ROOT does.
  Synth s;
  s.completed = false;
  s.build();
  const int64_t recEnd = 100 + 58 + 60;
  s.set["end"] = recEnd - 1;
  s.set["size"] = 6559;
  RootHeaderVerdict v = check(s.build());
  EXPECT_EQ(v.kind, K::kBad);
  EXPECT_EQ(v.why,
            "size 6559: its top directory's record ends at 218, past the end of its data at 217");
  s.set["end"] = recEnd;
  EXPECT_EQ(check(s.build()).kind, K::kNoClaim)
      << "ending at the end: ROOT's file with no keys yet";
  // Its own name record out of range.
  for (int32_t n : {9, 0, -1, 10001}) {
    Synth d;
    d.dirNameLen = n;
    v = check(d.build());
    EXPECT_EQ(v.kind, K::kBad) << n;
    EXPECT_EQ(v.why, "size " + std::to_string(d.end) +
                         ": its top directory's record gives its name record " + std::to_string(n) +
                         " bytes; ROOT reads 10 to 10000")
        << n;
  }
  Synth ok;
  ok.dirNameLen = 10;
  EXPECT_EQ(check(ok.build()).kind, K::kConfirmed);
  // Long names: the record lies past the first 300 bytes, and a second read fetches it.
  for (bool wide : {false, true}) {
    Synth l;
    l.wide = wide;
    l.nameLen = 900;
    std::vector<std::string> reads;
    v = check(l.build(), &reads);
    EXPECT_EQ(v.kind, K::kConfirmed) << v.why;
    ASSERT_EQ(reads.size(), 3u);
    EXPECT_EQ(reads[1], "1000+60") << "the record's 60 bytes at begin + nbytesName";
    // That read answered short: the copy is refused.
    v = check(l.build(), nullptr, 1);
    EXPECT_EQ(v.kind, K::kBad);
    EXPECT_EQ(v.why, "size " + std::to_string(l.end) + ": a read of 60 bytes at 1000 answered 59");
  }
  // The record just inside the first bytes (it ends at 300): no second read.
  Synth edge;
  edge.nameLen = 140;
  std::vector<std::string> reads;
  EXPECT_EQ(check(edge.build(), &reads).kind, K::kConfirmed);
  EXPECT_EQ(reads.size(), 2u);
  edge.nameLen = 141;
  reads.clear();
  EXPECT_EQ(check(edge.build(), &reads).kind, K::kConfirmed);
  EXPECT_EQ(reads.size(), 3u) << "ending at 301: read";
  // A copy shorter than 300 bytes whose record lies inside it.
  Synth tiny;
  tiny.objects = 0;
  tiny.completed = false;
  tiny.set["end"] = 218;
  Image img = tiny.build();
  EXPECT_EQ(img.size, 218);
  EXPECT_EQ(check(img).kind, K::kNoClaim);
}

// The records a completed header places: inside the file's records, a position of 0 meaning none.
TEST(RootCheck, RecordsThePlacesLieInside) {
  Synth base;
  base.build();
  const int64_t end = base.end;
  struct Row {
    const char* field;
    int64_t value;
    K kind;
    const char* what;
  };
  const Row rows[] = {
      {"seekKeys", end, K::kBad, "the list at the end"},
      {"seekKeys", end + 1000, K::kBad, "the list past the end"},
      {"seekKeys", end - 10, K::kBad, "the list running past the end"},
      {"nbytesKeys", 0, K::kBad, "a list of no bytes"},
      {"nbytesKeys", -5, K::kBad, "of negative length"},
      {"seekInfo", 0, K::kConfirmed, "no class descriptions"},
      {"seekInfo", 50, K::kBad, "class descriptions inside the header"},
      {"seekInfo", end - 1, K::kBad, "class descriptions running past the end"},
      {"seekInfo", -8, K::kBad, "class descriptions at a negative position"},
      {"nbytesInfo", -1, K::kBad, "class descriptions of negative length"},
      {"seekFree", 0, K::kConfirmed, "no list of free space"},
      {"seekFree", 99, K::kBad, "the free list before the first record"},
      {"seekFree", end + 1, K::kBad, "the free list past the end"},
      {"nbytesFree", 1000000, K::kBad, "the free list running past the end"},
  };
  for (const Row& r : rows) {
    Synth s;
    s.set[r.field] = r.value;
    const RootHeaderVerdict v = check(s.build());
    EXPECT_EQ(v.kind, r.kind) << r.what << ": " << v.why;
  }
  Synth s;
  s.set["seekInfo"] = 50;
  EXPECT_EQ(
      check(s.build()).why,
      "size " + std::to_string(end) +
          ": its class descriptions lie at 50, 400 bytes, outside the file's records (100 to " +
          std::to_string(end) + ")");
  Synth f;
  f.set["seekFree"] = end + 1;
  EXPECT_EQ(check(f.build()).why,
            "size " + std::to_string(end) + ": its list of free space lies at " +
                std::to_string(end + 1) + ", 60 bytes, outside the file's records (100 to " +
                std::to_string(end) + ")");
  Synth k;
  k.set["seekKeys"] = end + 1000;
  EXPECT_EQ(check(k.build()).why,
            "size " + std::to_string(end) + ": its list of top-level objects lies at " +
                std::to_string(end + 1000) + ", " + std::to_string(base.nbytesKeys) +
                " bytes, outside the file's records (100 to " + std::to_string(end) + ")");
  // A list longer than the bound is not read: the verdict stands as the header gives it.
  Synth big;
  big.build();
  big.set["nbytesKeys"] = static_cast<int64_t>(kRootKeysListMaxBytes) + 1;
  big.set["end"] = big.seekKeys + static_cast<int64_t>(kRootKeysListMaxBytes) + 100;
  big.set["seekFree"] = 0;
  std::vector<std::string> reads;
  const RootHeaderVerdict v = check(big.build(), &reads);
  EXPECT_EQ(v.kind, K::kConfirmed) << v.why;
  EXPECT_EQ(reads.size(), 1u);
}

// The list of top-level objects: its own label, the count, each object inside the records with the
// top directory as its parent.
TEST(RootCheck, ListOfTopLevelObjects) {
  Synth base;
  base.build();
  const int64_t end = base.end;
  const std::string obj = "size " + std::to_string(end) + ": its list of top-level objects";
  const std::string sz = "size " + std::to_string(end) + ": its list of top-level objects at " +
                         std::to_string(base.seekKeys);
  struct Row {
    const char* field;
    int64_t value;
    K kind;
    std::string why; // "" to skip the text
  };
  const Row rows[] = {
      {"ownNbytes", base.nbytesKeys + 1, K::kBad,
       sz + " is labelled as " + std::to_string(base.nbytesKeys + 1) + " bytes long, not " +
           std::to_string(base.nbytesKeys)},
      {"ownSeek", base.seekKeys - 1, K::kBad,
       sz + " is labelled as lying at " + std::to_string(base.seekKeys - 1)},
      {"ownSeek", 0, K::kBad, ""},
      {"ownPdir", 0, K::kBad,
       sz + " is labelled with the directory at 0, not the top directory at 100"},
      {"ownPdir", 158, K::kBad, ""},
      {"count", -1, K::kBad, sz + " gives a count of -1 objects"},
      {"count", 4, K::kBad, sz + " ends inside its entry 4 of 4"},
      {"count", 2, K::kConfirmed, ""}, // fewer listed than the record holds: ROOT reads those
      {"count", 0, K::kConfirmed, ""},
      {"entrySeek", 99, K::kBad, ""},
      {"entrySeek", 0, K::kBad, ""},
      {"entrySeek", end, K::kBad, ""},
      {"entrySeek", end - 1, K::kBad,
       obj + " names an object at " + std::to_string(end - 1) +
           ", 1037 bytes, outside the file's records (100 to " + std::to_string(end) + ")"},
      {"entryNbytes", 0, K::kBad, ""},
      {"entryNbytes", -1, K::kBad, ""},
      {"entryNbytes", 100000000, K::kBad, ""},
      {"entryPdir", 0, K::kBad, ""},
      {"entryPdir", 101, K::kBad, ""},
      {"entryPdir", 4000, K::kBad,
       obj + " names an object at " + std::to_string(base.listed[1].seekKey) +
           " in the directory at 4000, not the top directory at 100"},
  };
  for (const Row& r : rows) {
    Synth s;
    s.set[r.field] = r.value;
    const RootHeaderVerdict v = check(s.build());
    EXPECT_EQ(v.kind, r.kind) << r.field << "=" << r.value << ": " << v.why;
    if (!r.why.empty()) {
      EXPECT_EQ(v.why, r.why) << r.field << "=" << r.value;
    }
  }
  // The same for 64-bit labels; the process-id offset in a directory's top bits is not a position.
  Synth w;
  w.wide = true;
  w.base = 5000000000LL;
  w.build();
  w.set["entryPdir"] = (int64_t(3) << 48) | 100;
  EXPECT_EQ(check(w.build()).kind, K::kConfirmed);
  w.set["entryPdir"] = 5000000000LL;
  EXPECT_EQ(check(w.build()).kind, K::kBad);
  // The list zeroed, or overwritten with other bytes, at the right size.
  for (unsigned char fill : {0x00, 0xA5, 0xFF}) {
    Synth z;
    z.build();
    Image img = z.img;
    img.put(z.seekKeys, std::vector<unsigned char>(static_cast<size_t>(z.nbytesKeys), fill));
    const RootHeaderVerdict v = check(img);
    EXPECT_EQ(v.kind, K::kBad) << int(fill);
  }
  // Its read answered short.
  const RootHeaderVerdict v = check(base.img, nullptr, 1);
  EXPECT_EQ(v.kind, K::kBad);
  EXPECT_EQ(v.why, "size " + std::to_string(end) + ": a read of " +
                       std::to_string(base.nbytesKeys) + " bytes at " +
                       std::to_string(base.seekKeys) + " answered " +
                       std::to_string(base.nbytesKeys - 1));
  // A list that holds no label at all (shorter than one).
  Synth t;
  t.build();
  t.set["nbytesKeys"] = 20;
  EXPECT_EQ(check(t.build()).why, "size " + std::to_string(end) +
                                      ": its list of top-level objects at " +
                                      std::to_string(t.seekKeys) + " does not hold its own label");
}

// Every refusal's text fits in what the error naming every copy keeps of it (160 characters), with
// positions of 10 digits: 64-bit files of many GB, every rule broken in turn.
TEST(RootCheck, TextsFitTheError) {
  std::vector<std::pair<std::string, int64_t>> breaks = {
      {"end", 9999999999LL},
      {"seekInfo", 50},
      {"seekFree", 9999999999LL},
      {"seekKeys", 9999999999LL},
      {"nbytesKeys", 0},
      {"ownSeek", 9999999998LL},
      {"ownNbytes", 2147483647LL},
      {"ownPdir", 9999999997LL},
      {"count", -2147483647LL},
      {"entrySeek", 9999999996LL},
      {"entryNbytes", -2147483647LL},
      {"entryPdir", 9999999995LL},
  };
  size_t longest = 0;
  for (const auto& b : breaks) {
    Synth s;
    s.wide = true;
    s.base = 9000000000LL;
    s.build();
    s.set[b.first] = b.second;
    if (b.first == "end")
      s.set["size"] = 9999999998LL;
    const RootHeaderVerdict v = check(s.build());
    EXPECT_EQ(v.kind, K::kBad) << b.first;
    EXPECT_LE(v.why.size(), 150u) << b.first << ": " << v.why;
    longest = std::max(longest, v.why.size());
  }
  EXPECT_GT(longest, 130u) << "the rows reached the long texts";
}

// rootCheckRead on a verdict that asks for nothing, or with no bytes: no change, or a short answer.
TEST(RootCheck, CheckReadEdges) {
  RootHeaderVerdict v;
  v.kind = K::kConfirmed;
  rootCheckRead(v, nullptr, 0);
  EXPECT_EQ(v.kind, K::kConfirmed) << "nothing asked";
  v.next = RootHeaderVerdict::kKeysList;
  v.nextAt = 500;
  v.nextLen = 40;
  v.size = 1000;
  rootCheckRead(v, nullptr, 40);
  EXPECT_EQ(v.kind, K::kBad);
  EXPECT_EQ(v.why, "size 1000: a read of 40 bytes at 500 answered 0");
  EXPECT_EQ(v.next, RootHeaderVerdict::kNone);
}

// The limit of a further read: what is left of the checks' time, rounded up; none left, none.
TEST(RootCheck, CheckReadTimeout) {
  EXPECT_EQ(checkReadTimeoutS(0), 0);
  EXPECT_EQ(checkReadTimeoutS(-5), 0);
  EXPECT_EQ(checkReadTimeoutS(1), 1);
  EXPECT_EQ(checkReadTimeoutS(999), 1);
  EXPECT_EQ(checkReadTimeoutS(1000), 1);
  EXPECT_EQ(checkReadTimeoutS(1001), 2);
  EXPECT_EQ(checkReadTimeoutS(59000), 59);
}

namespace {
// Runs the checks over a copy made of `file` (its bytes; the reads answer from them, short past
// their end) that reports `size`, and holds every rule of the contract: at most three reads, each
// inside the copy and within the bound; a final verdict; a refusal says why. What the mutants came
// to: [reads made][kind].
uint64_t gOutcomes[4][3] = {};
void hostile(const std::vector<unsigned char>& file, long long size, const char* what,
             uint64_t iter) {
  const auto readAt = [&](long long at, size_t len) {
    std::vector<unsigned char> out;
    for (size_t i = 0;
         i < len && at + static_cast<long long>(i) < static_cast<long long>(file.size()) &&
         at + static_cast<long long>(i) < size;
         ++i)
      out.push_back(file[static_cast<size_t>(at) + i]);
    return out;
  };
  std::vector<unsigned char> b = readAt(0, rootHeaderBytes(size));
  RootHeaderVerdict v = rootHeaderVerdict(b.data(), b.size(), size);
  int reads = 1;
  while (v.next != RootHeaderVerdict::kNone) {
    ASSERT_LT(reads, 3) << what << " " << iter;
    ASSERT_GE(v.nextAt, 0) << what << " " << iter;
    ASSERT_LE(v.nextAt + static_cast<long long>(v.nextLen), size) << what << " " << iter;
    ASSERT_LE(v.nextLen, kRootKeysListMaxBytes) << what << " " << iter;
    ASSERT_GT(v.nextLen, 0u) << what << " " << iter;
    b = readAt(v.nextAt, v.nextLen);
    rootCheckRead(v, b.data(), b.size());
    ++reads;
  }
  ++gOutcomes[std::min(reads, 3)][v.kind];
  ASSERT_TRUE(v.kind == K::kBad || v.kind == K::kConfirmed || v.kind == K::kNoClaim);
  ASSERT_EQ(v.kind == K::kBad, !v.why.empty()) << what << " " << iter;
  if (v.kind == K::kConfirmed) {
    ASSERT_EQ(v.end, size) << what << " " << iter;
  }
}
std::vector<unsigned char> dense(const Image& img) {
  return img.read(0, static_cast<size_t>(img.size));
}
} // namespace

// Hostile bytes: random flips, big-endian length bombs and zeroed spans in the parts the checks
// read (the header, the directory's record, the list), and random sizes, over synthetic files and
// the ROOT files under tests/data. UCACHE_FUZZ_ITERS sets the mutants per seed file (default 4000).
TEST(RootCheck, HostileBytes) {
  std::vector<std::pair<std::string, std::vector<unsigned char>>> seeds;
  {
    Synth s;
    seeds.push_back({"synthetic", dense(s.build())});
    Synth l;
    l.nameLen = 700;
    l.objects = 40;
    seeds.push_back({"synthetic, long names", dense(l.build())});
    Synth n;
    n.completed = false;
    seeds.push_back({"synthetic, never completed", dense(n.build())});
  }
#ifdef UCACHE_TEST_DATA_DIR
  for (const char* f : {"unnamed_codec_fixture.root", "unnamed_codec_wide_fixture.root",
                        "rntuple_fixture.root", "rntuple_unnamed_codec_fixture.root"}) {
    FILE* fp = std::fopen((std::string(UCACHE_TEST_DATA_DIR) + "/" + f).c_str(), "rb");
    ASSERT_NE(fp, nullptr) << f;
    std::vector<unsigned char> b;
    unsigned char buf[65536];
    size_t r;
    while ((r = std::fread(buf, 1, sizeof buf, fp)) > 0)
      b.insert(b.end(), buf, buf + r);
    std::fclose(fp);
    // A ROOT file written by ROOT passes as it is.
    RootHeaderVerdict v;
    {
      std::vector<unsigned char> h(b.begin(), b.begin() + static_cast<long>(rootHeaderBytes(
                                                              static_cast<long long>(b.size()))));
      v = rootHeaderVerdict(h.data(), h.size(), static_cast<long long>(b.size()));
      while (v.next != RootHeaderVerdict::kNone) {
        std::vector<unsigned char> x(b.begin() + v.nextAt, b.begin() + v.nextAt + v.nextLen);
        rootCheckRead(v, x.data(), x.size());
      }
    }
    EXPECT_EQ(v.kind, K::kConfirmed) << f << ": " << v.why;
    seeds.push_back({f, b});
  }
#endif
  const char* it = std::getenv("UCACHE_FUZZ_ITERS");
  const uint64_t iters = it ? std::strtoull(it, nullptr, 10) : 4000;
  std::mt19937_64 rng(424242);
  for (const auto& seed : seeds) {
    const auto& file = seed.second;
    // The parts the checks read in the pristine file: mutations land there most of the time.
    std::vector<std::pair<size_t, size_t>> parts = {{0, std::min<size_t>(300, file.size())}};
    {
      std::vector<unsigned char> h(
          file.begin(),
          file.begin() + static_cast<long>(rootHeaderBytes(static_cast<long long>(file.size()))));
      RootHeaderVerdict v =
          rootHeaderVerdict(h.data(), h.size(), static_cast<long long>(file.size()));
      while (v.next != RootHeaderVerdict::kNone) {
        parts.push_back({static_cast<size_t>(v.nextAt), v.nextLen});
        std::vector<unsigned char> x(file.begin() + v.nextAt, file.begin() + v.nextAt + v.nextLen);
        rootCheckRead(v, x.data(), x.size());
      }
    }
    for (uint64_t i = 0; i < iters; ++i) {
      std::vector<unsigned char> m = file;
      const auto& part = parts[rng() % parts.size()];
      const int ops = 1 + static_cast<int>(rng() % 4);
      for (int o = 0; o < ops; ++o) {
        const size_t at = part.first + (part.second ? rng() % part.second : 0);
        if (at >= m.size())
          continue;
        switch (rng() % 5) {
        case 0: // a flipped byte
          m[at] ^= static_cast<unsigned char>(1u << (rng() % 8));
          break;
        case 1: { // a random byte
          m[at] = static_cast<unsigned char>(rng());
          break;
        }
        case 2: { // a length bomb: a big-endian extreme value
          const uint64_t bombs[] = {0,     0xFFFFFFFFull,         0x7FFFFFFFull, 0x80000000ull,
                                    ~0ull, 0x7FFFFFFFFFFFFFFFull, 255,           10001};
          const uint64_t val = bombs[rng() % 8];
          const int k = (rng() % 2) ? 4 : 8;
          for (int j = 0; j < k && at + static_cast<size_t>(j) < m.size(); ++j)
            m[at + static_cast<size_t>(j)] = static_cast<unsigned char>(val >> (8 * (k - 1 - j)));
          break;
        }
        case 3: { // a zeroed span
          const size_t len = 1 + rng() % 64;
          for (size_t j = 0; j < len && at + j < m.size(); ++j)
            m[at + j] = 0;
          break;
        }
        default: // nothing here; the size changes below
          break;
        }
      }
      long long size = static_cast<long long>(m.size());
      switch (rng() % 6) {
      case 0:
        size = static_cast<long long>(rng() % (m.size() + 1)); // cut anywhere
        break;
      case 1:
        size += static_cast<long long>(
            rng() % 5000); // longer than the bytes: reads past them come back short
        break;
      default:
        break;
      }
      if (size <= 0)
        continue;
      hostile(m, size, seed.first.c_str(), i);
      if (HasFatalFailure())
        return;
    }
  }
  // How far the mutants got, so that a run shows the deeper rules were reached.
  std::printf("mutants by reads made (1, 2, 3) and kind (no claim, confirmed, bad):");
  for (int r = 1; r <= 3; ++r)
    std::printf(" [%d: %llu %llu %llu]", r, static_cast<unsigned long long>(gOutcomes[r][0]),
                static_cast<unsigned long long>(gOutcomes[r][1]),
                static_cast<unsigned long long>(gOutcomes[r][2]));
  std::printf("\n");
  EXPECT_GT(gOutcomes[2][2] + gOutcomes[3][2], 0u)
      << "some mutants refused by the list's own rules";
}
