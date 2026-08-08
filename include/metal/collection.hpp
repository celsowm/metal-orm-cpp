#pragma once

#include "metal/mapping.hpp"
#include "metal/value.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace metal {

template <typename Pivot>
class pivot_patch {
public:
    struct entry {
        std::string column;
        Value value;
        std::function<void(Pivot&)> apply;
    };

    template <std::meta::info Member, typename V>
    pivot_patch& set(V&& value) {
        static_assert(std::meta::is_nonstatic_data_member(Member),
                      "MetalORM: pivot_patch::set requires a non-static data member reflection");
        using Owner = [: std::meta::parent_of(Member) :];
        static_assert(std::same_as<Owner, Pivot>,
                      "MetalORM: pivot_patch member must belong to the pivot type");
        using M = [: std::meta::type_of(Member) :];
        static_assert(PersistableValue<M>,
                      "MetalORM: pivot_patch only supports persistent scalar members");
        static_assert(compatible_value<M, V>(),
                      "MetalORM: pivot_patch value is incompatible with pivot member type");

        constexpr auto column = []() consteval -> std::string_view {
            auto annotations = std::meta::annotations_of_with_type(Member, ^^mapping::column);
            if (annotations.size() == 1) {
                return std::meta::extract<mapping::column>(annotations.front()).name.view();
            }
            return std::meta::identifier_of(Member);
        }();

        Value converted = to_value(std::forward<V>(value));
        entry next{
            std::string(column),
            converted,
            [converted](Pivot& pivot) {
                pivot.[:Member:] = from_value<M>(converted);
            }
        };

        auto existing = std::find_if(entries_.begin(), entries_.end(), [&](const auto& current) {
            return current.column == next.column;
        });
        if (existing == entries_.end()) entries_.push_back(std::move(next));
        else *existing = std::move(next);
        return *this;
    }

    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] const std::vector<entry>& entries() const noexcept { return entries_; }

    void apply_to(Pivot& pivot) const {
        for (const auto& value : entries_) value.apply(pivot);
    }

    void merge(const pivot_patch& other) {
        for (const auto& incoming : other.entries_) {
            auto existing = std::find_if(entries_.begin(), entries_.end(), [&](const auto& current) {
                return current.column == incoming.column;
            });
            if (existing == entries_.end()) entries_.push_back(incoming);
            else *existing = incoming;
        }
    }

    template <typename Predicate>
    pivot_patch filtered(Predicate&& predicate) const {
        pivot_patch out;
        for (const auto& value : entries_) {
            if (std::invoke(predicate, std::string_view(value.column))) out.entries_.push_back(value);
        }
        return out;
    }

private:
    template <typename Member, typename Input>
    static consteval bool compatible_value() {
        using M = std::remove_cvref_t<Member>;
        using V = std::remove_cvref_t<Input>;

        if constexpr (is_optional_v<M>) {
            using Inner = typename M::value_type;
            if constexpr (std::same_as<V, std::nullptr_t>) return true;
            if constexpr (is_optional_v<V>) {
                return compatible_value<Inner, typename V::value_type>();
            }
            return compatible_value<Inner, Input>();
        } else if constexpr (is_optional_v<V> || std::same_as<V, std::nullptr_t>) {
            return false;
        } else if constexpr (std::same_as<M, std::string>) {
            return std::same_as<V, std::string> ||
                   std::same_as<V, std::string_view> ||
                   std::is_convertible_v<Input, std::string_view>;
        } else if constexpr (std::same_as<M, bool>) {
            return std::same_as<V, bool>;
        } else if constexpr (std::is_integral_v<M>) {
            return std::is_integral_v<V> && !std::same_as<V, bool>;
        } else if constexpr (std::is_floating_point_v<M>) {
            return std::is_arithmetic_v<V> && !std::same_as<V, bool>;
        } else {
            return false;
        }
    }

    std::vector<entry> entries_;
};

template <typename T>
class has_many_collection {
public:
    using value_type = std::shared_ptr<T>;
    using const_iterator = typename std::vector<value_type>::const_iterator;

    [[nodiscard]] bool loaded() const noexcept { return loaded_; }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
    const value_type& operator[](std::size_t index) const { return items_.at(index); }
    const value_type& at(std::size_t index) const { return items_.at(index); }
    const std::vector<value_type>& get_items() const noexcept { return items_; }
    const_iterator begin() const noexcept { return items_.begin(); }
    const_iterator end() const noexcept { return items_.end(); }

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
        if (attach_hook_) attach_hook_(*value);
        items_.push_back(std::move(value));
    }

    bool remove(const value_type& value) {
        if (!value) return false;
        const auto it = std::find(items_.begin(), items_.end(), value);
        if (it == items_.end()) return false;
        items_.erase(it);
        return true;
    }

    void clear() { items_.clear(); }
    [[nodiscard]] bool dirty() const { return !_metal_added().empty() || !_metal_removed().empty(); }

    void _metal_bind_loader(std::function<void()> loader) { loader_ = std::move(loader); }
    void _metal_bind_attach(std::function<void(T&)> hook) { attach_hook_ = std::move(hook); }

    void _metal_hydrate(std::vector<value_type> values) {
        items_ = std::move(values);
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

    bool loaded_{false};
    std::vector<value_type> items_;
    std::vector<value_type> baseline_;
    std::function<void()> loader_;
    std::function<void(T&)> attach_hook_;
};

