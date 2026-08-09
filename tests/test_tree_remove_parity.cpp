#include <metal/metal.hpp>

#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct [[=metal::mapping::table{"tree_remove_categories"}]] TreeRemoveCategory {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;

    [[=metal::mapping::tree_parent]]
    std::optional<std::int64_t> parent_id;
    [[=metal::mapping::tree_left]]
    std::int64_t lft{};
    [[=metal::mapping::tree_right]]
    std::int64_t rght{};
    [[=metal::mapping::tree_depth]]
    std::optional<std::int64_t> depth;
    [[=metal::mapping::tree_scope]]
    std::int64_t tenant_id{};
};

static_assert(metal::reflect::validate_mapping<TreeRemoveCategory>());
static_assert(metal::reflect::validate_tree_mapping<TreeRemoveCategory>());

static metal::BulkRow row(std::string name) {
    return metal::tree_row<TreeRemoveCategory>()
        .set<^^TreeRemoveCategory::name>(std::move(name))
        .build();
}

static std::int64_t id(const metal::Value& value) {
    return metal::from_value<std::int64_t>(value);
}

static std::string name_of(const metal::TreeNodeResult& node) {
    return metal::from_value<std::string>(node.data.at("name"));
}

int main() {
    metal::SQLiteDialect dialect;
    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    db->execute(metal::create_table_sql<TreeRemoveCategory>(dialect));
    metal::Session session{db};

    auto base = metal::create_tree_manager<TreeRemoveCategory>(session);
    auto tree = base.with_scope<^^TreeRemoveCategory::tenant_id>(std::int64_t{1});
    auto other = base.with_scope<^^TreeRemoveCategory::tenant_id>(std::int64_t{2});

    const auto other_root_id = id(other.insert_as_child(metal::Value{nullptr}, row("Other Root")));

    const auto root_id = id(tree.insert_as_child(metal::Value{nullptr}, row("Root")));
    const auto a_id = id(tree.insert_as_child(metal::Value{root_id}, row("A")));
    const auto x_id = id(tree.insert_as_child(metal::Value{a_id}, row("X")));
    const auto y_id = id(tree.insert_as_child(metal::Value{a_id}, row("Y")));
    const auto b_id = id(tree.insert_as_child(metal::Value{root_id}, row("B")));

    auto a = tree.get_node(metal::Value{a_id});
    assert(a);
    assert(a->lft == 2 && a->rght == 7);
    assert(a->depth && *a->depth == 1);

    tree.remove_from_tree(*a);
    assert(tree.validate().empty());

    const auto roots = tree.get_roots();
    assert(roots.size() == 2);
    assert(name_of(roots[0]) == "Root");
    assert(roots[0].lft == 1 && roots[0].rght == 8);
    assert(roots[0].depth && *roots[0].depth == 0);
    assert(name_of(roots[1]) == "A");
    assert(roots[1].lft == 9 && roots[1].rght == 10);
    assert(roots[1].depth && *roots[1].depth == 0);
    assert(std::holds_alternative<std::nullptr_t>(roots[1].parent_id));
    assert(roots[1].is_leaf);

    const auto children = tree.get_children(metal::Value{root_id});
    assert(children.size() == 3);
    assert(name_of(children[0]) == "X");
    assert(name_of(children[1]) == "Y");
    assert(name_of(children[2]) == "B");

    const auto x = tree.get_node(metal::Value{x_id});
    const auto y = tree.get_node(metal::Value{y_id});
    const auto b = tree.get_node(metal::Value{b_id});
    assert(x && y && b);
    assert(x->lft == 2 && x->rght == 3);
    assert(y->lft == 4 && y->rght == 5);
    assert(b->lft == 6 && b->rght == 7);
    assert(x->depth && *x->depth == 1);
    assert(y->depth && *y->depth == 1);
    assert(b->depth && *b->depth == 1);
    assert(metal::value_key(x->parent_id) == metal::value_key(metal::Value{root_id}));
    assert(metal::value_key(y->parent_id) == metal::value_key(metal::Value{root_id}));
    assert(metal::value_key(b->parent_id) == metal::value_key(metal::Value{root_id}));

    const auto other_root = other.get_node(metal::Value{other_root_id});
    assert(other_root);
    assert(other_root->lft == 1 && other_root->rght == 2);
    assert(other.validate().empty());
}