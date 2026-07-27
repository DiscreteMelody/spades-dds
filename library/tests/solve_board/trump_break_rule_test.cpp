/// @file trump_break_rule_test.cpp
/// @brief Regression tests for the opt-in "trump must be broken to lead"
///     house rule (as used in Spades), added via Deal::enforceTrumpBreak
///     and Deal::trumpAlreadyBroken.
/// @details Every test uses the same 8-card (2 tricks) position with
///     spades as trump and North on lead holding one spade and one club,
///     so that "does North's candidate list ever include leading a spade"
///     is a direct, unambiguous probe of the move-generation restriction.
///     solutions == 3 is used throughout because it returns every legal
///     candidate card individually, not just the top-scoring ones (see
///     the "Target and mode don't matter; all cards" comment in
///     solver_if.cpp), so a missing suit means it was never generated as
///     a candidate at all - not merely that it scored worse.

#include <gtest/gtest.h>

#include <cstring>

#include <api/dds.h>
#include <moves/moves.hpp>

namespace {

constexpr int kSpades = 0;
constexpr int kHearts = 1;
constexpr int kDiamonds = 2;
constexpr int kClubs = 3;

constexpr int kNorth = 0;
constexpr int kEast = 1;
constexpr int kSouth = 2;
constexpr int kWest = 3;

// Rank bitmasks, matching examples/hands.cpp's convention.
constexpr unsigned int kR2 = 0x0004;
constexpr unsigned int kR3 = 0x0008;
constexpr unsigned int kR4 = 0x0010;
constexpr unsigned int kRQ = 0x1000;

auto EmptyFutureTricks() -> FutureTricks
{
  return FutureTricks{
    .nodes = 0,
    .cards = 0,
    .suit = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    .rank = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    .equals = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    .score = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
  };
}

// Whether any of the (fut.cards) returned candidates is of the given suit.
auto ContainsSuit(const FutureTricks& fut, int suit) -> bool
{
  for (int i = 0; i < fut.cards; i++) {
    if (fut.suit[i] == suit)
      return true;
  }
  return false;
}

} // namespace

class TrumpBreakRuleTests : public ::testing::Test
{
protected:
  TrumpBreakRuleTests() = default;

  // North: spade Q, club 2. East: hearts 2,3. South: diamonds 2,3.
  // West: spade 2, club 3. Trump is spades, North on lead, fresh trick.
  // North therefore has a genuine choice between leading the trump suit
  // (SQ) and leading a side suit (C2).
  struct Deal BaseDeal() const
  {
    return Deal{
      .trump = kSpades,
      .first = kNorth,
      .currentTrickSuit = {0, 0, 0},
      .currentTrickRank = {0, 0, 0},
      .remainCards = {
        { kRQ, 0, 0, kR2 },      // North: SQ, C2
        { 0, kR2 | kR3, 0, 0 },  // East: H2, H3
        { 0, 0, kR2 | kR3, 0 },  // South: D2, D3
        { kR2, 0, 0, kR3 },      // West: S2, C3
      },
    };
  }
};

/// With the rule on and trump not yet broken, North holds a non-trump
/// alternative (C2), so SQ must never appear as a lead candidate.
TEST_F(TrumpBreakRuleTests, TrumpExcludedWhenUnbrokenAndAlternativeExists)
{
  struct Deal dl = BaseDeal();
  dl.enforceTrumpBreak = 1;
  dl.trumpAlreadyBroken = 0;

  FutureTricks fut = EmptyFutureTricks();
  auto ret = SolveBoard(dl, /*target=*/-1, /*solutions=*/3, /*mode=*/0,
                         &fut, /*thrId=*/0);

  ASSERT_EQ(RETURN_NO_FAULT, ret);
  EXPECT_FALSE(ContainsSuit(fut, kSpades));
  EXPECT_TRUE(ContainsSuit(fut, kClubs));
  EXPECT_EQ(1, fut.cards);
}

/// Once trump is already broken, the restriction lifts: both the spade
/// and the club are legal candidates again, exactly as in classic bridge.
TEST_F(TrumpBreakRuleTests, TrumpAllowedOnceAlreadyBroken)
{
  struct Deal dl = BaseDeal();
  dl.enforceTrumpBreak = 1;
  dl.trumpAlreadyBroken = 1;

  FutureTricks fut = EmptyFutureTricks();
  auto ret = SolveBoard(dl, /*target=*/-1, /*solutions=*/3, /*mode=*/0,
                         &fut, /*thrId=*/0);

  ASSERT_EQ(RETURN_NO_FAULT, ret);
  EXPECT_TRUE(ContainsSuit(fut, kSpades));
  EXPECT_TRUE(ContainsSuit(fut, kClubs));
  EXPECT_EQ(2, fut.cards);
}

