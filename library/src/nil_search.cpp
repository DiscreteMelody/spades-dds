/*
   Spades DDS - nil-mode search.

   Structure mirrors ab_search.cpp at commit 138b9cc: four hand-position
   specialisations plus a terminal evaluator. The move loops, win_ranks
   propagation and cutoff bookkeeping are intentionally the same shape, so a
   diff against the original stays readable.

   Deliberate differences, all of them load-bearing:

   1. The threshold cutoffs at ab_search.cpp:310/315 compare tricks_max against
      target. Here they compare bounds on V against guess.
   2. Trick crediting is by exact seat identity, not by is_reference_hand().
   3. No transposition table and no estimators - Phase 2 runs unpruned apart
      from alpha-beta itself. See docs/nil-mode-map.md §5 and §6 for why the
      existing ones are unsound against this objective.
   4. Node polarity is fixed at the root by nilSeat and never consults
      misereOn.
*/

#include <cassert>

#include "nil_search.hpp"
#include "nil_objective.hpp"
#include "nil_trans_table.hpp"
#include "ab_search.hpp"
#include <lookup_tables/lookup_tables.hpp>
#include <solver_context/solver_context.hpp>

// undo_1 / undo_2 / undo_3 have external linkage in ab_search.cpp but are not
// declared in ab_search.hpp - only inside the .cpp. Declared here rather than
// added to the shared header, so nothing outside this module changes.
void undo_1(Pos* posPoint, const int depth, const MoveType& mply);
void undo_2(Pos* posPoint, const int depth, const MoveType& mply);
void undo_3(Pos* posPoint, const int depth, const MoveType& mply);

static bool nil_search_0(Pos* posPoint, int guess, int depth, SolverContext& ctx);
static bool nil_search_1(Pos* posPoint, int guess, int depth, SolverContext& ctx);
static bool nil_search_2(Pos* posPoint, int guess, int depth, SolverContext& ctx);
static bool nil_search_3(Pos* posPoint, int guess, int depth, SolverContext& ctx);

// ab_search.cpp's handDelta is `const` at namespace scope, which gives it
// internal linkage in C++ - it is not reachable from another translation unit.
// Redeclared here rather than exported, so ab_search.cpp stays untouched.
// The values encode hand_dist's per-suit stride and must match exactly.
static const int handDelta_nil[DDS_SUITS] = { 256, 16, 1, 0 };

// Local mirror of undo_0_ctx, which is static in ab_search.cpp. Identical
// behaviour; only reachability differs. Twelve lines is a cheaper price than
// widening ab_search.cpp's interface, which would mean touching a shared file.
static void nil_undo_0(
  Pos* posPoint,
  const int depth,
  const MoveType& mply,
  SolverContext& ctx)
{
  int h = HAND_ID(posPoint->first[depth], 3);
  int s = mply.suit;
  int r = mply.rank;

  posPoint->rank_in_suit[h][s] |= bit_map_rank[r];
  posPoint->aggr[s] |= bit_map_rank[r];
  posPoint->hand_dist[h] += handDelta_nil[s];
  posPoint->length[h][s]++;

  WinnersType const* wp = &ctx.search().winners((depth + 3) >> 2);

  for (int n = 0; n < wp->number; n++)
  {
    int st = wp->winner[n].suit;
    posPoint->winner[st].rank = wp->winner[n].winnerRank;
    posPoint->winner[st].hand = wp->winner[n].winnerHand;
    posPoint->second_best[st].rank = wp->winner[n].secondRank;
    posPoint->second_best[st].hand = wp->winner[n].secondHand;
  }
}


// ---------------------------------------------------------------------------
// Bounds
// ---------------------------------------------------------------------------

/// Assemble the running per-seat state at a node.
///
/// tricksLeft counts tricks still to be resolved at or below this node. In
/// ab_search_0_ctx's arithmetic, `tricks = depth >> 2` and the position has
/// `tricks + 1` tricks remaining - which is exactly why line 315 of the
/// original reads `tricks_max + tricks + 1 < target`. The same +1 applies here.
static auto nil_counts(
  const Pos* posPoint,
  const int tricksLeft,
  SolverContext& ctx) -> nil_mode::NodeCounts
{
  nil_mode::NodeCounts c{};
  c.nilTricks = ctx.search().nil_tricks();
  c.coverTricks = ctx.search().nil_cover_tricks();
  c.tricksLeft = tricksLeft;
  c.tricksTotal = ctx.search().nil_tricks_total();
  c.nilBroken = ctx.search().nil_already_broken() || (ctx.search().nil_tricks() > 0);
  c.m = ctx.search().nil_direction();
  return c;
}


