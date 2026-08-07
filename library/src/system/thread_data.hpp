#ifndef DDS_THREAD_DATA_H
#define DDS_THREAD_DATA_H

#include <utility/debug.h>

#include <api/dds.h>
#include <moves/moves.hpp>
#include <string>

#ifdef DDS_AB_STATS
#include "ab_stats.hpp"
#endif

#if defined(DDS_TOP_LEVEL) || defined(DDS_AB_STATS) || defined(DDS_AB_HITS) || \
    defined(DDS_TT_STATS) || defined(DDS_TIMING) || defined(DDS_MOVES)
#include "file.hpp"
#endif

#ifdef DDS_TIMING
  #include <system/timer_list.hpp>
#endif
enum TTmemory
{
  DDS_TT_SMALL = 0,
  DDS_TT_LARGE = 1
};

struct WinnerEntryType
{
  int suit;
  int winnerRank;
  int winnerHand;
  int secondRank;
  int secondHand;
};

struct WinnersType
{
  int number;
  WinnerEntryType winner[4];
};


struct ThreadData
{
  int nodeTypeStore[DDS_HANDS];
  int iniDepth;
  bool val;

  unsigned short int suit[DDS_HANDS][DDS_SUITS];
  int trump;

  // Opt-in "trump must be broken to lead" house rule (e.g. Spades).
  // False reproduces classic, unrestricted bridge behaviour exactly.
  // Set from Deal::enforceTrumpBreak at the start of solve_board_internal().
  bool trumpBreakRuleOn = false;

  // Opt-in "minimum tricks" (misère) mode. False reproduces classic
  // maximum-tricks behaviour exactly. Set from Deal::misere at the start
  // of solve_board_internal(). See SearchContext::is_reference_hand() for
  // why this needs to be tracked separately from nodeTypeStore.
  bool misereOn = false;

  // Parity (hand & 1) of the reference side - the partnership containing
  // whichever hand is on play at the start of the solve - fixed for the
  // whole solve regardless of misereOn. Used by
  // SearchContext::is_reference_hand() to decide which hand's trick wins
  // count toward Pos::tricks_max. In vanilla (non-misère) solves this
  // always coincides with "which hands are MAXNODE", because the reference
  // side is always assigned MAXNODE there; misère solves invert the
  // MAXNODE/MINNODE assignment (see solve_board_internal()) while this
  // parity - and therefore what tricks_max counts - stays the same.
  int scoreParity = 0;

  Pos lookAheadPos; // Recursive alpha-beta data
  bool analysisFlag;
  unsigned short int lowestWin[50][DDS_SUITS];
  WinnersType winners[13];
  MoveType forbiddenMoves[14];
  MoveType bestMove[50];
  MoveType bestMoveTT[50];

  double memUsed;
  int nodes;
  int trickNodes;

  // Constant for a given hand.
  // 960 KB
  RelRanksType rel[8192];

  // Deferred TT configuration for context-owned construction
  // TransTable configuration moved to SolverContext::SolverConfig and
  // per-context member in SolverContext::SearchContext.

  Moves moves;

#ifdef DDS_TOP_LEVEL
  dds::File fileTopLevel;
#endif

#ifdef DDS_AB_STATS
  ABstats ABStats;
  dds::File fileABstats;
#endif

#ifdef DDS_AB_HITS
  dds::File fileRetrieved;
  dds::File fileStored;
#endif

#ifdef DDS_TT_STATS
  dds::File fileTTstats;
#endif 

#ifdef DDS_TIMING
  TimerList timerList;
  dds::File fileTimerList;
#endif

#ifdef DDS_MOVES
  dds::File fileMoves;
#endif

  // True after init_debug_files(); cleared by close_debug_files().
  bool debug_files_initialized_ = false;

  // Initialize per-thread debug/stat files. suffix is appended to each debug
  // prefix (e.g. "0.txt" from SolverContext serial + DDS_DEBUG_SUFFIX).
  void init_debug_files([[maybe_unused]] const std::string& suffix);

  // Close any open per-thread debug/stat files.
  void close_debug_files();

  auto debug_files_initialized() const -> bool
  {
    return debug_files_initialized_;
  }

  // ---- Nil mode ----------------------------------------------------------
  // Appended at the very END of the struct, deliberately. Every field above
  // keeps its offset, so translation units that never touch nil mode compile
  // to the same instructions they did before.
  //
  // The two counters would sit more naturally on Pos, next to tricks_max, but
  // Pos is embedded in this struct by value - growing it would shift every
  // member declared after lookAheadPos and perturb the existing search's
  // generated code for no functional reason. There is exactly one Pos per
  // thread, so holding the counters here instead is equivalent.
  //
  // All of these are ignored unless nilOn is true, and nilOn is only ever set
  // by the nil entry point in nil_if.cpp. The existing search never reads
  // them. Defaults make a zero-initialised ThreadData a valid non-nil
  // configuration.

  /// Opt-in per-seat nil objective. False reproduces existing behaviour exactly.
  bool nilOn = false;

  /// DDS hand index (0-3) of the nil bidder. Only meaningful when nilOn.
  /// A raw seat here, not the +1-encoded form used on the public DealNil
  /// struct - nil_if.cpp decodes it once at the boundary.
  int nilSeat = 0;

  /// The nil bidder has already won a trick EARLIER IN THE ROUND, before this
  /// position. Cards already played to the in-progress trick do not count.
  /// Pins the primary term to "set" for the whole search.
  bool nilAlreadyBroken = false;

  /// Direction flag. True = each side prefers more of its own tricks; false =
  /// fewer. Never changes node polarity - see nil_objective.hpp.
  bool nilDirection = true;

  /// Total tricks in the search, including any in-progress trick. Fixed at the
  /// root; the packing radix derives from it and it must not be recomputed per
  /// node.
  int nilTricksTotal = 0;

  /// Tricks won by the nil seat along the current search path. The nil
  /// analogue of Pos::tricks_max, but counting a SEAT by exact identity
  /// rather than a side by parity.
  int nilTricks = 0;

  /// Tricks won by the nil bidder's partner along the current search path.
  int nilCoverTricks = 0;
};


#endif // DDS_THREAD_DATA_H