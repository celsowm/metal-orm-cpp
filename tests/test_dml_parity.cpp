#include <metal/metal.hpp>

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct [[=metal::mapping::table{"dml_sources"}]] DmlSource {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;
    std::int64_t score{};
};

struct [[=metal::mapping::table{"dml_targets"}]] DmlTarget {
    [[=metal::mapping::primary_key, =metal::mapping::generated]]
    std::int64_t id{};
    std::string name;
    std::int64_t score{};
};

static_assert(metal::reflect::validate_mapping<DmlSource>());
static_assert(metal::reflect::validate_mapping<DmlTarget>());

int main() {
    metal::SQLiteDialect dialect;

    std::vector<std::vector<metal::DmlAssignment>> rows{
        {
            {"name", std::string{"alpha"}},
            {"score", std::int64_t{10}}
        },
        {
            {"name", std::string{"beta"}},
            {"score", std::int64_t{20}}
        }
    };

    const auto multi = metal::InsertQueryBuilder{"dml_targets"}
        .values(rows)
        .returning({"id", "name", "score"})
        .compile(dialect);
    assert(
        multi.sql ==
        "INSERT INTO \"dml_targets\" (\"name\", \"score\") VALUES (?, ?), (?, ?) RETURNING \"id\", \"name\", \"score\";");
    assert(multi.params.size() == 4);

    auto source_query = metal::select<DmlSource>();
    source_query
        .clear_projection()
        .project(metal::field<^^DmlSource::name>)
        .project(metal::field<^^DmlSource::score>)
        .where(metal::field<^^DmlSource::score> >= std::int64_t{30});

    const auto insert_select = metal::insert_into<DmlTarget>()
        .from_select(source_query, {"name", "score"})
        .returning({"id", "name"})
        .compile(dialect);
    assert(insert_select.sql.find("INSERT INTO \"dml_targets\" (\"name\", \"score\") SELECT") == 0);
    assert(insert_select.sql.ends_with(" RETURNING \"id\", \"name\";"));
    assert(insert_select.params.size() == 1);

    const auto upsert = metal::InsertQueryBuilder{"dml_targets"}
        .values({
            {"name", std::string{"alpha"}},
            {"score", std::int64_t{99}}
        })
        .on_conflict({"name"})
        .do_update({
            {"score", metal::excluded("score")}
        })
        .returning({"id", "name", "score"})
        .compile(dialect);
    assert(
        upsert.sql ==
        "INSERT INTO \"dml_targets\" (\"name\", \"score\") VALUES (?, ?) ON CONFLICT (\"name\") DO UPDATE SET \"score\" = excluded.\"score\" RETURNING \"id\", \"name\", \"score\";");

    const auto conditional_upsert = metal::InsertQueryBuilder{"dml_targets"}
        .values({
            {"name", std::string{"alpha"}},
            {"score", std::int64_t{200}}
        })
        .on_conflict({"name"})
        .do_update(
            {{"score", metal::excluded("score")}},
            {{"score", metal::CompareOp::Lt, std::int64_t{150}}})
        .returning({"score"})
        .compile(dialect);
    assert(conditional_upsert.sql.find(" DO UPDATE SET \"score\" = excluded.\"score\" WHERE \"score\" < ? RETURNING") != std::string::npos);

    const auto do_nothing = metal::InsertQueryBuilder{"dml_targets"}
        .values({
            {"name", std::string{"alpha"}},
            {"score", std::int64_t{500}}
        })
        .on_conflict({"name"})
        .do_nothing()
        .returning({"id"})
        .compile(dialect);
    assert(do_nothing.sql.find("ON CONFLICT (\"name\") DO NOTHING RETURNING \"id\"") != std::string::npos);

    const auto update = metal::UpdateQueryBuilder{"dml_targets"}
        .set({{"score", std::int64_t{123}}})
        .where_eq("name", std::string{"beta"})
        .returning({metal::DmlReturning{"id", "updated_id"}, metal::DmlReturning{"score", std::nullopt}})
        .compile(dialect);
    assert(update.sql.ends_with(" RETURNING \"id\" AS \"updated_id\", \"score\";"));

    const auto erase = metal::DeleteQueryBuilder{"dml_targets"}
        .where_eq("name", std::string{"beta"})
        .returning({"id", "name"})
        .compile(dialect);
    assert(erase.sql.ends_with(" RETURNING \"id\", \"name\";"));

    auto db = std::make_shared<metal::SQLiteExecutor>(":memory:");
    db->execute(metal::create_table_sql<DmlSource>(dialect));
    db->execute(metal::create_table_sql<DmlTarget>(dialect));
    db->execute("CREATE UNIQUE INDEX dml_targets_name_uq ON dml_targets(name);");

    db->execute(
        "INSERT INTO dml_sources(name, score) VALUES (?, ?), (?, ?);",
        {std::string{"gamma"}, std::int64_t{30}, std::string{"delta"}, std::int64_t{40}});

    auto result = db->execute(multi.sql, multi.params);
    assert(result.rows.size() == 2);
    assert(metal::from_value<std::string>(result.rows[0].at("name")) == "alpha");
    assert(metal::from_value<std::string>(result.rows[1].at("name")) == "beta");

    result = db->execute(insert_select.sql, insert_select.params);
    assert(result.rows.size() == 2);

    result = db->execute(upsert.sql, upsert.params);
    assert(result.rows.size() == 1);
    assert(metal::from_value<std::int64_t>(result.rows[0].at("score")) == 99);

    result = db->execute(conditional_upsert.sql, conditional_upsert.params);
    assert(result.rows.size() == 1);
    assert(metal::from_value<std::int64_t>(result.rows[0].at("score")) == 200);

    result = db->execute(do_nothing.sql, do_nothing.params);
    assert(result.rows.empty());

    result = db->execute(update.sql, update.params);
    assert(result.rows.size() == 1);
    assert(metal::from_value<std::int64_t>(result.rows[0].at("score")) == 123);
    assert(result.rows[0].contains("updated_id"));

    result = db->execute(erase.sql, erase.params);
    assert(result.rows.size() == 1);
    assert(metal::from_value<std::string>(result.rows[0].at("name")) == "beta");

    const auto remaining = db->execute("SELECT COUNT(*) AS c FROM dml_targets;");
    assert(metal::from_value<std::int64_t>(remaining.rows[0].at("c")) == 3);

    bool mixed_source_rejected = false;
    try {
        auto invalid = metal::InsertQueryBuilder{"dml_targets"};
        invalid.values({{"name", std::string{"x"}}});
        invalid.from_select(source_query, {"name", "score"});
    } catch (const std::logic_error&) {
        mixed_source_rejected = true;
    }
    assert(mixed_source_rejected);

    bool empty_conflict_target_rejected = false;
    try {
        auto invalid = metal::InsertQueryBuilder{"dml_targets"}
            .values({{"name", std::string{"x"}}, {"score", std::int64_t{1}}})
            .on_conflict({})
            .do_update({{"score", std::int64_t{2}}})
            .compile(dialect);
        (void)invalid;
    } catch (const std::logic_error&) {
        empty_conflict_target_rejected = true;
    }
    assert(empty_conflict_target_rejected);
}
