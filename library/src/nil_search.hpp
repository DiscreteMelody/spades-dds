/*
   Spades DDS - nil-mode search.

   A separate alpha-beta search for the packed nil objective V. This runs
   BESIDE the existing search rather than inside it: ab_search.cpp is not
   modified, so the existing maximise and misere paths keep their object code
   byte for byte and cannot be perturbed by anything here.

   The duplication is deliberate. The nil objective changes the terminal
   cutoffs, the crediting granularity (seat, not side) and the node-type
   anchor, so a shared implementation would need runtime branches in the
   innermost test of the engine. See docs/nil-mode-map.md §1 and §3.

   What is NOT duplicated: move generation, the trump-break machinery, the
   make_* / undo_* helpers (all non-static and declared in ab_search.hpp), the
   PBN parser and the winners bookkeeping. Those are objective-independent and
   shared unchanged.

   Phase 2 scope: no transposition table, no QuickTricks, no LaterTricks.
*/

#pragma once

#include <api/dds.h>
#include <solver_context/solver_context.hpp>

/// \brief Can the nil side force the packed value to reach \p guess?
///
/// The predicate the whole nil search is built on. Mirrors ab_search()'s
/// contract - a boolean, never a value - but the question is about V rather
/// than about a side's trick count.
///
/// \param pos_point       current look-ahead position
/// \param guess           threshold on V
/// \param depth           remaining search depth, must be non-negative
/// \param hand_rel_first  seat to act, relative to the leader of the current
///                        trick (0-3). Selects the specialisation, exactly as
///                        AB_ptr_list[] does in solver_if.cpp.
/// \param ctx             solver context, with nil configuration already applied
/// \return true when the nil side can force V >= guess
auto nil_ab_search(
    Pos* pos_point,
    int guess,
    int depth,
    int hand_rel_first,
    SolverContext& ctx) -> bool;

/// \brief Exact terminal value of a completed position.
///
/// Resolves the final trick, credits it to the seat that actually won it, and
/// packs the result. Also fills \p win_ranks_out, so the trick winner is
/// determined exactly once. Exposed for unit testing against the oracle.
auto nil_evaluate(
    const Pos* pos_point,
    int trump,
    SolverContext& ctx,
    unsigned short win_ranks_out[DDS_SUITS]) -> int;
