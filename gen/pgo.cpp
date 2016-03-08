//===-- gen/pgo.cpp ---------------------------------------------*- C++ -*-===//
//
//                         LDC – the LLVM D compiler
//
// This file is adapted from CodeGenPGO.cpp (Clang, LLVM). Therefore,
// this file is distributed under the LLVM license.
// See the LICENSE file for details.
//
//===----------------------------------------------------------------------===//
//
// Instrumentation-based profile-guided optimization
//
//===----------------------------------------------------------------------===//

#include "gen/pgo.h"

// Conditionally include PGO
#if LDC_WITH_PGO

#include "globals.h"
#include "init.h"
#include "statement.h"
#include "llvm.h"
#include "gen/irstate.h"
#include "gen/logger.h"
#include "gen/recursivevisitor.h"

#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/ProfileData/InstrProfReader.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MD5.h"

/// \brief Stable hasher for PGO region counters.
///
/// PGOHash produces a stable hash of a given function's control flow.
/// It is used to detect whether the function has changed from the function with
/// the same name for which profile information is available.
/// Because only control flow is input to the hasher, other changes are not
/// detected and possibly wrong profiling data will be used. An example of
/// an undetected change is:
///   -  if (x==0) {...}
///   +  if (y==0) {...}
/// This can obviously result in very wrong branch weights. It's up for debate
/// whether these kind of changes should be detected or not; it is probably
/// difficult to distinguish such changes from simple changes in a variables
/// name.
///
/// Changing the output of this hash will invalidate all previously generated
/// profiles -- i.e., do it only with very strong arguments.
///
/// \note  When this hash does eventually change (years?), we still need to
/// support old hashes.  We'll need to pull in the version number from the
/// profile data format and use the matching hash function.
class PGOHash {
  uint64_t Working;
  unsigned Count;
  llvm::MD5 MD5;

  static const int NumBitsPerType = 6;
  static const unsigned NumTypesPerWord = sizeof(uint64_t) * 8 / NumBitsPerType;
  static const unsigned TooBig = 1u << NumBitsPerType;

public:
  // TODO: When this format changes, take in a version number here, and use the
  // old hash calculation for file formats that used the old hash.
  PGOHash() : Working(0), Count(0) {}

  /// \brief Hash values for AST nodes.
  ///
  /// Distinct values for AST nodes that have region counters attached.
  ///
  /// These values must be stable.  All new members must be added at the end,
  /// and no members should be removed.  Changing the enumeration value for an
  /// AST node will affect the hash of every function that contains that node.
  enum HashType : unsigned char {
    None = 0,
    LabelStmt = 1,
    WhileStmt,
    DoStmt,
    ForStmt,
    ForeachStmt,
    ForeachRangeStmt,
    SwitchStmt,
    CaseStmt,
    DefaultStmt,
    CaseGoto,
    IfStmt,
    TryCatchStmt,
    TryCatchCatch,
    TryFinallyStmt,
    ConditionalExpr,
    AndAndExpr,
    OrOrExpr,

    // Keep this last.  It's for the static assert that follows.
    LastHashType
  };
  static_assert(LastHashType <= TooBig, "Too many types in HashType");

  void combine(HashType Type) {
    // Check that we never combine 0 and only have six bits.
    assert(Type && "Hash is invalid: unexpected type 0");
    assert(unsigned(Type) < TooBig && "Hash is invalid: too many types");

    // Pass through MD5 if enough work has built up.
    if (Count && Count % NumTypesPerWord == 0) {
      using namespace llvm::support;
      uint64_t Swapped = endian::byte_swap<uint64_t, little>(Working);
      MD5.update(llvm::makeArrayRef((uint8_t *)&Swapped, sizeof(Swapped)));
      Working = 0;
    }

    // Accumulate the current type.
    ++Count;
    Working = Working << NumBitsPerType | Type;
  }

  uint64_t finalize() {
    // Use Working as the hash directly if we never used MD5.
    if (Count <= NumTypesPerWord)
      // No need to byte swap here, since none of the math was endian-dependent.
      // This number will be byte-swapped as required on endianness transitions,
      // so we will see the same value on the other side.
      return Working;

    // Check for remaining work in Working.
    if (Working)
      MD5.update(Working);

    // Finalize the MD5 and return the hash.
    llvm::MD5::MD5Result Result;
    MD5.final(Result);
    using namespace llvm::support;
    return endian::read<uint64_t, little, unaligned>(Result);
  }
};

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

/// An ASTVisitor that fills a map of (statements -> PGO counter numbers).
struct MapRegionCounters : public StoppableVisitor {
  /// The next counter value to assign.
  unsigned NextCounter;
  /// The function hash.
  PGOHash Hash;
  /// The map of statements to counters.
  llvm::DenseMap<const RootObject *, unsigned> &CounterMap;

  MapRegionCounters(llvm::DenseMap<const RootObject *, unsigned> &CounterMap)
      : NextCounter(0), CounterMap(CounterMap) {}

