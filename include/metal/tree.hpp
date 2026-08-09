#pragma once

#include "metal/bulk.hpp"
#include "metal/dml.hpp"
#include "metal/orm.hpp"
#include "metal/query.hpp"
#include "metal/reflection.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace metal::mapping {

struct tree_parent_t {};
struct tree_left_t {};
struct tree_right_t {};
struct tree_depth_t {};
struct tree_scope_t {};

inline constexpr tree_parent_t tree_parent{};
inline constexpr tree_left_t tree_left{};
inline constexpr tree_right_t tree_right{};
inline constexpr tree_depth_t tree_depth{};
inline constexpr tree_scope_t tree_scope{};

} // namespace metal::mapping

namespace metal::reflect {

template <Mapped T>
consteval info tree_parent_member() {
    info result{};
    std::size_t count = 0;
    template for (constexpr auto member : data_members<T>()) {
        if constexpr (is_persistent_member<member>() && has<mapping::tree_parent_t>(member)) {
            result = member;
            ++count;
        }
    }
    if (count != 1) {
        throw "MetalORM: tree mapping requires exactly one [[=metal::mapping::tree_parent]] column";
    }
    return result;
}

template <Mapped T>
consteval info tree_left_member() {
    info result{};
    std::size_t count = 0;
    template for (constexpr auto member : data_members<T>()) {
        if constexpr (is_persistent_member<member>() && has<mapping::tree_left_t>(member)) {
            result = member;
            ++count;
        }
    }
    if (count != 1) {
        throw "MetalORM: tree mapping requires exactly one [[=metal::mapping::tree_left]] column";
    }
    return result;
}

template <Mapped T>
consteval info tree_right_member() {
    info result{};
    std::size_t count = 0;
    template for (constexpr auto member : data_members<T>()) {
        if constexpr (is_persistent_member<member>() && has<mapping::tree_right_t>(member)) {
            result = member;
            ++count;
        }
    }
    if (count != 1) {
        throw "MetalORM: tree mapping requires exactly one [[=metal::mapping::tree_right]] column";
    }
    return result;
}

template <Mapped T>
consteval info tree_depth_member() {
    info result{};
    std::size_t count = 0;
    template for (constexpr auto member : data_members<T>()) {
        if constexpr (is_persistent_member<member>() && has<mapping::tree_depth_t>(member)) {
            result = member;
            ++count;
        }
    }
    if (count > 1) {
        throw "MetalORM: tree mapping allows at most one [[=metal::mapping::tree_depth]] column";
    }
    return result;
}

template <Mapped T>
consteval bool validate_tree_mapping() {
    constexpr auto parent = tree_parent_member<T>();
    constexpr auto left = tree_left_member<T>();
    constexpr auto right = tree_right_member<T>();
    constexpr auto depth = tree_depth_member<T>();
    constexpr auto pk = primary_key_member<T>();

    static_assert(key_types_compatible<parent, pk>(),
                  "MetalORM: tree parent column must be key-compatible with the primary key");
    static_assert(is_optional_v<member_type_t<parent>>,
                  "MetalORM: tree parent column must be std::optional<T> so root nodes can be NULL");

    using Left = optional_value_t<member_type_t<left>>;
    using Right = optional_value_t<member_type_t<right>>;
    static_assert(std::is_integral_v<Left> && !std::same_as<Left, bool>,
                  "MetalORM: tree left column must be an integer");
    static_assert(std::is_integral_v<Right> && !std::same_as<Right, bool>,
                  "MetalORM: tree right column must be an integer");

    if constexpr (depth != info{}) {
        using Depth = optional_value_t<member_type_t<depth>>;
        static_assert(std::is_integral_v<Depth> && !std::same_as<Depth, bool>,
                      "MetalORM: tree depth column must be an integer");
    }

    template for (constexpr auto member : data_members<T>()) {
        if constexpr (has<mapping::tree_scope_t>(member)) {
            static_assert(is_persistent_member<member>(),
                          "MetalORM: tree scope members must be persistent scalar columns");
        }
    }
    return true;
}

} // namespace metal::reflect

