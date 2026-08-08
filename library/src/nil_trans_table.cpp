/*
   Spades DDS - nil-mode transposition table.

   See nil_trans_table.hpp for the soundness argument and the definition of
   the normalised value G. This file is just the store.
*/

#include <cstring>

#include "nil_trans_table.hpp"


namespace
{
  /// Slots are a power of two so the index is a mask rather than a modulo.
  constexpr std::size_t NIL_TT_MIN_SLOTS = 1024;

  /// Default budget. Generous enough that a 13-trick deal keeps most of its
  /// working set, small enough not to be surprising.
  constexpr int NIL_TT_DEFAULT_MB = 96;
}


NilTransTable::NilTransTable()
{
  set_memory(NIL_TT_DEFAULT_MB);
}


auto NilTransTable::set_memory(const int megabytes) -> void
{
  const std::size_t budget =
    static_cast<std::size_t>(megabytes > 0 ? megabytes : 1) * 1024u * 1024u;

  std::size_t slots = NIL_TT_MIN_SLOTS;
  while ((slots * 2u) * sizeof(Entry) <= budget)
    slots *= 2u;

  // Idempotent. A caller that sets the same budget before every solve must not
  // pay a full reallocation and clear each time - and must not have its
  // generation counter reset out from under a sequence of solves.
  if (slots == table_.size())
    return;

  table_.assign(slots, Entry{});
  mask_ = slots - 1u;
  generation_ = 1;
  reset_stats();
}


auto NilTransTable::wipe() -> void
{
  std::memset(table_.data(), 0, table_.size() * sizeof(Entry));
  generation_ = 1;
}


auto NilTransTable::new_solve() -> void
{
  // Generation bump rather than a memset: invalidation is O(1), so a caller
  // solving many small deals in a row does not pay a 96 MB clear each time.
  // Entries from the previous solve stay in memory but can never match,
  // because lookup() and add() both require generation == generation_.
  if (generation_ == UINT32_MAX)
    wipe();
  else
    generation_++;

  reset_stats();
}


auto NilTransTable::reset_stats() -> void
{
  probes_ = 0;
  hits_ = 0;
  stores_ = 0;
  evictions_ = 0;
}


auto NilTransTable::mix64(std::uint64_t x) -> std::uint64_t
{
  // MurmurHash3 finaliser. Fixed arithmetic with no seeding from the
  // environment, so the same position hashes to the same slot in every
  // process and every run - which is what makes a failing search
  // reproducible from a deal alone.
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}


auto NilTransTable::hash_of(
  const unsigned short rank_in_suit[DDS_HANDS][DDS_SUITS],
  const int leader,
  const bool nil_broken) -> std::uint64_t
{
  // Four suits of 13 significant bits each fit a 64-bit word per hand with
  // room to spare, so one mix per hand covers the whole position.
  std::uint64_t h = 0x9e3779b97f4a7c15ULL;

  for (int hd = 0; hd < DDS_HANDS; hd++)
  {
    std::uint64_t v = 0;
    for (int s = 0; s < DDS_SUITS; s++)
      v = (v << 16) | static_cast<std::uint64_t>(rank_in_suit[hd][s]);
    h = mix64(h ^ v);
  }

  h = mix64(h
    ^ (static_cast<std::uint64_t>(leader & 3) << 1)
    ^ (nil_broken ? 1ULL : 0ULL));

  return h;
}


auto NilTransTable::same_position(
  const Entry& e,
  const unsigned short rank_in_suit[DDS_HANDS][DDS_SUITS],
  const int leader,
  const bool nil_broken) -> bool
{
  // Full comparison, not a hash-equality shortcut. A Zobrist-style key would
  // be smaller and faster, but a key collision in a solver is a silently
  // wrong answer rather than a slow one, and this table has to be checkable
  // against the brute-force oracle.
  if (e.leader != static_cast<unsigned char>(leader & 3))
    return false;
  if (e.broken != (nil_broken ? 1u : 0u))
    return false;

  for (int hd = 0; hd < DDS_HANDS; hd++)
    for (int s = 0; s < DDS_SUITS; s++)
      if (e.rank_in_suit[hd][s] != rank_in_suit[hd][s])
        return false;

  return true;
}


auto NilTransTable::lookup(
  const unsigned short rank_in_suit[DDS_HANDS][DDS_SUITS],
  const int leader,
  const bool nil_broken) -> NilNodeCards const *
{
  probes_++;

  const std::size_t index =
    static_cast<std::size_t>(hash_of(rank_in_suit, leader, nil_broken)) & mask_;

  Entry& e = table_[index];

  if (e.generation != generation_)
    return nullptr;

  if (! same_position(e, rank_in_suit, leader, nil_broken))
    return nullptr;

  hits_++;
  return &e.payload;
}


auto NilTransTable::add(
  const unsigned short rank_in_suit[DDS_HANDS][DDS_SUITS],
  const int leader,
  const bool nil_broken,
  const int tricks_left,
  const NilNodeCards& payload) -> void
{
  const std::size_t index =
    static_cast<std::size_t>(hash_of(rank_in_suit, leader, nil_broken)) & mask_;

  Entry& e = table_[index];

  const bool live = (e.generation == generation_);

  if (live && same_position(e, rank_in_suit, leader, nil_broken))
  {
    // Same position seen again under a different guess. The two results are
    // both sound bounds on the same G, so intersect rather than overwrite.
    if (payload.lower_bound > e.payload.lower_bound)
      e.payload.lower_bound = payload.lower_bound;
    if (payload.upper_bound < e.payload.upper_bound)
      e.payload.upper_bound = payload.upper_bound;

    // Keep whichever win_ranks is wider. A cutoff records only the branch
    // that cut, so the set stored under one guess can be narrower than the
    // set another guess would produce. Narrow is not unsound - win_ranks
    // drives move ordering here, not the value - but wider orders better.
    for (int s = 0; s < DDS_SUITS; s++)
      e.payload.win_ranks[s] = static_cast<unsigned short>(
        e.payload.win_ranks[s] | payload.win_ranks[s]);

    if (payload.best_move_rank != 0)
    {
      e.payload.best_move_suit = payload.best_move_suit;
      e.payload.best_move_rank = payload.best_move_rank;
    }

    stores_++;
    return;
  }

  // Depth-preferred replacement: an entry standing for a deeper subtree cost
  // more to produce and is worth more to keep. Ties go to the newcomer, which
  // keeps the table from ossifying around one early branch.
  if (live && e.tricks_left > static_cast<unsigned char>(tricks_left))
    return;

  if (live)
    evictions_++;

  for (int hd = 0; hd < DDS_HANDS; hd++)
    for (int s = 0; s < DDS_SUITS; s++)
      e.rank_in_suit[hd][s] = rank_in_suit[hd][s];

  e.payload = payload;
  e.generation = generation_;
  e.leader = static_cast<unsigned char>(leader & 3);
  e.broken = (nil_broken ? 1u : 0u);
  e.tricks_left = static_cast<unsigned char>(tricks_left);
  e.pad = 0;

  stores_++;
}


auto nil_trans_table() -> NilTransTable&
{
  // Function-local thread_local: one per solver thread, constructed on first
  // use, destroyed at thread exit. Nothing in SolverContext changes.
  static thread_local NilTransTable instance;
  return instance;
}
