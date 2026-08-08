#include <metal/metal.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct [[=metal::mapping::table{"advanced_employees"}]] AdvancedEmployee {
    [[=metal::mapping::primary_key]]
    std::int64_t id{};
    std::string department;
    std::int64_t salary{};
    bool active{};
    std::optional<std::int64_t> parent_id;
};

static_assert(metal::reflect::validate_mapping<AdvancedEmployee>());

static std::vector<std::int64_t> ids(const metal::QueryResult& result) {
    std::vector<std::int64_t> out;
    for (const auto& row : result.rows) {
        out.push_back(metal::from_value<std::int64_t>(row.at("id")));
    }
    return out;
}

int main() {
    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    metal::SQLiteDialect dialect;

    db->execute(metal::create_table_sql<AdvancedEmployee>(dialect));
    db->execute(
        "INSERT INTO advanced_employees(id, department, salary, active, parent_id) VALUES "
        "(1, 'engineering', 100, 1, NULL), "
        "(2, 'engineering', 150, 1, 1), "
        "(3, 'legal', 120, 0, 1), "
        "(4, 'legal', 90, 1, 3);");

    auto ranged = metal::select<AdvancedEmployee>()
        .where(metal::between(metal::field<^^AdvancedEmployee::salary>, 100, 130))
        .order_by(metal::field<^^AdvancedEmployee::id>);
    const auto ranged_compiled = ranged.compile(dialect);
    assert((ids(db->execute(ranged_compiled.sql, ranged_compiled.params)) ==
            std::vector<std::int64_t>{1, 3}));

    auto outside = metal::select<AdvancedEmployee>()
        .where(metal::not_between(metal::field<^^AdvancedEmployee::salary>, 100, 130))
        .order_by(metal::field<^^AdvancedEmployee::id>);
    const auto outside_compiled = outside.compile(dialect);
    assert((ids(db->execute(outside_compiled.sql, outside_compiled.params)) ==
            std::vector<std::int64_t>{2, 4}));

    auto any_inactive = metal::select<AdvancedEmployee>()
        .clear_projection()
        .project(metal::field<^^AdvancedEmployee::id>)
        .where(metal::field<^^AdvancedEmployee::active> == false)
        .limit(1);
    auto exists_query = metal::select<AdvancedEmployee>()
        .where(metal::exists(any_inactive))
        .order_by(metal::field<^^AdvancedEmployee::id>);
    const auto exists_compiled = exists_query.compile(dialect);
    assert(ids(db->execute(exists_compiled.sql, exists_compiled.params)).size() == 4);

    auto impossible = metal::select<AdvancedEmployee>()
        .clear_projection()
        .project(metal::field<^^AdvancedEmployee::id>)
        .where(metal::field<^^AdvancedEmployee::salary> > 1000);
    auto not_exists_query = metal::select<AdvancedEmployee>()
        .where(metal::not_exists(impossible));
    const auto not_exists_compiled = not_exists_query.compile(dialect);
    assert(ids(db->execute(not_exists_compiled.sql, not_exists_compiled.params)).size() == 4);

    auto make_high = [] {
        return metal::select<AdvancedEmployee>()
            .clear_projection()
            .project(metal::field<^^AdvancedEmployee::id>)
            .where(metal::field<^^AdvancedEmployee::salary> >= 120);
    };
    auto make_inactive = [] {
        return metal::select<AdvancedEmployee>()
            .clear_projection()
            .project(metal::field<^^AdvancedEmployee::id>)
            .where(metal::field<^^AdvancedEmployee::active> == false);
    };

    auto union_query = make_high();
    union_query.union_with(make_inactive()).order_by(metal::field<^^AdvancedEmployee::id>);
    const auto union_compiled = union_query.compile(dialect);
    assert((ids(db->execute(union_compiled.sql, union_compiled.params)) ==
            std::vector<std::int64_t>{2, 3}));

    auto union_all_query = make_high();
    union_all_query.union_all(make_inactive()).order_by(metal::field<^^AdvancedEmployee::id>);
    const auto union_all_compiled = union_all_query.compile(dialect);
    assert((ids(db->execute(union_all_compiled.sql, union_all_compiled.params)) ==
            std::vector<std::int64_t>{2, 3, 3}));

    auto intersect_query = make_high();
    intersect_query.intersect(make_inactive());
    const auto intersect_compiled = intersect_query.compile(dialect);
    assert((ids(db->execute(intersect_compiled.sql, intersect_compiled.params)) ==
            std::vector<std::int64_t>{3}));

    auto except_query = make_high();
    except_query.except_with(make_inactive());
    const auto except_compiled = except_query.compile(dialect);
    assert((ids(db->execute(except_compiled.sql, except_compiled.params)) ==
            std::vector<std::int64_t>{2}));

    auto active_source = metal::select<AdvancedEmployee>()
        .where(metal::field<^^AdvancedEmployee::active> == true);
    auto cte_query = metal::select<AdvancedEmployee>()
        .with("active_employees", active_source)
        .from("active_employees")
        .where(metal::field<^^AdvancedEmployee::salary> >= 100)
        .order_by(metal::field<^^AdvancedEmployee::id>);
    const auto cte_compiled = cte_query.compile(dialect);
    assert((ids(db->execute(cte_compiled.sql, cte_compiled.params)) ==
            std::vector<std::int64_t>{1, 2}));

    auto tree = metal::select<AdvancedEmployee>()
        .where(metal::is_null(metal::field<^^AdvancedEmployee::parent_id>));
    auto recursive_step = metal::select<AdvancedEmployee>();
    recursive_step.join_cte<^^AdvancedEmployee::parent_id>("employee_tree", "id");
    tree.union_all(recursive_step);

    auto recursive_query = metal::select<AdvancedEmployee>()
        .with_recursive(
            "employee_tree",
            tree,
            {"id", "department", "salary", "active", "parent_id"})
        .from("employee_tree")
        .order_by(metal::field<^^AdvancedEmployee::id>);
    const auto recursive_compiled = recursive_query.compile(dialect);
    assert(recursive_compiled.sql.starts_with("WITH RECURSIVE"));
    assert((ids(db->execute(recursive_compiled.sql, recursive_compiled.params)) ==
            std::vector<std::int64_t>{1, 2, 3, 4}));

    auto ranked = metal::select<AdvancedEmployee>()
        .clear_projection()
        .project(metal::field<^^AdvancedEmployee::id>)
        .project(
            metal::row_number()
                .partition_by(metal::field<^^AdvancedEmployee::department>)
                .order_by(metal::field<^^AdvancedEmployee::salary>, false)
                .as("rn"))
        .project(
            metal::lag(metal::field<^^AdvancedEmployee::salary>, 1, 0)
                .partition_by(metal::field<^^AdvancedEmployee::department>)
                .order_by(metal::field<^^AdvancedEmployee::id>)
                .as("previous_salary"))
        .order_by(metal::field<^^AdvancedEmployee::id>);
    const auto ranked_compiled = ranked.compile(dialect);
    auto ranked_result = db->execute(ranked_compiled.sql, ranked_compiled.params);
    assert(ranked_result.rows.size() == 4);
    assert(metal::from_value<std::int64_t>(ranked_result.rows[0].at("rn")) == 2);
    assert(metal::from_value<std::int64_t>(ranked_result.rows[1].at("rn")) == 1);
    assert(metal::from_value<std::int64_t>(ranked_result.rows[0].at("previous_salary")) == 0);
    assert(metal::from_value<std::int64_t>(ranked_result.rows[1].at("previous_salary")) == 100);

    auto catalog = metal::select<AdvancedEmployee>()
        .clear_projection()
        .project(metal::rank().as("rank"))
        .project(metal::dense_rank().as("dense_rank"))
        .project(metal::ntile(2).order_by(metal::field<^^AdvancedEmployee::salary>).as("bucket"))
        .project(metal::lead(metal::field<^^AdvancedEmployee::salary>).as("next_salary"))
        .project(metal::first_value(metal::field<^^AdvancedEmployee::salary>).as("first_salary"))
        .project(metal::last_value(metal::field<^^AdvancedEmployee::salary>).as("last_salary"));
    const auto catalog_compiled = catalog.compile(dialect);
    assert(catalog_compiled.sql.find("RANK() OVER") != std::string::npos);
    assert(catalog_compiled.sql.find("DENSE_RANK() OVER") != std::string::npos);
    assert(catalog_compiled.sql.find("NTILE(?) OVER") != std::string::npos);
    assert(catalog_compiled.sql.find("LEAD(") != std::string::npos);
    assert(catalog_compiled.sql.find("FIRST_VALUE(") != std::string::npos);
    assert(catalog_compiled.sql.find("LAST_VALUE(") != std::string::npos);
}
