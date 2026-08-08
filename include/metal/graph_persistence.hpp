#pragma once

#include "metal/orm.hpp"
#include "metal/reference_traits.hpp"

#include <algorithm>
#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace metal {

struct GraphOptions {
    bool prune_missing{false};
    bool transactional{true};
    bool flush{false};
};

template <reflect::Entity T>
class graph_payload;

namespace graph_detail {

template <std::meta::info Relation>
struct relation_info {
    using annotation = reflect::relation_annotation_t<Relation>;
    using traits = mapping::relation_annotation_traits<annotation>;
    using member_type = reflect::member_type_t<Relation>;
    static constexpr auto kind = traits::kind;

    using target_type = typename decltype([] {
        if constexpr (kind == mapping::relation_kind::belongs_to ||
                      kind == mapping::relation_kind::has_one) {
            return std::type_identity<reflect::single_target_t<member_type>>{};
        } else if constexpr (kind == mapping::relation_kind::has_many) {
            return std::type_identity<reflect::has_many_target_t<member_type>>{};
        } else if constexpr (kind == mapping::relation_kind::many_to_many) {
            return std::type_identity<reflect::many_to_many_target_t<member_type>>{};
        } else if constexpr (kind == mapping::relation_kind::morph_one) {
            return std::type_identity<reflect::morph_one_target_t<member_type>>{};
        } else if constexpr (kind == mapping::relation_kind::morph_many) {
            return std::type_identity<reflect::morph_many_target_t<member_type>>{};
        } else {
            return std::type_identity<void>{};
        }
    }())::type;

    using pivot_type = typename decltype([] {
        if constexpr (kind == mapping::relation_kind::many_to_many) {
            return std::type_identity<reflect::many_to_many_pivot_t<member_type>>{};
        } else {
            return std::type_identity<void>{};
        }
    }())::type;

    static consteval std::meta::info target_key() {
        if constexpr (kind == mapping::relation_kind::belongs_to ||
                      kind == mapping::relation_kind::many_to_many) {
            return reflect::key_or_primary<target_type>(traits::target_key());
        } else if constexpr (kind == mapping::relation_kind::has_one ||
                             kind == mapping::relation_kind::has_many ||
                             kind == mapping::relation_kind::morph_one ||
                             kind == mapping::relation_kind::morph_many) {
            return reflect::primary_key_member<target_type>();
        } else {
            return std::meta::info{};
        }
    }
};

template <std::meta::info Relation>
consteval bool is_collection_relation() {
    using Info = relation_info<Relation>;
    return Info::kind == mapping::relation_kind::has_many ||
           Info::kind == mapping::relation_kind::many_to_many ||
           Info::kind == mapping::relation_kind::morph_many;
}

template <typename Member, typename Input>
consteval bool value_compatible() {
    using M = std::remove_cvref_t<Member>;
    using V = std::remove_cvref_t<Input>;
    if constexpr (is_optional_v<M>) {
        using Inner = typename M::value_type;
        if constexpr (std::same_as<V, std::nullopt_t>) return true;
        if constexpr (is_optional_v<V>) return value_compatible<Inner, typename V::value_type>();
        return value_compatible<Inner, Input>();
    } else if constexpr (is_optional_v<V> || std::same_as<V, std::nullopt_t>) {
        return false;
    } else if constexpr (std::same_as<M, std::string>) {
        return std::constructible_from<std::string, Input>;
    } else if constexpr (std::same_as<M, bool>) {
        return std::same_as<V, bool>;
    } else if constexpr (std::is_integral_v<M>) {
        return std::is_integral_v<V> && !std::same_as<V, bool>;
    } else if constexpr (std::is_floating_point_v<M>) {
        return std::is_arithmetic_v<V> && !std::same_as<V, bool>;
    } else {
        return std::constructible_from<M, Input>;
    }
}

template <reflect::Entity T, std::meta::info Key>
bool key_missing(const T& entity) {
    const Value value = to_value(entity.[:Key:]);
    if (std::holds_alternative<std::nullptr_t>(value)) return true;
    constexpr auto pk = reflect::primary_key_member<T>();
    if constexpr (Key == pk && reflect::primary_key_is_generated<T>()) {
        return is_empty_generated_value(value);
    }
    return false;
}

template <reflect::Entity T, std::meta::info Key>
std::shared_ptr<T> find_by_key(Session& session, const Value& key) {
    using K = reflect::member_type_t<Key>;
    const K typed = from_value<K>(key);
    constexpr auto pk = reflect::primary_key_member<T>();
    if constexpr (Key == pk) return session.find<T>(typed);
    else return session.query<T>().where(field<Key> == typed).first();
}

template <reflect::Entity T, std::meta::info Key>
std::shared_ptr<T> stub_by_key(Session& session, const Value& key, bool tracked) {
    if (auto found = find_by_key<T, Key>(session, key)) return found;
    auto target = std::make_shared<T>();
    using K = reflect::member_type_t<Key>;
    target->[:Key:] = from_value<K>(key);
    if (tracked) session.persist(target);
    return target;
}

template <reflect::Entity T>
std::shared_ptr<T> materialize(Session&, const graph_payload<T>&, const GraphOptions&);

} // namespace graph_detail

