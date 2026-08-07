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
  const int sN = direct(nilTricks, tricksTotal, m);
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

/// \brief Loosest sound upper bound on V from this node - the most the nil side
///        could conceivably still reach.
///
/// Each term is maximised independently. That can describe a state no real
/// playout reaches (e.g. nil made AND the nil seat taking every remaining
/// trick), which is fine: an upper bound only has to be >= the true value.
/// Tightening this is a later optimisation, not a correctness matter.
constexpr auto upper_bound(const NodeCounts& c) -> int
{
  const int T = c.tricksTotal;
  const int R = radix(T);

  const int primary = c.nilBroken ? 0 : (R * R);

  // m = 1: cover wants more tricks, so assume it wins everything left.
  // m = 0: cover wants fewer, so assume it wins nothing more.
  const int sC = c.m
    ? clamp(c.coverTricks + c.tricksLeft, 0, T)
    : clamp(T - c.coverTricks, 0, T);

  const int sN = c.m
    ? clamp(c.nilTricks + c.tricksLeft, 0, T)
    : clamp(T - c.nilTricks, 0, T);

  return primary + R * sC + sN;
}

/// \brief Loosest sound lower bound on V from this node - the least the nil side
///        could be held to.
///
/// The primary term survives only when the nil is already safe, which below the
/// root means no tricks remain to break it.
constexpr auto lower_bound(const NodeCounts& c) -> int
{
  const int T = c.tricksTotal;
  const int R = radix(T);

  const bool nilStillSafe = (!c.nilBroken && c.tricksLeft <= 0);
  const int primary = nilStillSafe ? (R * R) : 0;

  const int sC = c.m
    ? clamp(c.coverTricks, 0, T)
    : clamp(T - (c.coverTricks + c.tricksLeft), 0, T);

  const int sN = c.m
    ? clamp(c.nilTricks, 0, T)
    : clamp(T - (c.nilTricks + c.tricksLeft), 0, T);

  return primary + R * sC + sN;
}

}  // namespace nil_mode
