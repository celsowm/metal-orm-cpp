#pragma once

#include "metal/value.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace metal {

template <typename T>
class has_many_collection {
public:
    using value_type = std::shared_ptr<T>;
    using const_iterator = typename std::vector<value_type>::const_iterator;

    has_many_collection() = default;

    [[nodiscard]] bool loaded() const noexcept { return loaded_; }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }

    const value_type& operator[](std::size_t index) const { return items_.at(index); }
    const value_type& at(std::size_t index) const { return items_.at(index); }
    const std::vector<value_type>& get_items() const noexcept { return items_; }

    const_iterator begin() const noexcept { return items_.begin(); }
    const_iterator end() const noexcept { return items_.end(); }
    const_iterator cbegin() const noexcept { return items_.cbegin(); }
    const_iterator cend() const noexcept { return items_.cend(); }

    const std::vector<value_type>& load() {
        if (!loaded_) {
            if (!loader_) throw std::logic_error("MetalORM: relation collection is not bound to a Session");
            loader_();
        }
        return items_;
    }

    value_type add() {
        auto entity = std::make_shared<T>();
        attach(entity);
        return entity;
    }

    value_type add(T value) {
        auto entity = std::make_shared<T>(std::move(value));
        attach(entity);
        return entity;
    }

    void attach(value_type value) {
        if (!value) throw std::invalid_argument("MetalORM: cannot attach a null entity");
        if (!contains(items_, value)) items_.push_back(std::move(value));
    }

    bool remove(const value_type& value) {
        if (!value) return false;
        const auto it = std::find(items_.begin(), items_.end(), value);
        if (it == items_.end()) return false;
        items_.erase(it);
        return true;
    }

    void clear() { items_.clear(); }

    [[nodiscard]] bool dirty() const {
        return !_metal_added().empty() || !_metal_removed().empty();
    }

    void _metal_bind_loader(std::function<void()> loader) { loader_ = std::move(loader); }

    void _metal_hydrate(std::vector<value_type> values) {
        items_ = deduplicate(std::move(values));
        baseline_ = items_;
        loaded_ = true;
    }

    [[nodiscard]] std::vector<value_type> _metal_added() const {
        std::vector<value_type> out;
        for (const auto& value : items_) if (!contains(baseline_, value)) out.push_back(value);
        return out;
    }

    [[nodiscard]] std::vector<value_type> _metal_removed() const {
        std::vector<value_type> out;
        for (const auto& value : baseline_) if (!contains(items_, value)) out.push_back(value);
        return out;
    }

    void _metal_accept_changes() { baseline_ = items_; }

private:
    static bool contains(const std::vector<value_type>& values, const value_type& value) {
        return std::find(values.begin(), values.end(), value) != values.end();
    }

    static std::vector<value_type> deduplicate(std::vector<value_type> values) {
        std::vector<value_type> out;
        out.reserve(values.size());
        for (auto& value : values) if (value && !contains(out, value)) out.push_back(std::move(value));
        return out;
    }

    bool loaded_{false};
    std::vector<value_type> items_;
    std::vector<value_type> baseline_;
    std::function<void()> loader_;
};

template <typename T, typename Pivot>
class many_to_many_collection {
public:
    using value_type = std::shared_ptr<T>;
    using pivot_type = Pivot;
    using const_iterator = typename std::vector<value_type>::const_iterator;

    many_to_many_collection() = default;

