// Copyright (C) 2018-2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

namespace gen {

template <typename SubClass>
struct base {
    using subclass_type = SubClass;

    subclass_type& self() {
        return static_cast<subclass_type&>(this);
    }
};

}