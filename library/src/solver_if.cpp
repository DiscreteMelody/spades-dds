/*
   DDS, a bridge double dummy solver.

   Copyright (C) 2006-2014 by Bo Haglund /
   2014-2018 by Bo Haglund & Soren Hein.

   See LICENSE and README.
*/

#include <ab_search.hpp>
#include <dump.hpp>
#include <init.hpp>
#include "solver_if.hpp"
#include <api/solve_board.hpp>
#include <lookup_tables/lookup_tables.hpp>
#include <solver_context/solver_context.hpp>
#include <system/scheduler.hpp>
#include <system/system.hpp>
#include <system/timer_list.hpp>
#include <trans_table/trans_table.hpp>

extern Memory memory;
extern Scheduler scheduler;


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

auto last_trick_winner(
  const Deal& dl,
  const std::shared_ptr<ThreadData>& thrp,
  const int handToPlay,
  const int hand_rel_first,
  int& leadRank,
  int& leadSuit,
  int& leadSideWins) -> void;

bool (* AB_ptr_list[DDS_HANDS])(
  Pos * posPoint,
  const int target,
  const int depth,
  SolverContext& ctx)
  = { ab_search, ab_search_1, ab_search_2, ab_search_3 };

bool (* AB_ptr_trace_list[DDS_HANDS])(
  Pos * posPoint,
  const int target,
  const int depth,
  SolverContext& ctx)
  = { ab_search_0, ab_search_1, ab_search_2, ab_search_3 };

void (* Make_ptr_list[3])(
  Pos * posPoint,
  const int depth,
  MoveType const * mply)
  = { make_0, make_1, make_2 };


int STDCALL SolveBoard(
  Deal dl,
  int target,
  int solutions,
  int mode,
  FutureTricks * futp,
  [[maybe_unused]] int thrId)
{
  SolverContext outer_ctx;
  return solve_board(outer_ctx, dl, target, solutions, mode, futp);
}