/// The rule is opt-in: leaving enforceTrumpBreak at 0 (its default, e.g.
/// for callers built against an older header) reproduces classic,
/// unrestricted bridge behaviour exactly, matching TrumpAllowedOnceAlreadyBroken.
TEST_F(TrumpBreakRuleTests, ClassicBehaviourUnaffectedByDefault)
{
  struct Deal dl = BaseDeal();
  dl.enforceTrumpBreak = 0;
  dl.trumpAlreadyBroken = 0;

  FutureTricks fut = EmptyFutureTricks();
  auto ret = SolveBoard(dl, /*target=*/-1, /*solutions=*/3, /*mode=*/0,
                         &fut, /*thrId=*/0);

  ASSERT_EQ(RETURN_NO_FAULT, ret);
  EXPECT_TRUE(ContainsSuit(fut, kSpades));
  EXPECT_TRUE(ContainsSuit(fut, kClubs));
  EXPECT_EQ(2, fut.cards);
}

/// If the leader holds nothing but trump, the rule's own exception
/// applies: leading trump is allowed (indeed unavoidable) even though it
/// has not been broken yet. Both of North's spades legitimately show up:
/// since no other hand holds any spade, DDS reports them as a single
/// equivalence group (Q with rank 4 folded into its "equals" bitmask)
/// rather than as two separate top-level candidates - that grouping is
/// standard DDS behaviour, not this house rule, so we check for it
/// explicitly rather than asserting a flat card count.
TEST_F(TrumpBreakRuleTests, TrumpAllowedWhenLeaderHasNoOtherSuit)
{
  struct Deal dl{
    .trump = kSpades,
    .first = kNorth,
    .currentTrickSuit = {0, 0, 0},
    .currentTrickRank = {0, 0, 0},
    .remainCards = {
      { kRQ | kR4, 0, 0, 0 },  // North: SQ, S4 (spades only)
      { 0, kR2 | kR3, 0, 0 },  // East: H2, H3
      { 0, 0, kR2 | kR3, 0 },  // South: D2, D3
      { 0, 0, 0, kR2 | kR3 },  // West: C2, C3
    },
  };
  dl.enforceTrumpBreak = 1;
  dl.trumpAlreadyBroken = 0;

  FutureTricks fut = EmptyFutureTricks();
  auto ret = SolveBoard(dl, /*target=*/-1, /*solutions=*/3, /*mode=*/0,
                         &fut, /*thrId=*/0);

  ASSERT_EQ(RETURN_NO_FAULT, ret);
  ASSERT_TRUE(ContainsSuit(fut, kSpades));

  // Find the spade entry and confirm both SQ (rank 12) and S4 (rank 4,
  // via the equals bitmask) were legitimately considered.
  bool foundQueenWithFourEquiv = false;
  for (int i = 0; i < fut.cards; i++) {
    if (fut.suit[i] == kSpades && fut.rank[i] == 12 &&
        (fut.equals[i] & kR4) != 0) {
      foundQueenWithFourEquiv = true;
    }
  }
  EXPECT_TRUE(foundQueenWithFourEquiv);
}

// --------------------------------------------------------------------
// White-box tests: exercise Moves::Init/MakeSpecific/TrumpBroken
// directly, independent of the full alpha-beta search. The full search
// interleaves iterative deepening, a transposition table, and
// quick-tricks/later-tricks shortcuts in an order that is not a simple
// top-to-bottom trace of one game, which makes it hard to read a broken-
// state propagation bug out of instrumented search output with
// confidence. Testing the underlying mechanism directly removes that
// ambiguity.
// --------------------------------------------------------------------

