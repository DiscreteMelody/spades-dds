/// @file nil_mode_test.cpp
/// @brief Invariant tests for nil mode that need no external oracle.
/// @details Several properties in the oracle specification are self-checks -
///     they constrain the solver's output against itself rather than against
///     an independently derived value, so they can run before the oracle
///     exists and will keep running afterwards as cheap regression cover.
///
///     What is covered here:
///       * pack/decode round-trip on real solver output
///       * n + c + o == T for every returned card
///       * nilAlreadyBroken pins the primary term to "set" for every card
///       * flipping the direction flag leaves the PRIMARY term unchanged
///       * the zero-initialised DealNil is rejected rather than read as North
///
///     What is NOT covered here, and cannot be: whether the values are
///     CORRECT. Every assertion below would still pass if the search returned
///     a consistently wrong answer. That is the oracle's job.
///
///     The direction-flag test is the one worth watching. Whether the nil can
///     be made is a question about the primary term, which strictly dominates
///     the other two, so both sides optimise it first regardless of how ties
///     below it are broken. If flipping the flag ever changes the primary,
///     polarity has leaked somewhere it should not have - which is exactly the
///     failure mode that made reusing misereOn's plumbing unsafe.

#include <gtest/gtest.h>

#include <api/dds.h>
#include <api/dll.h>
#include <nil_objective.hpp>

namespace {

constexpr int kSpades = 0;
constexpr int kHearts = 1;
constexpr int kDiamonds = 2;
constexpr int kClubs = 3;

constexpr int kNorth = 0;
constexpr int kEast = 1;
constexpr int kSouth = 2;

// Rank bitmasks, matching examples/hands.cpp's convention.
constexpr unsigned int kR2 = 0x0004;
constexpr unsigned int kR3 = 0x0008;
constexpr unsigned int kR4 = 0x0010;
constexpr unsigned int kR5 = 0x0020;
constexpr unsigned int kR6 = 0x0040;
constexpr unsigned int kR7 = 0x0080;
constexpr unsigned int kR8 = 0x0100;

auto EmptyFutureTricks() -> FutureTricks
{
  FutureTricks f{};
  return f;
}

/// A two-trick position. North (the nil bidder) leads and holds a low spade
/// and a low club; the other three hands are arranged so that whether North
/// can avoid winning a trick is a real question rather than a foregone one.
auto TwoTrickDeal(const int nilSeatPlus1,
                  const int nilAlreadyBroken,
                  const int direction) -> DealNil
{
  DealNil dl{};
  dl.trump = kSpades;
  dl.first = kNorth;

  for (int k = 0; k < 3; k++)
  {
    dl.currentTrickSuit[k] = 0;
    dl.currentTrickRank[k] = 0;
  }

  for (int h = 0; h < DDS_HANDS; h++)
    for (int s = 0; s < DDS_SUITS; s++)
      dl.remainCards[h][s] = 0;

  dl.remainCards[kNorth][kSpades] = kR2;
  dl.remainCards[kNorth][kClubs] = kR3;

  dl.remainCards[kEast][kHearts] = kR4;
  dl.remainCards[kEast][kClubs] = kR5;

  dl.remainCards[kSouth][kSpades] = kR6;
  dl.remainCards[kSouth][kClubs] = kR7;

  dl.remainCards[3][kHearts] = kR8;
  dl.remainCards[3][kClubs] = kR2;

  dl.enforceTrumpBreak = 1;
  dl.trumpAlreadyBroken = 0;
  dl.nilSeatPlus1 = nilSeatPlus1;
  dl.nilAlreadyBroken = nilAlreadyBroken;
  dl.direction = direction;
  return dl;
}

constexpr int kT = 2;  // tricks in the fixture above

}  // namespace


TEST(NilMode, RejectsZeroInitialisedSeat)
{
  // The whole point of the +1 encoding: a zero-filled struct must fail loudly
  // rather than quietly solve for North.
  DealNil dl{};
  FutureTricks fut = EmptyFutureTricks();

  EXPECT_NE(SolveBoardNil(dl, 0, &fut, 0), RETURN_NO_FAULT);
}


TEST(NilMode, RejectsOutOfRangeSeat)
{
  FutureTricks fut = EmptyFutureTricks();

  DealNil low = TwoTrickDeal(0, 0, 1);
  EXPECT_NE(SolveBoardNil(low, 0, &fut, 0), RETURN_NO_FAULT);

  DealNil high = TwoTrickDeal(5, 0, 1);
  EXPECT_NE(SolveBoardNil(high, 0, &fut, 0), RETURN_NO_FAULT);
}