auto solve_board_internal(
  SolverContext& ctx,
  const Deal& dl,
  const int target,
  const int solutions,
  const int mode,
  FutureTricks * futp) -> int
{
  // ----------------------------------------------------------
  // Formal parameter checks.
  // ----------------------------------------------------------

  int ret = board_range_checks(dl, target, solutions, mode);
  if (ret != RETURN_NO_FAULT)
    return ret;

  // ----------------------------------------------------------
  // Count and classify Deal.
  // ----------------------------------------------------------

  auto thrp = ctx.thread();
  bool newDeal = false;
  bool newTrump = false;
  unsigned diffDeal = 0;
  unsigned aggDeal = 0;
  bool similarDeal;
  int cardCount = 0;
  int ind, forb, noMoves;

  for (int h = 0; h < DDS_HANDS; h++)
  {
    for (int s = 0; s < DDS_SUITS; s++)
    {
      unsigned int c = dl.remainCards[h][s] >> 2;

      cardCount += count_table[c];
      diffDeal += (c ^ (thrp->suit[h][s]));
      aggDeal += c;

      if (thrp->suit[h][s] != c)
      {
        thrp->suit[h][s] = static_cast<unsigned short>(c);
        newDeal = true;
      }
    }
  }

  if (newDeal)
  {
    if (diffDeal == 0)
      similarDeal = true;
    else if ((aggDeal / diffDeal) > SIMILARDEALLIMIT)
      similarDeal = true;
    else
      similarDeal = false;
  }
  else
    similarDeal = false;

  if (dl.trump != thrp->trump)
    newTrump = true;

  if ((dl.enforceTrumpBreak != 0) != thrp->trumpBreakRuleOn)
    newTrump = true;

  // A misère/maximise switch also invalidates the transposition table, but it
  // is deliberately NOT folded into newTrump here. newTrump only triggers a
  // reset on the mode != 2 path below, whereas an objective switch has to
  // invalidate the table unconditionally. It is handled by
  // TransTable::set_objective() further down instead.

  // ----------------------------------------------------------
  // Generic initialization.
  // ----------------------------------------------------------

  thrp->trump = dl.trump;
  thrp->trumpBreakRuleOn = (dl.enforceTrumpBreak != 0);
  thrp->misereOn = (dl.misere != 0);
  ctx.search().ini_depth() = cardCount - 4;
  int ini_depth = ctx.search().ini_depth();
  int trick = (ini_depth + 3) >> 2;
  int hand_rel_first = (48 - ini_depth) % 4;
  int handToPlay = HAND_ID(dl.first, hand_rel_first);
  // Declared here (uninitialized) rather than where it's actually computed
  // below, because an earlier "only one legal move" shortcut further down
  // has a goto that jumps past that point - see the assignment site for
  // what this means and why it's needed.
  bool handToPlayIsMax;
  ctx.search().trick_nodes() = 0;

  thrp->lookAheadPos.hand_rel_first = hand_rel_first;
  thrp->lookAheadPos.first[ini_depth] = dl.first;
  thrp->lookAheadPos.tricks_max = 0;

  MoveType mv = {0, 0, 0, 0};

  ctx.search().clear_forbidden_moves();
  

  // ----------------------------------------------------------
  // Consistency checks.
  // ----------------------------------------------------------

  ret = board_value_checks(ctx, dl, target, solutions, mode);
  if (ret != RETURN_NO_FAULT)
    return ret;


  // ----------------------------------------------------------
  // Last trick, easy to solve.
  // ----------------------------------------------------------

  if (cardCount <= 4)
  {
    int leadRank, leadSuit, leadSideWins;

    last_trick_winner(dl, thrp, handToPlay, hand_rel_first,
      leadRank, leadSuit, leadSideWins);

    futp->nodes = 0;
    futp->cards = 1;
    futp->suit[0] = leadSuit;
    futp->rank[0] = leadRank;
    futp->equals[0] = 0;
    futp->score[0] = (target == 0 && solutions < 3 ? 0 : leadSideWins);

    goto SOLVER_DONE;
  }


  // ----------------------------------------------------------
  // More detailed initialization.
  // ----------------------------------------------------------

  {
    if ((mode != 2) &&
        (((newDeal) && (! similarDeal)) ||
         newTrump ||
         (ctx.search().nodes() > SIMILARMAXWINNODES)))
    {
        ResetReason reason = ResetReason::Unknown;
        if (ctx.search().nodes() > SIMILARMAXWINNODES)
          reason = ResetReason::TooManyNodes;
        else if (newDeal && ! similarDeal)
          reason = ResetReason::NewDeal;
        else if (newTrump)
          reason = ResetReason::NewTrump;
      
  ctx.trans_table()->reset_memory(reason);
    }
  }

  // Key the transposition table by search objective. Entries hold bounds on
  // trick counts "under optimal play", and optimal play means something
  // different under the maximise and misère objectives, so the same position
  // legitimately has two different values and the two sets of entries must
  // never mix.
  //
  // This matters on any entry point that reuses a SolverContext across solves,
  // because the table lives on the context: the modern API (dds_solve_board /
  // SolveBoard(ctx, ...)) hands the caller a long-lived context, and
  // calc_tables.cpp deliberately shares one across boards. A caller that
  // alternates misère and maximise solves on such a context would otherwise
  // read entries written under the other objective and get wrong scores. The
  // legacy SolveBoard/SolveBoardPBN entry points happen to be immune because
  // they construct a fresh context per call, but that is an implementation
  // detail of those wrappers, not a property of the search - so the guard
  // belongs here, once, rather than in each wrapper.
  //
  // Deliberately NOT conditional on mode, unlike the reset above: mode == 2
  // suppresses that reset precisely in order to preserve the table, so gating
  // this on mode would disable it exactly when it is needed.
  // set_objective() is a no-op when the objective has not changed, so the
  // normal case costs nothing. See trans_table.hpp.
  ctx.trans_table()->set_objective(thrp->misereOn);

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

  // scoreParity is fixed by *who is on play*, independent of misère mode:
  // it says which hands' trick wins count toward tricks_max (see
  // SearchContext::is_reference_hand()), and that's always the partnership
  // this solve is being asked about, whether we're maximizing or
  // minimizing their tricks.
  thrp->scoreParity = handToPlay & 1;

  // Vanilla mode: the reference side (handToPlay's partnership) is MAXNODE
  // - it plays to maximize its own trick count - and the other side is
  // MINNODE - it defends normally, which (because the two sides' trick
  // counts always sum to the tricks remaining) is the same policy as
  // maximizing its own. Misère mode (Deal::misere != 0) inverts this: both
  // partnerships play to minimize the tricks THEY take, which by the same
  // fixed-sum argument means the reference side becomes MINNODE and the
  // other side becomes MAXNODE. Scoring is unaffected by this flip -
  // tricks_max still counts the reference side's own tricks either way,
  // via is_reference_hand()/scoreParity above - only which side's decision
  // nodes pick "up" vs "down" changes.
  {
    const int refNode = thrp->misereOn ? MINNODE : MAXNODE;
    const int othNode = thrp->misereOn ? MAXNODE : MINNODE;

    if (handToPlay == 0 || handToPlay == 2)
    {
      ctx.search().node_type_store(0) = refNode;
      ctx.search().node_type_store(1) = othNode;
      ctx.search().node_type_store(2) = refNode;
      ctx.search().node_type_store(3) = othNode;
    }
    else
    {
      ctx.search().node_type_store(0) = othNode;
      ctx.search().node_type_store(1) = refNode;
      ctx.search().node_type_store(2) = othNode;
      ctx.search().node_type_store(3) = refNode;
    }
  }

  // Whether handToPlay's own decision nodes are MAXNODE (vanilla) or MINNODE
  // (misère). This is ALWAYS true in vanilla mode and ALWAYS false in
  // misère mode - handToPlay is by construction on the reference side (see
  // scoreParity above), and refNode is exactly what handToPlay gets.
  //
  // This matters beyond just alpha-beta policy: best_move(depth) is only
  // recorded by ab_search's move loop on a "cutoff" (a child whose returned
  // value matches this hand's own success = (node_type_store(hand) ==
  // MAXNODE)). When handToPlay is MAXNODE, a cutoff is exactly what makes
  // the search return true - so "did we get a legit best_move" and "did
  // ab_search return true" happen to be the same question, and the code
  // below (written before misère mode existed) reads best_move whenever
  // the raw return was true. When handToPlay is MINNODE (misère), that's
  // backwards: a cutoff (a real best_move) is what makes the search return
  // FALSE, while TRUE means every move was tried and none of them avoided
  // the target (i.e. reference is forced into it regardless of choice) -
  // best_move is stale in that case. Every "if (thrp->val) read best_move"
  // site below needs to check bestMoveValid (== thrp->val when
  // handToPlayIsMax, == !thrp->val when it isn't) instead of raw thrp->val
  // for the "is best_move trustworthy right now" question - the raw
  // thrp->val itself is still the right thing to drive the binary search's
  // lowerbound/upperbound narrowing either way, since "tricks_max >=
  // target achieved" is a mode-independent fact.
  handToPlayIsMax = (ctx.search().node_type_store(handToPlay) == MAXNODE);

  for (int k = 0; k < hand_rel_first; k++)
  {
    mv.rank = dl.currentTrickRank[k];
    mv.suit = dl.currentTrickSuit[k];
    mv.sequence = 0;

    ctx.move_gen().init(
      trick,
      k,
      dl.currentTrickRank,
      dl.currentTrickSuit,
      thrp->lookAheadPos.rank_in_suit,
      thrp->trump,
      thrp->lookAheadPos.first[ini_depth],
      thrp->trumpBreakRuleOn,
      (dl.trumpAlreadyBroken != 0));

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
      ctx.move_gen().move_gen_123(
        trick,
        k,
        thrp->lookAheadPos);

  thrp->lookAheadPos.move[ini_depth + hand_rel_first - k] = mv;
  ctx.move_gen().make_specific(mv, trick, k);
  }

  InitWinners(dl, thrp->lookAheadPos, thrp);

#ifdef DDS_AB_STATS
  thrp->ABStats.Reset();
  thrp->ABStats.ResetCum();
#endif

#ifdef DDS_TOP_LEVEL
  {
    ctx.search().nodes() = 0;
  }
#endif

  ctx.move_gen().init(
    trick,
    hand_rel_first,
    dl.currentTrickRank,
    dl.currentTrickSuit,
    thrp->lookAheadPos.rank_in_suit,
    thrp->trump,
    thrp->lookAheadPos.first[ini_depth],
    thrp->trumpBreakRuleOn,
    (dl.trumpAlreadyBroken != 0));

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
    ctx.move_gen().move_gen_123(
      trick,
      hand_rel_first,
      thrp->lookAheadPos);

  noMoves = ctx.move_gen().get_length(trick, hand_rel_first);

  // ----------------------------------------------------------
  // mode == 0: Check whether there is only one possible move
  // ----------------------------------------------------------

  if (mode == 0 && noMoves == 1 && solutions != 3)
  {
    MoveType const * mp = ctx.move_gen().make_next_simple(trick, hand_rel_first);

    futp->nodes = 0;
    futp->cards = 1;

    futp->suit[0] = mp->suit;
    futp->rank[0] = mp->rank;
    futp->equals[0] = mp->sequence << 2;
    futp->score[0] = -2;

    goto SOLVER_DONE;
  }

  // ----------------------------------------------------------
  // solutions == 3: Target and mode don't matter; all cards
  // ----------------------------------------------------------

  if (solutions == 3)
  {
    // 7 for hand 0 and 2, 6 for hand 1 and 3
    int guess = 7 - (handToPlay & 0x1);
    int upperbound = 13;
    int lowerbound = 0;
    futp->cards = noMoves;

    for (int mno = 0; mno < noMoves; mno++)
    {
      // The carried-over `upperbound` from the previous candidate is a
      // valid tight bound in vanilla mode (candidates are non-increasing
      // as more get forbidden, so the old upperbound == old lowerbound
      // still safely bounds this one from above). In misère mode the
      // sequence runs the other way (non-decreasing), so that same old
      // bound can wrongly cut this candidate's search off immediately -
      // before a single fresh query - leaving `mv` stale from the
      // previous (now-forbidden) candidate. Give misère mode a clean
      // upperbound each time so it always gets at least one real query
      // against the current forbidden-move set.
      if (thrp->misereOn)
        upperbound = 13;

      // Tracks whether the alpha-beta search ever actually populated
      // best_move for THIS candidate (see the capture-condition comment
      // below). This can diverge from "lowerbound != 0": in misère mode
      // capture happens on the opposite branch from bound-narrowing, so
      // a candidate whose whole binary search takes only one branch
      // (see below) leaves both mv AND mvCaptured at their prior state.
      bool mvCaptured = false;

      do
      {
        ctx.reset_best_moves_lite();

        TIMER_START(TIMER_NO_AB, ini_depth);
  thrp->val = (* AB_ptr_list[hand_rel_first])(
                      &thrp->lookAheadPos,
                      guess,
                      ini_depth,
          ctx);
        TIMER_END(TIMER_NO_AB, ini_depth);

#ifdef DDS_TOP_LEVEL
        DumpTopLevel(thrp->fileTopLevel.GetStream(), 
          thrp, guess, lowerbound, upperbound, 1);
#endif

        if (thrp->val)
        {
          // handToPlayIsMax is ALWAYS true in vanilla mode, so this is
          // where vanilla captures its best_move (see the long comment
          // at handToPlayIsMax's declaration). It's ALWAYS false in
          // misère mode, so this branch never captures there - misère
          // captures below instead. Either way, a cutoff on the
          // "achieved" side is a real, meaningful best_move only when
          // it matches handToPlayIsMax; the mismatched case just moves
          // the bound and leaves best_move untouched for this query.
          if (thrp->val == handToPlayIsMax)
          {
            mv = ctx.search().best_move(ini_depth);
            mvCaptured = true;
          }
          lowerbound = guess++;
        }
        else
        {
          // Mirror image of the branch above: this is where misère
          // mode captures its best_move (handToPlayIsMax == false, so
          // thrp->val == false matches here), and never vanilla.
          if (thrp->val == handToPlayIsMax)
          {
            mv = ctx.search().best_move(ini_depth);
            mvCaptured = true;
          }
          upperbound = --guess;
        }
      }
      while (lowerbound < upperbound);

      // In vanilla mode 0 is an absorbing floor (tricks can't go negative)
      // and, in misère mode, 13 (or whatever the ceiling is at this depth)
      // is the mirror-image absorbing ceiling - a candidate that hits
      // either extreme, with best_move never captured by a cutoff, means
      // every one of the not-yet-recorded candidates is provably tied at
      // that same extreme too (nothing can beat a floor of 0 or a ceiling
      // of "every remaining trick"). mvCaptured (not lowerbound, and not
      // thrp->misereOn alone) is the direct signal for this: it's false
      // exactly when the whole binary search for this candidate took only
      // the branch that doesn't populate best_move for the current mode
      // (see the two capture sites above), which is exactly the case
      // where mv cannot be trusted, in either mode.
      if (mvCaptured)
      {
        ctx.search().best_move(ini_depth) = mv;

        futp->suit[mno] = mv.suit;
        futp->rank[mno] = mv.rank;
        futp->equals[mno] = mv.sequence << 2;
        futp->score[mno] = lowerbound;

        ctx.search().forbidden_move(mno + 1).suit = mv.suit;
        ctx.search().forbidden_move(mno + 1).rank = mv.rank;

        guess = lowerbound;
        lowerbound = 0;
      }
      else
      {
        int noLeft = ctx.move_gen().get_length(trick, hand_rel_first);

        ctx.move_gen().rewind(trick, hand_rel_first);
        for (int j = 0; j < noLeft; j++)
        {
          MoveType const * mp = 
            ctx.move_gen().make_next_simple(trick, hand_rel_first);

          futp->suit[mno + j] = mp->suit;
          futp->rank[mno + j] = mp->rank;
          futp->equals[mno + j] = mp->sequence << 2;
          futp->score[mno + j] = lowerbound;
        }

        break;
      }
    }
    goto SOLVER_STATS;
  }

  // ----------------------------------------------------------
  // target == 0: Only cards required, no scoring
  // ----------------------------------------------------------

  else if (target == 0)
  {
    futp->nodes = 0;
    futp->cards = (solutions == 1 ? 1 : noMoves);

    for (int mno = 0; mno < noMoves; mno++)
    {
      MoveType const * mp = 
        ctx.move_gen().make_next_simple(trick, hand_rel_first);

      futp->suit[mno] = mp->suit;
      futp->rank[mno] = mp->rank;
      futp->equals[mno] = mp->sequence << 2;
      futp->score[mno] = 0;
    }

    goto SOLVER_DONE;
  }

  // ----------------------------------------------------------
  // target == -1: Find optimum score and 1 or more cards
  // ----------------------------------------------------------

  else if (target == -1)
  {
    /*
     * Reset semantics
     * ----------------
     * - reset_for_solve(): Heavy, per-solve reset (frees TT memory as needed and
     *   clears broad search state). Use this only at top-level initialization of
     *   a solve. Do NOT call it inside iterative search loops; it changes state
     *   beyond what the legacy code expected and can affect move ordering/output.
     *
     * - reset_best_moves_lite(): Lightweight, per-iteration reset that matches the
     *   legacy ResetBestMoves behavior. It only clears bestMove[*].rank and
     *   bestMoveTT[*].rank, updates memUsed and ABStats. Use this inside the
     *   do/while and other iterative loops below to preserve historical results.
     */
    // 7 for hand 0 and 2, 6 for hand 1 and 3
    int guess = 7 - (handToPlay & 0x1);
    int upperbound = 13;
    int lowerbound = 0;

    // See the equivalent tracking (and the two capture-site comments) in
    // the solutions==3 block above: mvCaptured is the direct signal for
    // "was best_move ever actually populated by a cutoff for this mode",
    // which is what "lowerbound == 0 && !thrp->misereOn" was trying to
    // approximate. It's the exact condition, in both modes, rather than
    // an approximation that only happens to hold in vanilla mode.
    bool mvCaptured = false;

    do
    {
        ctx.reset_best_moves_lite();

        TIMER_START(TIMER_NO_AB, ini_depth);
        thrp->val = (* AB_ptr_list[hand_rel_first])(&thrp->lookAheadPos,
            guess,
            ini_depth,
            ctx);
        TIMER_END(TIMER_NO_AB, ini_depth);

#ifdef DDS_TOP_LEVEL
      DumpTopLevel(thrp->fileTopLevel.GetStream(),
        thrp, guess, lowerbound, upperbound, 1);
#endif

      if (thrp->val)
      {
        if (thrp->val == handToPlayIsMax)
        {
          mv = ctx.search().best_move(ini_depth);
          mvCaptured = true;
        }
        lowerbound = guess++;
      }
      else
      {
        if (thrp->val == handToPlayIsMax)
        {
          mv = ctx.search().best_move(ini_depth);
          mvCaptured = true;
        }
        upperbound = --guess;
      }

    }
    while (lowerbound < upperbound);

    // See the solutions==3 block above: a candidate whose best_move was
    // never captured by a cutoff (mvCaptured == false) means every legal
    // move here is provably tied at `lowerbound` - 0 in vanilla mode's
    // floor case, or the ceiling in misère mode's mirror-image case
    // (handToPlay forced to take every remaining trick regardless of
    // choice). Read them straight from the move generator instead of
    // trusting best_move, which was never actually set for this call.
    if (!mvCaptured)
    {
      // ALL the other moves must also have payoff `lowerbound`.

      if (solutions == 1) // We only need one of them
        futp->cards = 1;
      else // solutions == 2, so return all cards
        futp->cards = noMoves;

      ctx.move_gen().rewind(trick, hand_rel_first);
      for (int i = 0; i < noMoves; i++)
      {
        MoveType const * mp = 
          ctx.move_gen().make_next_simple(trick, hand_rel_first);

        futp->score[i] = lowerbound;
        futp->suit[i] = mp->suit;
        futp->rank[i] = mp->rank;
        futp->equals[i] = mp->sequence << 2;
      }

      goto SOLVER_STATS;
    }
    else // best_move is trustworthy
    {
      ctx.search().best_move(ini_depth) = mv;

      futp->cards = 1;
      futp->score[0] = lowerbound;
      futp->suit[0] = mv.suit;
      futp->rank[0] = mv.rank;
      futp->equals[0] = mv.sequence << 2;

      if (solutions != 2)
        goto SOLVER_STATS;
    }
  }

  // ----------------------------------------------------------
  // target >= 1: Find optimum card(s) achieving user's target
  // ----------------------------------------------------------

  else
  {
    TIMER_START(TIMER_NO_AB, ini_depth);
  thrp->val = (* AB_ptr_list[hand_rel_first])(
                  &thrp->lookAheadPos,
                  target,
                  ini_depth,
          ctx);
    TIMER_END(TIMER_NO_AB, ini_depth);

#ifdef DDS_TOP_LEVEL
    DumpTopLevel(thrp->fileTopLevel.GetStream(), 
      thrp, target, -1, -1, 0);
#endif

    if (! thrp->val)
    {
      // No move. If target was 1, then we are sure that in
      // fact no tricks can be won. If target was > 1, then
      // it is still possible that no tricks can't be won.
      // We don't know. So that's arguably a small bug.
      futp->cards = 0;
      futp->score[0] = (target > 1 ? -1 : 0);

      goto SOLVER_STATS;
    }
    else if (thrp->val == handToPlayIsMax)
    {
      futp->cards = 1;
      futp->suit[0] = ctx.search().best_move(ini_depth).suit;
      futp->rank[0] = ctx.search().best_move(ini_depth).rank;
      futp->equals[0] = ctx.search().best_move(ini_depth).sequence << 2;
      futp->score[0] = target;

      if (solutions != 2)
        goto SOLVER_STATS;
    }
    else
    {
      // thrp->val is true (target achieved) but, in misère mode, that
      // means every move was tried and none of them avoided the target -
      // best_move wasn't populated by a cutoff. Every legal move is
      // equally "forced" here, so any one of them correctly reports this
      // score; just take whatever the plain move generator hands back
      // first rather than trusting best_move.
      ctx.move_gen().rewind(trick, hand_rel_first);
      MoveType const * mp = ctx.move_gen().make_next_simple(trick, hand_rel_first);

      futp->cards = 1;
      futp->suit[0] = mp->suit;
      futp->rank[0] = mp->rank;
      futp->equals[0] = mp->sequence << 2;
      futp->score[0] = target;

      if (solutions != 2)
        goto SOLVER_STATS;
    }
  }

  // ----------------------------------------------------------
  // solution == 2 && payoff > 0: Find other cards with score.
  // This applies both to target == -1 and target >= 1.
  // ----------------------------------------------------------

  forb = 1;
  ind = 1;

  while (ind < noMoves)
  {
    // Moves up to and including bestMove are now forbidden.

    ctx.move_gen().rewind(trick, hand_rel_first);
    int num = ctx.move_gen().get_length(trick, hand_rel_first);

    for (int k = 0; k < num; k++)
    {
      MoveType const * mp = 
        ctx.move_gen().make_next_simple(trick, hand_rel_first);
      
      ctx.search().forbidden_move(forb) = * mp;
      forb++;

        if ((ctx.search().best_move(ini_depth).suit == mp->suit) &&
          (ctx.search().best_move(ini_depth).rank == mp->rank))
        break;
    }

  /* No per-iteration full reset here; preserve original behavior */

    TIMER_START(TIMER_NO_AB, ini_depth);
  thrp->val = (* AB_ptr_list[hand_rel_first])(
                  &thrp->lookAheadPos,
                  futp->score[0],
                  ini_depth,
          ctx);
    TIMER_END(TIMER_NO_AB, ini_depth);

#ifdef DDS_TOP_LEVEL
    DumpTopLevel(thrp->fileTopLevel.GetStream(),
      thrp, target, -1, -1, 2);
#endif

    if (! thrp->val)
      break;

    // NOTE: this loop's job is "find OTHER moves tied at exactly
    // futp->score[0]". In vanilla mode that's equivalent to ">= score[0]"
    // because score[0] was already established as the maximum - nothing
    // can beat it. In misère mode score[0] is the *minimum*, so a move
    // that merely satisfies ">= score[0]" could be strictly worse (a
    // higher, non-tied trick count) rather than tied - this loop doesn't
    // currently distinguish those for misère. It isn't reachable from
    // SolveBoardPBN with the target/solutions values used elsewhere in
    // this codebase (this path needs an explicit target >= 1 together
    // with solutions == 2), so it's flagged rather than fully re-derived
    // here. Only the best_move read below - a definite bug regardless of
    // that open question - is fixed.
    if (thrp->val == handToPlayIsMax)
    {
      futp->cards = ind + 1;
      futp->suit[ind] = ctx.search().best_move(ini_depth).suit;
      futp->rank[ind] = ctx.search().best_move(ini_depth).rank;
      futp->equals[ind] = ctx.search().best_move(ini_depth).sequence << 2;

      futp->score[ind] = futp->score[0];
      ind++;
    }
    else
    {
      break;
    }
  }


SOLVER_STATS:
  {
    ctx.search().clear_forbidden_moves();
  }
#ifdef DDS_TIMING
  thrp->timerList.PrintStats(thrp->fileTimerList.GetStream());
#endif

#ifdef DDS_TT_STATS
  // These are for the large TT -- empty if not.
  // thrp->transTable->PrintAllSuits(thrp->fileTTstats.GetStream());
  // thrp->transTable->PrintAllSuitStats(thrp->fileTTstats.GetStream());
  // thrp->transTable->PrintAllEntries(thrp->fileTTstats.GetStream());
  // thrp->transTable->PrintAllEntryStats(thrp->fileTTstats.GetStream());

  {
  ctx.trans_table()->print_summary_suit_stats(thrp->fileTTstats.GetStream());
  ctx.trans_table()->print_summary_entry_stats(thrp->fileTTstats.GetStream());
  }

  // These are for the small TT -- empty if not.
  {
  ctx.trans_table()->print_node_stats(thrp->fileTTstats.GetStream());
  ctx.trans_table()->print_reset_stats(thrp->fileTTstats.GetStream());
  }
#endif

// Diagnostics are routed via the SolverContext MoveGen facade.
#ifdef DDS_MOVES
  ctx.move_gen().print_trick_stats(thrp->fileMoves.GetStream());
#ifdef DDS_MOVES_DETAILS
  ctx.move_gen().print_trick_details(thrp->fileMoves.GetStream());
#endif
  ctx.move_gen().print_function_stats(thrp->fileMoves.GetStream());
#endif

SOLVER_DONE:

  {
  thrp->memUsed = ctx.trans_table()->memory_in_use() + ThreadMemoryUsed();
  }
  {
    futp->nodes = ctx.search().trick_nodes();
  }

#ifdef DDS_MEMORY_LEAKS_WIN32
  _CrtDumpMemoryLeaks();
#endif

  return RETURN_NO_FAULT;
}


