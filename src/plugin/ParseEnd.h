// The end of a parse of a file's records that other requests may be waiting for (ReadRule.cc,
// keySeeksFor): whatever way the parse leaves -- its result, a failed read, an exception -- the
// table it fills says it is no longer parsing, and every request that waited for it is handed to
// `post`, once. Without it a parse that threw left the table parsing for good, and the requests
// waiting for it never completed.
//
// `Table` has `bool parsing` and `std::vector<std::function<void()>> waiters`, guarded by the
// mutex `lock` holds or takes. Not thread-safe itself: one per parse, on the parsing thread.
#pragma once

#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace ucache {

template <class Table> class ParseEnd {
public:
  using Post = void (*)(std::function<void()>&);
  ParseEnd(Table& table, std::unique_lock<std::mutex>& lock, Post post)
      : table_(table), lock_(lock), post_(post) {}
  ParseEnd(const ParseEnd&) = delete;
  ParseEnd& operator=(const ParseEnd&) = delete;
  ~ParseEnd() {
    if (!done_)
      finish();
  }
  // Ends the parse now: under the lock (taken when not held), parsing cleared and the waiters
  // taken; then the lock is let go and each waiter posted. Anything the caller sets in the table
  // under the same lock before this is seen by the waiters.
  void finish() noexcept {
    done_ = true;
    std::vector<std::function<void()>> w;
    try {
      if (!lock_.owns_lock())
        lock_.lock();
      table_.parsing = false;
      w.swap(table_.waiters);
      lock_.unlock();
    } catch (...) {
      return; // a lock that cannot be taken: nothing else can be done
    }
    for (auto& f : w)
      post_(f);
  }

private:
  Table& table_;
  std::unique_lock<std::mutex>& lock_;
  Post post_;
  bool done_ = false;
};

} // namespace ucache