  using StoppableVisitor::visit;

// FIXME: this macro should also stop deeper traversal at duplicate nodes, using
// "stop=false;"
//   However, the regexp microbench by David breaks in that case. I feel there
//   is a bug lingering somewhere: needs further investigation!
#define SKIP_VISITED(Stmt)                                                     \
  do {                                                                         \
    if (CounterMap.count(Stmt)) {                                              \
      return;                                                                  \
    }                                                                          \
  } while (0)

  void visit(Statement *stmt) override {}
  void visit(Expression *exp) override {}
  void visit(Declaration *decl) override {}
  void visit(Initializer *init) override {}

  void visit(FuncDeclaration *fd) override {
    if (NextCounter) {
      // This is a nested function declaration. Don't add counters for it, as it
      // is treated as a separate function elsewhere in the AST.
      // Stop recursion at this depth.
      stop = true;
    } else {
      CounterMap[fd->fbody] = NextCounter++;
    }
  }

  void visit(IfStatement *stmt) override {
    SKIP_VISITED(stmt);
    CounterMap[stmt] = NextCounter++;
    Hash.combine(PGOHash::IfStmt);
  }

  void visit(WhileStatement *stmt) override {
    SKIP_VISITED(stmt);
    CounterMap[stmt] = NextCounter++;
    Hash.combine(PGOHash::WhileStmt);
  }

  void visit(DoStatement *stmt) override {
    SKIP_VISITED(stmt);
    CounterMap[stmt] = NextCounter++;
    Hash.combine(PGOHash::DoStmt);
  }

  void visit(ForStatement *stmt) override {
    SKIP_VISITED(stmt);
    CounterMap[stmt] = NextCounter++;
    Hash.combine(PGOHash::ForStmt);
  }

  void visit(ForeachStatement *stmt) override {
    SKIP_VISITED(stmt);
    CounterMap[stmt] = NextCounter++;
    Hash.combine(PGOHash::ForeachStmt);
  }

  void visit(ForeachRangeStatement *stmt) override {
    SKIP_VISITED(stmt);
    CounterMap[stmt] = NextCounter++;
    Hash.combine(PGOHash::ForeachRangeStmt);
  }

  void visit(LabelStatement *stmt) override {
    SKIP_VISITED(stmt);
    CounterMap[stmt] = NextCounter++;
    Hash.combine(PGOHash::LabelStmt);
  }

  void visit(SwitchStatement *stmt) override {
    SKIP_VISITED(stmt);
    CounterMap[stmt] = NextCounter++;
    Hash.combine(PGOHash::SwitchStmt);
  }

  void visit(CaseStatement *stmt) override {
    SKIP_VISITED(stmt);
    CounterMap[stmt] = NextCounter++;
    Hash.combine(PGOHash::CaseStmt);
    // Iff this statement is the target of a goto case statement, add an extra
    // counter for this case (as if it is a label statement).
    if (stmt->gototarget) {
      CounterMap[CodeGenPGO::getCounterPtr(stmt, 1)] = NextCounter++;
      Hash.combine(PGOHash::CaseGoto);
    }
  }

  void visit(CaseRangeStatement *stmt) override {
    assert(0 &&
           "Case range statement should be lowered to regular case statements");
  }

  void visit(DefaultStatement *stmt) override {
    SKIP_VISITED(stmt);
    CounterMap[stmt] = NextCounter++;
    Hash.combine(PGOHash::DefaultStmt);

    // Iff this statement is the target of a goto case statement, add an extra
    // counter for this case (as if it is a label statement).
    if (stmt->gototarget) {
      CounterMap[CodeGenPGO::getCounterPtr(stmt, 1)] = NextCounter++;
      Hash.combine(PGOHash::CaseGoto);
    }
  }

  void visit(TryCatchStatement *stmt) override {
    SKIP_VISITED(stmt);
    CounterMap[stmt] = NextCounter++;
    Hash.combine(PGOHash::TryCatchStmt);
    // Note that this results in the exception counters obtaining their counter
    // numbers before recursing into the counter handlers:
    for (auto c : *stmt->catches) {
      CounterMap[c] = NextCounter++;
      Hash.combine(PGOHash::TryCatchCatch);
    }
  }

  void visit(TryFinallyStatement *stmt) override {
    // If there is nothing to "try" or no cleanup, do nothing:
    if (!stmt->_body || !stmt->finalbody)
      return;

    SKIP_VISITED(stmt);
    CounterMap[stmt] = NextCounter++;
    Hash.combine(PGOHash::TryFinallyStmt);
  }
  void visit(CondExp *expr) override {
    SKIP_VISITED(expr);
    CounterMap[expr] = NextCounter++;
    Hash.combine(PGOHash::ConditionalExpr);
  }