auto solve_same_board(
  SolverContext& ctx,
  const Deal& dl,
  FutureTricks * futp,
  const int hint) -> int
{
  // Specialized function for SolveChunkDDtable for repeat solves.
  // No further parameter checks! This function makes heavy reuse
  // of parameters that are already stored in various places. It
  // corresponds to:
  // target == -1, solutions == 1, mode == 2.
  // The function only needs to return fut.score[0].

  auto thrp = ctx.thread();
  int ini_depth = ctx.search().ini_depth();
  int trick = (ini_depth + 3) >> 2;
  {
    ctx.search().trick_nodes() = 0;
  }

  thrp->lookAheadPos.first[ini_depth] = dl.first;

  {
    if (dl.first == 0 || dl.first == 2)
    {
      ctx.search().node_type_store(0) = MAXNODE;
      ctx.search().node_type_store(1) = MINNODE;
      ctx.search().node_type_store(2) = MAXNODE;
      ctx.search().node_type_store(3) = MINNODE;
    }
    else
    {
      ctx.search().node_type_store(0) = MINNODE;
      ctx.search().node_type_store(1) = MAXNODE;
      ctx.search().node_type_store(2) = MINNODE;
      ctx.search().node_type_store(3) = MAXNODE;
    }
  }

#ifdef DDS_AB_STATS
  thrp->ABStats.Reset();
  thrp->ABStats.ResetCum();
#endif

#ifdef DDS_TOP_LEVEL
  {
    ctx.search().nodes() = 0;
  }
#endif

  ctx.move_gen().reinit(trick, dl.first);

  int guess = hint;
  int lowerbound = 0;
  int upperbound = 13;

  do
  {
  /* No per-iteration full reset here; preserve original behavior */

    TIMER_START(TIMER_NO_AB, ini_depth);
    thrp->val = ab_search(
        &thrp->lookAheadPos,
        guess,
        ini_depth,
        ctx);
    TIMER_END(TIMER_NO_AB, ini_depth);

#ifdef DDS_TOP_LEVEL
    DumpTopLevel(thrp->fileTopLevel.GetStream(),
      thrp, guess, lowerbound, upperbound, 1);
#endif

    if (thrp->val)
      lowerbound = guess++;
    else
      upperbound = --guess;
  }
  while (lowerbound < upperbound);

  futp->cards = 1;
  futp->score[0] = lowerbound;

  thrp->memUsed = ctx.trans_table()->memory_in_use() +
                    ThreadMemoryUsed();

#ifdef DDS_TIMING
  thrp->timerList.PrintStats(thrp->fileTimerList.GetStream());
#endif

#ifdef DDS_TT_STATS
  // These are for the large TT -- empty if not.
  // thrp->transTable->PrintAllSuits(thrp->fileTTstats.GetStream());
  // thrp->transTable->PrintAllSuitStats(thrp->fileTTstats.GetStream());
  // thrp->transTable->PrintAllEntries(thrp->fileTTstats.GetStream());
  // thrp->transTable->PrintAllEntryStats(thrp->fileTTstats.GetStream());

  {
  ctx.trans_table()->print_summary_suit_stats(thrp->fileTTstats.GetStream());
  ctx.trans_table()->print_summary_entry_stats(thrp->fileTTstats.GetStream());
  }

  // These are for the small TT -- empty if not.
  {
  ctx.trans_table()->print_node_stats(thrp->fileTTstats.GetStream());
  ctx.trans_table()->print_reset_stats(thrp->fileTTstats.GetStream());
  }
#endif

#ifdef DDS_MOVES
  ctx.move_gen().print_trick_stats(thrp->fileMoves.GetStream());
#ifdef DDS_MOVES_DETAILS
  ctx.move_gen().print_trick_details(thrp->fileMoves.GetStream());
#endif
  ctx.move_gen().print_function_stats(thrp->fileMoves.GetStream());
#endif

  {
    futp->nodes = ctx.search().trick_nodes();
  }

#ifdef DDS_MEMORY_LEAKS_WIN32
  _CrtDumpMemoryLeaks();
#endif

  return RETURN_NO_FAULT;
}


