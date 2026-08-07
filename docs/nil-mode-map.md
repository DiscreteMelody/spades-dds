# Nil-Mode Code Map

Read-only survey of the engine as it exists at `138b9cc`, written to support the nil-mode
implementation. Every line number below was read from the tree at that commit. No code is
changed by this document.

Scope is the six areas the nil work touches: the boolean-search contract, trick crediting,
node-type anchoring, transposition-table keying, estimator call sites, and the
move-generation / trump-break boundary.

---

## 1. The search is a boolean predicate

`ab_search_N_ctx(Pos* posPoint, int target, int depth, SolverContext& ctx)` returns
`bool`. The question is always *can the reference side take at least `target` tricks from
here?* No trick count is ever returned.

Function layout in `library/src/ab_search.cpp`:

| Function | Lines | Role |
|---|---|---|
| `ab_search_0_ctx` | 183–565 | Leader to a trick. TT, estimators, threshold cutoffs live here. |
| `ab_search_1_ctx` | 566–665 | Second hand. |
| `ab_search_2_ctx` | 666–754 | Third hand. |
| `ab_search_3_ctx` | 755–974 | Fourth hand — the trick completes, so this is where tricks are credited. |
| `make_3_ctx` | 975–1103 | |
| `undo_0_ctx` | 1104–1195 | |
| `evaluate_with_context` | 1196–1279 | Terminal evaluation. |

`Pos::tricks_max` (`api/dds.h:108`) accumulates reference-side tricks along the current
path. The threshold cutoffs are in `ab_search_0_ctx`:

```cpp
310:  if (posPoint->tricks_max >= target)                    // achieved -> true
315:  else if (posPoint->tricks_max + tricks + 1 < target)   // unreachable -> false
```

These are the two tests that generalise to `V_lower >= guess` / `V_upper < guess` in nil
mode.

> **Stale comment.** `api/dds.h:108` documents `tricks_max` as "Aggregated tricks won by
> maximizing side". That has been untrue since misère landed: the field counts the
> *reference* side, selected by `is_reference_hand()`/`scoreParity`, and under misère the
> reference side is `MINNODE`. `ab_search.cpp:806-808` and `:1230-1231` say so explicitly.
> Trust the `ab_search.cpp` comments, not the header.

---

## 2. Exact values come from binary search above the predicate

There are **four** binary-search loops, not one. All four hardcode the vanilla 0..13 trick
range and the vanilla initial guess. Each is a place where a nil-mode range of
`0..(T+1)²+(T+1)T+T` must be substituted, and each is a place a wrong range silently
truncates results.

| Site | Enclosing function | Context |
|---|---|---|
| `solver_if.cpp:431` | `solve_board_internal` (81–) | `solutions == 3`: per-candidate loop |
| `solver_if.cpp:603` | `solve_board_internal` (81–) | `solutions == 1` / `2` |
| `solver_if.cpp:942` | `solve_same_board` (889–) | |
| `solver_if.cpp:1117` | `analyse_later_board` (1018–) | |

The `solutions == 3` loop is the one the production wrapper reaches. Its shape:

```cpp
434:  int guess = 7 - (handToPlay & 0x1);   // 7 for hands 0/2, 6 for hands 1/3
435:  int upperbound = 13;
436:  int lowerbound = 0;
439:  for (int mno = 0; mno < noMoves; mno++)
451:    if (thrp->misereOn)
452:      upperbound = 13;                  // fresh bound per candidate
460:    bool mvCaptured = false;
494:        lowerbound = guess++;
506:        upperbound = --guess;
```

The comment at `441-450` is the single most useful passage in the file for this work. It
explains that carrying `upperbound` between candidates is sound in vanilla mode only
because successive candidates are non-increasing as moves get forbidden; misère runs the
other way, so the carried bound can cut a candidate off before it issues a single fresh
query, leaving `mv` stale from the previously-forbidden candidate. `mvCaptured` (460,
rationale at `454-459`) exists because "did this candidate actually populate `best_move`"
cannot be inferred from `lowerbound != 0`.

Nil mode inherits this hazard conditionally rather than globally — see §4.

---

## 3. Trick crediting: two different shapes

The four sites named in the kickoff document are not four instances of one pattern. They
split into an accumulator pair and a terminal pair, and they need different treatment.

### 3a. Path accumulation — `ab_search_3_ctx`

```cpp
809:  if (ctx.search().is_reference_hand(posPoint->first[depth - 1]))
810:    posPoint->tricks_max++;
...
819:  if (ctx.search().is_reference_hand(posPoint->first[depth - 1]))
820:    posPoint->tricks_max--;
```

Make at 809/810, undo at 819/820. The trick winner is `posPoint->first[depth - 1]` — an
exact hand index, already in scope. Adding `Pos::tricks_nil` / `Pos::tricks_cover`
incremented on `== nilSeat` and `== partner(nilSeat)` is a local addition at both sites.

### 3b. Terminal evaluation — `evaluate_with_context`