namespace metal {

struct NestedSetBounds {
    std::int64_t lft{};
    std::int64_t rght{};
};

struct TreeNodeResult {
    Row data;
    std::int64_t lft{};
    std::int64_t rght{};
    Value parent_id{nullptr};
    std::optional<std::int64_t> depth;
    bool is_leaf{false};
    bool is_root{false};
    std::int64_t child_count{};
};

struct ThreadedTreeNode {
    Row node;
    std::vector<ThreadedTreeNode> children;
};

struct RecoverResult {
    std::size_t processed{0};
    bool success{false};
    std::vector<std::string> errors;
};

struct RecoverNode {
    Value pk{nullptr};
    std::int64_t lft{};
    std::int64_t rght{};
    Value parent_id{nullptr};
    std::optional<std::int64_t> depth;
};

struct RecoverUpdate {
    Value pk{nullptr};
    std::int64_t lft{};
    std::int64_t rght{};
    std::int64_t depth{};
};

class NestedSetStrategy {
public:
    static std::int64_t child_count(std::int64_t lft, std::int64_t rght) {
        return (rght - lft - 1) / 2;
    }

    static bool is_leaf(std::int64_t lft, std::int64_t rght) {
        return rght - lft == 1;
    }

    static bool is_root(const Value& parent_id) {
        return std::holds_alternative<std::nullptr_t>(parent_id);
    }

    static bool is_ancestor_of(NestedSetBounds a, NestedSetBounds b) {
        return a.lft < b.lft && a.rght > b.rght;
    }

    static bool is_descendant_of(NestedSetBounds a, NestedSetBounds b) {
        return a.lft > b.lft && a.rght < b.rght;
    }

    static std::int64_t subtree_width(std::int64_t lft, std::int64_t rght) {
        return rght - lft + 1;
    }

    static std::pair<std::int64_t, std::int64_t> insert_as_last_child(
        std::int64_t parent_rght) {
        return {parent_rght, parent_rght + 1};
    }

    static std::pair<std::int64_t, std::int64_t> insert_as_root(
        std::int64_t max_rght) {
        return {max_rght + 1, max_rght + 2};
    }

    static std::vector<RecoverUpdate> recover(
        std::vector<RecoverNode> nodes,
        std::function<bool(const RecoverNode&, const RecoverNode&)> less = {}) {
        std::unordered_map<std::string, std::vector<RecoverNode>> children;
        for (auto& node : nodes) {
            children[value_key(node.parent_id)].push_back(std::move(node));
        }

        if (!less) {
            less = [](const RecoverNode& a, const RecoverNode& b) {
                if (const auto* ai = std::get_if<std::int64_t>(&a.pk)) {
                    if (const auto* bi = std::get_if<std::int64_t>(&b.pk)) return *ai < *bi;
                }
                return value_key(a.pk) < value_key(b.pk);
            };
        }
        for (auto& [_, siblings] : children) {
            std::sort(siblings.begin(), siblings.end(), less);
        }

        std::vector<RecoverUpdate> updates;
        std::function<std::int64_t(const Value&, std::int64_t, std::int64_t)> traverse;
        traverse = [&](const Value& parent, std::int64_t left, std::int64_t depth) {
            auto found = children.find(value_key(parent));
            if (found == children.end()) return left;
            auto current = left;
            for (const auto& child : found->second) {
                const auto child_lft = current;
                current = traverse(child.pk, current + 1, depth + 1);
                const auto child_rght = current;
                ++current;
                updates.push_back(RecoverUpdate{child.pk, child_lft, child_rght, depth});
            }
            return current;
        };
        traverse(Value{nullptr}, 1, 0);
        return updates;
    }

    static std::vector<ThreadedTreeNode> to_threaded(
        const std::vector<Row>& rows,
        std::string_view left_key,
        std::string_view right_key) {
        const auto as_i64 = [](const Row& row, std::string_view key) {
            auto it = row.find(std::string(key));
            if (it == row.end()) throw std::runtime_error("MetalORM: threaded tree row is missing a boundary column");
            return from_value<std::int64_t>(it->second);
        };

        std::size_t index = 0;
        std::function<ThreadedTreeNode()> parse_one;
        parse_one = [&]() -> ThreadedTreeNode {
            ThreadedTreeNode out{rows.at(index), {}};
            const auto boundary = as_i64(rows.at(index), right_key);
            ++index;
            while (index < rows.size() && as_i64(rows.at(index), left_key) < boundary) {
                out.children.push_back(parse_one());
            }
            return out;
        };

        std::vector<ThreadedTreeNode> result;
        while (index < rows.size()) result.push_back(parse_one());
        return result;
    }

