//===--- RewriteSystem.cpp - Generics with term rewriting -------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/AST/RewriteSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <vector>

using namespace swift;
using namespace rewriting;

int Atom::compare(Atom other) const {
  auto kind = getKind();
  auto otherKind = other.getKind();

  if (kind != otherKind)
    return int(kind) < int(otherKind) ? -1 : 1;

  switch (kind) {
  case Kind::Name:
    return getName().compare(other.getName());

  case Kind::Protocol:
    return TypeDecl::compare(getProtocol(), other.getProtocol());

  case Kind::AssociatedType:
    return TypeDecl::compare(getAssociatedType(), other.getAssociatedType());

  case Kind::GenericParam: {
    auto *param = getGenericParam();
    auto *otherParam = getGenericParam();

    if (param->getDepth() != otherParam->getDepth())
      return param->getDepth() < otherParam->getDepth() ? -1 : 1;

    if (param->getIndex() != otherParam->getIndex())
      return param->getIndex() < otherParam->getIndex() ? -1 : 1;

    return 0;
  }
  }

  llvm_unreachable("Bad atom kind");
}

void Atom::dump(llvm::raw_ostream &out) const {
  switch (getKind()) {
  case Kind::Name:
    out << getName();
    return;

  case Kind::Protocol:
    out << "[" << getProtocol()->getName() << "]";
    return;

  case Kind::AssociatedType: {
    auto *type = getAssociatedType();
    out << "[" << type->getProtocol()->getName()
        << ":" << type->getName() << "]";
    return;
  }

  case Kind::GenericParam:
    out << Type(getGenericParam());
    return;
  }

  llvm_unreachable("Bad atom kind");
}

int Term::compare(const Term &other) const {
  if (size() != other.size())
    return size() < other.size() ? -1 : 1;

  for (unsigned i = 0, e = size(); i < e; ++i) {
    auto lhs = (*this)[i];
    auto rhs = other[i];

    int result = lhs.compare(rhs);
    if (result != 0)
      return result;
  }

  return 0;
}

decltype(Term::Atoms)::const_iterator
Term::findSubTerm(const Term &other) const {
  if (other.size() > size())
    return end();

  return std::search(begin(), end(), other.begin(), other.end());
}

decltype(Term::Atoms)::iterator
Term::findSubTerm(const Term &other) {
  if (other.size() > size())
    return end();

  return std::search(begin(), end(), other.begin(), other.end());
}

bool Term::rewriteSubTerm(const Term &lhs, const Term &rhs) {
  auto found = findSubTerm(lhs);
  if (found == end())
    return false;

  auto oldSize = size();

  assert(rhs.compare(lhs) < 0);
  assert(rhs.size() <= lhs.size());

  auto newIter = std::copy(rhs.begin(), rhs.end(), found);
  auto oldIter = found + lhs.size();
  if (newIter != oldIter) {
    auto newEnd = std::copy(oldIter, end(), newIter);
    Atoms.erase(newEnd, end());
  }

  assert(size() == oldSize - lhs.size() + rhs.size());
  return true;
}

bool Term::checkForOverlap(const Term &other, Term &result) const {
  assert(result.size() == 0);

  if (other.size() > size())
    return false;

  auto first1 = begin();
  auto last1 = end();
  auto first2 = other.begin();
  auto last2 = other.end();

  while (last2 - first2 <= last1 - first1) {
    if (std::equal(first2, last2, first1)) {
      result = *this;
      return true;
    }

    ++first1;
  }

  while (first1 != last1) {
    --last2;

    if (std::equal(first1, last1, first2)) {
      std::copy(begin(), first1,
                std::back_inserter(result.Atoms));
      std::copy(other.begin(), other.end(),
                std::back_inserter(result.Atoms));
      return true;
    }

    ++first1;
  }

  return false;
}

void Term::dump(llvm::raw_ostream &out) const {
  bool first = true;

  for (auto atom : Atoms) {
    if (!first)
      out << ".";
    else
      first = false;

    atom.dump(out);
  }
}

void Rule::dump(llvm::raw_ostream &out) const {
  LHS.dump(out);
  out << " => ";
  RHS.dump(out);
  if (deleted)
    out << " [deleted]";
}

bool RewriteSystem::addRule(Term lhs, Term rhs) {
  simplify(lhs);
  simplify(rhs);

  int result = lhs.compare(rhs);
  if (result == 0)
    return false;
  if (result < 0)
    std::swap(lhs, rhs);

  Rules.emplace_back(lhs, rhs);

  return true;
}

bool RewriteSystem::simplify(Term &term) const {
  bool changed = false;

  while (true) {
    bool tryAgain = false;
    for (const auto &rule : Rules) {
      if (rule.isDeleted())
        continue;

      if (rule.apply(term)) {
        changed = true;
        tryAgain = true;
      }
    }

    if (!tryAgain)
      break;
  }

  return changed;
}

void RewriteSystem::computeConfluentCompletion(
    unsigned maxIterations) {
  SmallVector<std::pair<unsigned, unsigned>, 16> worklist;

  for (unsigned i : indices(Rules)) {
    for (unsigned j : indices(Rules)) {
      if (i == j)
        continue;

      worklist.emplace_back(i, j);
    }
  }

  while (!worklist.empty()) {
    auto pair = worklist.back();
    worklist.pop_back();

    Term first;

    const auto &lhs = Rules[pair.first];
    const auto &rhs = Rules[pair.second];

    if (lhs.isDeleted() || rhs.isDeleted())
      continue;

    if (!lhs.checkForOverlap(rhs, first))
      continue;

    assert(first.size() > 0);

    Term second = first;

    lhs.apply(first);
    rhs.apply(second);

    unsigned i = Rules.size();

    if (!addRule(first, second))
      continue;

    if (maxIterations == 0) {
      dump(llvm::errs());
      llvm::errs() << "Completion procedure exceeded max iteration count\n";
      abort();
    }

    maxIterations--;

    const auto &newRule = Rules[i];

    for (unsigned j : indices(Rules)) {
      if (i == j)
        continue;

      worklist.emplace_back(i, j);
      worklist.emplace_back(j, i);
    }

    for (unsigned j : indices(Rules)) {
      if (i == j)
        continue;

      auto &rule = Rules[j];

      if (rule.isDeleted())
        continue;

      if (rule.canReduceLeftHandSide(newRule))
        rule.markDeleted();
    }
  }
}

void RewriteSystem::dump(llvm::raw_ostream &out) const {
  out << "Rewrite system: {\n";
  for (const auto &rule : Rules) {
    out << "- ";
    rule.dump(out);
    out << "\n";
  }
  out << "}\n";
}