/// The node's path constant, measured from the value the same position would
/// have at zero running counts.
///
/// V* = G + nil_path_offset(), where G is what the transposition table stores.
/// Derivation is in nil_trans_table.hpp; the short version is that V splits
/// into a term fixed by the tricks already banked on this path and a term
/// fixed by the position below, so subtracting the former leaves something a
/// table can key on.
///
/// K0 is K at n0 = c0 = 0. With m = 1 that is 0; with m = 0 both complements
/// sit at their maximum, giving R*T + T.
static auto nil_path_offset(SolverContext& ctx) -> int
{
  const int T = ctx.search().nil_tricks_total();
  const int R = nil_mode::radix(T);
  const bool m = ctx.search().nil_direction();

  const int n0 = ctx.search().nil_tricks();
  const int c0 = ctx.search().nil_cover_tricks();

  const int K =
    R * nil_mode::direct(c0, T, m) + nil_mode::direct(n0, T, m);
  const int K0 =
    R * nil_mode::direct(0, T, m) + nil_mode::direct(0, T, m);

  return K - K0;
}


// ---------------------------------------------------------------------------
// Terminal evaluation
// ---------------------------------------------------------------------------

auto nil_evaluate(
  const Pos* posPoint,
  const int trump,
  SolverContext& ctx,
  unsigned short winRanksOut[DDS_SUITS]) -> int
{
  int s, h, hmax = 0, count = 0, k = 0;
  unsigned short rmax = 0;

  int firstHand = posPoint->first[0];
  assert((firstHand >= 0) && (firstHand <= 3));

  for (s = 0; s < DDS_SUITS; s++)
    winRanksOut[s] = 0;

  bool resolved = false;

  // Who wins the last trick? Highest trump, else highest card of the led suit.
  if (trump != DDS_NOTRUMP)
  {
    for (h = 0; h < DDS_HANDS; h++)
    {
      if (posPoint->rank_in_suit[h][trump] != 0)
        count++;
      if (posPoint->rank_in_suit[h][trump] > rmax)
      {
        hmax = h;
        rmax = posPoint->rank_in_suit[h][trump];
      }
    }

    if (rmax > 0)
    {
      if (count >= 2)
        winRanksOut[trump] = rmax;
      resolved = true;
    }
  }

  if (!resolved)
  {
    k = 0;
    while (k <= 3)
    {
      if (posPoint->rank_in_suit[firstHand][k] != 0)
        break;
      k++;
    }

    assert(k < 4);

    for (h = 0; h < DDS_HANDS; h++)
    {
      if (posPoint->rank_in_suit[h][k] != 0)
        count++;
      if (posPoint->rank_in_suit[h][k] > rmax)
      {
        hmax = h;
        rmax = posPoint->rank_in_suit[h][k];
      }
    }

    if (count >= 2)
      winRanksOut[k] = rmax;
  }

  // The one structural departure from evaluate_with_context(): that function
  // folds the last trick into its return value as `tricks_max + 1` and never
  // writes it back to Pos, which works only because a side's total is a single
  // int. Here the trick has to be attributed to a SEAT, so it is credited
  // locally and packed here, where hmax is still in scope.
  //
  // win_ranks is produced here too rather than by a second call to
  // evaluate_with_context(). Computing the trick winner twice would leave two
  // independent answers that could silently disagree, and move ordering would
  // then drift away from the values it is supposed to be ordering.
  const int nilTricks = ctx.search().nil_tricks() + (ctx.search().is_nil_seat(hmax) ? 1 : 0);
  const int coverTricks = ctx.search().nil_cover_tricks() + (ctx.search().is_cover_seat(hmax) ? 1 : 0);
  const bool nilBroken = ctx.search().nil_already_broken() || (nilTricks > 0);

  return nil_mode::pack(
    nilTricks,
    coverTricks,
    ctx.search().nil_tricks_total(),
    nilBroken,
    ctx.search().nil_direction());
}


// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

