#pragma once

#include "metal/collection.hpp"

#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace metal {

template <typename T>
class morph_one_reference {
public:
    using value_type = std::shared_ptr<T>;

    [[nodiscard]] bool loaded() const noexcept { return loaded_; }
    [[nodiscard]] bool dirty() const noexcept { return current_ != baseline_; }

    const value_type& load() {
        if (!loaded_) {
            if (!loader_) throw std::logic_error("MetalORM: polymorphic reference is not bound to a Session");
            loader_();
        }
        return current_;
    }

    const value_type& get() const noexcept { return current_; }
    explicit operator bool() const noexcept { return static_cast<bool>(current_); }
    T* operator->() const noexcept { return current_.get(); }
    T& operator*() const { return *current_; }

    void set(value_type value) {
        if (value && attach_hook_) attach_hook_(*value);
        current_ = std::move(value);
        loaded_ = true;
    }

    void reset() {
        current_.reset();
        loaded_ = true;
    }

    void _metal_bind_loader(std::function<void()> loader) { loader_ = std::move(loader); }
    void _metal_bind_attach(std::function<void(T&)> hook) { attach_hook_ = std::move(hook); }

    void _metal_hydrate(value_type value) {
        current_ = std::move(value);
        baseline_ = current_;
        loaded_ = true;
    }

    [[nodiscard]] value_type _metal_added() const {
        return current_ != baseline_ ? current_ : value_type{};
    }

    [[nodiscard]] value_type _metal_removed() const {
        return current_ != baseline_ ? baseline_ : value_type{};
    }

    void _metal_accept_changes() { baseline_ = current_; }

private:
    bool loaded_{false};
    value_type current_;
    value_type baseline_;
    std::function<void()> loader_;
    std::function<void(T&)> attach_hook_;
};

template <typename T>
class morph_many_collection : public has_many_collection<T> {};

template <typename T, typename... Ts>
inline constexpr bool morph_target_in_pack_v = (std::same_as<T, Ts> || ...);

template <typename... Targets>
class morph_to_reference {
public:
    using variant_type = std::variant<std::monostate, std::shared_ptr<Targets>...>;

    [[nodiscard]] bool loaded() const noexcept { return loaded_; }
    [[nodiscard]] bool dirty() const noexcept { return current_ != baseline_; }
    [[nodiscard]] bool empty() const noexcept { return std::holds_alternative<std::monostate>(current_); }

    const variant_type& load() {
        if (!loaded_) {
            if (!loader_) throw std::logic_error("MetalORM: morph_to reference is not bound to a Session");
            loader_();
        }
        return current_;
    }

    const variant_type& get() const noexcept { return current_; }

    template <typename T>
    requires morph_target_in_pack_v<T, Targets...>
    std::shared_ptr<T> get_as() const {
        if (const auto* value = std::get_if<std::shared_ptr<T>>(&current_)) return *value;
        return {};
    }

    template <typename T>
    requires morph_target_in_pack_v<T, Targets...>
    void set(std::shared_ptr<T> value) {
        if (!value) {
            reset();
            return;
        }
        current_ = std::move(value);
        loaded_ = true;
        if (attach_hook_) attach_hook_(current_);
    }

    void reset() {
        current_ = std::monostate{};
        loaded_ = true;
        if (attach_hook_) attach_hook_(current_);
    }

    template <typename Visitor>
    decltype(auto) visit(Visitor&& visitor) const {
        return std::visit(std::forward<Visitor>(visitor), current_);
    }

    void _metal_bind_loader(std::function<void()> loader) { loader_ = std::move(loader); }
    void _metal_bind_attach(std::function<void(const variant_type&)> hook) { attach_hook_ = std::move(hook); }

    void _metal_hydrate(variant_type value) {
        current_ = std::move(value);
        baseline_ = current_;
        loaded_ = true;
    }

    const variant_type& _metal_current() const noexcept { return current_; }
    const variant_type& _metal_baseline() const noexcept { return baseline_; }
    void _metal_accept_changes() { baseline_ = current_; }

private:
    bool loaded_{false};
    variant_type current_;
    variant_type baseline_;
    std::function<void()> loader_;
    std::function<void(const variant_type&)> attach_hook_;
};

} // namespace metal
