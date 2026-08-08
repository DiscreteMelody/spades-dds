/*
   Spades DDS - nil-mode objective.

   Packing, decoding and bounding for the nil-mode search value V.

   V is a single non-negative integer with three strictly lexicographic terms:

     1. primary   - whether the nil is made
     2. secondary - the COVER seat's trick count alone
     3. tertiary  - the NIL seat's own trick count

   The nil side maximises V; the opposing side minimises it. That assignment is
   unconditional - it does NOT depend on the direction flag m. The direction
   lives entirely in the sC / sN complements below. See docs/nil-mode-map.md §4
   and the kickoff document §3 for why reusing misereOn's global polarity flip
   here would be wrong.

   This header is self-contained and pulls in nothing from the search, so it can
   be unit-tested on its own.
*/

#pragma once

namespace nil_mode
{

/// Maximum tricks in a Spades deal; V's range is derived from the actual T of
/// the position, not from this, but callers sizing buffers can use it.
constexpr int MAX_TRICKS = 13;

/// \brief Partner of a DDS hand index. N=0/E=1/S=2/W=3, so partners differ by 2.
constexpr auto partner_of(const int hand) -> int { return hand ^ 2; }

/// \brief True when \p hand is on the same side as \p nilSeat.
///
/// Parity is an exact partnership test under the DDS seat numbering. What must
/// never happen is anchoring this on the seat to play instead of on nilSeat.
constexpr auto is_nil_side(const int hand, const int nilSeat) -> bool
{
  return ((hand & 1) == (nilSeat & 1));
}

/// \brief The multiplier for the secondary term, and the base of the packing.
constexpr auto radix(const int tricksTotal) -> int { return tricksTotal + 1; }

/// \brief Largest value V can take for a deal of \p tricksTotal tricks.
///
/// Used to seed the top-level search range. For T = 13 this is 391.
constexpr auto max_value(const int tricksTotal) -> int
{
  const int R = radix(tricksTotal);
  return R * R + R * tricksTotal + tricksTotal;
}

/// \brief Clamp helper - the bound arithmetic below can overshoot either end.
constexpr auto clamp(const int v, const int lo, const int hi) -> int
{
  return (v < lo) ? lo : ((v > hi) ? hi : v);
}

/// \brief Apply the direction flag to a raw seat trick count.
///
/// With m = 1 the side prefers more of its own tricks; with m = 0 it prefers
/// fewer, and the complement makes "maximise V" express that without touching
/// node polarity.
constexpr auto direct(const int count, const int tricksTotal, const bool m) -> int
{
  return m ? count : (tricksTotal - count);
}

/// \brief Pack a terminal position into V.
///
/// \param nilTricks    tricks won by the nil seat from the search root onward
/// \param coverTricks  tricks won by the cover seat from the search root onward
/// \param tricksTotal  T - tricks in the whole search, including any in-progress trick
/// \param nilBroken    the nil seat has won a trick, either before the root
///                     (nilAlreadyBroken) or during the search
/// \param m            direction flag
constexpr auto pack(
  const int nilTricks,
  const int coverTricks,
  const int tricksTotal,
  const bool nilBroken,
  const bool m) -> int
{
  const int R = radix(tricksTotal);
  const int sC = direct(coverTricks, tricksTotal, m);
#ifdef NIL_DROP_TERTIARY
  (void)nilTricks;
  const int sN = 0;
#else
  const int sN = direct(nilTricks, tricksTotal, m);
#endif
  return (nilBroken ? 0 : R * R) + R * sC + sN;
}

/// \brief Decoded per-seat split of a packed V.
struct Decoded
{
  bool nilMade;
  int nilTricks;    ///< n
  int coverTricks;  ///< c
  int oppTricks;    ///< o, the two opponents combined
};

/// \brief Invert pack(). Exact - V carries the whole split with no side channel.
constexpr auto decode(const int value, const int tricksTotal, const bool m) -> Decoded
{
  const int R = radix(tricksTotal);
  const int sC = (value / R) % R;
  const int sN = value % R;

  Decoded d{};
  d.nilMade = (value / (R * R)) != 0;
  d.coverTricks = direct(sC, tricksTotal, m);
  d.nilTricks = direct(sN, tricksTotal, m);
  d.oppTricks = tricksTotal - d.nilTricks - d.coverTricks;
  return d;
}

/// \brief Running per-seat state at an interior node.
///
/// Everything the bounds need. nilTricks / coverTricks count from the search
/// root, matching what ThreadData::nilTricks / nilCoverTricks accumulate.
struct NodeCounts
{
  int nilTricks;
  int coverTricks;
  int tricksLeft;   ///< tricks still to be played BELOW this node, inclusive of the one being formed
  int tricksTotal;  ///< T
  bool nilBroken;   ///< nilAlreadyBroken || nilTricks > 0
  bool m;
};

/// \brief How the tricks still to be played are shared between the cover seat
///        and the nil seat at a bound's extremum.
struct Split
{
  int cover;
  int nil;
};

/// \brief Greedy allocation of tricksLeft, secondary term first.
///
/// The Phase 2 bounds moved sC and sN independently, which let BOTH seats
/// absorb the whole of tricksLeft. No playout reaches that: the two seats
/// compete for the same tricks, so dC + dN <= tricksLeft.
///
/// Applying the joint constraint means deciding which term gets the tricks.
/// R = T + 1, so one unit of the secondary term outweighs the entire range of
/// the tertiary one - the extremum therefore always feeds the cover seat to
/// capacity and gives the nil seat the remainder. Each seat is separately
/// capped at T minus its own running count, so the results land inside [0, T]
/// on their own rather than relying on the caller's clamp.
///
/// This is a bound tightening only. It removes unreachable states from the
/// bound's range; it never removes a reachable one, so no value the search can
/// return changes.
constexpr auto split_cover_first(const NodeCounts& c) -> Split
{
  const int T = c.tricksTotal;
  const int left = clamp(c.tricksLeft, 0, T);

  const int capC = clamp(T - c.coverTricks, 0, T);
  const int dC = clamp(left, 0, capC);

  const int capN = clamp(T - c.nilTricks, 0, T);
  const int dN = clamp(left - dC, 0, capN);

  return Split{ dC, dN };
}

/// \brief Sound upper bound on V from this node - the most the nil side could
///        conceivably still reach.
///
/// Tightened over Phase 2 by the joint constraint above. The primary term is
/// deliberately left alone: proving that a still-intact nil must break needs
/// real search, not arithmetic.
///
/// m = 1: both seats prefer more tricks, so the maximum hands out the
/// remaining tricks cover-first. m = 0: both prefer fewer, so the maximum sits
/// at dC = dN = 0 and no allocation arises - that branch was already tight.
constexpr auto upper_bound(const NodeCounts& c) -> int
{
  const int T = c.tricksTotal;
  const int R = radix(T);

  const int primary = c.nilBroken ? 0 : (R * R);

  if (!c.m)
    return primary
      + R * clamp(T - c.coverTricks, 0, T)
#ifdef NIL_DROP_TERTIARY
      + 0;
#else
      + clamp(T - c.nilTricks, 0, T);
#endif

  const Split d = split_cover_first(c);

  return primary
    + R * clamp(c.coverTricks + d.cover, 0, T)
#ifdef NIL_DROP_TERTIARY
    + 0;
#else
    + clamp(c.nilTricks + d.nil, 0, T);
#endif
}

/// \brief Loosest sound lower bound on V from this node - the least the nil side
///        could be held to.
///
/// The primary term survives only when the nil is already safe, which below the
/// root means no tricks remain to break it.
/// The direction split mirrors upper_bound's, reversed: m = 1 is already tight
/// at dC = dN = 0, and m = 0 is the branch the joint constraint improves.
constexpr auto lower_bound(const NodeCounts& c) -> int
{
  const int T = c.tricksTotal;
  const int R = radix(T);

  const bool nilStillSafe = (!c.nilBroken && c.tricksLeft <= 0);
  const int primary = nilStillSafe ? (R * R) : 0;

  if (c.m)
    return primary
      + R * clamp(c.coverTricks, 0, T)
#ifdef NIL_DROP_TERTIARY
      + 0;
#else
      + clamp(c.nilTricks, 0, T);
#endif

  const Split d = split_cover_first(c);

  return primary
    + R * clamp(T - (c.coverTricks + d.cover), 0, T)
#ifdef NIL_DROP_TERTIARY
    + 0;
#else
    + clamp(T - (c.nilTricks + d.nil), 0, T);
#endif
}

}  // namespace nil_mode