template <std::meta::info Relation>
class graph_collection_input {
    using info = graph_detail::relation_info<Relation>;
public:
    using target_type = typename info::target_type;
    using pivot_type = typename info::pivot_type;

    struct item {
        std::shared_ptr<graph_payload<target_type>> payload;
        std::shared_ptr<target_type> entity;
        std::optional<Value> key;
        std::shared_ptr<void> pivot;
    };

    graph_collection_input& add(graph_payload<target_type> payload) {
        item next;
        next.payload = std::make_shared<graph_payload<target_type>>(std::move(payload));
        items_.push_back(std::move(next));
        return *this;
    }

    graph_collection_input& add(std::shared_ptr<target_type> entity) {
        if (!entity) throw std::invalid_argument("MetalORM: graph collection cannot add a null entity");
        item next;
        next.entity = std::move(entity);
        items_.push_back(std::move(next));
        return *this;
    }

    template <typename Key>
    graph_collection_input& add_id(Key&& key) {
        constexpr auto member = info::target_key();
        using K = reflect::member_type_t<member>;
        static_assert(graph_detail::value_compatible<K, Key>(),
                      "MetalORM: graph relation id is incompatible with the relation target key");
        item next;
        next.key = to_value(std::forward<Key>(key));
        items_.push_back(std::move(next));
        return *this;
    }

    template <typename P = pivot_type>
    requires (!std::is_void_v<P>)
    graph_collection_input& add(graph_payload<target_type> payload, pivot_patch<P> patch) {
        item next;
        next.payload = std::make_shared<graph_payload<target_type>>(std::move(payload));
        next.pivot = std::make_shared<pivot_patch<P>>(std::move(patch));
        items_.push_back(std::move(next));
        return *this;
    }

    template <typename Key, typename P = pivot_type>
    requires (!std::is_void_v<P>)
    graph_collection_input& add_id(Key&& key, pivot_patch<P> patch) {
        constexpr auto member = info::target_key();
        using K = reflect::member_type_t<member>;
        static_assert(graph_detail::value_compatible<K, Key>(),
                      "MetalORM: graph relation id is incompatible with the relation target key");
        item next;
        next.key = to_value(std::forward<Key>(key));
        next.pivot = std::make_shared<pivot_patch<P>>(std::move(patch));
        items_.push_back(std::move(next));
        return *this;
    }

    const std::vector<item>& items() const noexcept { return items_; }

private:
    std::vector<item> items_;
};

namespace graph_detail {

template <std::meta::info Relation, reflect::Entity Root, reflect::Entity Target>
void apply_single_payload(Session&, Root&, const graph_payload<Target>&, const GraphOptions&);

template <std::meta::info Relation, reflect::Entity Root>
void apply_single_key(Session&, Root&, const Value&, const GraphOptions&);

template <std::meta::info Relation, reflect::Entity Root>
void clear_single(Session&, Root&, const GraphOptions&);

template <std::meta::info Relation, reflect::Entity Root>
void apply_collection(Session&, Root&, const graph_collection_input<Relation>&, const GraphOptions&);

template <std::meta::info Relation, reflect::Entity Root, reflect::Entity Target>
void apply_morph_to_payload(Session&, Root&, const graph_payload<Target>&, const GraphOptions&);

} // namespace graph_detail