  void visit(AndAndExp *expr) override {
    SKIP_VISITED(expr);
    CounterMap[expr] = NextCounter++;
    Hash.combine(PGOHash::AndAndExpr);
  }

  void visit(OrOrExp *expr) override {
    SKIP_VISITED(expr);
    CounterMap[expr] = NextCounter++;
    Hash.combine(PGOHash::OrOrExpr);
  }

#undef SKIP_VISITED
};

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

/// An Recursive AST Visitor that propagates the raw counts through the AST and
/// records the count at statements where the value may change.
struct ComputeRegionCounts : public RecursiveVisitor {
  /// PGO state.
  CodeGenPGO &PGO;

  /// A flag that is set when the current count should be recorded on the
  /// next statement, such as at the exit of a loop.
  bool RecordNextStmtCount;

  /// The count at the current location in the traversal.
  uint64_t CurrentCount;

  /// The map of statements to count values.
  llvm::DenseMap<const RootObject *, uint64_t> &CountMap;

  /// BreakContinueStack - Keep counts of breaks and continues inside loops.
  struct BreakContinue {
    uint64_t BreakCount;
    uint64_t ContinueCount;
    BreakContinue() : BreakCount(0), ContinueCount(0) {}
  };
  llvm::SmallVector<BreakContinue, 8> BreakContinueStack;

  struct LoopLabel {
    // If a label is used as break/continue target, this struct stores the
    // BreakContinue stack index at the label point
    LabelStatement *label;
    size_t stackindex;
    LoopLabel(LabelStatement *_label, size_t index)
        : label(_label), stackindex(index) {}
  };
  llvm::SmallVector<LoopLabel, 8> LoopLabels;

  ComputeRegionCounts(llvm::DenseMap<const RootObject *, uint64_t> &CountMap,
                      CodeGenPGO &PGO)
      : PGO(PGO), RecordNextStmtCount(false), CountMap(CountMap) {}

  void RecordStmtCount(const RootObject *S) {
    if (RecordNextStmtCount) {
      CountMap[S] = CurrentCount;
      RecordNextStmtCount = false;
    }
  }

  /// Set and return the current count.
  uint64_t setCount(uint64_t Count) {
    CurrentCount = Count;
    return Count;
  }

  using RecursiveVisitor::visit;

  void visit(FuncDeclaration *fd) override {
    // Counter tracks entry to the function body.
    uint64_t BodyCount = setCount(PGO.getRegionCount(fd->fbody));
    CountMap[fd->fbody] = BodyCount;
    recurse(fd->fbody);
  }

  void visit(Statement *S) override { RecordStmtCount(S); }

  void visit(ReturnStatement *S) override {
    RecordStmtCount(S);
    recurse(S->exp);
    CurrentCount = 0;
    RecordNextStmtCount = true;
  }

  void visit(ThrowStatement *S) override {
    RecordStmtCount(S);
    recurse(S->exp);
    CurrentCount = 0;
    RecordNextStmtCount = true;
  }

  void visit(GotoStatement *S) override {
    RecordStmtCount(S);
    CurrentCount = 0;
    RecordNextStmtCount = true;
  }

  void visit(LabelStatement *S) override {
    RecordNextStmtCount = false;
    // Counter tracks the block following the label.
    uint64_t BlockCount = setCount(PGO.getRegionCount(S));
    CountMap[S] = BlockCount;

    // For each label pointing to a loop, store the current index of
    // BreakContinueStack. This is needed for `break label;` and `continue
    // label;` statements in loops.
    // Assume all labels point to loops. (TODO: find predicate to filter which
    // labels to add)
    LoopLabels.push_back(LoopLabel(S, BreakContinueStack.size()));

    recurse(S->statement);
  }

  void visit(BreakStatement *S) override {
    RecordStmtCount(S);
    assert(!BreakContinueStack.empty() && "break not in a loop or switch!");

    if (S->target) {
      auto it = std::find_if(
          LoopLabels.begin(), LoopLabels.end(),
          [S](const LoopLabel &LL) { return LL.label == S->target; });
      assert(it != LoopLabels.end() && "It is not possible to break to a label "
                                       "that has not been visited yet");
      auto LL = *it;
      assert(LL.stackindex < BreakContinueStack.size());
      BreakContinueStack[LL.stackindex].BreakCount += CurrentCount;
    } else {
      BreakContinueStack.back().BreakCount += CurrentCount;
    }

    CurrentCount = 0;
    RecordNextStmtCount = true;
  }

  void visit(ContinueStatement *S) override {
    RecordStmtCount(S);
    assert(!BreakContinueStack.empty() && "continue stmt not in a loop!");

    if (S->target) {
      auto it = std::find_if(
          LoopLabels.begin(), LoopLabels.end(),
          [S](const LoopLabel &LL) { return LL.label == S->target; });
      assert(it != LoopLabels.end() &&
             "It is not possible to continue to a label "
             "that has not been visited yet");
      auto LL = *it;
      assert(LL.stackindex < BreakContinueStack.size());
      BreakContinueStack[LL.stackindex].ContinueCount += CurrentCount;
    } else {
      BreakContinueStack.back().ContinueCount += CurrentCount;
    }

    CurrentCount = 0;
    RecordNextStmtCount = true;
  }