auto nil_ab_search(
  Pos* posPoint,
  const int guess,
  const int depth,
  const int hand_rel_first,
  SolverContext& ctx) -> bool
{
  // Mirrors the engine's AB_ptr_list[hand_rel_first] dispatch at
  // solver_if.cpp:467. Entering the wrong specialisation is not a crash - each
  // one generates moves for a different seat relative to the leader, so the
  // search would explore a coherent but wrong game. Deriving this from depth
  // alone would work at present (hand_rel_first == (48 - depth) % 4) but bakes
  // in the 48-card anchor, so it is passed explicitly instead.
  switch (hand_rel_first & 3)
  {
    case 0:  return nil_search_0(posPoint, guess, depth, ctx);
    case 1:  return nil_search_1(posPoint, guess, depth, ctx);
    case 2:  return nil_search_2(posPoint, guess, depth, ctx);
    default: return nil_search_3(posPoint, guess, depth, ctx);
  }
}


static bool nil_search_0(
  Pos* posPoint,
  const int guess,
  const int depth,
  SolverContext& ctx)
{
  ThreadData* thrp = ctx.thread_ptr();
  int trump = thrp->trump;
  int hand = posPoint->first[depth];
  int tricks = depth >> 2;

#ifdef DDS_TOP_LEVEL
  ctx.search().nodes()++;
#endif

  for (int ss = 0; ss < DDS_SUITS; ss++)
    posPoint->win_ranks[depth][ss] = 0;

  // Generalisation of ab_search.cpp:310/315. There, tricks_max is a guaranteed
  // floor and tricks_max + tricks + 1 a ceiling on the final count. Here the
  // same two roles are played by bounds on V. Loose bounds are sound; they cost
  // nodes, never correctness.
  const nil_mode::NodeCounts counts = nil_counts(posPoint, tricks + 1, ctx);

  if (nil_mode::lower_bound(counts) >= guess)
    return true;
  if (nil_mode::upper_bound(counts) < guess)
    return false;

  if (depth == 0)
  {
    const int value = nil_evaluate(posPoint, trump, ctx,
      posPoint->win_ranks[depth]);
    return value >= guess;
  }

  // -------------------------------------------------------------------------
  // Transposition table.
  //
  // Probed after the O(1) arithmetic cutoffs above, which are cheaper than a
  // hash and a 32-byte compare. Skipped on the last trick, where an entry
  // costs more to store than the subtree costs to search.
  //
  // Placement is safe at every depth here because the root driver in
  // nil_if.cpp evaluates each root card as an independent sub-search with no
  // forbidden-move set - so unlike solver_if.cpp's candidate loop, no node
  // below the root is ever searched under a restricted move set. An entry
  // therefore always describes the full game below the position.
  // -------------------------------------------------------------------------
  const bool ttUsable = (tricks >= 1);
  const int guessNorm = ttUsable ? (guess - nil_path_offset(ctx)) : 0;

  if (ttUsable)
  {
    NilNodeCards const* cardsP = nil_trans_table().lookup(
      posPoint->rank_in_suit, hand, counts.nilBroken);

    if (cardsP)
    {
      const bool resolvesTrue = (cardsP->lower_bound >= guessNorm);
      const bool resolvesFalse = (cardsP->upper_bound < guessNorm);

      if (resolvesTrue || resolvesFalse)
      {
        // win_ranks is restored from the entry for the same reason
        // ab_search.cpp:290 restores it on a hit: the parent ORs this in and
        // feeds it to make_next, so returning without setting it would hand
        // the parent an empty set and suppress its later moves.
        for (int ss = 0; ss < DDS_SUITS; ss++)
          posPoint->win_ranks[depth][ss] = cardsP->win_ranks[ss];

        return resolvesTrue;
      }

      // Bounds too loose for this guess, but the stored move is still the
      // best one found here under a neighbouring guess. Seeding best_move_tt
      // is what makes move_gen_0 try it first. Nothing in nil mode wrote this
      // channel before, so move_gen_0's tt-move argument was inert.
      if (cardsP->best_move_rank != 0)
      {
        ctx.search().best_move_tt(depth).suit = cardsP->best_move_suit;
        ctx.search().best_move_tt(depth).rank = cardsP->best_move_rank;
      }
    }
  }

  bool success = (ctx.search().node_type_store(hand) == MAXNODE ? true : false);
  bool value = !success;

  for (int ss = 0; ss < DDS_SUITS; ss++)
    ctx.search().lowest_win(depth, ss) = 0;

  ctx.move_gen().move_gen_0(
    tricks,
    *posPoint,
    ctx.search().best_move(depth),
    ctx.search().best_move_tt(depth),
    thrp->rel);

  for (int ss = 0; ss < DDS_SUITS; ss++)
    posPoint->win_ranks[depth][ss] = 0;

  while (1)
  {
    MoveType const* mply = ctx.move_gen().make_next(tricks, 0,
      posPoint->win_ranks[depth]);

    if (mply == NULL)
      break;

    make_0(posPoint, depth, mply);

    value = nil_search_1(posPoint, guess, depth - 1, ctx);

    undo_1(posPoint, depth, *mply);

    if (value == success) /* A cut-off? */
    {
      for (int ss = 0; ss < DDS_SUITS; ss++)
        posPoint->win_ranks[depth][ss] =
          posPoint->win_ranks[depth - 1][ss];

      ctx.search().best_move(depth) = *mply;
      goto ABexit;
    }

    for (int ss = 0; ss < DDS_SUITS; ss++)
      posPoint->win_ranks[depth][ss] |= posPoint->win_ranks[depth - 1][ss];
  }

ABexit:
  if (ttUsable)
  {
    // A null-window result gives one bound, not a value. `true` means
    // G >= guessNorm and says nothing about how much higher; `false` means
    // G <= guessNorm - 1. The opposite side is left open, and add()
    // intersects with whatever a previous guess established.
    //
    // counts.nilBroken and guessNorm were both computed on entry to this
    // node. The child calls in the loop above adjust nil_tricks() /
    // nil_cover_tricks() but restore them before returning, so both are
    // still the values that describe this position.
    NilNodeCards payload;

    payload.lower_bound = value
      ? static_cast<short>(guessNorm)
      : static_cast<short>(-NIL_TT_INF);
    payload.upper_bound = value
      ? static_cast<short>(NIL_TT_INF)
      : static_cast<short>(guessNorm - 1);

    for (int ss = 0; ss < DDS_SUITS; ss++)
      payload.win_ranks[ss] = posPoint->win_ranks[depth][ss];

    payload.best_move_suit =
      static_cast<unsigned char>(ctx.search().best_move(depth).suit);
    payload.best_move_rank =
      static_cast<unsigned char>(ctx.search().best_move(depth).rank);

    nil_trans_table().add(
      posPoint->rank_in_suit, hand, counts.nilBroken, tricks + 1, payload);
  }

  return value;
}