```cpp
1232:  if (ctx.search().is_reference_hand(hmax))
1233:    goto maxexit;
1234:  else
1235:    goto minexit;
...
1267:  if (ctx.search().is_reference_hand(hmax))   // (second path, same shape)
...
1272: maxexit:
1273:   eval.tricks = posPoint->tricks_max + 1;
1274:   return eval;
1276: minexit:
1277:   eval.tricks = posPoint->tricks_max;
1278:   return eval;
```

These are **not** accumulators. They are a two-way branch that decides whether the final
trick belongs to the reference side, and the `+ 1` is applied to the *return value only* —
it is never written back to `Pos`.

> **Gap in the kickoff plan.** §2.3 of the kickoff says to add `tricks_nil`/`tricks_cover`
> to `Pos` and "mirror the increment/decrement at all four crediting sites". That is
> necessary but not sufficient. `EvalType` (`api/dds.h:135-139`) carries exactly one
> integer:
>
> ```cpp
> struct EvalType { int tricks; unsigned short int win_ranks[DDS_SUITS]; };
> ```
>
> So the last trick's per-seat attribution has no channel out of `evaluate_with_context`.
> Reading `posPoint->tricks_nil` after the call returns will be short by that trick.
> Either `EvalType` gains the two counters (or a packed `V`), or `V` is computed inside
> `evaluate_with_context` where `hmax` is still in scope. This needs deciding before
> Phase 2 starts, not during.

---

## 4. Node type and the reference-side anchor

Three coupled pieces:

```
solver_if.cpp:273             thrp->scoreParity = handToPlay & 1;
solver_context.hpp:281        is_reference_hand(hand) -> (hand & 1) == thr_->scoreParity
solver_if.cpp:286-300         node_type_store(0..3) assigned from refNode/othNode
solver_if.cpp:287-288         refNode = misereOn ? MINNODE : MAXNODE;  (othNode inverse)
```

Parity is a *correct* partnership test — DDS seats N=0/E=1/S=2/W=3 make `(hand & 1)` an
exact partnership discriminator. The hazard is the anchor at `:273`, which ties the
reference side to whoever happens to be on lead. Nil mode needs `nilSeat & 1`, fixed once
at the root.

Nil mode also sets `refNode = MAXNODE` and `othNode = MINNODE` unconditionally — no
`misereOn` ternary. The direction flag `m` lives entirely in the `sC`/`sN` complements and
must not reach node polarity.

**Consequence: the root may be a MIN node.** In vanilla DDS the seat to play is by
construction on the reference side, so the root always maximises. Anchoring on `nilSeat`
breaks that: when an opponent is to act, the root minimises. The bound-carrying assumption
at `solver_if.cpp:441-450` is therefore violated *conditionally on which side is on play* —
a strictly nastier version of the misère case, which at least fails uniformly.

Note also that `:451` gates the reset on `thrp->misereOn`, so it is unconditional *per
candidate* but conditional on mode. The kickoff text calls it "unconditional"; it is not.
Nil mode needs its own gate, and Phase 2 should reset **both** bounds for every candidate.

---

## 5. Transposition table

Objective keying is a **binary** switch:

```cpp
trans_table.hpp:170-184   set_objective(const bool misere_on)
trans_table.hpp:188       objective_is_misere() const -> bool
solver_if.cpp:255         ctx.trans_table()->set_objective(thrp->misereOn);
```

`set_objective` no-ops when the objective is unchanged and calls
`reset_memory(ResetReason::NewObjective)` when it changes, so the table never holds entries
written under two objectives. The call at `:255` is deliberately not conditional on `mode`
— see the rationale at `:250-252`, which notes that `mode == 2` suppresses the reset above
it precisely to preserve the table.

The comment at `:240-248` documents the context-lifetime issue: the legacy
`SolveBoard`/`SolveBoardPBN` wrappers construct a fresh context per call and are
*incidentally* immune to cross-objective contamination, but `dds_solve_board` hands out a
long-lived context and `calc_tables.cpp` deliberately shares one. The wrapper's cleanliness
is not a property of the search.

Separately, `ttUsable` gates on trump-break state:

```cpp
ab_search.cpp:211-214   trumpLeadUnrestricted = !trumpBreakRuleOn
                                              || trump == DDS_NOTRUMP
                                              || ctx.move_gen().trump_broken(tricks)
ab_search.cpp:243       const bool ttUsable = trumpLeadUnrestricted;
ab_search.cpp:268       if (depth >= 20 && ttUsable)     // lookup
ab_search.cpp:390       if (depth < 20 && ttUsable)      // lookup
ab_search.cpp:535       if (ttUsable)                    // store
```

The reasoning at `:200-209` is the exact precedent for nil-mode TT keying: two positions
with identical remaining cards but different `trumpBroken` status are not interchangeable,
and the gate is safe because `trumpBroken` is monotonic — once true it stays true for the
whole subtree.

`nilBrokenSoFar` has the same monotonicity, so the same gating shape transfers directly.
What does *not* transfer is `set_objective`'s single bool, which has no room for a third
objective. Phase 6 needs either a widened objective key or a separate table.

