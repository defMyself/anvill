/*
 * Copyright (c) 2019 Trail of Bits, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "anvill/TypeParser.h"

#include <cstddef>
#include <sstream>
#include <vector>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/DerivedTypes.h>

#include <remill/BC/Compat/Error.h>

namespace anvill {
namespace {

// Parse some characters out of `spec` starting at index `i`, where
// the characters are accepted by the predicate `filter`, and store
// the result in `*out`.
template<typename Filter, typename T>
static bool Parse(llvm::StringRef spec, size_t &i, Filter filter, T *out) {
  std::stringstream ss;
  auto found = false;
  for (; i < spec.size(); ++i) {
    if (filter(spec[i])) {
      ss << spec[i];
      found = true;
    } else {
      break;
    }
  }

  if (!found) {
    return false;
  }

  ss >> *out;
  return !ss.bad();
}

// Parse a type specification into an LLVM type. See TypeParser.h
// for the grammar that generates the language which `ParseType`
// accepts.
static llvm::Expected<llvm::Type *> ParseType(
    llvm::LLVMContext &context, std::vector<llvm::Type *> &ids,
    llvm::StringRef spec, size_t &i) {

  llvm::StructType *struct_type = nullptr;

  while (i < spec.size()) {
    switch (spec[i]) {

      // Parse a structure type.
      case '{': {
        llvm::SmallVector<llvm::Type *, 4> elem_types;
        for (i += 1; i < spec.size() && spec[i] != '}';) {
          auto maybe_elem_type = ParseType(context, ids, spec, i);
          if (remill::IsError(maybe_elem_type)) {
            return maybe_elem_type;
          } else {
            if (auto elem_type = remill::GetReference(maybe_elem_type)) {
              elem_types.push_back(elem_type);
            } else {
              break;
            }
          }
        }

        if (elem_types.empty()) {
          return llvm::createStringError(
              std::make_error_code(std::errc::invalid_argument),
              "Invalid structure in type specification '%s'",
              spec.str().c_str());
        }

        if (i >= spec.size() || '}' != spec[i]) {
          return llvm::createStringError(
              std::make_error_code(std::errc::invalid_argument),
              "Missing closing '}' in type specification '%s'",
              spec.str().c_str());
        }

        i += 1;
        if (!struct_type) {
          return llvm::StructType::get(context, elem_types);
        } else {
          struct_type->setBody(elem_types);
          return struct_type;
        }
      }

      // Parse an array type.
      case '[': {
        i += 1;
        auto maybe_elem_type = ParseType(context, ids, spec, i);
        if (remill::IsError(maybe_elem_type)) {
          return maybe_elem_type;
        }

        llvm::Type *elem_type = remill::GetReference(maybe_elem_type);

        if (i >= spec.size() || 'x' != spec[i]) {
          return llvm::createStringError(
              std::make_error_code(std::errc::invalid_argument),
              "Missing 'x' in array type specification in '%s'",
              spec.str().c_str());
        }

        i += 1;
        size_t num_elems = 0;
        if (!Parse(spec, i, isdigit, &num_elems)) {
          return llvm::createStringError(
              std::make_error_code(std::errc::invalid_argument),
              "Unable to parse array size in type specification '%s'",
              spec.str().c_str());
        }

        if (i >= spec.size() || ']' != spec[i]) {
          return llvm::createStringError(
              std::make_error_code(std::errc::invalid_argument),
              "Missing closing ']' in type specification '%s'",
              spec.str().c_str());
        }

        if (!num_elems) {
          return llvm::createStringError(
              std::make_error_code(std::errc::invalid_argument),
              "Invalid zero-sized array in type specification '%s'",
              spec.str().c_str());
        }

        i += 1;
        return llvm::ArrayType::get(elem_type, num_elems);
      }

      // Parse a vector type.
      case '<': {
        i += 1;
        auto maybe_elem_type = ParseType(context, ids, spec, i);
        if (remill::IsError(maybe_elem_type)) {
          return maybe_elem_type;
        }

        llvm::Type *elem_type = remill::GetReference(maybe_elem_type);
        if (i >= spec.size() || 'x' != spec[i]) {
          return llvm::createStringError(
              std::make_error_code(std::errc::invalid_argument),
              "Missing 'x' in vector type specification in '%s'",
              spec.str().c_str());
        }

        i += 1;
        unsigned num_elems = 0;
        if (!Parse(spec, i, isdigit, &num_elems)) {
          return llvm::createStringError(
              std::make_error_code(std::errc::invalid_argument),
              "Unable to parse vector size in type specification '%s'",
              spec.str().c_str());
        }

        if (!num_elems) {
          return llvm::createStringError(
              std::make_error_code(std::errc::invalid_argument),
              "Invalid zero-sized vector in type specification '%s'",
              spec.str().c_str());
        }

        if (i >= spec.size() || '>' != spec[i]) {
          return llvm::createStringError(
              std::make_error_code(std::errc::invalid_argument),
              "Missing closing '>' in type specification '%s'",
              spec.str().c_str());
        }

        i += 1;
        return llvm::VectorType::get(elem_type, num_elems);
      }

      // Parse a pointer type.
      case '*': {
        i += 1;
        auto maybe_elem_type = ParseType(context, ids, spec, i);
        if (remill::IsError(maybe_elem_type)) {
          return maybe_elem_type;
        }

        llvm::Type *elem_type = remill::GetReference(maybe_elem_type);
        if (!elem_type) {
          return llvm::createStringError(
              std::make_error_code(std::errc::invalid_argument),
              "Missing subtype for pointer type in type specification '%s'",
              spec.str().c_str());
        }

        return llvm::PointerType::get(elem_type, 0);
      }

      // Parse a function type.
      case '(': {
        llvm::SmallVector<llvm::Type *, 4> elem_types;
        for (i += 1; i < spec.size() && spec[i] != ')';) {
          auto maybe_elem_type = ParseType(context, ids, spec, i);
          if (remill::IsError(maybe_elem_type)) {
            return maybe_elem_type;
          } else {
            if (auto elem_type = remill::GetReference(maybe_elem_type)) {
              elem_types.push_back(elem_type);
            } else {
              break;
            }
          }
        }

        // There always needs to be at least one parameter type and one
        // return type. Here are some examples:
        //
        //    In C:               Here:
        //    void foo(void)      (vv)
        //    int foo(void)       (vi)
        //    void foo(...)       (&v)
        //    int foo(int, ...)   (i&i)
        //    void foo(int, ...)  (i&v)
        //
        // Not valid: (v...v).
        if (elem_types.size() < 2) {
          return llvm::createStringError(
              std::make_error_code(std::errc::invalid_argument),
              "Function types must have at least two internal types. "
              "E.g. '(vv)' for a function taking nothing and returning nothing, "
              "in type specification '%s'",
              spec.str().c_str());
        }

        const auto ret_type = elem_types.pop_back_val();
        if (ret_type->isTokenTy()) {
          return llvm::createStringError(
              std::make_error_code(std::errc::invalid_argument),
              "Cannot have a variadic return type in type specification '%s'",
              spec.str().c_str());
        }

        // Second-to-last type can optionally be a token type, which is
        // a stand in for varargs.
        auto is_var_arg = false;
        if (elem_types.back()->isTokenTy()) {
          is_var_arg = true;
          elem_types.pop_back();

          // If second-to-last is a void type, then it must be the first.
        } else if (elem_types.back()->isVoidTy()) {
          if (1 == elem_types.size()) {
            elem_types.pop_back();
          } else {
            return llvm::createStringError(
                std::make_error_code(std::errc::invalid_argument),
                "Invalid placement of void parameter type in type specification '%s'",
                spec.str().c_str());
          }
        }

        // Minor sanity checking on the param types.
        for (auto param_type : elem_types) {
          if (param_type->isVoidTy() ||
              param_type->isTokenTy() ||
              param_type->isFunctionTy()) {
            return llvm::createStringError(
                std::make_error_code(std::errc::invalid_argument),
                "Invalid parameter type in type specification '%s'",
                spec.str().c_str());
          }
        }

        if (i >= spec.size() || ')' != spec[i]) {
          return llvm::createStringError(
              std::make_error_code(std::errc::invalid_argument),
              "Missing closing ')' in type specification '%s'",
              spec.str().c_str());
        }

        i += 1;
        return llvm::FunctionType::get(ret_type, elem_types, is_var_arg);
      }

      // Parse a varargs type, and represent it as a token type.
      case '&': {
        i += 1;
        return llvm::Type::getTokenTy(context);
      }

      // Parse an assignment of a type ID (local to this specification) for later
      // use, e.g. to do a linked list of `int`s, you would do:
      //
      //    In C:                 Here:
      //    struct LI32 {         =0{*%0i}
      //      struct LI32 *next;
      //      int val;
      //    };
      //
      // This also enables some compression, e.g.:
      //
      //    In C:                 Here:
      //    struct P {            {ii}
      //      int x, y;
      //    };
      //    struct PP {           {{ii}{ii}}
      //      struct P x, y;          or
      //    };                    {=0{ii}%0}
      case '=': {
        i += 1;
        unsigned type_id = 0;
        if (!Parse(spec, i, isdigit, &type_id)) {
          return llvm::createStringError(
              std::make_error_code(std::errc::invalid_argument),
              "Unable to parse type ID in type specification '%s'",
              spec.str().c_str());
        }

        if (type_id != ids.size()) {
          return llvm::createStringError(
              std::make_error_code(std::errc::invalid_argument),
              "Invalid type ID assignment '%u' in type specification '%s'; "
              "next expected type ID was '%u'",
              type_id, spec.str().c_str(), static_cast<unsigned>(ids.size()));
        }

        if (i >= spec.size() || spec[i] != '{') {
          return llvm::createStringError(
              std::make_error_code(std::errc::invalid_argument),
              "Non-structure type assigned to type ID '%u' in type "
              "specification '%s'",
              type_id, spec.str().c_str());
        }

        std::stringstream ss;
        ss << "anvill.struct." << type_id;
        struct_type = llvm::StructType::create(context, ss.str());
        ids.push_back(struct_type);

        // Jump to the next iteration, which will parse the struct.
        continue;
      }

      // Parse a use of a type ID.
      case '%': {
        i += 1;
        unsigned type_id = 0;
        if (!Parse(spec, i, isdigit, &type_id)) {
          return llvm::createStringError(
              std::make_error_code(std::errc::invalid_argument),
              "Unable to parse type ID in type specification '%s'",
              spec.str().c_str());
        }

        if (type_id >= ids.size()) {
          return llvm::createStringError(
              std::make_error_code(std::errc::invalid_argument),
              "Invalid type ID use '%u' in type specification '%s'",
              type_id, spec.str().c_str());
        }

        return ids[type_id];
      }

      case 'b':  // char.
      case 'B':  // unsigned char.
        i += 1;
        return llvm::IntegerType::getInt8Ty(context);
      case 'h':  // short.
      case 'H':  // unsigned short.
        i += 1;
        return llvm::IntegerType::getInt16Ty(context);
      case 'i':  // int.
      case 'I':  // unsigned int.
        i += 1;
        return llvm::IntegerType::getInt32Ty(context);
      case 'l':  // long.
      case 'L':  // unsigned long.
        i += 1;
        return llvm::IntegerType::getInt64Ty(context);
      case 'f':  // float.
        i += 1;
        return llvm::Type::getFloatTy(context);
      case 'd':  // double.
        i += 1;
        return llvm::Type::getDoubleTy(context);
      case 'D':  // long double.
        i += 1;
        return llvm::Type::getX86_FP80Ty(context);
      case 'M':  // MMX type.
        i += 1;
        return llvm::Type::getX86_MMXTy(context);
      case 'v':  // void.
        i += 1;
        return llvm::Type::getVoidTy(context);

      default:
        i += 1;
        return llvm::createStringError(
            std::make_error_code(std::errc::invalid_argument),
            "Unexpected character '%c' in type specification '%s'",
            spec[i - 1], spec.str().c_str());
    }
  }
  return nullptr;
}

}  // namespace

llvm::Expected<llvm::Type *>
ParseType(llvm::LLVMContext &context, llvm::StringRef spec) {
  std::vector<llvm::Type *> ids;
  size_t i = 0;
  auto ret = ParseType(context, ids, spec, i);
  if (!remill::IsError(ret) && i < spec.size()) {
    return llvm::createStringError(
        std::make_error_code(std::errc::invalid_argument),
        "Type specification '%s' contains trailing unparsed characters",
        spec.str().c_str());
  }

  return ret;
}

}  // namespace anvill