TEST(NilMode, EveryCardDecodesToAConsistentSplit)
{
  for (int seat = 1; seat <= 4; seat++)
  {
    for (int m = 0; m <= 1; m++)
    {
      DealNil dl = TwoTrickDeal(seat, 0, m);
      FutureTricks fut = EmptyFutureTricks();

      ASSERT_EQ(SolveBoardNil(dl, 0, &fut, 0), RETURN_NO_FAULT)
        << "seat=" << seat << " m=" << m;
      ASSERT_GT(fut.cards, 0);

      for (int i = 0; i < fut.cards; i++)
      {
        const int v = fut.score[i];

        EXPECT_GE(v, 0) << "packed values are never negative";
        EXPECT_LE(v, nil_mode::max_value(kT));

        const nil_mode::Decoded d = nil_mode::decode(v, kT, m != 0);

        EXPECT_GE(d.nilTricks, 0);
        EXPECT_GE(d.coverTricks, 0);
        EXPECT_GE(d.oppTricks, 0);
        EXPECT_EQ(d.nilTricks + d.coverTricks + d.oppTricks, kT)
          << "seat=" << seat << " m=" << m << " card=" << i << " V=" << v;

        // The primary term is definitional, not independent: made means the
        // nil seat took nothing.
        EXPECT_EQ(d.nilMade, d.nilTricks == 0)
          << "seat=" << seat << " m=" << m << " V=" << v;
      }
    }
  }
}


TEST(NilMode, AlreadyBrokenPinsPrimaryToSet)
{
  for (int seat = 1; seat <= 4; seat++)
  {
    for (int m = 0; m <= 1; m++)
    {
      DealNil dl = TwoTrickDeal(seat, 1, m);
      FutureTricks fut = EmptyFutureTricks();

      ASSERT_EQ(SolveBoardNil(dl, 0, &fut, 0), RETURN_NO_FAULT);
      ASSERT_GT(fut.cards, 0);

      for (int i = 0; i < fut.cards; i++)
      {
        const nil_mode::Decoded d =
          nil_mode::decode(fut.score[i], kT, m != 0);
        EXPECT_FALSE(d.nilMade)
          << "nilAlreadyBroken must pin the primary to 0; seat=" << seat
          << " m=" << m << " card=" << i;
      }
    }
  }
}


TEST(NilMode, DirectionDoesNotChangeWhetherNilCanBeMade)
{
  // Oracle spec §6: flipping m leaves the primary term unchanged for every
  // card. The secondary and tertiary are free to move.
  for (int seat = 1; seat <= 4; seat++)
  {
    FutureTricks futA = EmptyFutureTricks();
    FutureTricks futB = EmptyFutureTricks();

    DealNil a = TwoTrickDeal(seat, 0, 1);
    DealNil b = TwoTrickDeal(seat, 0, 0);

    ASSERT_EQ(SolveBoardNil(a, 0, &futA, 0), RETURN_NO_FAULT);
    ASSERT_EQ(SolveBoardNil(b, 0, &futB, 0), RETURN_NO_FAULT);

    ASSERT_EQ(futA.cards, futB.cards)
      << "the legal move set cannot depend on the direction flag";

    for (int i = 0; i < futA.cards; i++)
    {
      EXPECT_EQ(futA.suit[i], futB.suit[i]);
      EXPECT_EQ(futA.rank[i], futB.rank[i]);

      const bool madeA = nil_mode::decode(futA.score[i], kT, true).nilMade;
      const bool madeB = nil_mode::decode(futB.score[i], kT, false).nilMade;

      EXPECT_EQ(madeA, madeB)
        << "direction leaked into the primary term; seat=" << seat
        << " card=" << i;
    }
  }
}


TEST(NilMode, LastTrickPositionIsResolvedWithoutSearch)
{
  // cardCount == 4 drives ini_depth to zero. The shortcut must handle it;
  // falling through would search at a negative depth.
  DealNil dl{};
  dl.trump = kSpades;
  dl.first = kNorth;
  dl.remainCards[kNorth][kClubs] = kR3;
  dl.remainCards[kEast][kClubs] = kR5;
  dl.remainCards[kSouth][kClubs] = kR7;
  dl.remainCards[3][kClubs] = kR2;
  dl.enforceTrumpBreak = 1;
  dl.nilSeatPlus1 = kNorth + 1;
  dl.direction = 1;

  FutureTricks fut = EmptyFutureTricks();
  ASSERT_EQ(SolveBoardNil(dl, 0, &fut, 0), RETURN_NO_FAULT);
  ASSERT_EQ(fut.cards, 1);

  // South holds the club 7 and takes it, so North's nil survives.
  const nil_mode::Decoded d = nil_mode::decode(fut.score[0], 1, true);
  EXPECT_TRUE(d.nilMade);
  EXPECT_EQ(d.nilTricks, 0);
  EXPECT_EQ(d.coverTricks, 1);
  EXPECT_EQ(d.oppTricks, 0);
}
