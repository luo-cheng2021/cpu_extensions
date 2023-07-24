// Copyright (C) 2018-2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include <cstddef>
#include <type_traits>

#include "ptr.hpp"

namespace gen {

// https://stackoverflow.com/questions/50080560/constexpr-offsetof-with-pointer-to-member-data
template<typename> struct struct_member_types;
template<typename T, typename F> struct struct_member_types<F T::*> {
  using struct_type = T;
  using member_type = F;
};

template<typename T> using struct_type_t = typename struct_member_types<T>::struct_type;
template<typename T> using member_type_t = typename struct_member_types<T>::member_type;

template<typename T>
struct has_custom_base : std::false_type {};

template<typename T>
struct struct_base;

struct struct_base_empty {};

template<typename T>
struct struct_ : ptr<T>,
                 std::conditional_t<has_custom_base<T>::value,
                                    struct_base<struct_<T>>,
                                    struct_base_empty> {
    using struct_type = T;

    // template<typename MemberType>
    // wrapper_type<MemberType> get_value(int MemberOffset) const {
    //     wrapper_type<MemberType> ret;
    //     if constexpr(std::is_floating_point_v<MemberType>) {
    //         _CC.movss(ret.reg, asmjit::x86::dword_ptr(reg, (int32_t)(MemberOffset + offset)));
    //     } else if constexpr(std::is_array_v<MemberType>) {
    //         // array decay to pointer, just add offset to struct pointer
    //         //TODO: could just use struct pointer with fixed offset, no need for new register, similar to nested struct
    //         //_CC.lea(ret.reg, asmjit::x86::ptr(reg, offset_of_v<I, typename T::types> + offset));
    //         ret.reg = reg; // pass ptr register
    //         ret.offset = offset + MemberOffset; // change offset
    //     } else if constexpr(std::is_arithmetic_v<std::remove_pointer_t<MemberType>>) {
    //         switch(sizeof(MemberType)) {
    //             case 1: _CC.mov(ret.reg, asmjit::x86:: byte_ptr(reg, (int32_t)(MemberOffset + offset))); break;
    //             case 2: _CC.mov(ret.reg, asmjit::x86:: word_ptr(reg, (int32_t)(MemberOffset + offset))); break;
    //             case 4: _CC.mov(ret.reg, asmjit::x86::dword_ptr(reg, (int32_t)(MemberOffset + offset))); break;
    //             case 8: _CC.mov(ret.reg, asmjit::x86::qword_ptr(reg, (int32_t)(MemberOffset + offset))); break;
    //         }
    //     } else if constexpr(std::is_pointer_v<MemberType>) {
    //         // pointer to struct, load pointer
    //         _CC.mov(ret.reg, asmjit::x86::qword_ptr(reg, MemberOffset + offset));
    //     } else {
    //         // nested struct
    //         ret.reg = reg; // pass ptr register
    //         ret.offset = offset + MemberOffset; // change offset
    //     }

    //     return ret;
    // }

    template<auto field>
    auto get() const {
        using type = member_type_t<decltype(field)>;
        static constexpr const type struct_type::* const_field = field;

        int member_offset = static_cast<int>(reinterpret_cast<size_t>(&(((struct_type*)nullptr)->*const_field)));

        //return get_value<type>(member_offset);
    }

};

}