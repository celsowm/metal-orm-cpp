#include <metal/metal.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct CycleB;

struct [[=metal::mapping::table{"cycle_a"}]] CycleA {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    [[=metal::mapping::unique]]
    std::string code;

    [[=metal::mapping::reference_to<^^CycleB, "code">{}]]
    std::optional<std::string> b_code;

    [[=metal::mapping::belongs_to_key<^^CycleA::b_code, ^^CycleB, "code">{}]]
    metal::belongs_to_reference<CycleB> b;
};

struct [[=metal::mapping::table{"cycle_b"}]] CycleB {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};

    [[=metal::mapping::unique]]
    std::string code;

    [[=metal::mapping::reference_to<^^CycleA, "code">{}]]
    std::optional<std::string> a_code;

    [[=metal::mapping::belongs_to_key<^^CycleB::a_code, ^^CycleA, "code">{}]]
    metal::belongs_to_reference<CycleA> a;
};

static bool contains(const std::string& value, const std::string& needle) {
    return value.find(needle) != std::string::npos;
}

static_assert(metal::reflect::validate_mapping<CycleA>());
static_assert(metal::reflect::validate_mapping<CycleB>());

int main() {
    using ARelation = metal::reflect::relation_annotation_t<^^CycleA::b>;
    using BRelation = metal::reflect::relation_annotation_t<^^CycleB::a>;
    using ATraits = metal::mapping::relation_annotation_traits<ARelation>;
    using BTraits = metal::mapping::relation_annotation_traits<BRelation>;

    constexpr auto a_target_key = ATraits::target_key();
    constexpr auto b_target_key = BTraits::target_key();
    static_assert(std::same_as<metal::reflect::owner_type_t<a_target_key>, CycleB>);
    static_assert(std::same_as<metal::reflect::owner_type_t<b_target_key>, CycleA>);
    static_assert(metal::reflect::column_name_view<a_target_key>() == "code");
    static_assert(metal::reflect::column_name_view<b_target_key>() == "code");

    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    auto dialect = std::make_shared<metal::SQLiteDialect>();

    db->execute(metal::create_table_sql<CycleA>(*dialect));
    db->execute(metal::create_table_sql<CycleB>(*dialect));

    db->execute(
        "INSERT INTO cycle_a(code, b_code) VALUES (?, NULL);",
        {std::string{"A1"}});
    db->execute(
        "INSERT INTO cycle_b(code, a_code) VALUES (?, ?);",
        {std::string{"B1"}, std::string{"A1"}});
    db->execute(
        "UPDATE cycle_a SET b_code = ? WHERE code = ?;",
        {std::string{"B1"}, std::string{"A1"}});

    metal::Session session{db, dialect};
    auto a = session.find<CycleA>(std::int64_t{1});
    assert(a);
    assert(a->code == "A1");
    assert(!a->b.loaded());

    const auto b = a->b.load();
    assert(b);
    assert(b->code == "B1");
    assert(b->a_code == std::optional<std::string>{"A1"});
    assert(!b->a.loaded());

    const auto back_to_a = b->a.load();
    assert(back_to_a == a);

    const auto joined = metal::select<CycleA>()
        .join<^^CycleA::b>()
        .compile(*dialect);
    assert(contains(joined.sql, "\"b_code\""));
    assert(contains(joined.sql, "\"code\""));

    const auto filtered = metal::where_has<^^CycleA::b>(
        metal::select<CycleA>(),
        [](auto& child) {
            child.where(metal::field<^^CycleB::code> == std::string{"B1"});
        }).compile(*dialect);
    const auto filtered_rows = db->execute(filtered.sql, filtered.params);
    assert(filtered_rows.rows.size() == 1);

    auto replacement = std::make_shared<CycleB>();
    replacement->code = "B2";
    session.persist(replacement);
    session.commit();
    assert(replacement->id > 0);

    a->b.set(replacement);
    session.commit();
    assert(a->b_code == std::optional<std::string>{"B2"});

    const auto stored = db->execute(
        "SELECT b_code FROM cycle_a WHERE id = ?;",
        {a->id});
    assert(stored.rows.size() == 1);
    assert(metal::from_value<std::string>(stored.rows[0].at("b_code")) == "B2");

    const auto schema = metal::introspect_sqlite(
        *db,
        metal::IntrospectOptions{.include_tables = {"cycle_a", "cycle_b"}});
    const auto generated = metal::generate_entity_header(schema);

    assert(contains(
        generated.code,
        "metal::mapping::belongs_to_key<^^CycleA::b_code, ^^CycleB, \"code\">{}"));
    assert(contains(
        generated.code,
        "metal::mapping::belongs_to_key<^^CycleB::a_code, ^^CycleA, \"code\">{}"));
    assert(contains(generated.code, "metal::belongs_to_reference<CycleB> cycle_b;"));
    assert(contains(generated.code, "metal::belongs_to_reference<CycleA> cycle_a;"));
    assert(std::none_of(
        generated.warnings.begin(), generated.warnings.end(),
        [](const std::string& warning) {
            return warning.find("rather than the target's single primary key") != std::string::npos;
        }));

    metal::DatabaseSchema composite_schema;

    metal::DatabaseTable composite_target;
    composite_target.name = "composite_targets";
    composite_target.primary_key = {"left_id", "right_id"};
    composite_target.columns = {
        metal::DatabaseColumn{.name = "left_id", .type = "INTEGER", .not_null = true},
        metal::DatabaseColumn{.name = "right_id", .type = "INTEGER", .not_null = true},
        metal::DatabaseColumn{.name = "code", .type = "TEXT", .not_null = true, .unique = true}
    };
    composite_schema.tables.push_back(composite_target);

    metal::DatabaseTable composite_source;
    composite_source.name = "composite_sources";
    composite_source.primary_key = {"id"};
    composite_source.columns = {
        metal::DatabaseColumn{.name = "id", .type = "INTEGER", .not_null = true},
        metal::DatabaseColumn{
            .name = "target_code",
            .type = "TEXT",
            .references = metal::ForeignKeyReference{
                .table = "composite_targets",
                .column = "code"
            }
        }
    };
    composite_schema.tables.push_back(composite_source);

    const auto composite_generated = metal::generate_entity_header(composite_schema);
    assert(!contains(
        composite_generated.code,
        "metal::belongs_to_reference<CompositeTarget>"));
    assert(std::any_of(
        composite_generated.warnings.begin(), composite_generated.warnings.end(),
        [](const std::string& warning) {
            return warning.find(
                "belongs_to_reference requires a target with exactly one primary key") !=
                std::string::npos;
        }));
}