    static std::vector<std::string> validate_tree(
        const std::vector<Row>& rows,
        std::string_view pk_key,
        std::string_view left_key,
        std::string_view right_key) {
        struct Item { Value pk; std::int64_t lft; std::int64_t rght; };
        std::vector<Item> items;
        items.reserve(rows.size());
        for (const auto& row : rows) {
            const auto pk = row.find(std::string(pk_key));
            const auto l = row.find(std::string(left_key));
            const auto r = row.find(std::string(right_key));
            if (pk == row.end() || l == row.end() || r == row.end()) {
                throw std::runtime_error("MetalORM: tree validation row is missing required columns");
            }
            items.push_back({pk->second, from_value<std::int64_t>(l->second), from_value<std::int64_t>(r->second)});
        }
        std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.lft < b.lft; });

        std::vector<std::string> errors;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto& node = items[i];
            if (node.lft >= node.rght) {
                errors.push_back("Node " + value_key(node.pk) + ": lft must be less than rght");
            }
            if (node.lft < 1) {
                errors.push_back("Node " + value_key(node.pk) + ": lft must be positive");
            }
            for (std::size_t j = i + 1; j < items.size(); ++j) {
                const auto& other = items[j];
                if (other.lft < node.rght && other.rght > node.rght) {
                    errors.push_back("Node " + value_key(node.pk) + " overlaps with node " + value_key(other.pk));
                }
            }
        }
        return errors;
    }
};

template <reflect::Entity T>
class TreeQuery {
public:
    TreeQuery() {
        static_assert(reflect::validate_mapping<T>());
        static_assert(reflect::validate_tree_mapping<T>());
    }

    template <std::meta::info Member, typename V>
    TreeQuery with_scope(V&& value) const {
        static_assert(std::same_as<reflect::owner_type_t<Member>, T>,
                      "MetalORM: tree scope member must belong to the tree entity");
        static_assert(reflect::has<mapping::tree_scope_t>(Member),
                      "MetalORM: with_scope<> requires a [[=metal::mapping::tree_scope]] member");
        TreeQuery copy = *this;
        copy.set_scope(reflect::column_name<Member>(), to_value(std::forward<V>(value)));
        return copy;
    }

    SelectQuery<T> find_ancestors(NestedSetBounds bounds, bool include_self = true, bool ascending = true) const {
        auto left = comparison(left_name(), include_self ? CompareOp::Le : CompareOp::Lt, Value{bounds.lft});
        auto right = comparison(right_name(), include_self ? CompareOp::Ge : CompareOp::Gt, Value{bounds.rght});
        return ordered(and_node(std::move(left), std::move(right)), ascending);
    }

    SelectQuery<T> find_descendants(NestedSetBounds bounds) const {
        return ordered(and_node(
            comparison(left_name(), CompareOp::Gt, Value{bounds.lft}),
            comparison(right_name(), CompareOp::Lt, Value{bounds.rght})), true);
    }

    SelectQuery<T> find_direct_children(Value parent_id) const {
        if (std::holds_alternative<std::nullptr_t>(parent_id)) {
            return ordered(null_check(parent_name()), true);
        }
        return ordered(comparison(parent_name(), CompareOp::Eq, std::move(parent_id)), true);
    }

    SelectQuery<T> find_parent_by_id(Value parent_id) const {
        return filtered(comparison(pk_name(), CompareOp::Eq, std::move(parent_id)));
    }

    SelectQuery<T> find_siblings(Value parent_id, std::optional<Value> exclude_id = std::nullopt) const {
        auto node = std::holds_alternative<std::nullptr_t>(parent_id)
            ? null_check(parent_name())
            : comparison(parent_name(), CompareOp::Eq, std::move(parent_id));
        if (exclude_id) node = and_node(std::move(node), comparison(pk_name(), CompareOp::Ne, std::move(*exclude_id)));
        return ordered(std::move(node), true);
    }

    SelectQuery<T> find_roots() const {
        return ordered(null_check(parent_name()), true);
    }

    SelectQuery<T> find_subtree(NestedSetBounds bounds) const {
        return ordered(and_node(
            comparison(left_name(), CompareOp::Ge, Value{bounds.lft}),
            comparison(right_name(), CompareOp::Le, Value{bounds.rght})), true);
    }

    SelectQuery<T> find_tree_list() const {
        return ordered({}, true);
    }

    SelectQuery<T> find_at_depth(std::int64_t depth) const {
        constexpr auto Depth = reflect::tree_depth_member<T>();
        if constexpr (Depth == std::meta::info{}) {
            throw std::logic_error("MetalORM: find_at_depth requires a [[=metal::mapping::tree_depth]] column");
        } else {
            return ordered(comparison(reflect::column_name<Depth>(), CompareOp::Eq, Value{depth}), true);
        }
    }