static bool nil_search_1(
  Pos* posPoint,
  const int guess,
  const int depth,
  SolverContext& ctx)
{
  int hand = HAND_ID(posPoint->first[depth], 1);
  bool success = (ctx.search().node_type_store(hand) == MAXNODE ? true : false);
  bool value = !success;
  int tricks = (depth + 3) >> 2;

#ifdef DDS_TOP_LEVEL
  ctx.search().nodes()++;
#endif

  // QuickTricksSecondHand is called at this point in ab_search_1_ctx. It is
  // omitted rather than gated: it bounds a SIDE's trick total and cannot
  // attribute a trick to a seat, so it says nothing about V's secondary or
  // tertiary terms. Phase 7 revisits this.

  for (int ss = 0; ss < DDS_SUITS; ss++)
    ctx.search().lowest_win(depth, ss) = 0;

  ctx.move_gen().move_gen_123(tricks, 1, *posPoint);
  if (depth == ctx.search().ini_depth())
    ctx.move_gen().purge(tricks, 1, ctx.search().forbidden_moves());

  for (int ss = 0; ss < DDS_SUITS; ss++)
    posPoint->win_ranks[depth][ss] = 0;

  while (1)
  {
    MoveType const* mply = ctx.move_gen().make_next(tricks, 1,
      posPoint->win_ranks[depth]);

    if (mply == NULL)
      break;

    make_1(posPoint, depth, mply);

    value = nil_search_2(posPoint, guess, depth - 1, ctx);

    undo_2(posPoint, depth, *mply);

    if (value == success) /* A cut-off? */
    {
      for (int ss = 0; ss < DDS_SUITS; ss++)
        posPoint->win_ranks[depth][ss] = posPoint->win_ranks[depth - 1][ss];

      ctx.search().best_move(depth) = *mply;
      goto ABexit;
    }

    for (int ss = 0; ss < DDS_SUITS; ss++)
      posPoint->win_ranks[depth][ss] |= posPoint->win_ranks[depth - 1][ss];
  }

ABexit:
  return value;
}