  void visit(WhileStatement *S) override {
    RecordStmtCount(S);
    uint64_t ParentCount = CurrentCount;

    BreakContinueStack.push_back(BreakContinue());
    // Visit the body region first so the break/continue adjustments can be
    // included when visiting the condition.
    uint64_t BodyCount = setCount(PGO.getRegionCount(S));
    CountMap[S->_body] = CurrentCount;
    recurse(S->_body);
    uint64_t BackedgeCount = CurrentCount;

    // ...then go back and propagate counts through the condition. The count
    // at the start of the condition is the sum of the incoming edges,
    // the backedge from the end of the loop body, and the edges from
    // continue statements.
    BreakContinue BC = BreakContinueStack.pop_back_val();
    uint64_t CondCount =
        setCount(ParentCount + BackedgeCount + BC.ContinueCount);
    CountMap[S->condition] = CondCount;
    recurse(S->condition);
    setCount(BC.BreakCount + CondCount - BodyCount);
    RecordNextStmtCount = true;
  }

  void visit(DoStatement *S) override {
    RecordStmtCount(S);
    uint64_t FallThroughCount = CurrentCount;
    // The instr count includes the fallthrough from the parent scope.
    BreakContinueStack.push_back(BreakContinue());
    uint64_t BodyCount = setCount(PGO.getRegionCount(S));
    CountMap[S->_body] = BodyCount;
    recurse(S->_body);
    uint64_t BackedgeCount = CurrentCount;

    BreakContinue BC = BreakContinueStack.pop_back_val();
    // The count at the start of the condition is equal to the count at the
    // end of the body, plus any continues.
    uint64_t CondCount = setCount(BackedgeCount + BC.ContinueCount);
    CountMap[S->condition] = CondCount;
    recurse(S->condition);
    uint64_t LoopCount = BodyCount - FallThroughCount;
    setCount(BC.BreakCount + CondCount - LoopCount);
    RecordNextStmtCount = true;
  }

  void visit(ForStatement *S) override {
    RecordStmtCount(S);
    recurse(S->_init);

    uint64_t ParentCount = CurrentCount;

    BreakContinueStack.push_back(BreakContinue());
    // Visit the body region first. (This is basically the same as a while
    // loop; see further comments in VisitWhileStmt.)
    uint64_t BodyCount = setCount(PGO.getRegionCount(S));
    CountMap[S->_body] = BodyCount;
    recurse(S->_body);
    uint64_t BackedgeCount = CurrentCount;
    BreakContinue BC = BreakContinueStack.pop_back_val();

    // The increment is essentially part of the body but it needs to include
    // the count for all the continue statements.
    if (S->increment) {
      uint64_t IncCount = setCount(BackedgeCount + BC.ContinueCount);
      CountMap[S->increment] = IncCount;
      recurse(S->increment);
    }

    // ...then go back and propagate counts through the condition.
    uint64_t CondCount =
        setCount(ParentCount + BackedgeCount + BC.ContinueCount);

    // If condition is nullptr, store CondCount in a derived ptr
    CountMap[S->condition ? S->condition : PGO.getCounterPtr(S, 1)] = CondCount;
    recurse(S->condition);

    setCount(BC.BreakCount + CondCount - BodyCount);
    RecordNextStmtCount = true;
  }

  void visit(ForeachStatement *S) override {
    RecordStmtCount(S);
    recurse(S->aggr);

    uint64_t ParentCount = CurrentCount;
    BreakContinueStack.push_back(BreakContinue());
    // Visit the body region first. (This is basically the same as a while
    // loop; see further comments in VisitWhileStmt.)
    uint64_t BodyCount = setCount(PGO.getRegionCount(S));
    CountMap[S->_body] = BodyCount;
    recurse(S->_body);
    uint64_t BackedgeCount = CurrentCount;
    BreakContinue BC = BreakContinueStack.pop_back_val();

    uint64_t CondCount = ParentCount + BackedgeCount + BC.ContinueCount;
    // save the condition count as the second counter for the foreach statement
    // (there is no explicit condition statement).
    CountMap[PGO.getCounterPtr(S, 1)] = CondCount;

    setCount(BC.BreakCount + CondCount - BodyCount);
    RecordNextStmtCount = true;
  }