    SelectQuery<T> find_by_id(Value id) const {
        return filtered(comparison(pk_name(), CompareOp::Eq, std::move(id)));
    }

    [[nodiscard]] const std::vector<std::pair<std::string, Value>>& scope_values() const noexcept {
        return scope_;
    }

    static std::string parent_name() {
        constexpr auto Member = reflect::tree_parent_member<T>();
        return reflect::column_name<Member>();
    }

    static std::string left_name() {
        constexpr auto Member = reflect::tree_left_member<T>();
        return reflect::column_name<Member>();
    }

    static std::string right_name() {
        constexpr auto Member = reflect::tree_right_member<T>();
        return reflect::column_name<Member>();
    }

    static std::string depth_name() {
        constexpr auto Member = reflect::tree_depth_member<T>();
        if constexpr (Member == std::meta::info{}) return {};
        else return reflect::column_name<Member>();
    }

    static std::string pk_name() { return reflect::primary_key_name<T>(); }

private:
    static ExprPtr comparison(std::string column, CompareOp op, Value value) {
        auto left = std::make_shared<ScalarNode>(ScalarNode{
            ColumnRef{std::type_index(typeid(T)), std::move(column)}});
        auto right = std::make_shared<ScalarNode>(ScalarNode{std::move(value)});
        return std::make_shared<ExprNode>(ExprNode{ComparisonNode{std::move(left), op, std::move(right)}});
    }

    static ExprPtr null_check(std::string column) {
        auto operand = std::make_shared<ScalarNode>(ScalarNode{
            ColumnRef{std::type_index(typeid(T)), std::move(column)}});
        return std::make_shared<ExprNode>(ExprNode{NullCheckNode{std::move(operand), false}});
    }

    static ExprPtr and_node(ExprPtr left, ExprPtr right) {
        if (!left) return right;
        if (!right) return left;
        return std::make_shared<ExprNode>(ExprNode{LogicalNode{LogicOp::And, std::move(left), std::move(right)}});
    }

    ExprPtr apply_scope(ExprPtr node) const {
        for (const auto& [column, value] : scope_) {
            node = and_node(std::move(node), comparison(column, CompareOp::Eq, value));
        }
        return node;
    }

    SelectQuery<T> filtered(ExprPtr node) const {
        SelectQuery<T> query;
        node = apply_scope(std::move(node));
        if (node) query.where(Expression<T>{std::move(node)});
        return query;
    }

    SelectQuery<T> ordered(ExprPtr node, bool ascending) const {
        auto query = filtered(std::move(node));
        constexpr auto Left = reflect::tree_left_member<T>();
        query.order_by(field<Left>, ascending);
        return query;
    }

    void set_scope(std::string column, Value value) {
        auto found = std::find_if(scope_.begin(), scope_.end(), [&](const auto& item) {
            return item.first == column;
        });
        if (found == scope_.end()) scope_.emplace_back(std::move(column), std::move(value));
        else found->second = std::move(value);
    }

    std::vector<std::pair<std::string, Value>> scope_;
};

template <reflect::Entity T>
TreeQuery<T> tree_query() {
    return {};
}

template <reflect::Entity T>
auto tree_row() {
    static_assert(reflect::validate_tree_mapping<T>());
    return bulk_row<T>();
}

template <reflect::Entity T>
class TreeManager {
public:
    explicit TreeManager(Session& session) : session_(&session) {
        static_assert(reflect::validate_tree_mapping<T>());
    }

    template <std::meta::info Member, typename V>
    TreeManager with_scope(V&& value) const {
        TreeManager copy = *this;
        copy.query_ = query_.template with_scope<Member>(std::forward<V>(value));
        return copy;
    }

    std::optional<TreeNodeResult> get_node(Value id) const {
        auto rows = execute(query_.find_by_id(std::move(id)));
        if (rows.empty()) return std::nullopt;
        return make_result(std::move(rows.front()));
    }

    std::vector<TreeNodeResult> get_roots() const {
        return results(execute(query_.find_roots()));
    }

    std::vector<TreeNodeResult> get_children(Value parent_id) const {
        return results(execute(query_.find_direct_children(std::move(parent_id))));
    }

    std::vector<TreeNodeResult> get_descendants(NestedSetBounds bounds) const {
        return results(execute(query_.find_descendants(bounds)));
    }