    [[nodiscard]] bool loaded() const noexcept { return loaded_; }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }

    const value_type& operator[](std::size_t index) const { return items_.at(index); }
    const value_type& at(std::size_t index) const { return items_.at(index); }
    const std::vector<value_type>& get_items() const noexcept { return items_; }

    const_iterator begin() const noexcept { return items_.begin(); }
    const_iterator end() const noexcept { return items_.end(); }
    const_iterator cbegin() const noexcept { return items_.cbegin(); }
    const_iterator cend() const noexcept { return items_.cend(); }

    const std::vector<value_type>& load() {
        if (!loaded_) {
            if (!loader_) throw std::logic_error("MetalORM: relation collection is not bound to a Session");
            loader_();
        }
        return items_;
    }

    void attach(value_type value) { attach_impl(std::move(value), std::nullopt); }
    void attach(value_type value, Pivot pivot) { attach_impl(std::move(value), std::optional<Pivot>{std::move(pivot)}); }

    template <typename Key>
    requires (!std::same_as<std::remove_cvref_t<Key>, value_type>)
    value_type attach(Key&& key) {
        return attach_id_value(to_value(std::forward<Key>(key)), std::nullopt);
    }

    template <typename Key>
    requires (!std::same_as<std::remove_cvref_t<Key>, value_type>)
    value_type attach(Key&& key, Pivot pivot) {
        return attach_id_value(to_value(std::forward<Key>(key)), std::optional<Pivot>{std::move(pivot)});
    }

    bool detach(const value_type& value) {
        if (!value) return false;
        const auto index = find_equivalent(value);
        if (!index) return false;
        items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(*index));
        return true;
    }

    template <typename Key>
    requires (!std::same_as<std::remove_cvref_t<Key>, value_type>)
    bool detach(Key&& key) {
        ensure_identity_binding();
        const auto wanted = value_key(to_value(std::forward<Key>(key)));
        const auto it = std::find_if(items_.begin(), items_.end(), [&](const auto& item) {
            const auto identity = identity_key_(*item);
            return identity && *identity == wanted;
        });
        if (it == items_.end()) return false;
        items_.erase(it);
        return true;
    }

    template <typename Range>
    void sync_by_ids(const Range& ids) {
        load();
        ensure_identity_binding();
        std::unordered_set<std::string> desired;
        for (const auto& id : ids) {
            const Value value = to_value(id);
            const auto key = value_key(value);
            desired.insert(key);
            const bool exists = std::any_of(items_.begin(), items_.end(), [&](const auto& item) {
                const auto identity = identity_key_(*item);
                return identity && *identity == key;
            });
            if (!exists) attach_id_value(value, std::nullopt);
        }
        items_.erase(
            std::remove_if(items_.begin(), items_.end(), [&](const auto& item) {
                const auto identity = identity_key_(*item);
                return identity && !desired.contains(*identity);
            }),
            items_.end());
    }

    const Pivot* pivot(const value_type& value) const {
        if (!value) return nullptr;
        if (const auto index = find_equivalent(value)) {
            auto it = pivots_.find(items_[*index].get());
            return it == pivots_.end() ? nullptr : &it->second;
        }
        return nullptr;
    }

    [[nodiscard]] bool dirty() const {
        return !_metal_added().empty() || !_metal_removed().empty() || !pivot_updates_.empty();
    }

    void _metal_bind_loader(std::function<void()> loader) { loader_ = std::move(loader); }

    void _metal_bind_identity(
        std::function<std::optional<std::string>(const T&)> identity_key,
        std::function<value_type(const Value&)> id_factory) {
        identity_key_ = std::move(identity_key);
        id_factory_ = std::move(id_factory);
    }

    void _metal_hydrate(std::vector<std::pair<value_type, std::optional<Pivot>>> values) {
        items_.clear();
        pivots_.clear();
        for (auto& [entity, pivot_value] : values) {
            if (!entity || find_equivalent(entity)) continue;
            if (pivot_value) pivots_.emplace(entity.get(), std::move(*pivot_value));
            items_.push_back(std::move(entity));
        }
        baseline_ = items_;
        pivot_updates_.clear();
        loaded_ = true;
    }

    [[nodiscard]] std::vector<value_type> _metal_added() const {
        std::vector<value_type> out;
        for (const auto& value : items_) if (!contains_equivalent(baseline_, value)) out.push_back(value);
        return out;
    }

    [[nodiscard]] std::vector<value_type> _metal_removed() const {
        std::vector<value_type> out;
        for (const auto& value : baseline_) if (!contains_equivalent(items_, value)) out.push_back(value);
        return out;
    }

    [[nodiscard]] std::vector<value_type> _metal_pivot_updates() const {
        std::vector<value_type> out;
        for (const auto& value : items_) {
            if (pivot_updates_.contains(value.get()) && contains_equivalent(baseline_, value)) out.push_back(value);
        }
        return out;
    }

    const Pivot* _metal_pivot(const value_type& value) const { return pivot(value); }

    void _metal_accept_changes() {
        baseline_ = items_;
        pivot_updates_.clear();
    }