auto analyse_later_board(
  SolverContext& ctx,
  const int leadHand,
  MoveType const * move,
  const int hint,
  const int hintDir,
  FutureTricks * futp) -> int
{
  // Specialized function for PlayAnalyser for cards after the
  // opening lead. No further parameter checks! This function
  // makes heavy reuse of parameters that are already stored in
  // various places. It corresponds to:
  // target == -1, solutions == 1, mode == 2.
  // The function only needs to return fut.score[0].

  // Reuse the caller's context (and its warm transposition table) instead of
  // constructing a fresh one with a cold TT. The hint-bounded null-window
  // search below relies on the TT state built up by the initial solve and the
  // preceding cards; a cold TT yields wrong (under-counted) AnalysePlay
  // results. This mirrors the calc_dd_table fix in commit 27030ba.
  auto thrp = ctx.thread();
  int ini_depth = --ctx.search().ini_depth();
  int cardCount = ini_depth + 4;
  int trick = (ini_depth + 3) >> 2;
  int hand_rel_first = (48 - ini_depth) % 4;
  {
    ctx.search().trick_nodes() = 0;
  }
  {
    ctx.search().analysis_flag() = true;
  }
  int handToPlay = HAND_ID(leadHand, hand_rel_first);

  {
    if (handToPlay == 0 || handToPlay == 2)
    {
      ctx.search().node_type_store(0) = MAXNODE;
      ctx.search().node_type_store(1) = MINNODE;
      ctx.search().node_type_store(2) = MAXNODE;
      ctx.search().node_type_store(3) = MINNODE;
    }
    else
    {
      ctx.search().node_type_store(0) = MINNODE;
      ctx.search().node_type_store(1) = MAXNODE;
      ctx.search().node_type_store(2) = MINNODE;
      ctx.search().node_type_store(3) = MAXNODE;
    }
  }

  if (hand_rel_first == 0)
  {
    ctx.move_gen().make_specific(* move, trick + 1, 3);
  unsigned short int ourWinRanks[DDS_SUITS]; // Unused here
  make_3(&thrp->lookAheadPos, ourWinRanks, ini_depth + 1, move, ctx);
  }
  else if (hand_rel_first == 1)
  {
    ctx.move_gen().make_specific(* move, trick, 0);
    make_0(&thrp->lookAheadPos, ini_depth + 1, move);
  }
  else if (hand_rel_first == 2)
  {
    ctx.move_gen().make_specific(* move, trick, 1);
    make_1(&thrp->lookAheadPos, ini_depth + 1, move);
  }
  else
  {
    ctx.move_gen().make_specific(* move, trick, 2);
    make_2(&thrp->lookAheadPos, ini_depth + 1, move);
  }

  if (cardCount <= 4)
  {
    // Last trick.
    EvalType eval = evaluate_with_context(&thrp->lookAheadPos, thrp->trump, ctx);
    futp->score[0] = eval.tricks;
    futp->nodes = 0;

    return RETURN_NO_FAULT;
  }

#ifdef DDS_AB_STATS
  thrp->ABStats.Reset();
  thrp->ABStats.ResetCum();
#endif

#ifdef DDS_TOP_LEVEL
  {
    ctx.search().nodes() = 0;
  }
#endif

  int guess = hint,
      lowerbound,
      upperbound;

  if (hintDir == 0)
  {
    lowerbound = hint;
    upperbound = 13;
  }
  else
  {
    lowerbound = 0;
    upperbound = hint;
  }

  do
  {
  ctx.reset_best_moves_lite();

    TIMER_START(TIMER_NO_AB, ini_depth);
  thrp->val = (* AB_ptr_trace_list[hand_rel_first])(
                  &thrp->lookAheadPos,
                  guess,
                  ini_depth,
          ctx);
    TIMER_END(TIMER_NO_AB, ini_depth);

#ifdef DDS_TOP_LEVEL
    DumpTopLevel(thrp->fileTopLevel.GetStream(),
      thrp, guess, lowerbound, upperbound, 1);
#endif

    if (thrp->val)
      lowerbound = guess++;
    else
      upperbound = --guess;

  }
  while (lowerbound < upperbound);

  futp->score[0] = lowerbound;
  {
    futp->nodes = ctx.search().trick_nodes();
  }

  
  thrp->memUsed = ctx.trans_table()->memory_in_use() +
                    ThreadMemoryUsed();

#ifdef DDS_TIMING
  thrp->timerList.PrintStats(thrp->fileTimerList.GetStream());
#endif

#ifdef DDS_TT_STATS
  // These are for the large TT -- empty if not.
  // thrp->transTable->PrintAllSuits(thrp->fileTTstats.GetStream());
  // thrp->transTable->PrintAllSuitStats(thrp->fileTTstats.GetStream());
  // thrp->transTable->PrintAllEntries(thrp->fileTTstats.GetStream());
  // thrp->transTable->PrintAllEntryStats(thrp->fileTTstats.GetStream());

  {
  ctx.trans_table()->print_summary_suit_stats(thrp->fileTTstats.GetStream());
  ctx.trans_table()->print_summary_entry_stats(thrp->fileTTstats.GetStream());
  }

  // These are for the small TT -- empty if not.
  {
  ctx.trans_table()->print_node_stats(thrp->fileTTstats.GetStream());
  ctx.trans_table()->print_reset_stats(thrp->fileTTstats.GetStream());
  }
#endif

// Diagnostics are routed via the SolverContext MoveGen facade.
#ifdef DDS_MOVES
  ctx.move_gen().print_trick_stats(thrp->fileMoves.GetStream());
#ifdef DDS_MOVES_DETAILS
  ctx.move_gen().print_trick_details(thrp->fileMoves.GetStream());
#endif
  ctx.move_gen().print_function_stats(thrp->fileMoves.GetStream());
#endif

#ifdef DDS_MEMORY_LEAKS_WIN32
  _CrtDumpMemoryLeaks();
#endif

  return RETURN_NO_FAULT;
}


