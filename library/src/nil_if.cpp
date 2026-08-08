/*
   Spades DDS - nil-mode entry point.

   Mirrors the setup half of solve_board_internal() at commit 138b9cc. The
   setup is objective-independent almost everywhere - deal counting, position
   init, move-generator init, InitWinners - and is duplicated rather than
   factored out of solver_if.cpp, because extracting a shared helper would
   change that file's object code and forfeit the isolation guarantee this
   module exists to provide.

   Three things differ from the original setup, and only three:

     * scoreParity is anchored on nilSeat, not on handToPlay.
     * Node types are fixed: nil side MAXNODE, opponents MINNODE, with no
       misereOn ternary.
     * The transposition table is neither consulted nor stamped. Nil mode does
       not use it in Phase 2, and set_objective()'s bool has no room for a
       third objective anyway.
*/

#include <cstring>

#include "nil_if.hpp"
#include "nil_trans_table.hpp"
#include "nil_search.hpp"
#include "nil_objective.hpp"
#include "ab_search.hpp"
#include "init.hpp"
#include "pbn.hpp"
#include <lookup_tables/lookup_tables.hpp>
#include <solver_context/solver_context.hpp>
#include <system/memory.hpp>
#include <utility/constants.h>

// External linkage in ab_search.cpp but absent from ab_search.hpp.
void undo_0(
  Pos* posPoint,
  const int depth,
  const MoveType& mply,
  const std::shared_ptr<ThreadData>& thrp);
void undo_1(Pos* posPoint, const int depth, const MoveType& mply);
void undo_2(Pos* posPoint, const int depth, const MoveType& mply);
void undo_3(Pos* posPoint, const int depth, const MoveType& mply);

// Declared, not defined, here - both have external linkage in solver_if.cpp.
// Reusing them keeps validation behaviour identical without touching that file.
auto board_range_checks(
  const Deal& dl,
  const int target,
  const int solutions,
  const int mode) -> int;

auto board_value_checks(
  SolverContext& ctx,
  const Deal& dl,
  const int target,
  const int solutions,
  const int mode) -> int;


/// Resolve V for the position currently on the board, by staged probing.
///
/// nil_ab_search answers a monotone predicate: true iff the nil side can force
/// V >= guess. Any strategy for locating V is therefore just a strategy for
/// placing probes, and none of them can change the answer - only how long it
/// takes to find. The Phase 2 driver scanned down from max_value(T) one step at
/// a time, which is up to 391 full searches per root card at T = 13, and the
/// expensive ones are the ~196 below R*R where the primary term is already
/// settled and the bounds stop cutting.
///
/// V is lexicographic, so the same answer falls out of about nine probes placed
/// on the term boundaries:
///
///   1. one probe at R*R fixes the primary term - nil made or nil set,
///   2. a binary search on the secondary term inside the band that fixes,
///   3. a binary search on the tertiary term inside that.
///
/// This costs about what a flat binary search over 0..max_value(T) would, but
/// the probes land on boundaries the bounds understand, and a caller that only
/// needs the made/set answer can stop after stage 1.
///
/// Exactness rests on R = T + 1 > T. If the true secondary term were sC - 1
/// then R*(sC-1) + sN <= R*sC - R + T < R*sC, so a probe at base + R*sC fails -
/// the tertiary term can never bridge a secondary step. The same argument
/// separates the primary term, since R*T + T = R*R - 1.
///
/// Neither stage probes its own zero. V >= 0 always holds, and for base = R*R
/// stage 1 has already proved V >= base, so those probes are known-true and
/// skipping them costs nothing.
///
/// \param aspiration  The exact V of the previously resolved root card, or -1
///                    at the first card. Sibling root cards very often resolve
///                    to the same V - the nil side usually has several cards
///                    that are equally good, or equally bad - so two probes
///                    settle the card outright: V >= a and not V >= a+1 is
///                    exactly V == a, since the predicate is monotone in
///                    guess. A miss costs two probes and falls through to the
///                    staged search below, unchanged, so the returned value is
///                    identical either way. The two probes are not wasted
///                    either: they warm the transposition table for the staged
///                    search that follows.
static auto nil_resolve_value(
  Pos* posPoint,
  const int depth,
  const int childRel,
  const int tricksTotal,
  const bool primaryOnly,
  const int aspiration,
  SolverContext& ctx) -> int
{
  const int T = tricksTotal;
  const int R = nil_mode::radix(T);

  // Aspiration probe. Skipped in PrimaryOnly, which is already a single probe
  // and would only be made slower.
  if (!primaryOnly && aspiration >= 0)
  {
    // V >= 0 holds unconditionally, so the lower probe is known true at 0 and
    // is not issued - the same reasoning the staged search uses to skip its
    // own zero probes.
    const bool atLeast = (aspiration == 0) ||
      nil_ab_search(posPoint, aspiration, depth, childRel, ctx);

    if (atLeast &&
        ! nil_ab_search(posPoint, aspiration + 1, depth, childRel, ctx))
      return aspiration;
  }

  // Stage 1.
  const int base = nil_ab_search(posPoint, R * R, depth, childRel, ctx)
    ? (R * R)
    : 0;

  if (primaryOnly)
    return base;

  // Stage 2. Largest sC with V >= base + R*sC. hi starts one past T because a
  // seat's directed count never exceeds T, so that probe is known to fail and
  // is never issued - the loop only ever evaluates mid, which stays <= T.
  int lo = 0;
  int hi = T + 1;

  while (hi - lo > 1)
  {
    const int mid = lo + (hi - lo) / 2;

    if (nil_ab_search(posPoint, base + R * mid, depth, childRel, ctx))
      lo = mid;
    else
      hi = mid;
  }

  const int sC = lo;

  // Stage 3. Same shape, one term down.
  lo = 0;
  hi = T + 1;

  while (hi - lo > 1)
  {
    const int mid = lo + (hi - lo) / 2;

    if (nil_ab_search(posPoint, base + R * sC + mid, depth, childRel, ctx))
      lo = mid;
    else
      hi = mid;
  }

  return base + R * sC + lo;
}


