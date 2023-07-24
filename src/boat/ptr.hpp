// Copyright (C) 2018-2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include "base.hpp"
#include "value.hpp"
#include <optional>

namespace gen {

template<typename T>
struct ptr : base<ptr<T>> {
    using value_type = T;

    size_t offset = 0;
    std::optional<value<size_t>> index;

    ptr();
    ptr(const ptr& p);
    ptr& operator=(const ptr& p);
    ptr(const ptr&& p);
    ptr& operator=(const ptr&& p);

    // support abs pointer
    ptr(const T* p);
    ptr& operator=(const T* p);

    ptr(T, const value<size_t>& p);
    ptr& operator=(const value<size_t>& p);

    ptr& operator+=(const size_t idx);
    ptr& operator+=(const value<size_t>& idx);
    ptr& operator-=(const size_t idx);
    ptr& operator-=(const value<size_t>& idx);

    ptr operator+(const size_t idx) const;
    ptr operator+(const value<size_t>& idx) const;
    ptr operator-(const size_t idx) const;
    ptr operator-(const value<size_t>& idx) const;

    expr<T> operator==(const T* p) const;
    expr<T> operator==(const ptr& other) const;

    value<T> operator[](const int idx) const;
    value<T> operator[](const value<size_t>& idx) const;

    template<typename TO>
    auto cast();

    template<typename TO>
    ptr<TO> view_as();
};

// support unroll loop expression:
// p + pos + 0 * 16
// p + pos + 1 * 16
// p + pos + 2 * 16
// p + pos + 3 * 16
// pos += 4 * 16;
}