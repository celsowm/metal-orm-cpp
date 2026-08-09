#include <metal/metal.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct [[=metal::mapping::table{"tree_categories"}]] TreeCategory {
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

static_assert(metal::reflect::validate_mapping<TreeCategory>());
static_assert(metal::reflect::validate_tree_mapping<TreeCategory>());

static metal::BulkRow category_row(std::string name) {
    return metal::tree_row<TreeCategory>()
        .set<^^TreeCategory::name>(std::move(name))
        .build();
}

static std::int64_t as_id(const metal::Value& value) {
    return metal::from_value<std::int64_t>(value);
}

static std::int64_t row_i64(const metal::Row& row, const char* key) {
    return metal::from_value<std::int64_t>(row.at(key));
}

static std::string row_string(const metal::Row& row, const char* key) {
    return metal::from_value<std::string>(row.at(key));
}

int main() {
    metal::SQLiteDialect dialect;
    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    db->execute(metal::create_table_sql<TreeCategory>(dialect));
    db->execute("CREATE INDEX tree_categories_bounds_idx ON tree_categories(tenant_id, lft, rght);");
    db->execute("CREATE INDEX tree_categories_parent_idx ON tree_categories(tenant_id, parent_id);");
    metal::Session session{db};

    auto base = metal::create_tree_manager<TreeCategory>(session);
    auto tenant1 = base.with_scope<^^TreeCategory::tenant_id>(std::int64_t{1});
    auto tenant2 = base.with_scope<^^TreeCategory::tenant_id>(std::int64_t{2});

    const auto tenant2_root_id = as_id(tenant2.insert_as_child(metal::Value{nullptr}, category_row("Other Root")));
    const auto root_id = as_id(tenant1.insert_as_child(metal::Value{nullptr}, category_row("Root")));
    const auto a_id = as_id(tenant1.insert_as_child(metal::Value{root_id}, category_row("A")));
    const auto b_id = as_id(tenant1.insert_as_child(metal::Value{root_id}, category_row("B")));
    const auto grand_id = as_id(tenant1.insert_as_child(metal::Value{a_id}, category_row("Grand")));

    auto roots = tenant1.get_roots();
    assert(roots.size() == 1);
    assert(row_string(roots.front().data, "name") == "Root");
    assert(roots.front().lft == 1);
    assert(roots.front().rght == 8);
    assert(roots.front().child_count == 3);

    auto children = tenant1.get_children(metal::Value{root_id});
    assert(children.size() == 2);
    assert(row_string(children[0].data, "name") == "A");
    assert(row_string(children[1].data, "name") == "B");

    auto descendants = tenant1.get_descendants({1, 8});
    assert(descendants.size() == 3);
    assert(row_string(descendants[0].data, "name") == "A");
    assert(row_string(descendants[1].data, "name") == "Grand");
    assert(row_string(descendants[2].data, "name") == "B");

    auto grand = tenant1.get_node(metal::Value{grand_id});
    assert(grand);
    assert(grand->is_leaf);
    assert(!grand->is_root);
    assert(tenant1.get_level(*grand) == 2);

    auto path = tenant1.get_path({grand->lft, grand->rght});
    assert(path.size() == 3);
    assert(row_string(path[0].data, "name") == "Root");
    assert(row_string(path[1].data, "name") == "A");
    assert(row_string(path[2].data, "name") == "Grand");

    const auto leaf_query = tenant1.query().find_leaves().compile(dialect);
    assert(leaf_query.sql.find("\"rght\" - \"lft\"") != std::string::npos);
    assert(leaf_query.sql.find("\"tenant_id\" = ?") != std::string::npos);
    assert(leaf_query.params.size() == 2);
    assert(metal::from_value<std::int64_t>(leaf_query.params[0]) == 1);
    assert(metal::from_value<std::int64_t>(leaf_query.params[1]) == 1);

    auto leaves = tenant1.get_leaves();
    assert(leaves.size() == 2);
    assert(row_string(leaves[0].data, "name") == "Grand");
    assert(row_string(leaves[1].data, "name") == "B");

    auto threaded = tenant1.get_descendants_threaded({1, 8});
    assert(threaded.size() == 2);
    assert(row_string(threaded[0].node, "name") == "A");
    assert(threaded[0].children.size() == 1);
    assert(row_string(threaded[0].children[0].node, "name") == "Grand");
    assert(row_string(threaded[1].node, "name") == "B");

    // Regression for the temporary-range trap: A is a width-4 subtree. Moving it
    // below the right-hand sibling must not let gap shifts touch the isolated range.
    auto a = tenant1.get_node(metal::Value{a_id});
    auto b = tenant1.get_node(metal::Value{b_id});
    assert(a && b);
    assert(a->rght - a->lft + 1 == 4);
    tenant1.move_to(*a, metal::Value{b_id});
    assert(tenant1.validate().empty());

    auto root = tenant1.get_node(metal::Value{root_id});
    a = tenant1.get_node(metal::Value{a_id});
    b = tenant1.get_node(metal::Value{b_id});
    grand = tenant1.get_node(metal::Value{grand_id});
    assert(root && a && b && grand);
    assert(root->lft == 1 && root->rght == 8);
    assert(b->lft == 2 && b->rght == 7);
    assert(a->lft == 3 && a->rght == 6);
    assert(grand->lft == 4 && grand->rght == 5);
    assert(metal::value_key(a->parent_id) == metal::value_key(metal::Value{b_id}));

    // Move the same wide subtree back under the root. It becomes the last child,
    // then move_up restores the original A/B ordering for the remaining parity checks.
    tenant1.move_to(*a, metal::Value{root_id});
    assert(tenant1.validate().empty());
    a = tenant1.get_node(metal::Value{a_id});
    assert(a);
    assert(tenant1.move_up(*a));
    children = tenant1.get_children(metal::Value{root_id});
    assert(row_string(children[0].data, "name") == "A");
    assert(row_string(children[1].data, "name") == "B");

    a = tenant1.get_node(metal::Value{a_id});
    assert(a);
    assert(tenant1.move_down(*a));
    children = tenant1.get_children(metal::Value{root_id});
    assert(row_string(children[0].data, "name") == "B");
    assert(row_string(children[1].data, "name") == "A");

    a = tenant1.get_node(metal::Value{a_id});
    assert(a);
    assert(tenant1.move_up(*a));
    children = tenant1.get_children(metal::Value{root_id});
    assert(row_string(children[0].data, "name") == "A");
    assert(row_string(children[1].data, "name") == "B");

    b = tenant1.get_node(metal::Value{b_id});
    a = tenant1.get_node(metal::Value{a_id});
    assert(a && b);
    tenant1.move_to(*b, metal::Value{a_id});
    b = tenant1.get_node(metal::Value{b_id});
    assert(b);
    assert(metal::value_key(b->parent_id) == metal::value_key(metal::Value{a_id}));
    assert(tenant1.get_level(*b) == 2);

    bool invalid_move_rejected = false;
    try {
        root = tenant1.get_node(metal::Value{root_id});
        grand = tenant1.get_node(metal::Value{grand_id});
        assert(root && grand);
        tenant1.move_to(*root, metal::Value{grand_id});
    } catch (const std::invalid_argument&) {
        invalid_move_rejected = true;
    }
    assert(invalid_move_rejected);

    const auto other_before = tenant2.get_node(metal::Value{tenant2_root_id});
    assert(other_before);
    assert(other_before->lft == 1 && other_before->rght == 2);

    a = tenant1.get_node(metal::Value{a_id});
    assert(a);
    const auto deleted = tenant1.delete_subtree(*a);
    assert(deleted == 3);
    assert(!tenant1.get_node(metal::Value{a_id}));
    assert(!tenant1.get_node(metal::Value{b_id}));
    assert(!tenant1.get_node(metal::Value{grand_id}));

    root = tenant1.get_node(metal::Value{root_id});
    assert(root);
    assert(root->lft == 1 && root->rght == 2);
    assert(root->is_leaf);

    const auto c_id = as_id(tenant1.insert_as_child(metal::Value{root_id}, category_row("C")));
    const auto d_id = as_id(tenant1.insert_as_child(metal::Value{c_id}, category_row("D")));
    (void)d_id;

    db->execute(
        "UPDATE tree_categories SET lft = 0, rght = 0, depth = NULL WHERE tenant_id = 1;");
    const auto recovery = tenant1.recover();
    assert(recovery.success);
    assert(recovery.processed == 3);
    assert(tenant1.validate().empty());

    root = tenant1.get_node(metal::Value{root_id});
    auto c = tenant1.get_node(metal::Value{c_id});
    assert(root && c);
    assert(root->lft == 1 && root->rght == 6);
    assert(c->lft == 2 && c->rght == 5);
    assert(c->depth && *c->depth == 1);

    auto at_depth = tenant1.query().find_at_depth(1).compile(dialect);
    auto depth_rows = db->execute(at_depth.sql, at_depth.params).rows;
    assert(depth_rows.size() == 1);
    assert(row_string(depth_rows.front(), "name") == "C");

    db->execute(
        "UPDATE tree_categories SET lft = 5, rght = 3 WHERE id = ? AND tenant_id = 1;",
        {metal::Value{root_id}});
    const auto validation_errors = tenant1.validate();
    assert(!validation_errors.empty());
    assert(tenant1.recover().success);
    assert(tenant1.validate().empty());

    const auto other_after = tenant2.get_node(metal::Value{tenant2_root_id});
    assert(other_after);
    assert(other_after->lft == other_before->lft);
    assert(other_after->rght == other_before->rght);

    const std::vector<metal::RecoverNode> raw_nodes{
        {metal::Value{std::int64_t{1}}, 0, 0, metal::Value{nullptr}, std::nullopt},
        {metal::Value{std::int64_t{2}}, 0, 0, metal::Value{std::int64_t{1}}, std::nullopt},
        {metal::Value{std::int64_t{3}}, 0, 0, metal::Value{std::int64_t{1}}, std::nullopt},
        {metal::Value{std::int64_t{4}}, 0, 0, metal::Value{std::int64_t{2}}, std::nullopt}
    };
    const auto updates = metal::NestedSetStrategy::recover(raw_nodes);
    const auto find_update = [&](std::int64_t id) -> const metal::RecoverUpdate& {
        const auto it = std::find_if(updates.begin(), updates.end(), [&](const auto& item) {
            return as_id(item.pk) == id;
        });
        assert(it != updates.end());
        return *it;
    };
    assert(updates.size() == 4);
    const auto& u1 = find_update(1);
    const auto& u2 = find_update(2);
    const auto& u3 = find_update(3);
    const auto& u4 = find_update(4);
    assert(u1.lft == 1 && u1.rght == 8 && u1.depth == 0);
    assert(u2.lft == 2 && u2.rght == 5 && u2.depth == 1);
    assert(u4.lft == 3 && u4.rght == 4 && u4.depth == 2);
    assert(u3.lft == 6 && u3.rght == 7 && u3.depth == 1);

    const auto scoped_count = db->execute(
        "SELECT COUNT(*) AS c FROM tree_categories WHERE tenant_id = 2;");
    assert(row_i64(scoped_count.rows.front(), "c") == 1);
}