/// Project a DealNil onto the plain Deal that the shared validation helpers,
/// InitWinners and the move generator all expect. misere is forced to 0: the
/// nil search never consults it, and leaving it unset keeps a zero-initialised
/// Deal from meaning anything surprising.
static auto to_plain_deal(const DealNil& dl) -> Deal
{
  Deal out{};
  out.trump = dl.trump;
  out.first = dl.first;
  for (int k = 0; k <= 2; k++)
  {
    out.currentTrickRank[k] = dl.currentTrickRank[k];
    out.currentTrickSuit[k] = dl.currentTrickSuit[k];
  }
  for (int h = 0; h < DDS_HANDS; h++)
    for (int s = 0; s < DDS_SUITS; s++)
      out.remainCards[h][s] = dl.remainCards[h][s];
  out.enforceTrumpBreak = dl.enforceTrumpBreak;
  out.trumpAlreadyBroken = dl.trumpAlreadyBroken;
  out.misere = 0;
  return out;
}


auto nil_decode_value(
  const int value,
  const int tricksTotal,
  const int direction,
  int* nilMade,
  int* nilTricks,
  int* coverTricks,
  int* oppTricks) -> void
{
  const nil_mode::Decoded d =
    nil_mode::decode(value, tricksTotal, direction != 0);

  if (nilMade) *nilMade = d.nilMade ? 1 : 0;
  if (nilTricks) *nilTricks = d.nilTricks;
  if (coverTricks) *coverTricks = d.coverTricks;
  if (oppTricks) *oppTricks = d.oppTricks;
}


int STDCALL SolveBoardNil(
  DealNil dl,
  int mode,
  FutureTricks* futp,
  [[maybe_unused]] int thrId)
{
  SolverContext outer_ctx;
  return solve_board_nil_internal(outer_ctx, dl, mode, futp);
}


int STDCALL SolveBoardNilPBN(
  DealNilPBN dlpbn,
  int mode,
  FutureTricks* futp,
  [[maybe_unused]] int thrId)
{
  DealNil dl{};
  if (convert_from_pbn(dlpbn.remainCards, dl.remainCards) != RETURN_NO_FAULT)
    return RETURN_PBN_FAULT;

  for (int k = 0; k <= 2; k++)
  {
    dl.currentTrickRank[k] = dlpbn.currentTrickRank[k];
    dl.currentTrickSuit[k] = dlpbn.currentTrickSuit[k];
  }
  dl.first = dlpbn.first;
  dl.trump = dlpbn.trump;
  dl.enforceTrumpBreak = dlpbn.enforceTrumpBreak;
  dl.trumpAlreadyBroken = dlpbn.trumpAlreadyBroken;
  dl.nilSeatPlus1 = dlpbn.nilSeatPlus1;
  dl.nilAlreadyBroken = dlpbn.nilAlreadyBroken;
  dl.direction = dlpbn.direction;

  return SolveBoardNil(dl, mode, futp, thrId);
}