  void visit(ForeachRangeStatement *S) override {
    RecordStmtCount(S);
    recurse(S->lwr);
    recurse(S->upr);

    uint64_t ParentCount = CurrentCount;
    BreakContinueStack.push_back(BreakContinue());
    // Visit the body region first. (This is basically the same as a while
    // loop; see further comments in VisitWhileStmt.)
    uint64_t BodyCount = setCount(PGO.getRegionCount(S));
    CountMap[S->_body] = BodyCount;
    recurse(S->_body);
    uint64_t BackedgeCount = CurrentCount;
    BreakContinue BC = BreakContinueStack.pop_back_val();

    uint64_t CondCount = ParentCount + BackedgeCount + BC.ContinueCount;
    // save the condition count as the second counter for the foreach statement
    // (there is no explicit condition statement).
    CountMap[PGO.getCounterPtr(S, 1)] = CondCount;

    setCount(BC.BreakCount + CondCount - BodyCount);
    RecordNextStmtCount = true;
  }

  void visit(SwitchStatement *S) override {
    RecordStmtCount(S);
    recurse(S->condition);
    CurrentCount = 0;
    BreakContinueStack.push_back(BreakContinue());
    recurse(S->_body);
    // If the switch is inside a loop, add the continue counts.
    BreakContinue BC = BreakContinueStack.pop_back_val();
    if (!BreakContinueStack.empty())
      BreakContinueStack.back().ContinueCount += BC.ContinueCount;
    // Counter tracks the exit block of the switch.
    setCount(PGO.getRegionCount(S));
    RecordNextStmtCount = true;
  }

  void visit(CaseStatement *S) override {
    // Counter for this particular case. This counts only jumps from the
    // switch header and does not include fallthrough from the case before
    // this one. We need the count without fallthrough in the mapping, so it's
    // more useful for branch probabilities.
    uint64_t CaseCount = PGO.getRegionCount(S);
    CountMap[S] = CaseCount;

    // If this Case is the target of a goto case, it will have its own extra
    // counter and behaves like a LabelStatement.
    if (S->gototarget) {
      RootObject *cntr = PGO.getCounterPtr(S, 1);
      CountMap[cntr] = setCount(PGO.getRegionCount(cntr));
    } else {
      setCount(CurrentCount + CaseCount);
    }
    RecordNextStmtCount = true;

    recurse(S->statement);
  }

  void visit(DefaultStatement *S) override {
    // Identical to CaseStatement handler.
    uint64_t CaseCount = PGO.getRegionCount(S);
    CountMap[S] = CaseCount;
    if (S->gototarget) {
      RootObject *cntr = PGO.getCounterPtr(S, 1);
      CountMap[cntr] = setCount(PGO.getRegionCount(cntr));
    } else {
      setCount(CurrentCount + CaseCount);
    }
    RecordNextStmtCount = true;
    recurse(S->statement);
  }

  void visit(GotoDefaultStatement *S) override {
    // Identical to GotoStatement
    RecordStmtCount(S);
    CurrentCount = 0;
    RecordNextStmtCount = true;
  }

  void visit(GotoCaseStatement *S) override {
    // Identical to GotoStatement
    RecordStmtCount(S);
    CurrentCount = 0;
    RecordNextStmtCount = true;
  }

  void visit(IfStatement *S) override {
    RecordStmtCount(S);
    uint64_t ParentCount = CurrentCount;
    recurse(S->condition);

    // Counter tracks the "then" part of an if statement. The count for
    // the "else" part, if it exists, will be calculated from this counter.
    uint64_t ThenCount = setCount(PGO.getRegionCount(S));
    CountMap[S->ifbody] = ThenCount;
    recurse(S->ifbody);
    uint64_t OutCount = CurrentCount;

    uint64_t ElseCount = ParentCount - ThenCount;
    if (S->elsebody) {
      setCount(ElseCount);
      CountMap[S->elsebody] = ElseCount;
      recurse(S->elsebody);
      OutCount += CurrentCount;
    } else {
      OutCount += ElseCount;
    }
    setCount(OutCount);
    RecordNextStmtCount = true;
  }

  void visit(TryCatchStatement *S) override {
    RecordStmtCount(S);
    // Because the order of codegen, the body is generated after the catch
    // handlers and the current count (from the try statement) will be wrong
    // going into codegen for the body. Safest to store the current count in the
    // body too.
    RecordNextStmtCount = true;
    recurse(S->_body);
    for (auto c : *S->catches) {
      // Catch counter tracks the entry block of catch handler
      setCount(PGO.getRegionCount(c));
      RecordNextStmtCount = true;
      recurse(c->handler);
    }
    // Try counter tracks the continuation block of the try statement.
    setCount(PGO.getRegionCount(S));
    RecordNextStmtCount = true;
  }

  void visit(TryFinallyStatement *S) override {
    RecordStmtCount(S);
    uint64_t ParentCount = CurrentCount;
    // Because the order of codegen, the body is generated after the catch
    // handlers and the current count (from the try statement) will be wrong
    // going into codegen for the body. Safest to store the current count in the
    // body too.
    RecordNextStmtCount = true;
    recurse(S->_body);

    // Finally is always executed, so has same incoming count as the parent
    // count of the try statement.
    setCount(ParentCount);
    RecordNextStmtCount = true;
    recurse(S->finalbody);

    // The TryFinally counter tracks the continuation block of the try
    // statement.
    setCount(PGO.getRegionCount(S));
    RecordNextStmtCount = true;
  }

