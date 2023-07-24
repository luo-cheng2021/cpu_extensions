// Copyright (C) 2018-2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

namespace gen {

template<typename T>
struct func;

template<typename R, typename...Args>
struct func<R(*)(Args...)> {
    using func_type = R (*)(Args...);
    using return_type = R;

    template<typename...Names>
    auto get_args(Names...);

    func_type finalize();
};

template<typename T>
auto create_func(T) {
    return func<T>();
}

}