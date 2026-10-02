/// @file unbroken_trump_table_test.cpp
/// @brief Control-arm tests for the transposition table while trump is
///     unbroken (SetUnbrokenTrumpTable).
/// @details With the trump-must-be-broken rule on, the table used to be off
///     until trump was broken. It is now on, with unbroken-trump entries kept
///     apart from broken ones in the key (ab_search_0_ctx). The table may only
///     change how fast a solve finishes, never what it returns. These tests
///     check:
///
///       - every card's score, and the order the cards come back in, are
///         identical with the table on and off, in both objectives, at every
///         seat and with 0-3 cards on the trick;
///       - one context that keeps its table across solves gives the same
///         answers as fresh contexts when the same cards are solved first
///         with trump unbroken and then with trump broken (and back). This
///         is the case the key bit exists for: the same cards, two
///         different games, one table;
///       - without the rule the setting is inert, node for node;
///       - the table is live (node counts move).

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <tuple>
#include <vector>

#include <api/dds.h>
#include <api/dll.h>
#include <api/solve_board.hpp>
#include <solver_context/solver_context.hpp>

namespace {

struct Position
{
  unsigned int remain[DDS_HANDS][DDS_SUITS];
  int first;
  int trickSuit[3];
  int trickRank[3];
};

/// Deal `cards` per hand and play `onTrick` cards of the current trick
/// legally, never leading a spade while the leader holds anything else.
auto MakePosition(
  const int cards,
  const int first,
  const int onTrick,
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

  int led = -1;
  for (int i = 0; i < onTrick; i++)
  {
    const int h = (first + i) & 3;
    std::vector<int> suits;
    for (int s = 0; s < DDS_SUITS; s++)
    {
      if (! p.remain[h][s])
        continue;
      if (i > 0 && p.remain[h][led] && s != led)
        continue;
      suits.push_back(s);
    }
    if (i == 0 && suits.size() > 1)
      suits.erase(std::remove(suits.begin(), suits.end(), 0), suits.end());
    const int s = suits[rng() % suits.size()];
    int r = 2;
    while (! (p.remain[h][s] & (1u << r)))
      r++;
    if (i == 0)
      led = s;
    p.remain[h][s] &= ~(1u << r);
    p.trickSuit[i] = s;
    p.trickRank[i] = r;
  }
  return p;
}

auto MakeDeal(const Position& p, int breakRule, int broken, int misere) -> Deal
{
  Deal dl{};
  dl.trump = 0;
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
  dl.trumpAlreadyBroken = broken;
  dl.misere = misere;
  return dl;
}

auto InOrder(const FutureTricks& ft)
  -> std::vector<std::tuple<int, int, int, int>>
{
  std::vector<std::tuple<int, int, int, int>> v;
  for (int c = 0; c < ft.cards; c++)
    v.emplace_back(ft.suit[c], ft.rank[c], ft.equals[c], ft.score[c]);
  return v;
}

auto Solve(const Deal& dl, int table, int solutions) -> FutureTricks
{
  SetUnbrokenTrumpTable(table);
  FutureTricks ft{};
  EXPECT_EQ(SolveBoard(dl, -1, solutions, 1, &ft, 0), RETURN_NO_FAULT);
  return ft;
}

class UnbrokenTrumpTableTest : public ::testing::Test
{
  protected:
    void TearDown() override
    {
      SetUnbrokenTrumpTable(1);
    }
};

}  // namespace


TEST_F(UnbrokenTrumpTableTest, FlagRoundTrip)
{
  SetUnbrokenTrumpTable(0);
  EXPECT_EQ(GetUnbrokenTrumpTable(), 0);
  SetUnbrokenTrumpTable(7);
  EXPECT_EQ(GetUnbrokenTrumpTable(), 1);
}


