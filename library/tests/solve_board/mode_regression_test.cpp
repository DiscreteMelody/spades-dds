/// @file mode_regression_test.cpp
/// @brief Cross-mode regression cover for maximise, misère and nil.
/// @details This suite exists to answer one question: does a change to the
///     search leave the three objectives producing the same answers, at the
///     same rough cost, as they did before the change?
///
///     It is a CHANGE DETECTOR, not a correctness oracle. Almost every
///     assertion below would still pass if a mode returned a consistently
///     wrong answer, exactly as nil_mode_test.cpp warns about itself. What it
///     will catch is a change that moves one mode relative to another, or
///     relative to its own recorded baseline - which is the failure mode that
///     matters when adding a transposition table or an estimator.
///
///     Deal sizes are 4-8 cards per hand so the whole suite stays inside a
///     "small" test budget.
///
///     Deliberately NOT asserted: any ordering between the misère value and
///     the maximise value. Measured counterexample at 7 cards/hand,
///     seed 12352, trump=spades, North leading: maximise returns 1, misère
///     returns 2. Both sides' preferences flip under misère, so it is a
///     different game rather than a sign-flipped one, and no inequality holds
///     in general. An earlier draft of this file asserted misère <= maximise
///     and was wrong.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <random>
#include <vector>

#include <api/dds.h>
#include <api/dll.h>
#include <nil_objective.hpp>

namespace {

constexpr int kNorth = 0;

/// Deterministic deal generator. Uses mt19937 with an explicit seed rather
/// than anything process-dependent - a shuffle that varies between runs would
/// make a baseline table meaningless.
void DealCards(
  const int cardsPerHand,
  const unsigned seed,
  unsigned int remain[DDS_HANDS][DDS_SUITS])
{
  std::vector<int> deck;
  for (int s = 0; s < DDS_SUITS; s++)
    for (int r = 2; r <= 14; r++)
      deck.push_back(s * 16 + r);

  std::mt19937 rng(seed);
  std::shuffle(deck.begin(), deck.end(), rng);

  for (int h = 0; h < DDS_HANDS; h++)
    for (int s = 0; s < DDS_SUITS; s++)
      remain[h][s] = 0;

  int k = 0;
  for (int h = 0; h < DDS_HANDS; h++)
    for (int c = 0; c < cardsPerHand; c++, k++)
      remain[h][deck[k] / 16] |= (1u << (deck[k] % 16));
}

auto MakeDeal(
  const unsigned int remain[DDS_HANDS][DDS_SUITS],
  const int trump,
  const int first,
  const int misere) -> Deal
{
  Deal dl{};
  dl.trump = trump;
  dl.first = first;
  for (int i = 0; i < 3; i++)
  {
    dl.currentTrickSuit[i] = 0;
    dl.currentTrickRank[i] = 0;
  }
  for (int h = 0; h < DDS_HANDS; h++)
    for (int s = 0; s < DDS_SUITS; s++)
      dl.remainCards[h][s] = remain[h][s];
  dl.enforceTrumpBreak = 0;
  dl.trumpAlreadyBroken = 0;
  dl.misere = misere;
  return dl;
}

auto MakeDealNil(
  const unsigned int remain[DDS_HANDS][DDS_SUITS],
  const int trump,
  const int first,
  const int nilSeat,
  const int direction) -> DealNil
{
  DealNil dn{};
  dn.trump = trump;
  dn.first = first;
  for (int i = 0; i < 3; i++)
  {
    dn.currentTrickSuit[i] = 0;
    dn.currentTrickRank[i] = 0;
  }
  for (int h = 0; h < DDS_HANDS; h++)
    for (int s = 0; s < DDS_SUITS; s++)
      dn.remainCards[h][s] = remain[h][s];
  dn.enforceTrumpBreak = 0;
  dn.trumpAlreadyBroken = 0;
  dn.nilSeatPlus1 = nilSeat + 1;
  dn.nilAlreadyBroken = 0;
  dn.direction = direction;
  return dn;
}

/// DDS returns cards best-first, so score[0] is the value of optimal play
/// whichever objective is in force. Taking a max over the array instead is
/// wrong under misère, where the best card carries the LOWEST score - an
/// earlier draft did exactly that and reported a false failure at 8 cards.
auto BestScore(const FutureTricks& ft) -> int
{
  return ft.score[0];
}

const int kSizes[] = { 4, 5, 6, 7, 8 };
constexpr unsigned kSeedBase = 12345u;

}  // namespace


