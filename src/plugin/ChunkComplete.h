// complete_chunks for byte-cache fills (Config.h). ROOT's read cache reads a cluster's baskets
// together, and which ones it reads ahead depends on the layout the file is shown in: after a
// sweep has made a file's replica, a later pass reads it through the replica's map and may ask for
// a basket of a cluster that the pass which filled the byte cache never read. So when a read
// misses baskets of a TTree branch, the rest of that branch's baskets in the same clusters go to
// the byte cache too: for a vector read with the miss's own fetch (completionFor), which is then
// answered once they have come too, within the read's element limit; after a single read's miss in
// the background (completeAfterMiss), which no reader waits for. A basket taken along that fails
// its check is not kept and never fails the read.
//
// Only the branches readers have asked for are known, from the file's own records, read from the
// cache alone (ROOT's open read them through it; until they are there, a miss takes nothing along
// and a later one looks again): completion makes no read of its own but the baskets it fetches.
// Only branches in a codec the sweep converts (recompress_codecs) are completed; the background
// read is not made while the handle has no open origin; nothing is
// taken along for a file read directly (max_read_fraction), by a handle whose failover is still
// undecided, or for bytes the cache holds or another fetch is bringing.
//
// Thread-safety: both entry points may be called from any thread; on a client callback thread
// nothing is parsed (the tables are used as they are). completeAfterMiss's work runs on the
// executor. Each file's table has its own lock, never held across a read; the set of tables,
// another.
#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace ucache {

struct HandleState;
class FileEntry;

// The ranges ([off, end), one per basket, not page-rounded) to fetch with a miss of the original
// ranges `asked` of `entry`: the rest of their clusters, as above, about 64 MiB of whole baskets at
// most (past it the rest is not taken); `baskets` how many. Empty when nothing applies. May parse
// the file's records from the cache (never the origin), except on a client callback thread. For a
// vector read's miss, whose fetch takes them along.
std::vector<std::pair<uint64_t, uint64_t>>
completionFor(const std::shared_ptr<HandleState>& st, const std::shared_ptr<FileEntry>& entry,
              const std::vector<std::pair<uint64_t, uint64_t>>& asked, uint64_t& baskets);

// The same for a single read's miss, fetched in the background through `st` (only when its origin
// is open), on the executor: no reader waits for it.
void completeAfterMiss(const std::shared_ptr<HandleState>& st,
                       const std::shared_ptr<FileEntry>& entry,
                       std::vector<std::pair<uint64_t, uint64_t>> asked);

// fork(): before, any first use of the tables under way is finished (the plugin's fork prepare
// handler); after, in the child, the tables it inherits are moved aside untouched (a parent thread
// may have been changing them) and it builds its own (the child handler: no I/O, no thread).
void chunkCompleteBeforeFork();
void chunkCompleteAfterForkChild();

} // namespace ucache