template <reflect::Entity T>
class graph_payload {
public:
    using entity_type = T;
    using scalar_action = std::function<void(T&)>;
    using relation_action = std::function<void(Session&, T&, const GraphOptions&)>;

    template <std::meta::info Member, typename V>
    graph_payload& set(V&& value) {
        static_assert(std::meta::is_nonstatic_data_member(Member),
                      "MetalORM: graph::set requires a non-static data member reflection");
        static_assert(std::same_as<reflect::owner_type_t<Member>, T>,
                      "MetalORM: graph::set member must belong to the graph entity type");
        static_assert(reflect::is_persistent_member<Member>(),
                      "MetalORM: graph::set only accepts persistent scalar members");
        using M = reflect::member_type_t<Member>;
        static_assert(graph_detail::value_compatible<M, V>(),
                      "MetalORM: graph value is incompatible with the reflected member type");

        M converted{std::forward<V>(value)};
        const Value stored = to_value(converted);
        scalars_.push_back([converted = std::move(converted)](T& entity) {
            entity.[:Member:] = converted;
        });
        if constexpr (Member == reflect::primary_key_member<T>()) primary_key_ = stored;
        return *this;
    }

    template <std::meta::info Relation, reflect::Entity Target>
    graph_payload& relation(graph_payload<Target> payload) {
        validate_relation<Relation>();
        using Info = graph_detail::relation_info<Relation>;
        if constexpr (Info::kind == mapping::relation_kind::morph_to) {
            using Wrapper = reflect::member_type_t<Relation>;
            static_assert(
                reflect::morph_to_reference_traits<std::remove_cvref_t<Wrapper>>::template contains<Target>,
                "MetalORM: graph morph_to target is not declared by the relation wrapper");
            pre_.push_back([payload = std::move(payload)](Session& session, T& root, const GraphOptions& options) {
                graph_detail::apply_morph_to_payload<Relation>(session, root, payload, options);
            });
        } else {
            static_assert(std::same_as<typename Info::target_type, Target>,
                          "MetalORM: graph relation payload type does not match the reflected target");
            auto action = [payload = std::move(payload)](Session& session, T& root, const GraphOptions& options) {
                graph_detail::apply_single_payload<Relation>(session, root, payload, options);
            };
            if constexpr (Info::kind == mapping::relation_kind::belongs_to) pre_.push_back(std::move(action));
            else post_.push_back(std::move(action));
        }
        return *this;
    }

    template <std::meta::info Relation, typename Key>
    graph_payload& relation_id(Key&& key) {
        validate_relation<Relation>();
        using Info = graph_detail::relation_info<Relation>;
        static_assert(Info::kind == mapping::relation_kind::belongs_to ||
                      Info::kind == mapping::relation_kind::has_one ||
                      Info::kind == mapping::relation_kind::morph_one,
                      "MetalORM: relation_id requires belongs_to, has_one or morph_one");
        constexpr auto member = Info::target_key();
        using K = reflect::member_type_t<member>;
        static_assert(graph_detail::value_compatible<K, Key>(),
                      "MetalORM: graph relation id is incompatible with the relation target key");
        const Value stored = to_value(std::forward<Key>(key));
        auto action = [stored](Session& session, T& root, const GraphOptions& options) {
            graph_detail::apply_single_key<Relation>(session, root, stored, options);
        };
        if constexpr (Info::kind == mapping::relation_kind::belongs_to) pre_.push_back(std::move(action));
        else post_.push_back(std::move(action));
        return *this;
    }