---

## 6. Estimator call sites

```cpp
ab_search.cpp:259   quickTricksUsable = trumpLeadUnrestricted && !thrp->misereOn;
ab_search.cpp:587   int res = (thrp->misereOn ? 0
                       : QuickTricksSecondHand(*posPoint, hand, depth, target, trump, ctx));
```

`:259` is in `ab_search_0_ctx`; `:587` is in `ab_search_1_ctx`.

The rationale at `:245-258` is worth reading in full before attempting Phase 7. Two
distinct reasons the estimators fail under a changed objective:

1. **Coupling.** The heuristic assumes a hand on lead is a `MAXNODE` exactly when it is on
   the reference side. Misère breaks that coupling.
2. **Direction.** The bound is an *achievable-if-I-try lower bound* — a bound in the
   direction each side is trying to avoid under misère, so it constrains nothing about
   optimal play. The comment is explicit that a sign flip cannot repair this; the analogue
   would need a *forced*-tricks bound, which is a different computation.

Nil mode has reason 2 in a weaker form (polarity is fixed, so the coupling survives) plus a
third problem the estimators cannot address at all: they bound a **side's** total and
cannot attribute a trick to a seat, so they say nothing about `sC` or `sN`. They can in
principle bound the primary term from one direction only — if the opponents can force all
`T` tricks then `n == 0` and the nil is made — which is sound but nearly useless. Start
disabled.

---

## 7. Move generation / trump-break boundary

This boundary is clean and nil mode should not need to cross it.

Move generation owns legality:

```cpp
solver_if.cpp:157       thrp->trumpBreakRuleOn = (dl.enforceTrumpBreak != 0);
solver_if.cpp:143       rule change forces a reset
solver_if.cpp:346,:389  dl.trumpAlreadyBroken passed into move-gen init
moves.cpp:118           track[tricks].trumpBroken = trump_already_broken;
moves.cpp:185-186       trumpLeadRestricted = trumpBreakRuleOn
                                            && trump != DDS_NOTRUMP
                                            && !trackp->trumpBroken
moves.cpp:188-190+      the "unless the leader holds nothing else" escape
moves.cpp:462,:565,:629 trumpBroken propagated: prev || trumpPlayedThisTrick
moves.hpp:290           trump_broken(tricks) accessor
```

The search reads this state exactly once, at `ab_search.cpp:214`, purely to gate TT and
estimators. It never re-derives legality.

Nil mode reuses all of it unchanged. The forced-all-spades-lead clause is already handled
inside `moves.cpp`.

---

## 8. Public struct surface

```cpp
api/dll.h:212-222   struct Deal      { trump; first; currentTrickSuit[3]; currentTrickRank[3];
                                       remainCards[4][4]; enforceTrumpBreak;
                                       trumpAlreadyBroken; misere; }
api/dll.h:243-253   struct DealPBN   { ... char remainCards[80]; ... misere; }
api/dll.h:163-171   struct FutureTricks { nodes; cards; suit[13]; rank[13];
                                          equals[13]; score[13]; }
```

Both deal structs are all-`int` POD ending at `misere`. `FutureTricks::score` is `int[13]`;
a packed `V` topping out at 391 for `T = 13` has ample headroom and never collides with the
negative sentinels.

The forced-move shortcut that must be suppressed in nil mode:

```cpp
solver_if.cpp:412   if (mode == 0 && noMoves == 1 && solutions != 3)
solver_if.cpp:422     futp->score[0] = -2;
```

Note it already excludes `solutions == 3`, so the `Exact` path is unaffected. `PrimaryOnly`
is the mode that has to be careful here.

Existing test to copy as a template:
`library/tests/solve_board/trump_break_rule_test.cpp`.

---

## 9. Corrections to the kickoff document

Recorded so the kickoff can be amended rather than re-litigated:

1. `EvalType` carries one int; adding counters to `Pos` alone does not get the final
   trick's attribution out of `evaluate_with_context`. (§3b)
2. The four crediting sites are two accumulator sites and two terminal-branch sites, not
   four of a kind. (§3)
3. `solver_if.cpp:451` is gated on `misereOn` — "unconditional" only per-candidate. (§4)
4. There are four binary-search loops hardcoding 0..13, not one. (§2)
5. `api/dds.h:108`'s comment on `tricks_max` is stale and misleading. (§1)
6. `Deal`/`DealPBN` are at `api/dll.h:212` and `:243` (the `struct` keyword line); the
   kickoff cites `:213`/`:245`.

---

## 10. Open before Phase 2

- **`EvalType` vs. `Pos`**: where does terminal per-seat attribution live? (§3b)
- **Which entry points does nil mode expose?** If only the `solutions == 3` equivalent,
  three of the four binary-search sites in §2 are out of scope and should be explicitly
  declared so.
- **TT objective key width** — deferred to Phase 6, but the choice between widening
  `set_objective` and standing up a separate table affects whether Phase 2's new module can
  stay entirely additive.