private:
    void attach_impl(value_type value, std::optional<Pivot> pivot_value) {
        if (!value) throw std::invalid_argument("MetalORM: cannot attach a null entity");
        if (const auto index = find_equivalent(value)) {
            auto& existing = items_[*index];
            if (pivot_value) {
                pivots_.insert_or_assign(existing.get(), std::move(*pivot_value));
                if (contains_equivalent(baseline_, existing)) pivot_updates_.insert(existing.get());
            }
            return;
        }
        if (pivot_value) pivots_.emplace(value.get(), std::move(*pivot_value));
        items_.push_back(std::move(value));
    }

    value_type attach_id_value(const Value& key, std::optional<Pivot> pivot_value) {
        ensure_identity_binding();
        const auto wanted = value_key(key);
        for (const auto& item : items_) {
            const auto identity = identity_key_(*item);
            if (identity && *identity == wanted) {
                if (pivot_value) {
                    pivots_.insert_or_assign(item.get(), std::move(*pivot_value));
                    if (contains_equivalent(baseline_, item)) pivot_updates_.insert(item.get());
                }
                return item;
            }
        }
        if (!id_factory_) throw std::logic_error("MetalORM: relation collection cannot materialize an ID stub");
        auto entity = id_factory_(key);
        attach_impl(entity, std::move(pivot_value));
        return entity;
    }

    void ensure_identity_binding() const {
        if (!identity_key_) throw std::logic_error("MetalORM: ID-based collection operation requires a Session-bound relation");
    }

    std::optional<std::size_t> find_equivalent(const value_type& value) const {
        for (std::size_t i = 0; i < items_.size(); ++i) if (same_entity(items_[i], value)) return i;
        return std::nullopt;
    }

    bool contains_equivalent(const std::vector<value_type>& values, const value_type& value) const {
        return std::any_of(values.begin(), values.end(), [&](const auto& candidate) { return same_entity(candidate, value); });
    }

    bool same_entity(const value_type& left, const value_type& right) const {
        if (left == right) return true;
        if (!left || !right || !identity_key_) return false;
        const auto l = identity_key_(*left);
        const auto r = identity_key_(*right);
        return l && r && *l == *r;
    }

    bool loaded_{false};
    std::vector<value_type> items_;
    std::vector<value_type> baseline_;
    std::unordered_map<const T*, Pivot> pivots_;
    std::unordered_set<const T*> pivot_updates_;
    std::function<void()> loader_;
    std::function<std::optional<std::string>(const T&)> identity_key_;
    std::function<value_type(const Value&)> id_factory_;
};

} // namespace metal

namespace metal::reflect {

template <typename T>
struct many_collection_traits {
    static constexpr bool value = false;
};

template <typename Target>
struct many_collection_traits<metal::has_many_collection<Target>> {
    static constexpr bool value = true;
    using target_type = Target;
};

template <typename Target, typename Pivot>
struct many_collection_traits<metal::many_to_many_collection<Target, Pivot>> {
    static constexpr bool value = true;
    using target_type = Target;
};

template <typename T>
inline constexpr bool is_many_collection_v = many_collection_traits<std::remove_cvref_t<T>>::value;

template <typename T>
using many_target_t = typename many_collection_traits<std::remove_cvref_t<T>>::target_type;

} // namespace metal::reflect
