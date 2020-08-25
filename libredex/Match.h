/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <functional>
#include <type_traits>
#include <vector>

#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "MethodUtil.h"
#include "ReachableClasses.h"

namespace m {

namespace detail {

bool is_assignable_to(const DexType* child, const DexType* parent);
bool is_default_constructor(const DexMethod* meth);

} // namespace detail

// N.B. recursive template for matching opcode pattern against insn sequence
template <typename T, typename N>
struct insns_matcher {
  static bool matches_at(int at,
                         const std::vector<IRInstruction*>& insns,
                         const T& t) {
    const auto& insn = insns.at(at);
    typename std::tuple_element<N::value, T>::type insn_match =
        std::get<N::value>(t);
    return insn_match.matches(insn) &&
           insns_matcher<T, std::integral_constant<size_t, N::value + 1>>::
               matches_at(at + 1, insns, t);
  }
};

// N.B. base case of recursive template where N = opcode pattern length
template <typename T>
struct insns_matcher<
    T,
    std::integral_constant<size_t, std::tuple_size<T>::value>> {
  static bool matches_at(int at,
                         const std::vector<IRInstruction*>& insns,
                         const T& t) {
    return true;
  }
};

// Find all sequences in `insns` that match `p` and put them into `matches`
template <typename P, size_t N = std::tuple_size<P>::value>
void find_matches(const std::vector<IRInstruction*>& insns,
                  const P& p,
                  std::vector<std::vector<IRInstruction*>>& matches) {
  // No way to match if we have fewer insns than N
  if (insns.size() >= N) {
    // Try to match starting at i
    for (size_t i = 0; i <= insns.size() - N; ++i) {
      if (m::insns_matcher<P, std::integral_constant<size_t, 0>>::matches_at(
              i, insns, p)) {
        matches.emplace_back();
        auto& matching_insns = matches.back();
        matching_insns.reserve(N);
        for (size_t c = 0; c < N; ++c) {
          matching_insns.push_back(insns.at(i + c));
        }
      }
    }
  }
}

// Find all instructions in `insns` that match `p`
template <typename P>
void find_insn_match(const std::vector<IRInstruction*>& insns,
                     const P& p,
                     std::vector<IRInstruction*>& matches) {
  for (auto insn : insns) {
    if (p.matches(insn)) {
      matches.emplace_back(insn);
    }
  }
}

/**
 * Zero cost wrapper over a callable type with the following signature:
 *
 *   (const T*) const -> bool
 *
 * The resulting object can be used with the combinators defined in this
 * header to form more complex predicates.  This wrapper serves two purposes:
 *
 * - It prevents the combinators defined below from interfering with the
 *   overload resolution for any callable object -- they must be opted-in by
 *   wrapping.
 * - It allows us to use template deduction to hide the implementation of the
 *   predicate (the second template parameter), while still constraining over
 *   the type being matched over.
 */
template <typename T, typename P>
struct match_t {
  explicit match_t(P fn) : m_fn(std::move(fn)) {}
  bool matches(const T* t) const { return m_fn(t); }