  void visit(CondExp *E) override {
    RecordStmtCount(E);
    uint64_t ParentCount = CurrentCount;
    recurse(E->econd);

    // Counter tracks the "true" part of a conditional operator. The
    // count in the "false" part will be calculated from this counter.
    uint64_t TrueCount = setCount(PGO.getRegionCount(E));
    CountMap[E->e1] = TrueCount;
    recurse(E->e1);
    uint64_t OutCount = CurrentCount;

    uint64_t FalseCount = setCount(ParentCount - TrueCount);
    CountMap[E->e2] = FalseCount;
    recurse(E->e2);
    OutCount += CurrentCount;

    setCount(OutCount);
    RecordNextStmtCount = true;
  }

  void visit(AndAndExp *E) override {
    RecordStmtCount(E);
    uint64_t ParentCount = CurrentCount;
    recurse(E->e1);
    // Counter tracks the right hand side of a logical and operator.
    uint64_t RHSCount = setCount(PGO.getRegionCount(E));
    CountMap[E->e2] = RHSCount;
    recurse(E->e2);
    setCount(ParentCount + RHSCount - CurrentCount);
    RecordNextStmtCount = true;
  }

  void visit(OrOrExp *E) override {
    RecordStmtCount(E);
    uint64_t ParentCount = CurrentCount;
    recurse(E->e1);
    // Counter tracks the right hand side of a logical or operator.
    uint64_t RHSCount = setCount(PGO.getRegionCount(E));
    CountMap[E->e2] = RHSCount;
    recurse(E->e2);
    setCount(ParentCount + RHSCount - CurrentCount);
    RecordNextStmtCount = true;
  }
};

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

// Pointer math to add an extra counter for one statement/expression.
// Increasing (the size_t value of) the pointer by counter results in a new
// "pointer" that will never clash with the other RootObject pointers (the size
// of a statement/expression object is much larger).
RootObject *CodeGenPGO::getCounterPtr(const RootObject *ptr,
                                      unsigned counter_idx) {
  return reinterpret_cast<RootObject *>(reinterpret_cast<size_t>(ptr) +
                                        counter_idx);
}

void CodeGenPGO::setFuncName(llvm::StringRef Name,
                             llvm::GlobalValue::LinkageTypes Linkage) {
#if LDC_LLVM_VER >= 308
  llvm::IndexedInstrProfReader *PGOReader = gIR->getPGOReader();
  FuncName = llvm::getPGOFuncName(Name, Linkage, "",
                                  PGOReader ? PGOReader->getVersion()
                                            : llvm::IndexedInstrProf::Version);

  // If we're generating a profile, create a variable for the name.
  if (global.params.genInstrProf && emitInstrumentation)
    FuncNameVar = llvm::createPGOFuncNameVar(gIR->module, Linkage, FuncName);
#else
  llvm::StringRef RawFuncName = Name;
  // Function names may be prefixed with a binary '1' to indicate
  // that the backend should not modify the symbols due to any platform
  // naming convention. Do not include that '1' in the PGO profile name.
  if (RawFuncName[0] == '\1')
    RawFuncName = RawFuncName.substr(1);
  FuncName = RawFuncName;

  // If we're generating a profile, create a variable for the name.
  if (global.params.genInstrProf && emitInstrumentation)
    createFuncNameVar(Linkage);
#endif
}

void CodeGenPGO::setFuncName(llvm::Function *fn) {
  setFuncName(fn->getName(), fn->getLinkage());
}

#if LDC_LLVM_VER < 308
void CodeGenPGO::createFuncNameVar(llvm::GlobalValue::LinkageTypes Linkage) {
  // We generally want to match the function's linkage, but available_externally
  // and extern_weak both have the wrong semantics, and anything that doesn't
  // need to link across compilation units doesn't need to be visible at all.
  if (Linkage == llvm::GlobalValue::ExternalWeakLinkage)
    Linkage = llvm::GlobalValue::LinkOnceAnyLinkage;
  else if (Linkage == llvm::GlobalValue::AvailableExternallyLinkage)
    Linkage = llvm::GlobalValue::LinkOnceODRLinkage;
  else if (Linkage == llvm::GlobalValue::InternalLinkage ||
           Linkage == llvm::GlobalValue::ExternalLinkage)
    Linkage = llvm::GlobalValue::PrivateLinkage;

  auto *value =
      llvm::ConstantDataArray::getString(gIR->context(), FuncName, false);
  FuncNameVar =
      new llvm::GlobalVariable(gIR->module, value->getType(), true, Linkage,
                               value, "__llvm_profile_name_" + FuncName);

  // Hide the symbol so that we correctly get a copy for each executable.
  if (!llvm::GlobalValue::isLocalLinkage(FuncNameVar->getLinkage()))
    FuncNameVar->setVisibility(llvm::GlobalValue::HiddenVisibility);
}
#endif

