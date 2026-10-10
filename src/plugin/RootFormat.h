// The parts of a ROOT file's format that the checks made at open read: the file's header (its
// first bytes), the top directory's record, and the label at the head of a record (ROOT calls a
// record with its label a key). The layouts are ROOT's own (TFile::WriteHeader,
// TDirectoryFile::FillBuffer, TKey::FillBuffer), which every ROOT version goes on reading; every
// integer is big-endian.
//
// The bytes come from a copy at an origin and may be anything. Every function checks each offset
// and length against the bytes it is given, never reads past them, and answers false for bytes
// that do not hold what it parses. Nothing here allocates or throws.
//
// Thread-safety: pure functions of their arguments; no state.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ucache {

// A ROOT file's header: "root", the version (one of 1000000 or more marks a file whose positions
// are 64-bit integers), then the fields below. ROOT reads the 32-bit fields as signed integers.
struct RootFileHeader {
  int32_t version = 0;
  int64_t begin = 0;      // where the first record, the top directory's own, begins (fBEGIN)
  int64_t end = 0;        // where the file's data end (fEND)
  int64_t seekFree = 0;   // the record of the list of free space (fSeekFree); 0 for none
  int32_t nbytesFree = 0; // its length
  int32_t nfree = 0;
  int32_t nbytesName = 0; // the length of the top directory's name record (fNbytesName)
  uint8_t units = 0;
  int32_t compress = 0;
  int64_t seekInfo = 0;   // the record of the class descriptions (fSeekInfo); 0 for none
  int32_t nbytesInfo = 0; // its length
  bool wide() const { return version >= 1000000; }
};
// Where the header's fields end, for a file of `version`: 45 bytes, or 57 with 64-bit positions.
size_t rootHeaderFieldsEnd(int32_t version);
// The header in the first `n` bytes of `b`: false when they do not begin with "root" or end
// before the header's fields do.
bool parseRootFileHeader(const unsigned char* b, size_t n, RootFileHeader& h);

// The top directory's record, which follows its name record: it lies at the header's begin plus
// its nbytesName. Its version above 1000 marks 64-bit positions (ROOT reads 32-bit ones as signed).
struct RootDirRecord {
  int16_t version = 0;
  uint32_t created = 0, modified = 0;
  int32_t nbytesKeys = 0; // the length of the list of top-level objects
  int32_t nbytesName = 0; // the length of the name record, again
  int64_t seekDir = 0;    // where the directory's own record begins
  int64_t seekParent = 0;
  int64_t seekKeys = 0; // where the list of top-level objects begins; 0 for none
};
// The record's length as ROOT reads it (TDirectoryFile::Sizeof): 60 bytes, or 48 in a file of a
// version below 40000.
size_t rootDirRecordBytes(int32_t version);
// The record in the `n` bytes of `b`: false when they end before its fields do.
bool parseRootDirRecord(const unsigned char* b, size_t n, RootDirRecord& d);

// The label at the head of a record. Its version above 1000 marks 64-bit positions; the top 16
// bits of a 64-bit seekPdir carry another number (ROOT's process-id offset) and are dropped here.
// ROOT reads 32-bit positions in a label as unsigned integers.
struct RootKeyLabel {
  int32_t nbytes = 0; // the record's length, its label included
  int16_t version = 0;
  int32_t objlen = 0;
  uint32_t datime = 0;
  int16_t keylen = 0;
  int16_t cycle = 0;
  int64_t seekKey = 0;  // where the record says it begins
  int64_t seekPdir = 0; // where the record of its directory begins
};
// The label at `at` in the `n` bytes of `b`, with its three names (class, name, title: each a
// length byte, 255 then a 32-bit length for a long one, and the characters): on success `at` is
// moved past it. False, `at` unchanged, when it does not fit in the bytes.
bool parseRootKeyLabel(const unsigned char* b, size_t n, size_t& at, RootKeyLabel& k);

} // namespace ucache
