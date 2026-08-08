#pragma once

#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

namespace metal {

template <typename T>
class relation_reference {
public:
    using value_type = std::shared_ptr<T>;

    [[nodiscard]] bool loaded() const noexcept { return loaded_; }
    [[nodiscard]] bool dirty() const noexcept { return current_ != baseline_; }
    [[nodiscard]] bool empty() const noexcept { return !current_; }

    const value_type& load() {
        if (!loaded_) {
            if (!loader_) throw std::logic_error("MetalORM: relation reference is not bound to a Session");
            loader_();
        }
        return current_;
    }

    const value_type& get() const noexcept { return current_; }
    explicit operator bool() const noexcept { return static_cast<bool>(current_); }
    T* operator->() const noexcept { return current_.get(); }
    T& operator*() const { return *current_; }

    bool operator==(const value_type& other) const noexcept { return current_ == other; }
    bool operator!=(const value_type& other) const noexcept { return current_ != other; }

    // Assignment is reserved for ORM hydration. User mutations should call set()
    // so the reference remains dirty until the Unit of Work accepts it.
    relation_reference& operator=(value_type value) {
        _metal_hydrate(std::move(value));
        return *this;
    }

    void set(value_type value) {
        if (value && attach_hook_) attach_hook_(*value);
        current_ = std::move(value);
        loaded_ = true;
    }

    void reset() {
        current_.reset();
        loaded_ = true;
        if (reset_hook_) reset_hook_();
    }

    void _metal_bind_loader(std::function<void()> loader) { loader_ = std::move(loader); }
    void _metal_bind_attach(std::function<void(T&)> hook) { attach_hook_ = std::move(hook); }
    void _metal_bind_reset(std::function<void()> hook) { reset_hook_ = std::move(hook); }

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
    std::function<void()> reset_hook_;
};

template <typename T>
class belongs_to_reference : public relation_reference<T> {};

template <typename T>
class has_one_reference : public relation_reference<T> {};

} // namespace metal