// ---------------------------------------------------------------------------
// Forced play: with one card per hand there are no choices anywhere, so all
// three objectives are evaluating the same single line. Any disagreement is a
// trick-attribution or polarity bug, not a search bug. This is the cheapest
// test in the file and the one most likely to localise a regression.
// ---------------------------------------------------------------------------

TEST(ModeRegression, ForcedPlayAgreesAcrossModes)
{
  for (unsigned seed = 0; seed < 40; seed++)
  {
    unsigned int remain[DDS_HANDS][DDS_SUITS];
    DealCards(1, kSeedBase + seed, remain);

    for (int trump = 0; trump <= DDS_NOTRUMP; trump++)
    {
      FutureTricks ftMax{}, ftMis{}, ftNil{};

      Deal dlMax = MakeDeal(remain, trump, kNorth, 0);
      ASSERT_EQ(SolveBoard(dlMax, -1, 1, 1, &ftMax, 0), RETURN_NO_FAULT);

      Deal dlMis = MakeDeal(remain, trump, kNorth, 1);
      ASSERT_EQ(SolveBoard(dlMis, -1, 1, 1, &ftMis, 0), RETURN_NO_FAULT);

      DealNil dlNil = MakeDealNil(remain, trump, kNorth, kNorth, 1);
      ASSERT_EQ(SolveBoardNil(dlNil, 0, &ftNil, 0), RETURN_NO_FAULT);

      // One trick, no choices: maximise and misère must report the same
      // count for the leading side.
      EXPECT_EQ(BestScore(ftMax), BestScore(ftMis))
        << "seed=" << seed << " trump=" << trump;

      // Nil mode reports per SEAT; SolveBoard reports per SIDE. With North
      // as the nil seat, South is the cover hand, so nilTricks + coverTricks
      // is the same quantity SolveBoard returns for the leading side. This
      // asymmetry is the whole reason the estimators cannot be reused in nil
      // mode, so it is worth pinning down explicitly.
      ASSERT_GT(ftNil.cards, 0);
      const nil_mode::Decoded d =
        nil_mode::decode(ftNil.score[0], 1, true);
      EXPECT_EQ(d.nilTricks + d.coverTricks, BestScore(ftMax))
        << "seed=" << seed << " trump=" << trump;
    }
  }
}


// ---------------------------------------------------------------------------
// solutions=1 and solutions=3 are different code paths. A bug that only shows
// up under per-card enumeration is invisible to single-solve validation.
// ---------------------------------------------------------------------------

TEST(ModeRegression, SolutionModesAgreeOnBestScore)
{
  for (int cards : kSizes)
  {
    for (unsigned seed = 0; seed < 12; seed++)
    {
      unsigned int remain[DDS_HANDS][DDS_SUITS];
      DealCards(cards, kSeedBase + cards * 100 + seed, remain);

      for (int misere : { 0, 1 })
      {
        FutureTricks ft1{}, ft3{};
        Deal dl = MakeDeal(remain, 0, kNorth, misere);

        ASSERT_EQ(SolveBoard(dl, -1, 1, 1, &ft1, 0), RETURN_NO_FAULT);
        ASSERT_EQ(SolveBoard(dl, -1, 3, 1, &ft3, 0), RETURN_NO_FAULT);

        EXPECT_EQ(BestScore(ft1), BestScore(ft3))
          << "cards=" << cards << " seed=" << seed << " misere=" << misere;
      }
    }
  }
}


// ---------------------------------------------------------------------------
// Nil packed-value self-consistency across a corpus. nil_mode_test.cpp checks
// this on one fixture; here it runs over every returned card at every size.
// ---------------------------------------------------------------------------