 private:
  P m_fn;
};

/**
 * Create a match_t from a matching function, fn, of type `const T* -> bool`.
 * Supports template deduction so lambdas can be wrapped without referring to
 * their type (which cannot be easily named).
 */
template <typename T, typename P>
inline match_t<T, P> matcher(P fn) {
  return match_t<T, P>(std::move(fn));
}

/** Match a subordinate match whose logical not is true */
template <typename T, typename P>
inline auto operator!(match_t<T, P> p) {
  return matcher<T>([p = std::move(p)](const T* t) { return !p.matches(t); });
}

/** Match two subordinate matches whose logical or is true */
template <typename T, typename P, typename Q>
inline auto operator||(match_t<T, P> p, match_t<T, Q> q) {
  return matcher<T>([p = std::move(p), q = std::move(q)](const T* t) {
    return p.matches(t) || q.matches(t);
  });
}

/** Match two subordinate matches whose logical and is true */
template <typename T, typename P, typename Q>
inline auto operator&&(match_t<T, P> p, match_t<T, Q> q) {
  return matcher<T>([p = std::move(p), q = std::move(q)](const T* t) {
    return p.matches(t) && q.matches(t);
  });
}

/** Match two subordinate matches whose logical xor is true */
template <typename T, typename P, typename Q>
inline auto operator^(match_t<T, P> p, match_t<T, Q> q) {
  return matcher<T>([p = std::move(p), q = std::move(q)](const T* t) {
    return p.matches(t) ^ q.matches(t);
  });
}

/** Match any T (always matches) */
template <typename T>
inline auto any() {
  return matcher<T>([](const T*) { return true; });
}

/** Compare two T at pointers */
template <typename T>
inline auto ptr_eq(const T* expected) {
  return matcher<T>([expected](const T* actual) { return expected == actual; });
}

/** Match any T named thusly */
template <typename T>
inline auto named(const char* name) {
  return matcher<T>(
      [name](const T* t) { return t->get_name()->str() == name; });
}

/** Match T's which are external */
template <typename T>
inline auto is_external() {
  return matcher<T>([](const T* t) { return t->is_external(); });
}

/** Match T's which are final */
template <typename T>
inline auto is_final() {
  return matcher<T>(
      [](const T* t) { return (bool)(t->get_access() & ACC_FINAL); });
}

/** Match T's which are static */
template <typename T>
inline auto is_static() {
  return matcher<T>(
      [](const T* t) { return (bool)(t->get_access() & ACC_STATIC); });
}

/** Match T's which are interfaces */
template <typename T>
inline auto is_abstract() {
  return matcher<T>(
      [](const T* t) { return (bool)(t->get_access() & ACC_ABSTRACT); });
}

/** Match classes that are enums */
inline auto is_enum() {
  return matcher<DexClass>(
      [](const DexClass* cls) { return (bool)(cls->get_access() & ACC_ENUM); });
}

/** Match classes that are interfaces */
inline auto is_interface() {
  return matcher<DexClass>([](const DexClass* cls) {
    return (bool)(cls->get_access() & ACC_INTERFACE);
  });
}

/**
 * Matches IRInstructions
 */

/** Any instruction which holds a type reference */
inline auto has_type() {
  return matcher<IRInstruction>(
      [](const IRInstruction* insn) { return insn->has_type(); });
}

/** const-string flavors */
inline auto const_string() {
  return matcher<IRInstruction>([](const IRInstruction* insn) {
    return insn->opcode() == OPCODE_CONST_STRING;
  });
}

/** move-result-pseudo flavors */
inline auto move_result_pseudo() {
  return matcher<IRInstruction>([](const IRInstruction* insn) {
    return opcode::is_move_result_pseudo(insn->opcode());
  });
}

/** new-instance flavors */
template <typename P>
inline auto new_instance(match_t<IRInstruction, P> p) {
  return matcher<IRInstruction>([p = std::move(p)](const IRInstruction* insn) {
    return insn->opcode() == OPCODE_NEW_INSTANCE && p.matches(insn);
  });
}

inline auto new_instance() { return new_instance(any<IRInstruction>()); }

/** throw flavors */
inline auto throwex() {
  return matcher<IRInstruction>(
      [](const IRInstruction* insn) { return insn->opcode() == OPCODE_THROW; });
}

/** invoke-direct flavors */
template <typename P>
inline auto invoke_direct(match_t<IRInstruction, P> p) {
  return matcher<IRInstruction>([p = std::move(p)](const IRInstruction* insn) {
    return insn->opcode() == OPCODE_INVOKE_DIRECT && p.matches(insn);
  });
}

inline auto invoke_direct() { return invoke_direct(any<IRInstruction>()); }

/** invoke-static flavors */
template <typename P>
inline auto invoke_static(match_t<IRInstruction, P> p) {
  return matcher<IRInstruction>([p = std::move(p)](const IRInstruction* insn) {
    return insn->opcode() == OPCODE_INVOKE_STATIC && p.matches(insn);
  });
}

inline auto invoke_static() { return invoke_static(any<IRInstruction>()); }

/** invoke-virtual flavors */
template <typename P>
inline auto invoke_virtual(match_t<IRInstruction, P> p) {
  return matcher<IRInstruction>([p = std::move(p)](const IRInstruction* insn) {
    return insn->opcode() == OPCODE_INVOKE_VIRTUAL && p.matches(insn);
  });
}

inline auto invoke_virtual() { return invoke_virtual(any<IRInstruction>()); }

/** invoke of any kind */
template <typename P>
inline auto invoke(match_t<IRInstruction, P> p) {
  return matcher<IRInstruction>([p = std::move(p)](const IRInstruction* insn) {
    return is_invoke(insn->opcode()) && p.matches(insn);
  });
}

inline auto invoke() { return invoke(any<IRInstruction>()); }

/** iput flavors */
template <typename P>
inline auto iput(match_t<IRInstruction, P> p) {
  return matcher<IRInstruction>([p = std::move(p)](const IRInstruction* insn) {
    return is_iput(insn->opcode()) && p.matches(insn);
  });
}

inline auto iput() { return iput(any<IRInstruction>()); };

/** iget flavors */
template <typename P>
inline auto iget(match_t<IRInstruction, P> p) {
  return matcher<IRInstruction>([p = std::move(p)](const IRInstruction* insn) {
    return is_iget(insn->opcode()) && p.matches(insn);
  });
}

inline auto iget() { return iget(any<IRInstruction>()); };

/** return-void */
inline auto return_void() {
  return matcher<IRInstruction>([](const IRInstruction* insn) {
    return insn->opcode() == OPCODE_RETURN_VOID;
  });
}

/** Matches instructions with specified number of arguments. */
inline auto has_n_args(size_t n) {
  return matcher<IRInstruction>(
      [n](const IRInstruction* insn) { return insn->srcs_size() == n; });
}

/** Matches instructions with specified opcode */
inline auto is_opcode(IROpcode opcode) {
  return matcher<IRInstruction>(
      [opcode](const IRInstruction* insn) { return insn->opcode() == opcode; });
}

/** Matchers that map from IRInstruction -> other types */
template <typename P>
inline auto opcode_method(match_t<DexMethodRef, P> p) {
  return matcher<IRInstruction>([p = std::move(p)](const IRInstruction* insn) {
    return insn->has_method() && p.matches(insn->get_method());
  });
}

template <typename P>
inline auto opcode_field(match_t<DexFieldRef, P> p) {
  return matcher<IRInstruction>([p = std::move(p)](const IRInstruction* insn) {
    return insn->has_field() && p.matches(insn->get_field());
  });
}

template <typename P>
inline auto opcode_type(match_t<DexType, P> p) {
  return matcher<IRInstruction>([p = std::move(p)](const IRInstruction* insn) {
    return insn->has_type() && p.matches(insn->get_type());
  });
}

template <typename P>
inline auto opcode_string(match_t<DexString, P> p) {
  return matcher<IRInstruction>([p = std::move(p)](const IRInstruction* insn) {
    return insn->has_string() && p.matches(insn->get_string());
  });
}

/** Match types which can be assigned to the given type */
inline auto is_assignable_to(const DexType* parent) {
  return matcher<DexType>([parent](const DexType* child) {
    return detail::is_assignable_to(child, parent);
  });
}

/** Match members and check predicate on their type */
template <typename Member, typename P>
inline auto member_of(match_t<DexType, P> p) {
  return matcher<Member>([p = std::move(p)](const Member* member) {
    return p.matches(member->get_class());
  });
}

/** Match methods that are default constructors */
inline auto is_default_constructor() {
  return matcher<DexMethod>(detail::is_default_constructor);
}

inline auto can_be_default_constructor() {
  return matcher<DexMethodRef>([](const DexMethodRef* meth) {
    const DexMethod* def = meth->as_def();
    return def && detail::is_default_constructor(def);
  });
}

/** Match methods that are constructors. INCLUDES static constructors! */
inline auto is_constructor() {
  return matcher<DexMethod>(
      [](const DexMethod* meth) { return method::is_constructor(meth); });
}

inline auto can_be_constructor() {
  return matcher<DexMethodRef>(
      [](const DexMethodRef* meth) { return method::is_constructor(meth); });
}

/** Match classes that have class data */
inline auto has_class_data() {
  return matcher<DexClass>(
      [](const DexClass* cls) { return cls->has_class_data(); });
}

/** Match classes satisfying the given method match for any vmethods */
template <typename P>
inline auto any_vmethods(match_t<DexMethod, P> p) {
  return matcher<DexClass>([p = std::move(p)](const DexClass* cls) {
    const auto& vmethods = cls->get_vmethods();
    return std::any_of(vmethods.begin(),
                       vmethods.end(),
                       [&p](const DexMethod* meth) { return p.matches(meth); });
  });
}

/** Match classes satisfying the given method match for any dmethods */
template <typename P>
inline auto any_dmethods(match_t<DexMethod, P> p) {
  return matcher<DexClass>([p = std::move(p)](const DexClass* cls) {
    const auto& dmethods = cls->get_dmethods();
    return std::any_of(dmethods.begin(),
                       dmethods.end(),
                       [&p](const DexMethod* meth) { return p.matches(meth); });
  });
}

/** Match classes satisfying the given field match for any ifields */
template <typename P>
inline auto any_ifields(match_t<DexField, P> p) {
  return matcher<DexClass>([p = std::move(p)](const DexClass* cls) {
    const auto& ifields = cls->get_ifields();
    return std::any_of(ifields.begin(),
                       ifields.end(),
                       [&p](const DexField* meth) { return p.matches(meth); });
  });
}

/** Match classes satisfying the given field match for any sfields */
template <typename P>
inline auto any_sfields(match_t<DexField, P> p) {
  return matcher<DexClass>([p = std::move(p)](const DexClass* cls) {
    const auto& sfields = cls->get_sfields();
    return std::any_of(sfields.begin(),
                       sfields.end(),
                       [&p](const DexField* meth) { return p.matches(meth); });
  });
}

/** Match dex members containing any annotation that matches the given match */
template <typename T, typename P>
inline auto any_annos(match_t<DexAnnotation, P> p) {
  return matcher<T>([p = std::move(p)](const T* t) {
    if (!t->is_def()) {
      return false;
    }

    const auto& anno_set = t->get_anno_set();
    if (!anno_set) {
      return false;
    }

    const auto& annos = anno_set->get_annotations();
    return std::any_of(
        annos.begin(), annos.end(), [&p](const DexAnnotation* anno) {
          return p.matches(anno);
        });
  });
}

/**
 * Match which checks for membership of T in container C via C::find(T)
 * Does not take ownership of the container.
 */
template <typename T, typename C>
inline auto in(const C* c) {
  return matcher<T>(
      [c](const T* t) { return c->find(const_cast<T*>(t)) != c->end(); });
}

/**
 * Maps match<T, X> => match<DexType(t), X>
 */
template <typename T, typename P>
inline auto as_type(match_t<DexType, P> p) {
  return matcher<T>(
      [p = std::move(p)](const T* t) { return p.matches(t->type()); });
}

/**
 * Maps match<DexType, X> => match<DexClass, X>
 */
template <typename P>
inline auto as_class(match_t<DexClass, P> p) {
  return matcher<DexType>([p = std::move(p)](const DexType* t) {
    auto cls = type_class(t);
    return cls && p.matches(cls);
  });
}

/** Match which checks can_delete helper for DexMembers */
template <typename T>
inline auto can_delete() {
  return matcher<T>(can_delete);
}

/** Match which checks can_rename helper for DexMembers */
template <typename T>
inline auto can_rename() {
  return matcher<T>(can_rename);
}

/** Match which checks keep helper for DexMembers */
template <typename T>
inline auto has_keep() {
  return matcher<T>(has_keep);
}

} // namespace m