// Same answers, same card order, with the table on and off.
TEST_F(UnbrokenTrumpTableTest, ResultsIdenticalOnAndOff)
{
  long long nodesOff = 0, nodesOn = 0;
  unsigned seed = 9001;
  for (int cards = 4; cards <= 8; cards++)
  {
    for (int first = 0; first < DDS_HANDS; first++)
    {
      for (int onTrick = 0; onTrick <= 3; onTrick++)
      {
        const Position p = MakePosition(cards, first, onTrick, seed++);
        for (int misere = 0; misere <= 1; misere++)
        {
          const Deal dl = MakeDeal(p, 1, 0, misere);
          const FutureTricks off = Solve(dl, 0, 3);
          const FutureTricks on = Solve(dl, 1, 3);
          nodesOff += off.nodes;
          nodesOn += on.nodes;
          EXPECT_EQ(InOrder(off), InOrder(on))
            << "cards=" << cards << " first=" << first
            << " onTrick=" << onTrick << " misere=" << misere;

          const FutureTricks off1 = Solve(dl, 0, 1);
          const FutureTricks on1 = Solve(dl, 1, 1);
          EXPECT_EQ(InOrder(off1), InOrder(on1))
            << "cards=" << cards << " first=" << first
            << " onTrick=" << onTrick << " misere=" << misere;
        }
      }
    }
  }
  EXPECT_LT(nodesOn, nodesOff);
}


// One context, one table, the same cards solved as two different games.
// Every answer must equal a fresh-context solve of the same game.
TEST_F(UnbrokenTrumpTableTest, SharedTableKeepsBrokenAndUnbrokenApart)
{
  SetUnbrokenTrumpTable(1);
  unsigned seed = 31337;
  int differing = 0;
  for (int cards = 5; cards <= 8; cards++)
  {
    for (int first = 0; first < DDS_HANDS; first++)
    {
      const Position p = MakePosition(cards, first, 0, seed++);
      for (int misere = 0; misere <= 1; misere++)
      {
        SolverContext ctx;
        // unbroken, broken, unbroken again: each later solve meets entries
        // the earlier ones left in the shared table.
        const int sequence[] = { 0, 1, 0, 1 };
        for (const int broken : sequence)
        {
          for (const int mode : { 1, 2 })
          {
            const Deal dl = MakeDeal(p, 1, broken, misere);
            FutureTricks shared{}, fresh{};
            ASSERT_EQ(SolveBoard(ctx, dl, -1, 3, mode, &shared),
              RETURN_NO_FAULT);
            ASSERT_EQ(SolveBoard(dl, -1, 3, 1, &fresh, 0), RETURN_NO_FAULT);
            EXPECT_EQ(InOrder(fresh), InOrder(shared))
              << "cards=" << cards << " first=" << first
              << " misere=" << misere << " broken=" << broken
              << " mode=" << mode;
          }
        }
        FutureTricks u{}, b{};
        SolveBoard(MakeDeal(p, 1, 0, misere), -1, 3, 1, &u, 0);
        SolveBoard(MakeDeal(p, 1, 1, misere), -1, 3, 1, &b, 0);
        if (InOrder(u) != InOrder(b))
          differing++;
      }
    }
  }
  // The two games must actually differ somewhere, or the test proves little.
  EXPECT_GT(differing, 0);
}


// Without the break rule the setting has nothing to do.
TEST_F(UnbrokenTrumpTableTest, InertWithoutTheRule)
{
  unsigned seed = 4711;
  for (int cards = 4; cards <= 7; cards++)
  {
    for (int first = 0; first < DDS_HANDS; first++)
    {
      const Position p = MakePosition(cards, first, first & 3, seed++);
      for (int misere = 0; misere <= 1; misere++)
      {
        const Deal dl = MakeDeal(p, 0, 0, misere);
        const FutureTricks off = Solve(dl, 0, 3);
        const FutureTricks on = Solve(dl, 1, 3);
        EXPECT_EQ(InOrder(off), InOrder(on));
        EXPECT_EQ(off.nodes, on.nodes);
      }
    }
  }
}
