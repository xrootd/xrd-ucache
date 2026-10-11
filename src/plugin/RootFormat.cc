#include "RootFormat.h"

#include <cstring>

namespace ucache {

namespace {
constexpr int64_t kPidOffsetMask = 0xffffffffffffLL; // TKey: the offset lives in the top 16 bits
// The bytes of a record, read in order; every read is checked against the end.
struct Cursor {
  const unsigned char* b;
  size_t n;
  size_t at;
  bool ok = true;
  uint64_t take(size_t k) {
    if (!ok || k > n || at > n - k) {
      ok = false;
      return 0;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < k; ++i)
      v = v << 8 | b[at + i];
    at += k;
    return v;
  }
  int16_t i16() { return static_cast<int16_t>(static_cast<uint16_t>(take(2))); }
  int32_t i32() { return static_cast<int32_t>(static_cast<uint32_t>(take(4))); }
  uint32_t u32() { return static_cast<uint32_t>(take(4)); }
  int64_t i64() { return static_cast<int64_t>(take(8)); }
  // One of ROOT's names: a length byte, or 255 and a 32-bit length; then the characters (`from`,
  // `len`: where they lie).
  void name(size_t* from = nullptr, size_t* chars = nullptr) {
    uint64_t len = take(1);
    if (len == 255) {
      const int32_t l = i32();
      if (l < 0)
        ok = false;
      len = static_cast<uint64_t>(l);
    }
    if (!ok || len > n - at) {
      ok = false;
      return;
    }
    if (from)
      *from = at;
    if (chars)
      *chars = static_cast<size_t>(len);
    at += static_cast<size_t>(len);
  }
  // A label's fields up to its directory's position (TKey::ReadKeyBuffer).
  void keyFields(RootKeyLabel& r) {
    r.nbytes = i32();
    r.version = i16();
    r.objlen = i32();
    r.datime = u32();
    r.keylen = i16();
    r.cycle = i16();
    if (r.version > 1000) {
      r.seekKey = i64();
      r.seekPdir = i64() & kPidOffsetMask;
    } else {
      r.seekKey = u32();
      r.seekPdir = u32();
    }
  }
};
} // namespace

size_t rootHeaderFieldsEnd(int32_t version) {
  return version >= 1000000 ? 57 : 45;
}

bool parseRootFileHeader(const unsigned char* b, size_t n, RootFileHeader& h) {
  if (!b || n < 8 || std::memcmp(b, "root", 4) != 0)
    return false;
  Cursor c{b, n, 4};
  RootFileHeader r;
  r.version = c.i32();
  if (n < rootHeaderFieldsEnd(r.version))
    return false;
  r.begin = c.i32();
  const bool wide = r.wide();
  r.end = wide ? c.i64() : c.i32();
  r.seekFree = wide ? c.i64() : c.i32();
  r.nbytesFree = c.i32();
  r.nfree = c.i32();
  r.nbytesName = c.i32();
  r.units = static_cast<uint8_t>(c.take(1));
  r.compress = c.i32();
  r.seekInfo = wide ? c.i64() : c.i32();
  r.nbytesInfo = c.i32();
  if (!c.ok)
    return false;
  h = r;
  return true;
}

size_t rootDirRecordBytes(int32_t version) {
  // 22 bytes of fields, two dates of 4, a UUID of 18; 12 more (64-bit room) since version 40000.
  return version >= 40000 ? 60 : 48;
}

bool parseRootDirRecord(const unsigned char* b, size_t n, RootDirRecord& d) {
  if (!b)
    return false;
  Cursor c{b, n, 0};
  RootDirRecord r;
  r.version = c.i16();
  r.created = c.u32();
  r.modified = c.u32();
  r.nbytesKeys = c.i32();
  r.nbytesName = c.i32();
  const bool wide = r.version > 1000;
  r.seekDir = wide ? c.i64() : c.i32();
  r.seekParent = wide ? c.i64() : c.i32();
  r.seekKeys = wide ? c.i64() : c.i32();
  if (!c.ok)
    return false;
  d = r;
  return true;
}

bool parseRootKeyLabel(const unsigned char* b, size_t n, size_t& at, RootKeyLabel& k) {
  if (!b || at > n)
    return false;
  Cursor c{b, n, at};
  RootKeyLabel r;
  c.keyFields(r);
  c.name(&r.classAt, &r.classLen);
  c.name(&r.nameAt, &r.nameLen);
  c.name(); // title
  if (!c.ok)
    return false;
  k = r;
  at = c.at;
  return true;
}

bool parseRootKeyFields(const unsigned char* b, size_t n, RootKeyLabel& k) {
  if (!b)
    return false;
  Cursor c{b, n, 0};
  RootKeyLabel r;
  c.keyFields(r);
  if (!c.ok)
    return false;
  k = r;
  return true;
}

} // namespace ucache