template <typename T, typename Pivot>
class many_to_many_collection {
public:
    using value_type = std::shared_ptr<T>;
    using pivot_type = Pivot;
    using patch_type = pivot_patch<Pivot>;
    using const_iterator = typename std::vector<value_type>::const_iterator;

    [[nodiscard]] bool loaded() const noexcept { return loaded_; }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
    const value_type& operator[](std::size_t index) const { return items_.at(index); }
    const value_type& at(std::size_t index) const { return items_.at(index); }
    const std::vector<value_type>& get_items() const noexcept { return items_; }
    const_iterator begin() const noexcept { return items_.begin(); }
    const_iterator end() const noexcept { return items_.end(); }

    const std::vector<value_type>& load() {
        if (!loaded_) {
            if (!loader_) throw std::logic_error("MetalORM: relation collection is not bound to a Session");
            loader_();
        }
        return items_;
    }

    void attach(value_type value) { attach_impl(std::move(value), patch_type{}); }
    void attach(value_type value, patch_type patch) { attach_impl(std::move(value), sanitize(std::move(patch))); }

    template <typename Key>
    requires (!std::same_as<std::remove_cvref_t<Key>, value_type>)
    value_type attach(Key&& key) { return attach_id_value(to_value(std::forward<Key>(key)), patch_type{}); }

    template <typename Key>
    requires (!std::same_as<std::remove_cvref_t<Key>, value_type>)
    value_type attach(Key&& key, patch_type patch) {
        return attach_id_value(to_value(std::forward<Key>(key)), sanitize(std::move(patch)));
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
            if (!exists) attach_id_value(value, patch_type{});
        }
        items_.erase(std::remove_if(items_.begin(), items_.end(), [&](const auto& item) {
            const auto identity = identity_key_(*item);
            return identity && !desired.contains(*identity);
        }), items_.end());
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
        return !_metal_added().empty() || !_metal_removed().empty() || !pivot_patches_.empty();
    }

    void _metal_bind_loader(std::function<void()> loader) { loader_ = std::move(loader); }
    void _metal_bind_identity(
        std::function<std::optional<std::string>(const T&)> identity_key,
        std::function<value_type(const Value&)> id_factory) {
        identity_key_ = std::move(identity_key);
        id_factory_ = std::move(id_factory);
    }
    void _metal_bind_pivot_filter(std::function<patch_type(const patch_type&)> filter) {
        pivot_filter_ = std::move(filter);
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
        pivot_patches_.clear();
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
            if (pivot_patches_.contains(value.get()) && contains_equivalent(baseline_, value)) out.push_back(value);
        }
        return out;
    }

    const patch_type* _metal_pivot_patch(const value_type& value) const {
        if (!value) return nullptr;
        if (const auto index = find_equivalent(value)) {
            auto it = pivot_patches_.find(items_[*index].get());
            return it == pivot_patches_.end() ? nullptr : &it->second;
        }
        return nullptr;
    }

    void _metal_accept_changes() {
        baseline_ = items_;
        pivot_patches_.clear();
        for (auto it = pivots_.begin(); it != pivots_.end();) {
            const bool present = std::any_of(items_.begin(), items_.end(), [&](const auto& item) {
                return item.get() == it->first;
            });
            if (!present) it = pivots_.erase(it);
            else ++it;
        }
    }

private:
    patch_type sanitize(patch_type patch) const {
        return pivot_filter_ ? pivot_filter_(patch) : std::move(patch);
    }

    void apply_patch(const value_type& value, const patch_type& patch) {
        if (patch.empty()) return;
        auto [it, inserted] = pivots_.try_emplace(value.get(), Pivot{});
        patch.apply_to(it->second);
        pivot_patches_[value.get()].merge(patch);
    }

    void attach_impl(value_type value, patch_type patch) {
        if (!value) throw std::invalid_argument("MetalORM: cannot attach a null entity");
        if (const auto index = find_equivalent(value)) {
            auto& existing = items_[*index];
            apply_patch(existing, patch);
            return;
        }
        apply_patch(value, patch);
        items_.push_back(std::move(value));
    }

    value_type attach_id_value(const Value& key, patch_type patch) {
        ensure_identity_binding();
        const auto wanted = value_key(key);
        for (const auto& item : items_) {
            const auto identity = identity_key_(*item);
            if (identity && *identity == wanted) {
                apply_patch(item, patch);
                return item;
            }
        }
        if (!id_factory_) throw std::logic_error("MetalORM: relation collection cannot materialize an ID target");
        auto entity = id_factory_(key);
        attach_impl(entity, std::move(patch));
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
    std::unordered_map<const T*, patch_type> pivot_patches_;
    std::function<void()> loader_;
    std::function<std::optional<std::string>(const T&)> identity_key_;
    std::function<value_type(const Value&)> id_factory_;
    std::function<patch_type(const patch_type&)> pivot_filter_;
};

} // namespace metal

namespace metal::reflect {

template <typename T> struct many_collection_traits { static constexpr bool value = false; };
template <typename Target> struct many_collection_traits<metal::has_many_collection<Target>> {
    static constexpr bool value = true; using target_type = Target;
};
template <typename Target, typename Pivot> struct many_collection_traits<metal::many_to_many_collection<Target, Pivot>> {
    static constexpr bool value = true; using target_type = Target;
};
template <typename T> inline constexpr bool is_many_collection_v = many_collection_traits<std::remove_cvref_t<T>>::value;
template <typename T> using many_target_t = typename many_collection_traits<std::remove_cvref_t<T>>::target_type;

} // namespace metal::reflect
