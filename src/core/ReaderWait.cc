#include "ReaderWait.h"

#include <algorithm>

namespace ucache {

namespace {
struct Slot {
  std::shared_ptr<ReaderWait::Thread> rec;
  uint64_t generation = 0;
};
} // namespace

std::shared_ptr<ReaderWait::Thread> ReaderWait::current(uint64_t generation, bool& created) {
  // Held by the thread and by each request it has outstanding, so a request
  // answered after its thread ended still has a record to close.
  thread_local Slot slot;
  created = false;
  if (!slot.rec || slot.generation != generation) {
    slot.rec = std::make_shared<Thread>();
    slot.generation = generation;
    created = true;
  }
  return slot.rec;
}

void ReaderWait::begin(Thread& t, uint64_t nowUs) {
  std::lock_guard<std::mutex> g(t.mu);
  if (t.outstanding++ == 0) {
    t.startUs = nowUs;
    t.tier = kByte;
  }
}

namespace {
uint64_t close(ReaderWait::Thread& t, uint64_t nowUs, ReaderWait::Tier& intervalTier) {
  if (t.outstanding <= 0) // not issued through begin(): nothing to close
    return 0;
  if (--t.outstanding)
    return 0;
  intervalTier = static_cast<ReaderWait::Tier>(t.tier);
  return nowUs > t.startUs ? nowUs - t.startUs : 0;
}
} // namespace

uint64_t ReaderWait::end(Thread& t, uint64_t nowUs, Tier tier, Tier& intervalTier) {
  std::lock_guard<std::mutex> g(t.mu);
  t.tier = std::max<uint8_t>(t.tier, tier);
  return close(t, nowUs, intervalTier);
}

uint64_t ReaderWait::cancel(Thread& t, uint64_t nowUs, Tier& intervalTier) {
  std::lock_guard<std::mutex> g(t.mu);
  return close(t, nowUs, intervalTier);
}

} // namespace ucache