    template <std::meta::info Relation>
    graph_payload& clear_relation() {
        validate_relation<Relation>();
        using Info = graph_detail::relation_info<Relation>;
        static_assert(Info::kind == mapping::relation_kind::belongs_to ||
                      Info::kind == mapping::relation_kind::has_one ||
                      Info::kind == mapping::relation_kind::morph_one ||
                      Info::kind == mapping::relation_kind::morph_to,
                      "MetalORM: clear_relation requires a single-target relation");
        auto action = [](Session& session, T& root, const GraphOptions& options) {
            graph_detail::clear_single<Relation>(session, root, options);
        };
        if constexpr (Info::kind == mapping::relation_kind::belongs_to ||
                      Info::kind == mapping::relation_kind::morph_to) pre_.push_back(std::move(action));
        else post_.push_back(std::move(action));
        return *this;
    }

    template <std::meta::info Relation, typename Configure>
    requires (graph_detail::is_collection_relation<Relation>() &&
              std::invocable<Configure, graph_collection_input<Relation>&>)
    graph_payload& relation(Configure&& configure) {
        validate_relation<Relation>();
        graph_collection_input<Relation> input;
        std::invoke(std::forward<Configure>(configure), input);
        post_.push_back([input = std::move(input)](Session& session, T& root, const GraphOptions& options) {
            graph_detail::apply_collection<Relation>(session, root, input, options);
        });
        return *this;
    }

    const std::optional<Value>& primary_key() const noexcept { return primary_key_; }
    bool has_post_relations() const noexcept { return !post_.empty(); }

    void apply_scalars(T& entity) const {
        for (const auto& action : scalars_) action(entity);
    }
    void apply_pre(Session& session, T& entity, const GraphOptions& options) const {
        for (const auto& action : pre_) action(session, entity, options);
    }
    void apply_post(Session& session, T& entity, const GraphOptions& options) const {
        for (const auto& action : post_) action(session, entity, options);
    }

private:
    template <std::meta::info Relation>
    static consteval void validate_relation() {
        static_assert(std::meta::is_nonstatic_data_member(Relation),
                      "MetalORM: graph relation requires a non-static data member reflection");
        static_assert(std::same_as<reflect::owner_type_t<Relation>, T>,
                      "MetalORM: graph relation member must belong to the graph entity type");
        static_assert(reflect::has_relation_annotation<Relation>(),
                      "MetalORM: graph relation member must have a relationship annotation");
    }

    std::optional<Value> primary_key_;
    std::vector<scalar_action> scalars_;
    std::vector<relation_action> pre_;
    std::vector<relation_action> post_;
};

template <reflect::Entity T>
graph_payload<T> graph() {
    static_assert(reflect::validate_mapping<T>());
    return {};
}