auto board_range_checks(
  const Deal& dl,
  const int target,
  const int solutions,
  const int mode) -> int
{
  if (target < -1)
  {
    DumpInput(RETURN_TARGET_WRONG_LO, dl, target, solutions, mode);
    return RETURN_TARGET_WRONG_LO;
  }

  if (target > 13)
  {
    DumpInput(RETURN_TARGET_WRONG_HI, dl, target, solutions, mode);
    return RETURN_TARGET_WRONG_HI;
  }

  if (solutions < 1)
  {
    DumpInput(RETURN_SOLNS_WRONG_LO, dl, target, solutions, mode);
    return RETURN_SOLNS_WRONG_LO;
  }

  if (solutions > 3)
  {
    DumpInput(RETURN_SOLNS_WRONG_HI, dl, target, solutions, mode);
    return RETURN_SOLNS_WRONG_HI;
  }

  if (mode < 0)
  {
    DumpInput(RETURN_MODE_WRONG_LO, dl, target, solutions, mode);
    return RETURN_MODE_WRONG_LO;
  }

  if (mode > 2)
  {
    DumpInput(RETURN_MODE_WRONG_HI, dl, target, solutions, mode);
    return RETURN_MODE_WRONG_HI;
  }

  if (dl.trump < 0 || dl.trump > 4)
  {
    DumpInput(RETURN_TRUMP_WRONG, dl, target, solutions, mode);
    return RETURN_TRUMP_WRONG;
  }

  if (dl.first < 0 || dl.first > 3)
  {
    DumpInput(RETURN_FIRST_WRONG, dl, target, solutions, mode);
    return RETURN_FIRST_WRONG;
  }

  int rankSeen[3] = {0, 0, 0};
  for (int k = 0; k < 3; k++)
  {
    int r = dl.currentTrickRank[k];
    if (r == 0)
      continue;

    rankSeen[k] = 1;

    if (r < 2 || r > 14)
    {
      DumpInput(RETURN_SUIT_OR_RANK, dl, target, solutions, mode);
      return RETURN_SUIT_OR_RANK;
    }

    if (dl.currentTrickSuit[k] < 0 || dl.currentTrickSuit[k] > 3)
    {
      DumpInput(RETURN_SUIT_OR_RANK, dl, target, solutions, mode);
      return RETURN_SUIT_OR_RANK;
    }
  }

  if ((rankSeen[2] && (! rankSeen[1] || ! rankSeen[0])) ||
      (rankSeen[1] && ! rankSeen[0]))
  {
    DumpInput(RETURN_SUIT_OR_RANK, dl, target, solutions, mode);
    return RETURN_SUIT_OR_RANK;
  }

  for (int h = 0; h < DDS_HANDS; h++)
  {
    for (int s = 0; s < DDS_SUITS; s++)
    {
      unsigned c = dl.remainCards[h][s];
      if (c != 0 && (c < 0x0004 || c >= 0x8000))
      {
        DumpInput(RETURN_SUIT_OR_RANK, dl, target, solutions, mode);
        return RETURN_SUIT_OR_RANK;
      }
    }
  }

  return RETURN_NO_FAULT;
}