    std::vector<TreeNodeResult> get_path(NestedSetBounds bounds, bool include_self = true) const {
        return results(execute(query_.find_ancestors(bounds, include_self)));
    }

    std::vector<TreeNodeResult> get_siblings(
        const TreeNodeResult& node,
        bool include_self = true) const {
        std::optional<Value> exclude;
        if (!include_self) exclude = require_value(node.data, TreeQuery<T>::pk_name());
        return results(execute(query_.find_siblings(node.parent_id, std::move(exclude))));
    }

    std::optional<TreeNodeResult> get_parent(const TreeNodeResult& node) const {
        if (node.is_root) return std::nullopt;
        return get_node(node.parent_id);
    }

    std::vector<ThreadedTreeNode> get_descendants_threaded(NestedSetBounds bounds) const {
        const auto rows = execute(query_.find_descendants(bounds));
        return NestedSetStrategy::to_threaded(rows, TreeQuery<T>::left_name(), TreeQuery<T>::right_name());
    }

    std::vector<TreeNodeResult> get_leaves() const {
        std::string sql = "SELECT ";
        bool first = true;
        reflect::for_each_column<T>([&]<std::meta::info Member>() {
            if (!first) sql += ", ";
            first = false;
            sql += session_->dialect().quote_identifier(reflect::column_name<Member>());
        });
        sql += " FROM " + session_->dialect().quote_identifier(reflect::table_name<T>());
        sql += " WHERE (" + session_->dialect().quote_identifier(TreeQuery<T>::right_name()) + " - " +
               session_->dialect().quote_identifier(TreeQuery<T>::left_name()) + ") = 1";
        std::vector<Value> params;
        append_scope(sql, params, true);
        sql += " ORDER BY " + session_->dialect().quote_identifier(TreeQuery<T>::left_name()) + " ASC;";
        return results(session_->executor().execute(sql, params).rows);
    }

    std::int64_t child_count(NestedSetBounds bounds) const {
        return NestedSetStrategy::child_count(bounds.lft, bounds.rght);
    }

    std::int64_t get_level(const TreeNodeResult& node) const {
        if (node.depth) return *node.depth;
        return static_cast<std::int64_t>(get_path({node.lft, node.rght}, false).size());
    }

    bool move_up(const TreeNodeResult& node) {
        auto siblings = get_siblings(node, true);
        const auto id = require_value(node.data, TreeQuery<T>::pk_name());
        const auto found = std::find_if(siblings.begin(), siblings.end(), [&](const auto& sibling) {
            return value_key(require_value(sibling.data, TreeQuery<T>::pk_name())) == value_key(id);
        });
        if (found == siblings.end() || found == siblings.begin()) return false;
        return swap_adjacent(*(found - 1), *found);
    }

    bool move_down(const TreeNodeResult& node) {
        auto siblings = get_siblings(node, true);
        const auto id = require_value(node.data, TreeQuery<T>::pk_name());
        const auto found = std::find_if(siblings.begin(), siblings.end(), [&](const auto& sibling) {
            return value_key(require_value(sibling.data, TreeQuery<T>::pk_name())) == value_key(id);
        });
        if (found == siblings.end() || found + 1 == siblings.end()) return false;
        return swap_adjacent(*found, *(found + 1));
    }

    void move_to(const TreeNodeResult& node, Value new_parent_id) {
        const auto width = NestedSetStrategy::subtree_width(node.lft, node.rght);
        std::int64_t target;
        std::int64_t new_depth = 0;

        if (std::holds_alternative<std::nullptr_t>(new_parent_id)) {
            target = max_rght() + 1;
        } else {
            auto parent = get_node(new_parent_id);
            if (!parent) throw std::runtime_error("MetalORM: tree parent node not found");
            if (NestedSetStrategy::is_descendant_of({parent->lft, parent->rght}, {node.lft, node.rght}) ||
                (parent->lft == node.lft && parent->rght == node.rght)) {
                throw std::invalid_argument("MetalORM: cannot move a tree node below itself or one of its descendants");
            }
            target = parent->rght;
            new_depth = get_level(*parent) + 1;
        }

        if (target > node.rght) target -= width;
        move_subtree(node, target, std::move(new_parent_id), new_depth);
    }