void CodeGenPGO::assignRegionCounters(const FuncDeclaration *D,
                                      llvm::Function *fn) {
  llvm::IndexedInstrProfReader *PGOReader = gIR->getPGOReader();
  if (!global.params.genInstrProf && !PGOReader)
    return;

  //  CGM.ClearUnusedCoverageMapping(D);
  emitInstrumentation = D->emitInstrumentation;
  setFuncName(fn);

  mapRegionCounters(D);
  //  if (CGM.getCodeGenOpts().CoverageMapping)
  //    emitCounterRegionMapping(D);
  if (PGOReader) {
    // SourceManager &SM = CGM.getContext().getSourceManager();
    loadRegionCounts(PGOReader, D);
    computeRegionCounts(D);
    applyFunctionAttributes(fn);
  }
}

void CodeGenPGO::mapRegionCounters(const FuncDeclaration *D) {
  RegionCounterMap.reset(new llvm::DenseMap<const RootObject *, unsigned>);
  MapRegionCounters regioncounter(*RegionCounterMap);
  RecursiveWalker walker(&regioncounter);

  walker.visit(const_cast<FuncDeclaration *>(D));
  assert(regioncounter.NextCounter > 0 && "no entry counter mapped for decl");
  assert(regioncounter.NextCounter == RegionCounterMap->size());
  NumRegionCounters = regioncounter.NextCounter;
  FunctionHash = regioncounter.Hash.finalize();
}

void CodeGenPGO::computeRegionCounts(const FuncDeclaration *FD) {
  StmtCountMap.reset(new llvm::DenseMap<const RootObject *, uint64_t>);
  ComputeRegionCounts Walker(*StmtCountMap, *this);
  Walker.visit(const_cast<FuncDeclaration *>(FD));
}

/// Apply attributes to llvm::Function based on profiling data.
void CodeGenPGO::applyFunctionAttributes(llvm::Function *Fn) {
  if (!haveRegionCounts())
    return;

  uint64_t FunctionCount = getRegionCount(nullptr);
  Fn->setEntryCount(FunctionCount);
}

void CodeGenPGO::emitCounterIncrement(const RootObject *S) const {
  if (!global.params.genInstrProf || !RegionCounterMap || !emitInstrumentation)
    return;

  auto counter_it = (*RegionCounterMap).find(S);
  assert(counter_it != (*RegionCounterMap).end() &&
         "Statement not found in PGO counter map!");
  unsigned counter = counter_it->second;
  auto *I8PtrTy = llvm::Type::getInt8PtrTy(gIR->context());
  gIR->ir->CreateCall(GET_INTRINSIC_DECL(instrprof_increment),
                      {llvm::ConstantExpr::getBitCast(FuncNameVar, I8PtrTy),
                       gIR->ir->getInt64(FunctionHash),
                       gIR->ir->getInt32(NumRegionCounters),
                       gIR->ir->getInt32(counter)});
}

void CodeGenPGO::loadRegionCounts(llvm::IndexedInstrProfReader *PGOReader,
                                  const FuncDeclaration *fd) {
  RegionCounts.clear();
  if (std::error_code EC =
          PGOReader->getFunctionCounts(FuncName, FunctionHash, RegionCounts)) {
    if (EC == llvm::instrprof_error::unknown_function) {
      IF_LOG Logger::println("No profile data for function: %s",
                             FuncName.c_str());
      // Don't output a compiler warning when profile data is missing for a
      // function, because it could be intentional.
    } else if (EC == llvm::instrprof_error::hash_mismatch) {
      IF_LOG Logger::println(
          "Ignoring profile data: hash mismatch for function: %s",
          FuncName.c_str());
      warning(fd->loc, "Ignoring profile data for function '%s' ('%s'): "
                       "control-flow hash mismatch",
              const_cast<FuncDeclaration *>(fd)->toPrettyChars(),
              FuncName.c_str());
    } else if (EC == llvm::instrprof_error::malformed) {
      IF_LOG Logger::println("Profile data is malformed for function: %s",
                             FuncName.c_str());
      warning(fd->loc, "Ignoring profile data for function '%s' ('%s'): "
                       "control-flow hash mismatch",
              const_cast<FuncDeclaration *>(fd)->toPrettyChars(),
              FuncName.c_str());
    } else {
      IF_LOG Logger::println("Error loading profile counts for function: %s",
                             FuncName.c_str());
      warning(fd->loc, "Error loading profile data for function '%s' ('%s')",
              const_cast<FuncDeclaration *>(fd)->toPrettyChars(),
              FuncName.c_str());
    }
    RegionCounts.clear();
  } else {
    IF_LOG Logger::println("Loaded profile counts for function: %s",
                           FuncName.c_str());
  }
}

