// Copyright (C) 2018-2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include <deque>
template<typename T> class expr;

#include "base.hpp"

namespace gen {

template<typename Left, typename Right>
struct expr_add : base<expr_add<Left, Right>> {
    Left left;
    Right right;
};

template<typename Left, typename Right>
auto operator+(const Left& left, const Right& right) {

}
}