static bool nil_search_2(
  Pos* posPoint,
  const int guess,
  const int depth,
  SolverContext& ctx)
{
  int hand = HAND_ID(posPoint->first[depth], 2);
  bool success = (ctx.search().node_type_store(hand) == MAXNODE ? true : false);
  bool value = !success;
  int tricks = (depth + 3) >> 2;

#ifdef DDS_TOP_LEVEL
  ctx.search().nodes()++;
#endif

  for (int ss = 0; ss < DDS_SUITS; ss++)
    ctx.search().lowest_win(depth, ss) = 0;

  ctx.move_gen().move_gen_123(tricks, 2, *posPoint);
  if (depth == ctx.search().ini_depth())
    ctx.move_gen().purge(tricks, 2, ctx.search().forbidden_moves());

  for (int ss = 0; ss < DDS_SUITS; ss++)
    posPoint->win_ranks[depth][ss] = 0;

  while (1)
  {
    MoveType const* mply = ctx.move_gen().make_next(tricks, 2,
      posPoint->win_ranks[depth]);

    if (mply == NULL)
      break;

    make_2(posPoint, depth, mply);

    value = nil_search_3(posPoint, guess, depth - 1, ctx);

    undo_3(posPoint, depth, *mply);

    if (value == success) /* A cut-off? */
    {
      for (int ss = 0; ss < DDS_SUITS; ss++)
        posPoint->win_ranks[depth][ss] = posPoint->win_ranks[depth - 1][ss];

      ctx.search().best_move(depth) = *mply;
      goto ABexit;
    }

    for (int ss = 0; ss < DDS_SUITS; ss++)
      posPoint->win_ranks[depth][ss] |= posPoint->win_ranks[depth - 1][ss];
  }

ABexit:
  return value;
}


static bool nil_search_3(
  Pos* posPoint,
  const int guess,
  const int depth,
  SolverContext& ctx)
{
  /* Trick completes here, so this is where tricks are credited. */

  unsigned short int makeWinRank[DDS_SUITS];

  int hand = HAND_ID(posPoint->first[depth], 3);
  bool success = (ctx.search().node_type_store(hand) == MAXNODE ? true : false);
  bool value = !success;

#ifdef DDS_TOP_LEVEL
  ctx.search().nodes()++;
#endif

  for (int ss = 0; ss < DDS_SUITS; ss++)
    ctx.search().lowest_win(depth, ss) = 0;
  int tricks = (depth + 3) >> 2;

  ctx.move_gen().move_gen_123(tricks, 3, *posPoint);
  if (depth == ctx.search().ini_depth())
    ctx.move_gen().purge(tricks, 3, ctx.search().forbidden_moves());

  for (int ss = 0; ss < DDS_SUITS; ss++)
    posPoint->win_ranks[depth][ss] = 0;

  while (1)
  {
    MoveType const* mply = ctx.move_gen().make_next(tricks, 3,
      posPoint->win_ranks[depth]);

    if (mply == NULL)
      break;

    make_3(posPoint, makeWinRank, depth, mply, ctx);

    ctx.search().trick_nodes()++;

    // Exact-identity crediting. ab_search_3_ctx:809 uses is_reference_hand()
    // here, which answers a partnership question; the nil objective needs the
    // seat. posPoint->first[depth - 1] is the winner of the trick just
    // completed, already set by make_3.
    const int winner = posPoint->first[depth - 1];
    if (ctx.search().is_nil_seat(winner))
      ctx.search().nil_tricks()++;
    else if (ctx.search().is_cover_seat(winner))
      ctx.search().nil_cover_tricks()++;

    value = nil_search_0(posPoint, guess, depth - 1, ctx);

    nil_undo_0(posPoint, depth, *mply, ctx);

    if (ctx.search().is_nil_seat(winner))
      ctx.search().nil_tricks()--;
    else if (ctx.search().is_cover_seat(winner))
      ctx.search().nil_cover_tricks()--;

    if (value == success) /* A cut-off? */
    {
      for (int ss = 0; ss < DDS_SUITS; ss++)
        posPoint->win_ranks[depth][ss] = static_cast<unsigned short>(
          posPoint->win_ranks[depth - 1][ss] | makeWinRank[ss]);

      ctx.search().best_move(depth) = *mply;
      goto ABexit;
    }

    for (int ss = 0; ss < DDS_SUITS; ss++)
      posPoint->win_ranks[depth][ss] |=
        posPoint->win_ranks[depth - 1][ss] | makeWinRank[ss];
  }

ABexit:
  return value;
}
