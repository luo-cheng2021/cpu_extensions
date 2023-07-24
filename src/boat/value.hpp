// Copyright (C) 2018-2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include "base.hpp"
#include "expr.hpp"

namespace gen {

template<typename T>
struct value : base<value<T>> {
    using value_type = T;

	static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
		"only plain arithmetic types supported of sizes: 1, 2, 4 or 8 bytes");

    value();
    value(const value& v);
    value& operator=(const value& v);
    value(const value&& v);
    value& operator=(const value&& v);

    value(const T v);
    value& operator=(const T v);

    value(const expr<T>& e);
    value& operator=(const expr<T>& e);

    value& operator+=(const T v);
    value& operator+=(const value& other);
    value& operator+=(const expr<T>& e);

    expr<T> operator+(const T v) const;
    expr<T> operator+(const value& other) const;
    expr<T> operator+(const expr<T>& e) const;

    expr<T> operator==(const T v) const;
    expr<T> operator==(const value& other) const;
    expr<T> operator==(const expr<T>& e) const;

    // other operations
};

// reuse register in rvalue, opt expression, only one new registern created: i + j + 6
template<typename T, typename Right>
struct value<T> operator+(value<T>&& left, Right&& right) {
    left += right;
    return left;
}

}