TEST(ModeRegression, NilPackedValuesAreConsistent)
{
  for (int cards : kSizes)
  {
    for (unsigned seed = 0; seed < 12; seed++)
    {
      unsigned int remain[DDS_HANDS][DDS_SUITS];
      DealCards(cards, kSeedBase + cards * 100 + seed, remain);

      FutureTricks ft{};
      DealNil dn = MakeDealNil(remain, 0, kNorth, kNorth, 1);
      ASSERT_EQ(SolveBoardNil(dn, 0, &ft, 0), RETURN_NO_FAULT);
      ASSERT_GT(ft.cards, 0);

      for (int i = 0; i < ft.cards; i++)
      {
        const nil_mode::Decoded d =
          nil_mode::decode(ft.score[i], cards, true);

        EXPECT_EQ(d.nilTricks + d.coverTricks + d.oppTricks, cards)
          << "cards=" << cards << " seed=" << seed << " card=" << i;
        EXPECT_EQ(d.nilMade, d.nilTricks == 0)
          << "cards=" << cards << " seed=" << seed << " card=" << i;
        EXPECT_GE(d.nilTricks, 0);
        EXPECT_GE(d.coverTricks, 0);
        EXPECT_GE(d.oppTricks, 0);
      }
    }
  }
}


// ---------------------------------------------------------------------------
// PrimaryOnly (mode 1) resolves only whether the nil can be made. It must
// agree with Exact (mode 0) on that bit for the best card. This is the check
// that keeps the staged probe placement honest: the two modes place their
// probes differently and could diverge without it.
// ---------------------------------------------------------------------------

TEST(ModeRegression, NilPrimaryOnlyAgreesWithExact)
{
  for (int cards : kSizes)
  {
    for (unsigned seed = 0; seed < 12; seed++)
    {
      unsigned int remain[DDS_HANDS][DDS_SUITS];
      DealCards(cards, kSeedBase + cards * 100 + seed, remain);

      FutureTricks ftExact{}, ftPrimary{};
      DealNil dn = MakeDealNil(remain, 0, kNorth, kNorth, 1);

      ASSERT_EQ(SolveBoardNil(dn, 0, &ftExact, 0), RETURN_NO_FAULT);
      ASSERT_EQ(SolveBoardNil(dn, 1, &ftPrimary, 0), RETURN_NO_FAULT);
      ASSERT_GT(ftExact.cards, 0);
      ASSERT_GT(ftPrimary.cards, 0);

      int bestExact = ftExact.score[0];
      for (int i = 1; i < ftExact.cards; i++)
        bestExact = std::max(bestExact, ftExact.score[i]);

      const bool madeExact =
        nil_mode::decode(bestExact, cards, true).nilMade;

      int bestPrimary = ftPrimary.score[0];
      for (int i = 1; i < ftPrimary.cards; i++)
        bestPrimary = std::max(bestPrimary, ftPrimary.score[i]);

      const bool madePrimary =
        nil_mode::decode(bestPrimary, cards, true).nilMade;

      EXPECT_EQ(madeExact, madePrimary)
        << "cards=" << cards << " seed=" << seed;
    }
  }
}


// ---------------------------------------------------------------------------
// nilAlreadyBroken pins the primary term to "set" regardless of the cards.
// ---------------------------------------------------------------------------

TEST(ModeRegression, NilAlreadyBrokenForcesSet)
{
  for (int cards : { 4, 5, 6 })
  {
    for (unsigned seed = 0; seed < 8; seed++)
    {
      unsigned int remain[DDS_HANDS][DDS_SUITS];
      DealCards(cards, kSeedBase + cards * 100 + seed, remain);

      FutureTricks ft{};
      DealNil dn = MakeDealNil(remain, 0, kNorth, kNorth, 1);
      dn.nilAlreadyBroken = 1;
      ASSERT_EQ(SolveBoardNil(dn, 0, &ft, 0), RETURN_NO_FAULT);

      for (int i = 0; i < ft.cards; i++)
        EXPECT_FALSE(nil_mode::decode(ft.score[i], cards, true).nilMade)
          << "cards=" << cards << " seed=" << seed << " card=" << i;
    }
  }
}


// ---------------------------------------------------------------------------
// Cross-objective sequencing. Solving misère (or nil) in between two identical
// maximise solves must not change the maximise answer. Through the legacy
// SolveBoard entry point this passes trivially - a fresh context per call
// makes it immune by construction. It is here because it stops being trivial
// the moment any table outlives a call, which is exactly what a nil
// transposition table would introduce.
// ---------------------------------------------------------------------------

