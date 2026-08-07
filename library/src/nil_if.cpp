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
  // The per-card value is found by descending linear scan. Phase 5 replaces
  // this with a binary search over 0..max_value(T); until then this is the
  // simplest thing that cannot be subtly wrong about bounds.

  const int vMax = nil_mode::max_value(tricksTotal);
  const int radix = nil_mode::radix(tricksTotal);

  futp->cards = 0;
  futp->nodes = 0;

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
    MoveType const* mply = NULL;
    unsigned short dummyWin[DDS_SUITS] = {0, 0, 0, 0};
    for (int step = 0; step <= mno; step++)
      mply = ctx.move_gen().make_next(trick, hand_rel_first, dummyWin);

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

      value = vMax;
      while (value > 0 &&
             !nil_ab_search(&thrp->lookAheadPos, value, ini_depth - 1,
                            childRel, ctx))
        value--;

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

      value = vMax;
      while (value > 0 &&
             !nil_ab_search(&thrp->lookAheadPos, value, ini_depth - 1,
                            childRel, ctx))
        value--;

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

    if (mode == static_cast<int>(NilMode::PrimaryOnly) && value >= radix * radix)
    {
      // Primary resolved favourably; the remaining cards cannot say more about
      // the made/set question. Phase 4 replaces the scan above with a single
      // query at guess = (T+1)^2, which is what makes this mode cheap.
      break;
    }
  }

#ifdef DDS_TOP_LEVEL
  futp->nodes = ctx.search().nodes();
#endif

  thrp->nilOn = false;

  return RETURN_NO_FAULT;
}
