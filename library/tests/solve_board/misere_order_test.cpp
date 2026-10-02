/// @file misere_order_test.cpp
/// @brief Control-arm tests for the misère ("shed") move ordering.
/// @details The misère ordering (heuristic_sorting.cpp, SetMisereMoveOrdering)
///     may change how fast a misère solve finishes, never what it returns.
///     These tests solve the same positions with the ordering off (the
///     classic DDS order, flags 0), fully on, and with each refinement on its
///     own, and check:
///
///       - every card's score is identical in every arm;
///       - with DDS_MISERE_ORDER_KEEP_ROOT the cards come back in exactly the
///         classic order too, so a caller that takes the first of several
///         equally good cards still gets the same one;
///       - maximum-tricks solves are untouched, node for node;
///       - the ordering is live (misère node counts do move).
///
///     Positions are 4-7 cards per hand, every seat on lead, 0-3 cards
///     already on the trick, spades trump with and without the
///     trump-must-be-broken rule, plus no-trump.

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <tuple>
#include <vector>

#include <api/dds.h>
#include <api/dll.h>

namespace {

struct Position
{
  unsigned int remain[DDS_HANDS][DDS_SUITS];
  int first;
  int trickSuit[3];
  int trickRank[3];
  int broken;
};

/// Deal `cards` per hand, then play `onTrick` cards of the current trick
/// legally (lowest card of a random suit to lead, lowest card of the led
/// suit, or of a random suit when void, to follow). When `breakRule` is on
/// the leader never leads a spade while it holds anything else.
auto MakePosition(
  const int cards,
  const int first,
  const int onTrick,
  const bool breakRule,
  const unsigned seed) -> Position
{
  std::vector<int> deck;
  for (int s = 0; s < DDS_SUITS; s++)
    for (int r = 2; r <= 14; r++)
      deck.push_back(s * 16 + r);
  std::mt19937 rng(seed);
  std::shuffle(deck.begin(), deck.end(), rng);

  Position p{};
  int k = 0;
  for (int h = 0; h < DDS_HANDS; h++)
    for (int c = 0; c < cards; c++, k++)
      p.remain[h][deck[k] / 16] |= (1u << (deck[k] % 16));
  p.first = first;
  p.broken = 0;

  auto lowestIn = [&](int h, int s) {
    for (int r = 2; r <= 14; r++)
      if (p.remain[h][s] & (1u << r))
        return r;
    return 0;
  };

  int led = -1;
  for (int i = 0; i < onTrick; i++)
  {
    const int h = (first + i) & 3;
    std::vector<int> suits;
    for (int s = 0; s < DDS_SUITS; s++)
    {
      if (! p.remain[h][s])
        continue;
      if (i > 0 && led >= 0 && p.remain[h][led] && s != led)
        continue;
      suits.push_back(s);
    }
    if (i == 0 && breakRule && suits.size() > 1)
      suits.erase(std::remove(suits.begin(), suits.end(), 0), suits.end());
    const int s = suits[rng() % suits.size()];
    const int r = lowestIn(h, s);
    if (i == 0)
      led = s;
    p.remain[h][s] &= ~(1u << r);
    p.trickSuit[i] = s;
    p.trickRank[i] = r;
  }
  return p;
}

auto MakeDeal(const Position& p, int trump, int breakRule, int misere) -> Deal
{
  Deal dl{};
  dl.trump = trump;
  dl.first = p.first;
  for (int i = 0; i < 3; i++)
  {
    dl.currentTrickSuit[i] = p.trickSuit[i];
    dl.currentTrickRank[i] = p.trickRank[i];
  }
  for (int h = 0; h < DDS_HANDS; h++)
    for (int s = 0; s < DDS_SUITS; s++)
      dl.remainCards[h][s] = p.remain[h][s];
  dl.enforceTrumpBreak = breakRule;
  dl.trumpAlreadyBroken = p.broken;
  dl.misere = misere;
  return dl;
}

auto Solve(const Deal& dl, int flags, int solutions) -> FutureTricks
{
  SetMisereMoveOrdering(flags);
  FutureTricks ft{};
  EXPECT_EQ(SolveBoard(dl, -1, solutions, 1, &ft, 0), RETURN_NO_FAULT);
  return ft;
}

auto CardScores(const FutureTricks& ft)
  -> std::vector<std::tuple<int, int, int, int>>
{
  std::vector<std::tuple<int, int, int, int>> v;
  for (int c = 0; c < ft.cards; c++)
    v.emplace_back(ft.suit[c], ft.rank[c], ft.equals[c], ft.score[c]);
  std::sort(v.begin(), v.end());
  return v;
}

auto InOrder(const FutureTricks& ft)
  -> std::vector<std::tuple<int, int, int, int>>
{
  std::vector<std::tuple<int, int, int, int>> v;
  for (int c = 0; c < ft.cards; c++)
    v.emplace_back(ft.suit[c], ft.rank[c], ft.equals[c], ft.score[c]);
  return v;
}

struct Strain
{
  int trump;
  int breakRule;
};
const Strain kStrains[] = { {0, 1}, {0, 0}, {DDS_NOTRUMP, 0} };

const int kArms[] = {
  DDS_MISERE_ORDER_DEFAULT,
  DDS_MISERE_ORDER_DEFAULT & ~DDS_MISERE_ORDER_KEEP_ROOT,
  DDS_MISERE_ORDER_SHED,
  DDS_MISERE_ORDER_SHED | DDS_MISERE_ORDER_BREAK_TRUMP,
  DDS_MISERE_ORDER_SHED | DDS_MISERE_ORDER_PARTNER_RUFF,
  DDS_MISERE_ORDER_SHED | DDS_MISERE_ORDER_LEAD_KILLER,
  DDS_MISERE_ORDER_SHED | DDS_MISERE_ORDER_KEEP_ROOT
};

class MisereOrderTest : public ::testing::Test
{
  protected:
    void TearDown() override
    {
      SetMisereMoveOrdering(DDS_MISERE_ORDER_DEFAULT);
    }
};

}  // namespace