TEST(ModeRegression, InterleavedObjectivesDoNotContaminate)
{
  for (int cards : kSizes)
  {
    for (unsigned seed = 0; seed < 8; seed++)
    {
      unsigned int remain[DDS_HANDS][DDS_SUITS];
      DealCards(cards, kSeedBase + cards * 100 + seed, remain);

      Deal dlMax = MakeDeal(remain, 0, kNorth, 0);
      Deal dlMis = MakeDeal(remain, 0, kNorth, 1);
      DealNil dlNil = MakeDealNil(remain, 0, kNorth, kNorth, 1);

      FutureTricks a{}, b{}, c{}, d{};

      ASSERT_EQ(SolveBoard(dlMax, -1, 3, 1, &a, 0), RETURN_NO_FAULT);
      ASSERT_EQ(SolveBoard(dlMis, -1, 3, 1, &b, 0), RETURN_NO_FAULT);
      ASSERT_EQ(SolveBoardNil(dlNil, 0, &c, 0), RETURN_NO_FAULT);
      ASSERT_EQ(SolveBoard(dlMax, -1, 3, 1, &d, 0), RETURN_NO_FAULT);

      ASSERT_EQ(a.cards, d.cards) << "cards=" << cards << " seed=" << seed;
      for (int i = 0; i < a.cards; i++)
      {
        EXPECT_EQ(a.suit[i], d.suit[i]);
        EXPECT_EQ(a.rank[i], d.rank[i]);
        EXPECT_EQ(a.score[i], d.score[i]);
      }
    }
  }
}


// ---------------------------------------------------------------------------
// Repeatability. Same deal, same answer, every time. Guards against anything
// that leaks state between solves - uninitialised fields, a table that is not
// being cleared, a generation counter that wraps.
// ---------------------------------------------------------------------------

TEST(ModeRegression, RepeatedSolvesAreIdentical)
{
  for (int cards : kSizes)
  {
    unsigned int remain[DDS_HANDS][DDS_SUITS];
    DealCards(cards, kSeedBase + cards, remain);

    DealNil dn = MakeDealNil(remain, 0, kNorth, kNorth, 1);

    FutureTricks first{};
    ASSERT_EQ(SolveBoardNil(dn, 0, &first, 0), RETURN_NO_FAULT);

    for (int rep = 0; rep < 3; rep++)
    {
      FutureTricks again{};
      ASSERT_EQ(SolveBoardNil(dn, 0, &again, 0), RETURN_NO_FAULT);
      ASSERT_EQ(first.cards, again.cards) << "cards=" << cards;
      for (int i = 0; i < first.cards; i++)
      {
        EXPECT_EQ(first.suit[i], again.suit[i]);
        EXPECT_EQ(first.rank[i], again.rank[i]);
        EXPECT_EQ(first.score[i], again.score[i]);
      }
    }
  }
}


// ---------------------------------------------------------------------------
// Performance guard. Not an assertion on absolute speed - that would be
// machine-dependent and flaky in CI. It asserts only a generous ceiling that
// a genuine regression (an estimator disabled by accident, a table that stops
// hitting) would blow through, and prints the numbers so a baseline table can
// be kept by hand.
// ---------------------------------------------------------------------------

TEST(ModeRegression, TimingBaseline)
{
  constexpr double kCeilingMs = 5000.0;

  for (int cards : kSizes)
  {
    unsigned int remain[DDS_HANDS][DDS_SUITS];
    DealCards(cards, kSeedBase + cards, remain);

    const auto timed = [](auto&& fn) {
      const auto t0 = std::chrono::steady_clock::now();
      fn();
      const auto t1 = std::chrono::steady_clock::now();
      return std::chrono::duration<double, std::milli>(t1 - t0).count();
    };

    FutureTricks ftMax{}, ftMis{}, ftNil{};
    Deal dlMax = MakeDeal(remain, 0, kNorth, 0);
    Deal dlMis = MakeDeal(remain, 0, kNorth, 1);
    DealNil dlNil = MakeDealNil(remain, 0, kNorth, kNorth, 1);

    const double msMax = timed([&] { SolveBoard(dlMax, -1, 3, 1, &ftMax, 0); });
    const double msMis = timed([&] { SolveBoard(dlMis, -1, 3, 1, &ftMis, 0); });
    const double msNil = timed([&] { SolveBoardNil(dlNil, 0, &ftNil, 0); });

    std::printf("[ timing  ] cards=%d  max=%.1fms  misere=%.1fms  nil=%.1fms\n",
                cards, msMax, msMis, msNil);

    EXPECT_LT(msMax, kCeilingMs) << "cards=" << cards;
    EXPECT_LT(msMis, kCeilingMs) << "cards=" << cards;
    EXPECT_LT(msNil, kCeilingMs) << "cards=" << cards;
  }
}