    Value insert_as_child(Value parent_id, BulkRow row) {
        std::int64_t lft;
        std::int64_t rght;
        std::int64_t depth = 0;

        if (std::holds_alternative<std::nullptr_t>(parent_id)) {
            std::tie(lft, rght) = NestedSetStrategy::insert_as_root(max_rght());
        } else {
            auto parent = get_node(parent_id);
            if (!parent) throw std::runtime_error("MetalORM: tree parent node not found");
            shift_for_insert(parent->rght, 2);
            std::tie(lft, rght) = NestedSetStrategy::insert_as_last_child(parent->rght);
            depth = get_level(*parent) + 1;
        }

        set_assignment(row, TreeQuery<T>::parent_name(), parent_id);
        set_assignment(row, TreeQuery<T>::left_name(), Value{lft});
        set_assignment(row, TreeQuery<T>::right_name(), Value{rght});
        if (!TreeQuery<T>::depth_name().empty()) {
            set_assignment(row, TreeQuery<T>::depth_name(), Value{depth});
        }
        for (const auto& [column, value] : query_.scope_values()) set_assignment(row, column, value);

        auto builder = insert_into<T>();
        builder.values(std::move(row)).returning({TreeQuery<T>::pk_name()});
        auto compiled = builder.compile(session_->dialect());
        auto result = session_->executor().execute(compiled.sql, compiled.params);
        if (!result.rows.empty()) return require_value(result.rows.front(), TreeQuery<T>::pk_name());
        if (result.last_insert_id != 0) return Value{result.last_insert_id};
        return Value{nullptr};
    }

    std::size_t delete_subtree(const TreeNodeResult& node) {
        const auto width = NestedSetStrategy::subtree_width(node.lft, node.rght);
        std::string sql = "DELETE FROM " + session_->dialect().quote_identifier(reflect::table_name<T>()) +
            " WHERE " + session_->dialect().quote_identifier(TreeQuery<T>::left_name()) + " >= ? AND " +
            session_->dialect().quote_identifier(TreeQuery<T>::right_name()) + " <= ?";
        std::vector<Value> params{Value{node.lft}, Value{node.rght}};
        append_scope(sql, params, true);
        sql += ";";
        session_->executor().execute(sql, params);
        shift_for_delete(node.rght, width);
        return static_cast<std::size_t>(width / 2);
    }

    RecoverResult recover(
        std::function<bool(const RecoverNode&, const RecoverNode&)> less = {}) {
        try {
            auto nodes = recovery_nodes();
            auto updates = NestedSetStrategy::recover(std::move(nodes), std::move(less));
            for (const auto& item : updates) {
                std::vector<DmlAssignment> assignments{
                    {TreeQuery<T>::left_name(), Value{item.lft}},
                    {TreeQuery<T>::right_name(), Value{item.rght}}
                };
                if (!TreeQuery<T>::depth_name().empty()) {
                    assignments.push_back({TreeQuery<T>::depth_name(), Value{item.depth}});
                }
                auto builder = update<T>();
                builder.set(std::move(assignments)).where_eq(TreeQuery<T>::pk_name(), item.pk);
                for (const auto& [column, value] : query_.scope_values()) builder.where_eq(column, value);
                auto compiled = builder.compile(session_->dialect());
                session_->executor().execute(compiled.sql, compiled.params);
            }
            return RecoverResult{updates.size(), true, {}};
        } catch (const std::exception& error) {
            return RecoverResult{0, false, {error.what()}};
        }
    }

    std::vector<std::string> validate() const {
        const auto rows = execute(query_.find_tree_list());
        return NestedSetStrategy::validate_tree(
            rows,
            TreeQuery<T>::pk_name(),
            TreeQuery<T>::left_name(),
            TreeQuery<T>::right_name());
    }

    const TreeQuery<T>& query() const noexcept { return query_; }

private:
    static Value require_value(const Row& row, std::string_view column) {
        auto found = row.find(std::string(column));
        if (found == row.end()) {
            throw std::runtime_error("MetalORM: tree row is missing required column '" + std::string(column) + "'");
        }
        return found->second;
    }

    static void set_assignment(BulkRow& row, const std::string& column, Value value) {
        auto found = std::find_if(row.begin(), row.end(), [&](const auto& assignment) {
            return assignment.column == column;
        });
        if (found == row.end()) row.push_back(DmlAssignment{column, std::move(value)});
        else found->value = std::move(value);
    }

