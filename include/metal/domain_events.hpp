#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace metal {

class Session;

template <typename... Events>
class domain_event_queue {
public:
    static_assert(sizeof...(Events) > 0,
                  "MetalORM: domain_event_queue requires at least one event type");
    static_assert((std::copy_constructible<Events> && ...),
                  "MetalORM: domain event types must be copy constructible");

    using variant_type = std::variant<Events...>;

    template <typename Event>
    static constexpr bool accepts = (std::same_as<std::remove_cvref_t<Event>, Events> || ...);

    template <typename Event>
    requires accepts<Event>
    void add(Event&& event) {
        events_.emplace_back(std::forward<Event>(event));
    }

    template <typename Event>
    requires accepts<Event>
    void raise(Event&& event) {
        add(std::forward<Event>(event));
    }

    [[nodiscard]] bool empty() const noexcept { return events_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return events_.size(); }
    void clear() noexcept { events_.clear(); }

    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (const auto& event : events_) {
            std::visit(std::forward<Fn>(fn), event);
        }
    }

private:
    std::vector<variant_type> events_;
};

template <typename T>
struct domain_event_queue_traits {
    static constexpr bool value = false;
};

template <typename... Events>
struct domain_event_queue_traits<domain_event_queue<Events...>> {
    static constexpr bool value = true;
};

template <typename T>
inline constexpr bool is_domain_event_queue_v =
    domain_event_queue_traits<std::remove_cvref_t<T>>::value;

class DomainEventBus {
public:
    template <typename Event, typename Handler>
    void on(Handler&& handler) {
        using E = std::remove_cvref_t<Event>;
        static_assert(std::invocable<Handler&, const E&, Session&>,
                      "MetalORM: domain event handler must accept (const Event&, Session&)");
        handlers_[std::type_index(typeid(E))].push_back(
            [fn = std::function<void(const E&, Session&)>(std::forward<Handler>(handler))]
            (const void* value, Session& session) {
                fn(*static_cast<const E*>(value), session);
            });
    }

    template <typename Event>
    void dispatch(const Event& event, Session& session) const {
        using E = std::remove_cvref_t<Event>;
        auto found = handlers_.find(std::type_index(typeid(E)));
        if (found == handlers_.end()) return;
        for (const auto& handler : found->second) handler(&event, session);
    }

    template <typename Queue>
    requires is_domain_event_queue_v<Queue>
    void dispatch_queue(Queue& queue, Session& session) const {
        queue.for_each([&](const auto& event) {
            dispatch(event, session);
        });
        queue.clear();
    }

private:
    using ErasedHandler = std::function<void(const void*, Session&)>;
    std::unordered_map<std::type_index, std::vector<ErasedHandler>> handlers_;
};

} // namespace metal
