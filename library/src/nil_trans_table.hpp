/*
   Spades DDS - nil-mode transposition table.

   A standalone table for the nil objective. It shares no code, no memory and
   no types with TransTable / TransTableS / TransTableL, which stay exactly as
   they are. Nothing in ab_search.cpp, quick_tricks.cpp, later_tricks.cpp or
   trans_table/ is touched by this file's existence.

   WHY A SEPARATE TABLE

   NodeCards is 8 bytes with `char` bounds sized for 0..13, because the value
   it caches is one side's trick count. The nil value V is a lexicographic
   triple packed into 0..391 (see nil_objective.hpp), so the bounds need 16
   bits. Widening NodeCards would change the footprint and cache behaviour of
   the max/min solver's table, which is the one thing this must not do.

   SOUNDNESS - WHY A TABLE IS LEGAL FOR THIS OBJECTIVE AT ALL

   nil_search.cpp's header comment says the existing table is unsound here.
   That is true of the existing table, whose entries are bounds on a SIDE's
   trick count. It is not true of the idea. V decomposes:

     V = P + R*direct(c0 + dc) + direct(n0 + dn)

   with (n0, c0) the counts already banked on the path to this node and
   (dn, dc) the counts still to come. With m = 1 that is

     V = [R*c0 + n0]        + [P + R*dc + dn]
         ^ path constant      ^ depends only on the subtree

   and with m = 0 the same split falls out with the bracket subtracted rather
   than added. Either way V splits into a path constant plus a quantity that
   depends only on the position below - GIVEN the nilBroken bit.

   That bit is the one genuine path dependence, and it changes the game below
   it: while the nil is intact the primary term dominates every other term
   combined (R*R > R*T + T), so the nil side plays to hold dn at zero; once
   broken the primary term is dead and the subtree is a plain (dc, dn)
   optimisation. So nilBroken goes in the key. This is the same shape as
   trumpAlreadyBroken in ab_search.cpp: monotone, permanent, and load-bearing.

   NORMALISED VALUE G

   Entries store bounds on G, defined as the value V would take at this
   position if the node had been reached with n0 = c0 = 0. Then

     V* = G + delta,  delta = K - K0

   where K is the node's path constant and K0 is K evaluated at zero counts.
   The caller computes delta (it is cheap and needs the search state), shifts
   its guess into normalised space, and compares against the stored bounds.
   Because the normalisation is exact rather than node-type-relative, none of
   the MAXNODE / MINNODE branching that ab_search.cpp needs around `limit`
   appears here.

   WHAT THIS TABLE DOES NOT DO

   TransTableL/S match one entry against many positions using the
   least_win / win_ranks rank abstraction ("cards below the lowest card that
   actually won anything are interchangeable"). That abstraction is sound
   here too - it is a statement about trick mechanics, not about who wanted to
   win - but it is also the intricate half of those files. This table matches
   positions exactly instead. Exact matching still collects the large majority
   of transpositions, which come from move-order permutations reaching
   identical card positions, and it is straightforward to validate against the
   oracle. Adding the abstraction on top is a later, separate change.

   THREADING

   One table per thread, same contract as TransTable ("not thread-safe, must
   be accessed from a single solver thread"). Held in a function-local
   thread_local rather than on SolverContext so that solver_context.hpp,
   solver_context.cpp and solver_context_adapter.cpp are all untouched.
*/

#pragma once

#include <cstdint>
#include <vector>

#include <api/dll.h>


/// \brief Sentinel standing in for "no bound known on this side".
///
/// Wider than any reachable G (0..391 for a 13-trick deal) and than any
/// normalised guess, which can overshoot that range in either direction by up
/// to R*T + T once delta is subtracted.
constexpr short NIL_TT_INF = 30000;


/// \brief Cached search result for one nil-mode position.
///
/// Bounds are on the normalised value G, not on V and not on a trick count.
/// win_ranks is stored verbatim rather than encoded through least_win,
/// because this table matches positions exactly and so has no abstraction to
/// reconstruct.
struct NilNodeCards // 14 bytes
{
  short lower_bound;                     ///< G >= this. -NIL_TT_INF if unknown.
  short upper_bound;                     ///< G <= this. +NIL_TT_INF if unknown.
  unsigned short win_ranks[DDS_SUITS];   ///< Exact win_ranks produced when searched.
  unsigned char best_move_suit;          ///< Move ordering hint. Rank 0 = none.
  unsigned char best_move_rank;
};


/// \brief Exact-match transposition table for the nil objective.
class NilTransTable
{
  public:

    NilTransTable();

    /// \brief Size the table. Rounded down to a power of two of slots.
    ///
    /// Safe to call at any time; it discards the current contents.
    auto set_memory(int megabytes) -> void;

    /// \brief Invalidate every entry in O(1) and start a new solve.
    ///
    /// Call once per solve, before the driver's first probe - NOT once per
    /// driver stage and NOT once per root move. The staged binary search runs
    /// ~10 probes over the same deal and sibling root moves share most of
    /// their subtrees; that reuse is the entire point of the table.
    auto new_solve() -> void;

    /// \brief Look up a position. Returns nullptr on a miss.
    ///
    /// The returned pointer is invalidated by the next add() or new_solve().
    auto lookup(
      const unsigned short rank_in_suit[DDS_HANDS][DDS_SUITS],
      int leader,
      bool nil_broken) -> NilNodeCards const *;

    /// \brief Store or tighten a result.
    ///
    /// If a live entry for the same position is present its bounds are
    /// intersected with the new ones; otherwise the slot is claimed, evicting
    /// whatever was there if the incumbent represents less work.
    ///
    /// \param tricks_left  Used only for the replacement policy.
    auto add(
      const unsigned short rank_in_suit[DDS_HANDS][DDS_SUITS],
      int leader,
      bool nil_broken,
      int tricks_left,
      const NilNodeCards& payload) -> void;

    // --- Instrumentation ---------------------------------------------------
    // Counters are plain reads, cost nothing when unused, and answer the
    // "where is the time actually going" question per driver stage.

    auto probes() const -> std::uint64_t { return probes_; }
    auto hits() const -> std::uint64_t { return hits_; }
    auto stores() const -> std::uint64_t { return stores_; }
    auto evictions() const -> std::uint64_t { return evictions_; }
    auto reset_stats() -> void;

  private:

    struct Entry
    {
      unsigned short rank_in_suit[DDS_HANDS][DDS_SUITS];
      NilNodeCards payload;
      std::uint32_t generation;
      unsigned char leader;
      unsigned char broken;
      unsigned char tricks_left;
      unsigned char pad;
    };

    static auto mix64(std::uint64_t x) -> std::uint64_t;

    static auto hash_of(
      const unsigned short rank_in_suit[DDS_HANDS][DDS_SUITS],
      int leader,
      bool nil_broken) -> std::uint64_t;

    static auto same_position(
      const Entry& e,
      const unsigned short rank_in_suit[DDS_HANDS][DDS_SUITS],
      int leader,
      bool nil_broken) -> bool;

    auto wipe() -> void;

    std::vector<Entry> table_;
    std::size_t mask_ = 0;
    std::uint32_t generation_ = 1;

    std::uint64_t probes_ = 0;
    std::uint64_t hits_ = 0;
    std::uint64_t stores_ = 0;
    std::uint64_t evictions_ = 0;
};


/// \brief The calling thread's nil table.
auto nil_trans_table() -> NilTransTable&;