/// \brief Calculate what to divide by to scale weights.
///
/// Given the maximum weight, calculate a divisor that will scale all the
/// weights to strictly less than UINT32_MAX.
static uint64_t calculateWeightScale(uint64_t MaxWeight) {
  return MaxWeight < UINT32_MAX ? 1 : MaxWeight / UINT32_MAX + 1;
}

/// \brief Scale an individual branch weight (and add 1).
///
/// Scale a 64-bit weight down to 32-bits using \c Scale.
///
/// According to Laplace's Rule of Succession, it is better to compute the
/// weight based on the count plus 1, so universally add 1 to the value.
///
/// \pre \c Scale was calculated by \a calculateWeightScale() with a weight no
/// greater than \c Weight.
static uint32_t scaleBranchWeight(uint64_t Weight, uint64_t Scale) {
  assert(Scale && "scale by 0?");
  uint64_t Scaled = Weight / Scale + 1;
  assert(Scaled <= UINT32_MAX && "overflow 32-bits");
  return Scaled;
}

llvm::MDNode *CodeGenPGO::createProfileWeights(uint64_t TrueCount,
                                               uint64_t FalseCount) const {
  // Check for empty weights.
  if (!TrueCount && !FalseCount)
    return nullptr;

  // Calculate how to scale down to 32-bits.
  uint64_t Scale = calculateWeightScale(std::max(TrueCount, FalseCount));

  llvm::MDBuilder MDHelper(gIR->context());
  return MDHelper.createBranchWeights(scaleBranchWeight(TrueCount, Scale),
                                      scaleBranchWeight(FalseCount, Scale));
}

llvm::MDNode *
CodeGenPGO::createProfileWeights(llvm::ArrayRef<uint64_t> Weights) const {
  // We need at least two elements to create meaningful weights.
  if (Weights.size() < 2)
    return nullptr;

  // Check for empty weights.
  uint64_t MaxWeight = *std::max_element(Weights.begin(), Weights.end());
  if (MaxWeight == 0)
    return nullptr;

  // Calculate how to scale down to 32-bits.
  uint64_t Scale = calculateWeightScale(MaxWeight);

  llvm::SmallVector<uint32_t, 16> ScaledWeights;
  ScaledWeights.reserve(Weights.size());
  for (uint64_t W : Weights) {
    ScaledWeights.push_back(scaleBranchWeight(W, Scale));
  }

  llvm::MDBuilder MDHelper(gIR->context());
  return MDHelper.createBranchWeights(ScaledWeights);
}

llvm::MDNode *
CodeGenPGO::createProfileWeightsWhileLoop(const RootObject *Cond,
                                          uint64_t LoopCount) const {
  if (!haveRegionCounts())
    return nullptr;

  auto StmtCount = getStmtCount(Cond);
  assert(StmtCount.first && "missing expected while loop condition count");
  auto CondCount = StmtCount.second;
  if (CondCount == 0)
    return nullptr;
  return createProfileWeights(LoopCount,
                              std::max(CondCount, LoopCount) - LoopCount);
}

llvm::MDNode *
CodeGenPGO::createProfileWeightsForLoop(const ForStatement *stmt) const {
  if (!haveRegionCounts())
    return nullptr;
  auto LoopCount = getRegionCount(stmt);
  auto StmtCount =
      getStmtCount(stmt->condition ? stmt->condition : getCounterPtr(stmt, 1));
  assert(StmtCount.first && "missing expected for loop condition count");
  auto CondCount = StmtCount.second;
  if (CondCount == 0)
    return nullptr;
  return createProfileWeights(LoopCount,
                              std::max(CondCount, LoopCount) - LoopCount);
}

llvm::MDNode *
CodeGenPGO::createProfileWeightsForeach(const ForeachStatement *stmt) const {
  if (!haveRegionCounts())
    return nullptr;

  auto LoopCount = getRegionCount(stmt);
  auto StmtCount = getStmtCount(getCounterPtr(stmt, 1));
  assert(StmtCount.first && "missing expected foreach loop condition count");
  auto CondCount = StmtCount.second;
  if (CondCount == 0)
    return nullptr;
  return createProfileWeights(LoopCount,
                              std::max(CondCount, LoopCount) - LoopCount);
}

llvm::MDNode *CodeGenPGO::createProfileWeightsForeachRange(
    const ForeachRangeStatement *stmt) const {
  if (!haveRegionCounts())
    return nullptr;

  auto LoopCount = getRegionCount(stmt);
  auto StmtCount = getStmtCount(getCounterPtr(stmt, 1));
  assert(StmtCount.first &&
         "missing expected foreachrange loop condition count");
  auto CondCount = StmtCount.second;
  if (CondCount == 0)
    return nullptr;
  return createProfileWeights(LoopCount,
                              std::max(CondCount, LoopCount) - LoopCount);
}

#endif // LDC_WITH_PGO