auto board_value_checks(
  SolverContext& ctx,
  const Deal& dl,
  const int target,
  const int solutions,
  const int mode) -> int
{
  auto thrp = ctx.thread();
  int cardCount = ctx.search().ini_depth() + 4;
  if (cardCount <= 0)
  {
    DumpInput(RETURN_ZERO_CARDS, dl, target, solutions, mode);
    return RETURN_ZERO_CARDS;
  }

  if (cardCount > 52)
  {
    DumpInput(RETURN_TOO_MANY_CARDS, dl, target, solutions, mode);
    return RETURN_TOO_MANY_CARDS;
  }

  int totalTricks;
  if (cardCount % 4)
    totalTricks = ((cardCount - 4) >> 2) + 2;
  else
    totalTricks = ((cardCount - 4) >> 2) + 1;

  if (totalTricks < target)
  {
    DumpInput(RETURN_TARGET_TOO_HIGH, dl, target, solutions, mode);
    return RETURN_TARGET_TOO_HIGH;
  }

  int hand_rel_first = thrp->lookAheadPos.hand_rel_first;

  int noOfCardsPerHand[DDS_HANDS] = {0, 0, 0, 0};
  for (int k = 0; k < hand_rel_first; k++)
    noOfCardsPerHand[HAND_ID(dl.first, k)] = 1;

  for (int h = 0; h < DDS_HANDS; h++)
    for (int s = 0; s < DDS_SUITS; s++)
  noOfCardsPerHand[h] += count_table[thrp->suit[h][s]];

  for (int h = 1; h < DDS_HANDS; h++)
  {
    if (noOfCardsPerHand[h] != noOfCardsPerHand[0])
    {
      DumpInput(RETURN_CARD_COUNT, dl, target, solutions, mode);
      return RETURN_CARD_COUNT;
    }
  }

  for (int k = 0; k < hand_rel_first; k++)
  {
    unsigned short int aggrRemain = 0;
    for (int h = 0; h < DDS_HANDS; h++)
      aggrRemain |= (dl.remainCards[h][dl.currentTrickSuit[k]] >> 2);

    if ((aggrRemain & bit_map_rank[dl.currentTrickRank[k]]) != 0)
    {
      DumpInput(RETURN_PLAYED_CARD, dl, target, solutions, mode);
      return RETURN_PLAYED_CARD;
    }
  }

  for (int s = 0; s < DDS_SUITS; s++)
  {
    for (int r = 2; r <= 14; r++)
    {
      bool found = false;
      for (int h = 0; h < DDS_HANDS; h++)
      {
        if ((thrp->suit[h][s] & bit_map_rank[r]) != 0)
        {
          if (found)
          {
            DumpInput(RETURN_DUPLICATE_CARDS, dl,
                      target, solutions, mode);
            return RETURN_DUPLICATE_CARDS;
          }
          else
            found = true;
        }
      }
    }
  }

  return RETURN_NO_FAULT;
}