auto solve_board_nil_internal(
  SolverContext& ctx,
  const DealNil& dl,
  const int mode,
  FutureTricks* futp) -> int
{
  // ----------------------------------------------------------
  // Nil-specific parameter checks, before anything else touches state.
  // ----------------------------------------------------------

  if (dl.nilSeatPlus1 < 1 || dl.nilSeatPlus1 > 4)
    return RETURN_UNKNOWN_FAULT;

  const int nilSeat = dl.nilSeatPlus1 - 1;

  if (mode != static_cast<int>(NilMode::Exact) &&
      mode != static_cast<int>(NilMode::PrimaryOnly))
    return RETURN_MODE_WRONG_LO;

  const Deal plain = to_plain_deal(dl);

  // target/solutions are not part of the nil contract - every legal root card
  // always gets a value - so fixed values are passed to the shared checks.
  int ret = board_range_checks(plain, -1, 3, 0);
  if (ret != RETURN_NO_FAULT)
    return ret;

  // ----------------------------------------------------------
  // Count and classify the deal.
  // ----------------------------------------------------------

  auto thrp = ctx.thread();
  bool newDeal = false;
  int cardCount = 0;

  for (int h = 0; h < DDS_HANDS; h++)
  {
    for (int s = 0; s < DDS_SUITS; s++)
    {
      unsigned int c = plain.remainCards[h][s] >> 2;
      cardCount += count_table[c];

      if (thrp->suit[h][s] != c)
      {
        thrp->suit[h][s] = static_cast<unsigned short>(c);
        newDeal = true;
      }
    }
  }

  // ----------------------------------------------------------
  // Generic initialization.
  // ----------------------------------------------------------

  thrp->trump = plain.trump;
  thrp->trumpBreakRuleOn = (plain.enforceTrumpBreak != 0);
  thrp->misereOn = false;

  ctx.search().ini_depth() = cardCount - 4;
  const int ini_depth = ctx.search().ini_depth();
  const int trick = (ini_depth + 3) >> 2;
  const int hand_rel_first = (48 - ini_depth) % 4;

  // T counts tricks remaining INCLUDING the in-progress trick. cardCount is the
  // total cards left across all four hands; the hands that have already played
  // to the current trick hold one fewer, so rounding up recovers the count of
  // tricks still to be resolved.
  const int tricksTotal = (cardCount + 3) / 4;

  thrp->nilOn = true;
  thrp->nilSeat = nilSeat;
  thrp->nilAlreadyBroken = (dl.nilAlreadyBroken != 0);
  thrp->nilDirection = (dl.direction != 0);
  thrp->nilTricksTotal = tricksTotal;

  ctx.search().trick_nodes() = 0;

  thrp->lookAheadPos.hand_rel_first = hand_rel_first;
  thrp->lookAheadPos.first[ini_depth] = plain.first;
  thrp->lookAheadPos.tricks_max = 0;
  thrp->nilTricks = 0;
  thrp->nilCoverTricks = 0;

  MoveType mv = {0, 0, 0, 0};

  ctx.search().clear_forbidden_moves();

  ret = board_value_checks(ctx, plain, -1, 3, 0);
  if (ret != RETURN_NO_FAULT)
    return ret;

  // ----------------------------------------------------------
  // Last trick. Mirrors solver_if.cpp:193-208.
  // ----------------------------------------------------------
  //
  // Not an optimisation - a correctness requirement. ini_depth is cardCount-4,
  // so at cardCount <= 4 it is zero or negative and the root driver below
  // would search at a negative depth and index win_ranks out of bounds.
  //
  // last_trick_winner() cannot be reused here: it resolves the trick but only
  // reports leadSideWins, a partnership-level answer. Nil mode needs the SEAT,
  // so the same resolution is done locally. Every hand holds at most one card
  // at this point, so there is exactly one legal card and no search to run.
  if (cardCount <= 4)
  {
    int lastTrickSuit[DDS_HANDS] = {0, 0, 0, 0};
    int lastTrickRank[DDS_HANDS] = {0, 0, 0, 0};

    for (int h = 0; h < hand_rel_first; h++)
    {
      const int hp = HAND_ID(plain.first, h);
      lastTrickSuit[hp] = plain.currentTrickSuit[h];
      lastTrickRank[hp] = plain.currentTrickRank[h];
    }

    for (int h = hand_rel_first; h < DDS_HANDS; h++)
    {
      const int hp = HAND_ID(plain.first, h);
      for (int s = 0; s < DDS_SUITS; s++)
      {
        if (thrp->suit[hp][s] != 0)
        {
          lastTrickSuit[hp] = s;
          lastTrickRank[hp] = highest_rank[thrp->suit[hp][s]];
          break;
        }
      }
    }

    int maxRank = 0;
    int maxSuit = 0;
    int maxHand = plain.first;

    if (plain.trump != DDS_NOTRUMP)
    {
      for (int h = 0; h < DDS_HANDS; h++)
      {
        if ((lastTrickSuit[h] == plain.trump) && (lastTrickRank[h] > maxRank))
        {
          maxRank = lastTrickRank[h];
          maxSuit = plain.trump;
          maxHand = h;
        }
      }
    }

    if (maxRank == 0)
    {
      maxRank = lastTrickRank[plain.first];
      maxSuit = lastTrickSuit[plain.first];
      maxHand = plain.first;

      for (int h = 0; h < DDS_HANDS; h++)
      {
        if (lastTrickSuit[h] == maxSuit && lastTrickRank[h] > maxRank)
        {
          maxHand = h;
          maxRank = lastTrickRank[h];
        }
      }
    }

    const int n = (maxHand == nilSeat) ? 1 : 0;
    const int c = (maxHand == (nilSeat ^ 2)) ? 1 : 0;
    const bool broken = thrp->nilAlreadyBroken || (n > 0);

    const int hp = HAND_ID(plain.first, hand_rel_first);

    futp->nodes = 0;
    futp->cards = 1;
    futp->suit[0] = lastTrickSuit[hp];
    futp->rank[0] = lastTrickRank[hp];
    futp->equals[0] = 0;
    futp->score[0] = nil_mode::pack(n, c, 1, broken, thrp->nilDirection);

    thrp->nilOn = false;
    return RETURN_NO_FAULT;
  }

  if (newDeal)
  {
    SetDeal(thrp);
    SetDealTables(ctx);
  }
  else if (ctx.search().analysis_flag())
  {
    SetDeal(thrp);
  }
  ctx.search().analysis_flag() = false;

  // ----------------------------------------------------------
  // Node types. Anchored on nilSeat, fixed for the whole solve.
  // ----------------------------------------------------------
  //
  // solver_if.cpp:273 sets scoreParity from handToPlay, which ties the
  // reference side to whoever happens to be on lead. Copied unchanged into a
  // nil solve that is exactly the silent-wrong-answer bug flagged in
  // docs/nil-mode-map.md §4: the search stays internally consistent while the
  // objective drifts.
  //
  // No misereOn ternary here either. The direction flag lives entirely in the
  // sC / sN complements inside nil_objective.hpp; letting it reach node
  // polarity would make the nil side play to break its own nil.
  thrp->scoreParity = nilSeat & 1;

  for (int h = 0; h < DDS_HANDS; h++)
  {
    ctx.search().node_type_store(h) =
      nil_mode::is_nil_side(h, nilSeat) ? MAXNODE : MINNODE;
  }

  // ----------------------------------------------------------
  // Replay the cards already played to the in-progress trick.
  // ----------------------------------------------------------

  for (int k = 0; k < hand_rel_first; k++)
  {
    mv.rank = plain.currentTrickRank[k];
    mv.suit = plain.currentTrickSuit[k];
    mv.sequence = 0;

    ctx.move_gen().init(
      trick,
      k,
      plain.currentTrickRank,
      plain.currentTrickSuit,
      thrp->lookAheadPos.rank_in_suit,
      thrp->trump,
      thrp->lookAheadPos.first[ini_depth],
      thrp->trumpBreakRuleOn,
      (plain.trumpAlreadyBroken != 0));

    if (k == 0)
    {
      ctx.move_gen().move_gen_0(
        trick,
        thrp->lookAheadPos,
        ctx.search().best_move(ini_depth),
        ctx.search().best_move_tt(ini_depth),
        thrp->rel);
    }
    else
      ctx.move_gen().move_gen_123(trick, k, thrp->lookAheadPos);

    thrp->lookAheadPos.move[ini_depth + hand_rel_first - k] = mv;
    ctx.move_gen().make_specific(mv, trick, k);
  }

  InitWinners(plain, thrp->lookAheadPos, thrp);

#ifdef DDS_TOP_LEVEL
  ctx.search().nodes() = 0;
#endif

  ctx.move_gen().init(
    trick,
    hand_rel_first,
    plain.currentTrickRank,
    plain.currentTrickSuit,
    thrp->lookAheadPos.rank_in_suit,
    thrp->trump,
    thrp->lookAheadPos.first[ini_depth],
    thrp->trumpBreakRuleOn,
    (plain.trumpAlreadyBroken != 0));

  if (hand_rel_first == 0)
  {
    ctx.move_gen().move_gen_0(
      trick,
      thrp->lookAheadPos,
      ctx.search().best_move(ini_depth),
      ctx.search().best_move_tt(ini_depth),
      thrp->rel);
  }
  else
    ctx.move_gen().move_gen_123(trick, hand_rel_first, thrp->lookAheadPos);

  const int noMoves = ctx.move_gen().get_length(trick, hand_rel_first);

  // The -2 forced-move shortcut at solver_if.cpp:412 is deliberately absent.
  // A single legal move still needs its value computed, because the caller
  // reads the count and not just the card.

  // ----------------------------------------------------------
  // Root driver.
  // ----------------------------------------------------------
  //
  // Each root card is evaluated as an independent sub-search: make the card,
  // resolve the resulting position, undo. That sidesteps the candidate-loop
  // bound carrying at solver_if.cpp:441-450 entirely, and with it the
  // mvCaptured failure mode - there are no forbidden-move sets and no bounds
  // shared between candidates, so a stale best_move cannot leak from one card
  // to the next. Slower and obviously correct, which is the Phase 2 trade.
  //
  // The per-card value comes from nil_resolve_value(), which probes on the
  // term boundaries of V rather than scanning down from max_value(T). Probe
  // placement cannot affect the answer - nil_ab_search's predicate is monotone
  // in guess - so this is a pure cost change, and the values written to
  // futp->score are identical to the Phase 2 driver's in Exact mode.
  //
  // Root candidates are still resolved independently, with no bounds carried
  // between them. That is what keeps the mvCaptured failure mode sealed off,
  // and it is not what makes the driver slow.

  const int radix = nil_mode::radix(tricksTotal);

  // In PrimaryOnly the resolution stops after stage 1, so score carries the
  // primary term alone and the loop below exits at the first card that makes
  // the nil. Exact runs all three stages and yields a full packed V.
  const bool primaryOnly = (mode == static_cast<int>(NilMode::PrimaryOnly));

  futp->cards = 0;
  futp->nodes = 0;

  // Exact V of the previously resolved root card, seeding the next card's
  // aspiration probe. -1 until the first card is resolved. Purely a probe
  // placement hint: nil_resolve_value falls back to the full staged search
  // whenever the guess misses, so this cannot change any score written below.
  int aspiration = -1;

  // One invalidation for the whole solve, deliberately outside the root loop.
  //
  // Entries encode G, which is fixed by (deal, nilSeat, direction, trump, T) -
  // all constant here and all varying between solves. Bumping per root card
  // or per driver stage instead would throw away exactly the reuse that makes
  // the table worth having: sibling root cards share most of their subtrees,
  // and the three-stage binary search re-walks the same tree ~10 times with
  // different guesses, which is what the stored bounds are for.
  nil_trans_table().new_solve();

  for (int mno = 0; mno < noMoves; mno++)
  {
    ctx.move_gen().init(
      trick,
      hand_rel_first,
      plain.currentTrickRank,
      plain.currentTrickSuit,
      thrp->lookAheadPos.rank_in_suit,
      thrp->trump,
      thrp->lookAheadPos.first[ini_depth],
      thrp->trumpBreakRuleOn,
      (plain.trumpAlreadyBroken != 0));

    if (hand_rel_first == 0)
    {
      ctx.move_gen().move_gen_0(
        trick,
        thrp->lookAheadPos,
        ctx.search().best_move(ini_depth),
        ctx.search().best_move_tt(ini_depth),
        thrp->rel);
    }
    else
      ctx.move_gen().move_gen_123(trick, hand_rel_first, thrp->lookAheadPos);

    // Walk to the mno'th generated move.
    //
    // The win_ranks argument is NOT a don't-care. MakeNext uses it to allow
    // one "small" move per suit and then require later cards in that suit to
    // clear lowest_rank[win_ranks[suit]]. An all-zero array makes that lookup
    // return 0, which the guard inside MakeNext turns into 15 - a threshold no
    // card can meet - so every card of a suit after the first is silently
    // skipped. At the root that is fatal: the caller reads futp->cards and a
    // dropped card is simply absent from the answer, best card included.
    //
    // bit_map_rank[2] makes lowest_rank return 2. No card ranks below 2, so
    // the threshold is never armed and every legal move is enumerated. The
    // interior search in nil_search.cpp passes real win_ranks and is unaffected.
    MoveType const* mply = NULL;
    unsigned short allowAll[DDS_SUITS] = {
      bit_map_rank[2], bit_map_rank[2], bit_map_rank[2], bit_map_rank[2]};
    for (int step = 0; step <= mno; step++)
      mply = ctx.move_gen().make_next(trick, hand_rel_first, allowAll);

    if (mply == NULL)
      break;

    const MoveType rootMove = *mply;

    // After the root card is played the next node belongs to the following
    // seat, so the child specialisation is one further round the trick. When
    // the root card completes a trick this wraps to 0, the next leader.
    const int childRel = (hand_rel_first + 1) & 3;

    int value = 0;

    if (hand_rel_first == 3)
    {
      // The root card completes a trick, so it has to be credited before the
      // sub-search - the same attribution nil_search_3 does at its own depth.
      unsigned short makeWinRank[DDS_SUITS];
      make_3(&thrp->lookAheadPos, makeWinRank, ini_depth, &rootMove, ctx);

      const int winner = thrp->lookAheadPos.first[ini_depth - 1];
      if (winner == nilSeat)
        thrp->nilTricks++;
      else if (winner == (nilSeat ^ 2))
        thrp->nilCoverTricks++;

      value = nil_resolve_value(&thrp->lookAheadPos, ini_depth - 1, childRel,
                                tricksTotal, primaryOnly, aspiration, ctx);

      if (winner == nilSeat)
        thrp->nilTricks--;
      else if (winner == (nilSeat ^ 2))
        thrp->nilCoverTricks--;

      undo_0(&thrp->lookAheadPos, ini_depth, rootMove, thrp);
    }
    else
    {
      if (hand_rel_first == 0)
        make_0(&thrp->lookAheadPos, ini_depth, &rootMove);
      else if (hand_rel_first == 1)
        make_1(&thrp->lookAheadPos, ini_depth, &rootMove);
      else
        make_2(&thrp->lookAheadPos, ini_depth, &rootMove);

      value = nil_resolve_value(&thrp->lookAheadPos, ini_depth - 1, childRel,
                                tricksTotal, primaryOnly, aspiration, ctx);

      if (hand_rel_first == 0)
        undo_1(&thrp->lookAheadPos, ini_depth, rootMove);
      else if (hand_rel_first == 1)
        undo_2(&thrp->lookAheadPos, ini_depth, rootMove);
      else
        undo_3(&thrp->lookAheadPos, ini_depth, rootMove);
    }

    futp->suit[futp->cards] = rootMove.suit;
    futp->rank[futp->cards] = rootMove.rank;
    futp->equals[futp->cards] = rootMove.sequence << 2;
    futp->score[futp->cards] = value;
    futp->cards++;

    aspiration = value;

    if (primaryOnly && value >= radix * radix)
    {
      // Primary resolved favourably; the remaining cards cannot say more about
      // the made/set question. Combined with the single-probe resolution above,
      // this is what makes the mode cheap: one search per card, stopping at the
      // first card that makes the nil.
      break;
    }
  }

#ifdef DDS_TOP_LEVEL
  futp->nodes = ctx.search().nodes();
#endif

  thrp->nilOn = false;

  return RETURN_NO_FAULT;
}
