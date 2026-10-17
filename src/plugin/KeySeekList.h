// The process's list of file maps for check_origin_integrity (ReadRule.cc, keySeeksFor), and the
// rule for which of them it lets go.
//
// A map is found by its file entry, by identity: a request's check looks it up when its fetch's
// answer arrives, and a request that waited for another handle's parse looks it up when that
// parse ends. So a map stays in the list for as long as anything that may look for it holds the
// entry -- a handle, a fetch in flight, a request waiting for a parse, read-ahead -- and it is let
// go only once the entry itself is gone (a later entry of the same file makes a map of its own),
// and never while a parse fills it or requests wait for it. A list bounded by a count instead let
// go of maps still in use when more files than that were read at once, and the fetches that then
// found none were served unchecked and not kept.
//
// The list is therefore as long as the number of file entries alive in the process. A map costs
// 12 bytes a basket (about 3 MB for a GB of NanoAOD), which a handle that took the map holds
// anyway.
//
// `Table` has `std::weak_ptr<...> entry`, `bool parsing` and a container `waiters`. Not
// thread-safe: called under the list's own lock.
#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <memory>

namespace ucache {

// Removes from `d` every table whose entry is gone and that no parse fills and no request waits
// for; returns how many it removed. The order of the others is kept.
template <class Table> size_t dropUnusedKeySeekTables(std::deque<std::shared_ptr<Table>>& d) {
  const size_t before = d.size();
  d.erase(std::remove_if(d.begin(), d.end(),
                         [](const std::shared_ptr<Table>& t) {
                           return !t || (t->entry.expired() && !t->parsing && t->waiters.empty());
                         }),
          d.end());
  return before - d.size();
}

} // namespace ucache