/// Once a trump card is played to a trick that was not led in trump (a
/// discard or ruff), the broken state must be carried forward into the
/// next trick's slot - this is what lets that next trick's leader freely
/// lead trump even though the position's original input said trump had
/// not yet been broken.
TEST(TrumpBreakPropagationTests, BrokenStateCarriesToNextTrick)
{
  Moves moves;
  unsigned short rank_in_suit[DDS_HANDS][DDS_SUITS] = {};
  rank_in_suit[kNorth][kClubs] = kR2;
  rank_in_suit[kEast][kHearts] = kR2;
  rank_in_suit[kSouth][kDiamonds] = kR2;
  rank_in_suit[kWest][kSpades] = kR3;

  int initialRanks[3] = {0, 0, 0};
  int initialSuits[3] = {0, 0, 0};

  const int trick = 2;
  moves.Init(trick, /*relStartHand=*/0, initialRanks, initialSuits,
             rank_in_suit, /*trump=*/kSpades, /*leadHand=*/kNorth,
             /*trumpBreakRuleOn=*/true, /*trumpAlreadyBroken=*/false);

  ASSERT_FALSE(moves.TrumpBroken(trick));

  // North leads clubs; East and South follow in their own (irrelevant)
  // suits; West, void in clubs, ruffs with its only card - a spade.
  moves.MakeSpecific(MoveType{kClubs, 2, 0, 0}, trick, 0);
  moves.MakeSpecific(MoveType{kHearts, 2, 0, 0}, trick, 1);
  moves.MakeSpecific(MoveType{kDiamonds, 2, 0, 0}, trick, 2);
  moves.MakeSpecific(MoveType{kSpades, 3, 0, 0}, trick, 3);

  EXPECT_TRUE(moves.TrumpBroken(trick - 1));
}

/// If no trump card is played during a trick, the broken state must stay
/// false going into the next trick - the flag must not drift true on its
/// own, and must not be set merely because trump was led (this test uses
/// a trick that never touches the trump suit at all).
TEST(TrumpBreakPropagationTests, StaysUnbrokenWhenNoTrumpPlayed)
{
  Moves moves;
  unsigned short rank_in_suit[DDS_HANDS][DDS_SUITS] = {};
  rank_in_suit[kNorth][kClubs] = kR2;
  rank_in_suit[kEast][kHearts] = kR2;
  rank_in_suit[kSouth][kDiamonds] = kR2;
  rank_in_suit[kWest][kClubs] = kR3;

  int initialRanks[3] = {0, 0, 0};
  int initialSuits[3] = {0, 0, 0};

  const int trick = 2;
  moves.Init(trick, /*relStartHand=*/0, initialRanks, initialSuits,
             rank_in_suit, /*trump=*/kSpades, /*leadHand=*/kNorth,
             /*trumpBreakRuleOn=*/true, /*trumpAlreadyBroken=*/false);

  // North leads clubs; East and South discard in their own suits; West
  // follows suit with its own club - nobody touches spades this trick.
  moves.MakeSpecific(MoveType{kClubs, 2, 0, 0}, trick, 0);
  moves.MakeSpecific(MoveType{kHearts, 2, 0, 0}, trick, 1);
  moves.MakeSpecific(MoveType{kDiamonds, 2, 0, 0}, trick, 2);
  moves.MakeSpecific(MoveType{kClubs, 3, 0, 0}, trick, 3);

  EXPECT_FALSE(moves.TrumpBroken(trick - 1));
}

// --------------------------------------------------------------------
// Confirms the PBN entry point (SolveBoardPBN / DealPBN) threads the
// same two fields through correctly, using a human-readable hand
// instead of raw bitmasks. Same position as
// TrumpExcludedWhenUnbrokenAndAlternativeExists, just built from PBN.
// --------------------------------------------------------------------

TEST(TrumpBreakRulePBNTests, WorksViaSolveBoardPBN)
{
  DealPBN dl{};
  dl.trump = kSpades;
  dl.first = kNorth;
  // North: SQ, C2. East: H2,H3. South: D2,D3. West: S2,C3.
  std::strcpy(dl.remainCards, "N:Q...2 .32.. ..32. 2...3");

  dl.enforceTrumpBreak = 1;
  dl.trumpAlreadyBroken = 0;

  FutureTricks fut{};
  auto ret = SolveBoardPBN(dl, /*target=*/-1, /*solutions=*/3, /*mode=*/0,
                            &fut, /*thrId=*/0);

  ASSERT_EQ(RETURN_NO_FAULT, ret);
  EXPECT_FALSE(ContainsSuit(fut, kSpades));
  EXPECT_TRUE(ContainsSuit(fut, kClubs));
  EXPECT_EQ(1, fut.cards);
}
