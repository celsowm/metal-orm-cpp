#pragma once

#include <functional>

namespace metal {

class Session;

template <typename Entity>
struct TableHooks {
    std::function<void(Session&, Entity&)> before_insert;
    std::function<void(Session&, Entity&)> after_insert;
    std::function<void(Session&, Entity&)> before_update;
    std::function<void(Session&, Entity&)> after_update;
    std::function<void(Session&, Entity&)> before_delete;
    std::function<void(Session&, Entity&)> after_delete;
};

struct SessionInterceptor {
    std::function<void(Session&)> before_flush;
    std::function<void(Session&)> after_flush;
};

} // namespace metal