auto last_trick_winner(
  const Deal& dl,
  const std::shared_ptr<ThreadData>& thrp,
  const int handToPlay,
  const int hand_rel_first,
  int& leadRank,
  int& leadSuit,
  int& leadSideWins) -> void
{
  int lastTrickSuit[DDS_HANDS],
      lastTrickRank[DDS_HANDS],
      h,
      hp;

  for (h = 0; h < hand_rel_first; h++)
  {
    hp = HAND_ID(dl.first, h);
    lastTrickSuit[hp] = dl.currentTrickSuit[h];
    lastTrickRank[hp] = dl.currentTrickRank[h];
  }

  for (h = hand_rel_first; h < DDS_HANDS; h++)
  {
    hp = HAND_ID(dl.first, h);
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

  int maxRank = 0,
      maxSuit,
      maxHand = -1;

  /* Highest trump? */
  if (dl.trump != DDS_NOTRUMP)
  {
    for (h = 0; h < DDS_HANDS; h++)
    {
      if ((lastTrickSuit[h] == dl.trump) &&
          (lastTrickRank[h] > maxRank))
      {
        maxRank = lastTrickRank[h];
        maxSuit = dl.trump;
        maxHand = h;
      }
    }
  }

  /* Highest card in leading suit */
  if (maxRank == 0)
  {
    maxRank = lastTrickRank[dl.first];
    maxSuit = lastTrickSuit[dl.first];
    maxHand = dl.first;

    for (h = 0; h < DDS_HANDS; h++)
    {
      if (lastTrickSuit[h] == maxSuit &&
          lastTrickRank[h] > maxRank)
      {
        maxHand = h;
        maxRank = lastTrickRank[h];
      }
    }
  }

  hp = HAND_ID(dl.first, hand_rel_first);
  leadRank = lastTrickRank[hp];
  leadSuit = lastTrickSuit[hp];
  leadSideWins = ((handToPlay == maxHand ||
    partner[handToPlay] == maxHand) ? 1 : 0);
}