    TreeNodeResult make_result(Row row) const {
        const auto lft = from_value<std::int64_t>(require_value(row, TreeQuery<T>::left_name()));
        const auto rght = from_value<std::int64_t>(require_value(row, TreeQuery<T>::right_name()));
        const auto parent = require_value(row, TreeQuery<T>::parent_name());
        std::optional<std::int64_t> depth;
        if (!TreeQuery<T>::depth_name().empty()) {
            const auto value = require_value(row, TreeQuery<T>::depth_name());
            if (!std::holds_alternative<std::nullptr_t>(value)) depth = from_value<std::int64_t>(value);
        }
        return TreeNodeResult{
            std::move(row), lft, rght, parent, depth,
            NestedSetStrategy::is_leaf(lft, rght),
            NestedSetStrategy::is_root(parent),
            NestedSetStrategy::child_count(lft, rght)
        };
    }

    std::vector<TreeNodeResult> results(std::vector<Row> rows) const {
        std::vector<TreeNodeResult> out;
        out.reserve(rows.size());
        for (auto& row : rows) out.push_back(make_result(std::move(row)));
        return out;
    }

    std::vector<Row> execute(SelectQuery<T> query) const {
        auto compiled = query.compile(session_->dialect());
        return session_->executor().execute(compiled.sql, compiled.params).rows;
    }

    void append_scope(std::string& sql, std::vector<Value>& params, bool has_where) const {
        bool where = has_where;
        for (const auto& [column, value] : query_.scope_values()) {
            sql += where ? " AND " : " WHERE ";
            where = true;
            sql += session_->dialect().quote_identifier(column) + " = " +
                   session_->dialect().placeholder(params.size() + 1);
            params.push_back(value);
        }
    }

    std::int64_t max_rght() const {
        std::string sql = "SELECT MAX(" + session_->dialect().quote_identifier(TreeQuery<T>::right_name()) +
            ") AS max_rght FROM " + session_->dialect().quote_identifier(reflect::table_name<T>());
        std::vector<Value> params;
        append_scope(sql, params, false);
        sql += ";";
        auto result = session_->executor().execute(sql, params);
        if (result.rows.empty()) return 0;
        const auto value = require_value(result.rows.front(), "max_rght");
        if (std::holds_alternative<std::nullptr_t>(value)) return 0;
        return from_value<std::int64_t>(value);
    }

    void shift_for_insert(std::int64_t point, std::int64_t width) {
        auto update_boundary = [&](const std::string& column) {
            std::string sql = "UPDATE " + session_->dialect().quote_identifier(reflect::table_name<T>()) +
                " SET " + session_->dialect().quote_identifier(column) + " = " +
                session_->dialect().quote_identifier(column) + " + ? WHERE " +
                session_->dialect().quote_identifier(column) + " >= ?";
            std::vector<Value> params{Value{width}, Value{point}};
            append_scope(sql, params, true);
            sql += ";";
            session_->executor().execute(sql, params);
        };
        update_boundary(TreeQuery<T>::right_name());
        update_boundary(TreeQuery<T>::left_name());
    }

    void shift_for_delete(std::int64_t deleted_rght, std::int64_t width) {
        auto update_boundary = [&](const std::string& column) {
            std::string sql = "UPDATE " + session_->dialect().quote_identifier(reflect::table_name<T>()) +
                " SET " + session_->dialect().quote_identifier(column) + " = " +
                session_->dialect().quote_identifier(column) + " - ? WHERE " +
                session_->dialect().quote_identifier(column) + " > ?";
            std::vector<Value> params{Value{width}, Value{deleted_rght}};
            append_scope(sql, params, true);
            sql += ";";
            session_->executor().execute(sql, params);
        };
        update_boundary(TreeQuery<T>::left_name());
        update_boundary(TreeQuery<T>::right_name());
    }

    bool swap_adjacent(const TreeNodeResult& left, const TreeNodeResult& right) {
        if (left.rght >= right.lft) return false;
        const auto left_width = NestedSetStrategy::subtree_width(left.lft, left.rght);
        const auto right_width = NestedSetStrategy::subtree_width(right.lft, right.rght);
        constexpr std::int64_t temp = 10000000;

        move_range(right.lft, right.rght, temp);
        move_range(left.lft, left.rght, right_width);
        move_range(right.lft + temp, right.rght + temp, -temp - left_width);
        return true;
    }