namespace graph_detail {

template <reflect::Entity T>
std::shared_ptr<T> materialize(Session& session, const graph_payload<T>& payload, const GraphOptions& options) {
    std::shared_ptr<T> entity;
    if (payload.primary_key()) {
        constexpr auto pk = reflect::primary_key_member<T>();
        entity = find_by_key<T, pk>(session, *payload.primary_key());
    }
    if (!entity) {
        entity = std::make_shared<T>();
        if (payload.primary_key()) reflect::set_primary_key_value(*entity, *payload.primary_key());
    }

    payload.apply_scalars(*entity);
    payload.apply_pre(session, *entity, options);
    if (!session.unit_of_work().contains(entity.get())) session.persist(entity);

    auto* tracked = session.unit_of_work().find(entity.get());
    const bool needs_root_key = tracked && tracked->status == EntityStatus::New &&
        reflect::primary_key_is_generated<T>() && payload.has_post_relations();
    if (needs_root_key) session.flush();

    payload.apply_post(session, *entity, options);
    return entity;
}

template <std::meta::info Relation, reflect::Entity Root, reflect::Entity Target>
void apply_single_payload(Session& session, Root& root, const graph_payload<Target>& payload, const GraphOptions& options) {
    using Info = relation_info<Relation>;
    using Traits = typename Info::traits;
    auto target = materialize(session, payload, options);

    if constexpr (Info::kind == mapping::relation_kind::belongs_to) {
        constexpr auto target_key = reflect::key_or_primary<Target>(Traits::target_key());
        constexpr auto foreign_key = Traits::foreign_key();
        if (key_missing<Target, target_key>(*target)) session.flush();
        if (key_missing<Target, target_key>(*target))
            throw std::runtime_error("MetalORM: graph belongs_to target has no persisted relation key");
        using FK = reflect::member_type_t<foreign_key>;
        root.[:foreign_key:] = from_value<FK>(to_value(target->[:target_key:]));
        root.[:Relation:].set(target);
    } else if constexpr (Info::kind == mapping::relation_kind::has_one) {
        constexpr auto root_key = reflect::key_or_primary<Root>(Traits::local_key());
        constexpr auto target_fk = Traits::target_foreign_key();
        using FK = reflect::member_type_t<target_fk>;
        target->[:target_fk:] = from_value<FK>(to_value(root.[:root_key:]));
        root.[:Relation:].set(target);
    } else if constexpr (Info::kind == mapping::relation_kind::morph_one) {
        constexpr auto root_key = reflect::key_or_primary<Root>(Traits::local_key());
        constexpr auto id_field = Traits::id_field();
        constexpr auto type_field = Traits::type_field();
        using Id = reflect::member_type_t<id_field>;
        using Type = reflect::member_type_t<type_field>;
        target->[:id_field:] = from_value<Id>(to_value(root.[:root_key:]));
        target->[:type_field:] = from_value<Type>(Value{std::string(Traits::type_value.view())});
        root.[:Relation:].set(target);
    }
}

template <std::meta::info Relation, reflect::Entity Root>
void apply_single_key(Session& session, Root& root, const Value& key, const GraphOptions&) {
    using Info = relation_info<Relation>;
    using Target = typename Info::target_type;
    using Traits = typename Info::traits;
    constexpr auto target_key = Info::target_key();
    auto target = stub_by_key<Target, target_key>(session, key, Info::kind != mapping::relation_kind::belongs_to);

    if constexpr (Info::kind == mapping::relation_kind::belongs_to) {
        constexpr auto foreign_key = Traits::foreign_key();
        using FK = reflect::member_type_t<foreign_key>;
        root.[:foreign_key:] = from_value<FK>(key);
        root.[:Relation:].set(target);
    } else if constexpr (Info::kind == mapping::relation_kind::has_one) {
        constexpr auto root_key = reflect::key_or_primary<Root>(Traits::local_key());
        constexpr auto target_fk = Traits::target_foreign_key();
        using FK = reflect::member_type_t<target_fk>;
        target->[:target_fk:] = from_value<FK>(to_value(root.[:root_key:]));
        root.[:Relation:].set(target);
    } else if constexpr (Info::kind == mapping::relation_kind::morph_one) {
        constexpr auto root_key = reflect::key_or_primary<Root>(Traits::local_key());
        constexpr auto id_field = Traits::id_field();
        constexpr auto type_field = Traits::type_field();
        using Id = reflect::member_type_t<id_field>;
        using Type = reflect::member_type_t<type_field>;
        target->[:id_field:] = from_value<Id>(to_value(root.[:root_key:]));
        target->[:type_field:] = from_value<Type>(Value{std::string(Traits::type_value.view())});
        root.[:Relation:].set(target);
    }
}

template <std::meta::info Relation, reflect::Entity Root>
void clear_single(Session& session, Root& root, const GraphOptions&) {
    using Info = relation_info<Relation>;
    using Traits = typename Info::traits;

    if constexpr (Info::kind == mapping::relation_kind::belongs_to) {
        constexpr auto foreign_key = Traits::foreign_key();
        using FK = reflect::member_type_t<foreign_key>;
        if constexpr (mapping::cascades_remove(Traits::cascade)) {
            if (auto previous = root.[:Relation:].get()) session.remove(previous);
        }
        if constexpr (is_optional_v<FK>) root.[:foreign_key:] = std::nullopt;
        else throw std::runtime_error("MetalORM: clearing belongs_to requires a nullable foreign key");
        root.[:Relation:].reset();
    } else if constexpr (Info::kind == mapping::relation_kind::has_one) {
        using Target = typename Info::target_type;
        constexpr auto target_fk = Traits::target_foreign_key();
        constexpr auto root_key = reflect::key_or_primary<Root>(Traits::local_key());
        auto previous = session.query<Target>().where(field<target_fk> == root.[:root_key:]).first();
        if (previous) {
            if constexpr (mapping::cascades_remove(Traits::cascade)) session.remove(previous);
            else {
                using FK = reflect::member_type_t<target_fk>;
                if constexpr (is_optional_v<FK>) {
                    previous->[:target_fk:] = std::nullopt;
                    session.persist(previous);
                } else throw std::runtime_error(
                    "MetalORM: clearing has_one requires nullable target FK or cascade remove");
            }
        }
        root.[:Relation:].reset();
    } else if constexpr (Info::kind == mapping::relation_kind::morph_one) {
        using Target = typename Info::target_type;
        constexpr auto id_field = Traits::id_field();
        constexpr auto type_field = Traits::type_field();
        constexpr auto root_key = reflect::key_or_primary<Root>(Traits::local_key());
        auto previous = session.query<Target>()
            .where((field<id_field> == root.[:root_key:]) &&
                   (field<type_field> == std::string(Traits::type_value.view())))
            .first();
        if (previous) {
            if constexpr (mapping::cascades_remove(Traits::cascade)) session.remove(previous);
            else {
                using Id = reflect::member_type_t<id_field>;
                using Type = reflect::member_type_t<type_field>;
                if constexpr (is_optional_v<Id> && is_optional_v<Type>) {
                    previous->[:id_field:] = std::nullopt;
                    previous->[:type_field:] = std::nullopt;
                    session.persist(previous);
                } else throw std::runtime_error(
                    "MetalORM: clearing morph_one requires nullable morph fields or cascade remove");
            }
        }
        root.[:Relation:].reset();
    } else if constexpr (Info::kind == mapping::relation_kind::morph_to) {
        constexpr auto id_field = Traits::id_field();
        constexpr auto type_field = Traits::type_field();
        using Id = reflect::member_type_t<id_field>;
        using Type = reflect::member_type_t<type_field>;
        if constexpr (is_optional_v<Id> && is_optional_v<Type>) {
            root.[:id_field:] = std::nullopt;
            root.[:type_field:] = std::nullopt;
        } else throw std::runtime_error("MetalORM: clearing morph_to requires nullable id/type fields");
        root.[:Relation:].reset();
    }
}

template <typename Target, typename Case>
consteval bool morph_case_matches() {
    using CaseTraits = mapping::morph_target_traits<Case>;
    using CaseTarget = [: CaseTraits::target() :];
    return std::same_as<Target, CaseTarget>;
}

template <std::meta::info Relation, reflect::Entity Root, reflect::Entity Target>
void apply_morph_to_payload(Session& session, Root& root, const graph_payload<Target>& payload, const GraphOptions& options) {
    using A = reflect::relation_annotation_t<Relation>;
    using Traits = mapping::relation_annotation_traits<A>;
    constexpr auto id_field = Traits::id_field();
    constexpr auto type_field = Traits::type_field();
    auto target = materialize(session, payload, options);
    bool matched = false;

    auto apply_case = [&]<typename Case>() {
        if constexpr (morph_case_matches<Target, Case>()) {
            using CaseTraits = mapping::morph_target_traits<Case>;
            constexpr auto target_key = reflect::key_or_primary<Target>(CaseTraits::target_key());
            if (key_missing<Target, target_key>(*target)) session.flush();
            if (key_missing<Target, target_key>(*target))
                throw std::runtime_error("MetalORM: graph morph_to target has no persisted relation key");
            using Id = reflect::member_type_t<id_field>;
            using Type = reflect::member_type_t<type_field>;
            root.[:id_field:] = from_value<Id>(to_value(target->[:target_key:]));
            root.[:type_field:] = from_value<Type>(Value{std::string(CaseTraits::type_value.view())});
            root.[:Relation:].template set<Target>(target);
            matched = true;
        }
    };
    [&]<typename... Cases>(mapping::type_list<Cases...>) {
        (apply_case.template operator()<Cases>(), ...);
    }(typename Traits::targets{});
    if (!matched) throw std::logic_error("MetalORM: graph morph_to target is not declared by the mapping");
}

template <std::meta::info Relation, reflect::Entity Root>
void apply_collection(Session& session, Root& root, const graph_collection_input<Relation>& input, const GraphOptions& options) {
    using Info = relation_info<Relation>;
    using Target = typename Info::target_type;
    constexpr auto key_member = Info::target_key();
    auto& collection = root.[:Relation:];
    collection.load();

    std::vector<std::string> seen;
    auto remember = [&](const std::shared_ptr<Target>& target) {
        if (!target || key_missing<Target, key_member>(*target)) return;
        seen.push_back(value_key(to_value(target->[:key_member:])));
    };

    for (const auto& item : input.items()) {
        std::shared_ptr<Target> target;
        if (item.payload) target = materialize(session, *item.payload, options);
        else if (item.entity) {
            target = item.entity;
            if (!session.unit_of_work().contains(target.get())) session.persist(target);
        } else if (item.key) {
            if constexpr (Info::kind == mapping::relation_kind::many_to_many) {
                using K = reflect::member_type_t<key_member>;
                const K typed = from_value<K>(*item.key);
                if (item.pivot) {
                    using Pivot = typename Info::pivot_type;
                    collection.attach(typed, *std::static_pointer_cast<pivot_patch<Pivot>>(item.pivot));
                } else collection.attach(typed);
                seen.push_back(value_key(*item.key));
                continue;
            } else target = stub_by_key<Target, key_member>(session, *item.key, true);
        }
        if (!target) continue;

        if constexpr (Info::kind == mapping::relation_kind::many_to_many) {
            if (item.pivot) {
                using Pivot = typename Info::pivot_type;
                collection.attach(target, *std::static_pointer_cast<pivot_patch<Pivot>>(item.pivot));
            } else collection.attach(target);
        } else {
            bool present = false;
            for (const auto& current : collection.get_items()) {
                if (current == target) { present = true; break; }
                if (current && !key_missing<Target, key_member>(*current) &&
                    !key_missing<Target, key_member>(*target) &&
                    value_key(to_value(current->[:key_member:])) ==
                    value_key(to_value(target->[:key_member:]))) {
                    present = true;
                    break;
                }
            }
            if (!present) collection.attach(target);
        }
        remember(target);
    }

    if (!options.prune_missing) return;
    const auto current = collection.get_items();
    for (const auto& target : current) {
        if (!target || key_missing<Target, key_member>(*target)) continue;
        const auto key = value_key(to_value(target->[:key_member:]));
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        if constexpr (Info::kind == mapping::relation_kind::many_to_many) collection.detach(target);
        else collection.remove(target);
    }
}

} // namespace graph_detail

