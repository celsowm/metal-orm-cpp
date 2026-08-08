#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace metal {

template <typename T>
class collection {
public:
    using value_type = std::shared_ptr<T>;
    using const_iterator = typename std::vector<value_type>::const_iterator;

    collection() = default;

    [[nodiscard]] bool loaded() const noexcept { return loaded_; }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }

    const value_type& operator[](std::size_t index) const { return items_.at(index); }
    const value_type& at(std::size_t index) const { return items_.at(index); }
    const std::vector<value_type>& items() const noexcept { return items_; }

    const_iterator begin() const noexcept { return items_.begin(); }
    const_iterator end() const noexcept { return items_.end(); }
    const_iterator cbegin() const noexcept { return items_.cbegin(); }
    const_iterator cend() const noexcept { return items_.cend(); }

    void attach(value_type value) {
        if (!value) throw std::invalid_argument("MetalORM: cannot attach a null entity");
        if (!contains(items_, value)) items_.push_back(std::move(value));
    }

    bool detach(const value_type& value) {
        if (!value) return false;
        const auto it = std::find(items_.begin(), items_.end(), value);
        if (it == items_.end()) return false;
        items_.erase(it);
        return true;
    }

    void sync(std::vector<value_type> desired) {
        std::vector<value_type> normalized;
        normalized.reserve(desired.size());
        for (auto& value : desired) {
            if (!value) throw std::invalid_argument("MetalORM: collection::sync cannot contain null entities");
            if (!contains(normalized, value)) normalized.push_back(std::move(value));
        }
        items_ = std::move(normalized);
    }

    [[nodiscard]] bool dirty() const {
        return !_metal_added().empty() || !_metal_removed().empty();
    }

    // ORM hooks. They are intentionally public but prefixed: relation loading and
    // Unit of Work are generated from reflected members and need no friend registry.
    void _metal_hydrate(std::vector<value_type> values) {
        items_ = deduplicate(std::move(values));
        baseline_ = items_;
        loaded_ = true;
    }

    [[nodiscard]] std::vector<value_type> _metal_added() const {
        std::vector<value_type> out;
        for (const auto& value : items_) {
            if (!contains(baseline_, value)) out.push_back(value);
        }
        return out;
    }

    [[nodiscard]] std::vector<value_type> _metal_removed() const {
        std::vector<value_type> out;
        for (const auto& value : baseline_) {
            if (!contains(items_, value)) out.push_back(value);
        }
        return out;
    }

    void _metal_accept_changes() {
        baseline_ = items_;
    }

private:
    static bool contains(const std::vector<value_type>& values, const value_type& value) {
        return std::find(values.begin(), values.end(), value) != values.end();
    }

    static std::vector<value_type> deduplicate(std::vector<value_type> values) {
        std::vector<value_type> out;
        out.reserve(values.size());
        for (auto& value : values) {
            if (value && !contains(out, value)) out.push_back(std::move(value));
        }
        return out;
    }

    bool loaded_{false};
    std::vector<value_type> items_;
    std::vector<value_type> baseline_;
};

} // namespace metal