    void move_range(std::int64_t lft, std::int64_t rght, std::int64_t delta) {
        std::string sql = "UPDATE " + session_->dialect().quote_identifier(reflect::table_name<T>()) +
            " SET " + session_->dialect().quote_identifier(TreeQuery<T>::left_name()) + " = " +
            session_->dialect().quote_identifier(TreeQuery<T>::left_name()) + " + ?, " +
            session_->dialect().quote_identifier(TreeQuery<T>::right_name()) + " = " +
            session_->dialect().quote_identifier(TreeQuery<T>::right_name()) + " + ? WHERE " +
            session_->dialect().quote_identifier(TreeQuery<T>::left_name()) + " >= ? AND " +
            session_->dialect().quote_identifier(TreeQuery<T>::right_name()) + " <= ?";
        std::vector<Value> params{Value{delta}, Value{delta}, Value{lft}, Value{rght}};
        append_scope(sql, params, true);
        sql += ";";
        session_->executor().execute(sql, params);
    }

    void move_subtree(
        const TreeNodeResult& node,
        std::int64_t target_lft,
        Value new_parent_id,
        std::int64_t new_depth) {
        const auto width = NestedSetStrategy::subtree_width(node.lft, node.rght);
        const auto old_depth = node.depth.value_or(get_level(node));
        const auto depth_delta = new_depth - old_depth;
        constexpr std::int64_t temp = 10000000;

        // Isolate the subtree below zero before opening/closing gaps. A positive
        // temporary range would itself be shifted by shift_for_insert(), which
        // corrupts the restore delta (the current TS implementation has this edge).
        const auto isolate_delta = -temp - node.rght;
        const auto isolated_lft = node.lft + isolate_delta;
        const auto isolated_rght = node.rght + isolate_delta;
        move_range(node.lft, node.rght, isolate_delta);
        shift_for_delete(node.rght, width);
        shift_for_insert(target_lft, width);

        const auto restore_delta = target_lft - node.lft - isolate_delta;
        std::string sql = "UPDATE " + session_->dialect().quote_identifier(reflect::table_name<T>()) +
            " SET " + session_->dialect().quote_identifier(TreeQuery<T>::left_name()) + " = " +
            session_->dialect().quote_identifier(TreeQuery<T>::left_name()) + " + ?, " +
            session_->dialect().quote_identifier(TreeQuery<T>::right_name()) + " = " +
            session_->dialect().quote_identifier(TreeQuery<T>::right_name()) + " + ?";
        std::vector<Value> params{Value{restore_delta}, Value{restore_delta}};
        if (!TreeQuery<T>::depth_name().empty() && depth_delta != 0) {
            sql += ", " + session_->dialect().quote_identifier(TreeQuery<T>::depth_name()) + " = " +
                   session_->dialect().quote_identifier(TreeQuery<T>::depth_name()) + " + ?";
            params.push_back(Value{depth_delta});
        }
        sql += " WHERE " + session_->dialect().quote_identifier(TreeQuery<T>::left_name()) + " >= ? AND " +
               session_->dialect().quote_identifier(TreeQuery<T>::right_name()) + " <= ?";
        params.push_back(Value{isolated_lft});
        params.push_back(Value{isolated_rght});
        append_scope(sql, params, true);
        sql += ";";
        session_->executor().execute(sql, params);

        auto builder = update<T>();
        builder.set({DmlAssignment{TreeQuery<T>::parent_name(), std::move(new_parent_id)}})
            .where_eq(TreeQuery<T>::pk_name(), require_value(node.data, TreeQuery<T>::pk_name()));
        for (const auto& [column, value] : query_.scope_values()) builder.where_eq(column, value);
        auto compiled = builder.compile(session_->dialect());
        session_->executor().execute(compiled.sql, compiled.params);
    }

    std::vector<RecoverNode> recovery_nodes() const {
        const auto rows = execute(query_.find_tree_list());
        std::vector<RecoverNode> nodes;
        nodes.reserve(rows.size());
        for (const auto& row : rows) {
            RecoverNode node;
            node.pk = require_value(row, TreeQuery<T>::pk_name());
            node.parent_id = require_value(row, TreeQuery<T>::parent_name());
            node.lft = from_value<std::int64_t>(require_value(row, TreeQuery<T>::left_name()));
            node.rght = from_value<std::int64_t>(require_value(row, TreeQuery<T>::right_name()));
            if (!TreeQuery<T>::depth_name().empty()) {
                const auto depth = require_value(row, TreeQuery<T>::depth_name());
                if (!std::holds_alternative<std::nullptr_t>(depth)) node.depth = from_value<std::int64_t>(depth);
            }
            nodes.push_back(std::move(node));
        }
        return nodes;
    }

    Session* session_;
    TreeQuery<T> query_;
};

template <reflect::Entity T>
TreeManager<T> create_tree_manager(Session& session) {
    return TreeManager<T>{session};
}

} // namespace metal
