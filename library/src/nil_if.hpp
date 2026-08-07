/*
   Spades DDS - nil-mode entry point.

   Separate from solve_board_internal() rather than a mode flag on it. That
   keeps solver_if.cpp's object code untouched, and it lets DealNil pick a
   seat encoding whose zero value is safe rather than inheriting Deal's
   layout and its pre-existing C# ABI mismatch.
*/

#pragma once

#include <api/dll.h>
#include <solver_context/solver_context.hpp>

/// \brief Nil-mode solve modes.
enum class NilMode : int
{
  /// Exact V for every legal card at the root. What the ISMCTS ScoreMoves
  /// path consumes, so bounds are unacceptable anywhere in the returned set.
  Exact = 0,

  /// Resolve only whether the nil can be made - a single boolean query at
  /// guess = (T+1)^2 per card, stopping at the first card that makes it.
  /// Serves the bid-evaluation batch, which needs no trick counts.
  ///
  /// The scores returned in this mode are (T+1)^2 or 0 - the primary term
  /// alone, with both trick-count terms left at zero. Test them against
  /// (T+1)^2; do NOT pass them to nil_decode_value() expecting trick counts,
  /// which would report the zeroed terms as though they had been searched.
  /// Use Exact for anything that reads nilTricks or coverTricks.
  PrimaryOnly = 1
};

/// \brief Internal nil solve against an explicit context.
auto solve_board_nil_internal(
    SolverContext& ctx,
    const DealNil& dl,
    int mode,
    FutureTricks* futp) -> int;

/// \brief Decode a packed V produced by a nil solve.
///
/// Callers need T and the direction flag they passed in; both are echoed on
/// the DealNil they supplied, so no extra state has to be threaded through.
auto nil_decode_value(
    int value,
    int tricksTotal,
    int direction,
    int* nilMade,
    int* nilTricks,
    int* coverTricks,
    int* oppTricks) -> void;