template <reflect::Entity T>
std::shared_ptr<T> save_graph(Session& session, const graph_payload<T>& payload, GraphOptions options = {}) {
    auto execute = [&]() { return graph_detail::materialize(session, payload, options); };
    if (options.transactional) return session.transaction([&](Session&) { return execute(); });
    auto result = execute();
    if (options.flush) session.flush();
    return result;
}

template <reflect::Entity T>
std::shared_ptr<T> update_graph(Session& session, const graph_payload<T>& payload, GraphOptions options = {}) {
    if (!payload.primary_key())
        throw std::invalid_argument("MetalORM: update_graph requires the root primary key in the graph payload");

    auto execute = [&]() -> std::shared_ptr<T> {
        constexpr auto pk = reflect::primary_key_member<T>();
        auto existing = graph_detail::find_by_key<T, pk>(session, *payload.primary_key());
        if (!existing) return {};
        payload.apply_scalars(*existing);
        payload.apply_pre(session, *existing, options);
        payload.apply_post(session, *existing, options);
        return existing;
    };

    if (options.transactional) return session.transaction([&](Session&) { return execute(); });
    auto result = execute();
    if (result && options.flush) session.flush();
    return result;
}

template <reflect::Entity T>
std::shared_ptr<T> patch_graph(Session& session, const graph_payload<T>& payload, GraphOptions options = {}) {
    if (!payload.primary_key())
        throw std::invalid_argument("MetalORM: patch_graph requires the root primary key in the graph payload");
    return update_graph(session, payload, options);
}

} // namespace metal