TEST_F(MisereOrderTest, FlagsRoundTrip)
{
  SetMisereMoveOrdering(0);
  EXPECT_EQ(GetMisereMoveOrdering(), 0);
  SetMisereMoveOrdering(DDS_MISERE_ORDER_SHED);
  EXPECT_EQ(GetMisereMoveOrdering(), DDS_MISERE_ORDER_SHED);
  SetMisereMoveOrdering(-1);  // unknown bits are dropped
  EXPECT_EQ(GetMisereMoveOrdering(), DDS_MISERE_ORDER_DEFAULT);
}


// Every arm returns the same score for every card. With KEEP_ROOT the cards
// also come back in the classic order.
TEST_F(MisereOrderTest, ResultsIdenticalInEveryArm)
{
  long long nodesOff = 0, nodesOn = 0;
  unsigned seed = 4242;
  for (int cards = 4; cards <= 7; cards++)
  {
    for (int first = 0; first < DDS_HANDS; first++)
    {
      for (int onTrick = 0; onTrick <= 3; onTrick++)
      {
        for (const Strain& st : kStrains)
        {
          const Position p =
            MakePosition(cards, first, onTrick, st.breakRule != 0, seed++);
          const Deal dl = MakeDeal(p, st.trump, st.breakRule, 1);

          const FutureTricks off = Solve(dl, 0, 3);
          nodesOff += off.nodes;
          for (const int arm : kArms)
          {
            const FutureTricks on = Solve(dl, arm, 3);
            if (arm == DDS_MISERE_ORDER_DEFAULT)
              nodesOn += on.nodes;
            ASSERT_EQ(CardScores(off), CardScores(on))
              << "cards=" << cards << " first=" << first
              << " onTrick=" << onTrick << " trump=" << st.trump
              << " break=" << st.breakRule << " arm=" << arm;
            if (arm & DDS_MISERE_ORDER_KEEP_ROOT)
              EXPECT_EQ(InOrder(off), InOrder(on))
                << "cards=" << cards << " first=" << first
                << " onTrick=" << onTrick << " arm=" << arm;
          }

          // solutions=1: the same score, and with KEEP_ROOT the same card.
          const FutureTricks off1 = Solve(dl, 0, 1);
          const FutureTricks on1 = Solve(dl, DDS_MISERE_ORDER_DEFAULT, 1);
          EXPECT_EQ(InOrder(off1), InOrder(on1))
            << "cards=" << cards << " first=" << first
            << " onTrick=" << onTrick << " trump=" << st.trump;
        }
      }
    }
  }
  // The ordering is live: it must change (here: reduce) the work done.
  EXPECT_LT(nodesOn, nodesOff);
}


// Maximum-tricks solves never see the misère ordering.
TEST_F(MisereOrderTest, MaximumTricksUntouchedNodeForNode)
{
  unsigned seed = 777;
  for (int cards = 4; cards <= 7; cards++)
  {
    for (int first = 0; first < DDS_HANDS; first++)
    {
      for (int onTrick = 0; onTrick <= 3; onTrick += 3)
      {
        for (const Strain& st : kStrains)
        {
          const Position p =
            MakePosition(cards, first, onTrick, st.breakRule != 0, seed++);
          const Deal dl = MakeDeal(p, st.trump, st.breakRule, 0);
          const FutureTricks off = Solve(dl, 0, 3);
          const FutureTricks on = Solve(dl, DDS_MISERE_ORDER_DEFAULT, 3);
          EXPECT_EQ(InOrder(off), InOrder(on));
          EXPECT_EQ(off.nodes, on.nodes)
            << "cards=" << cards << " first=" << first
            << " trump=" << st.trump << " break=" << st.breakRule;
        }
      }
    }
  }
}
