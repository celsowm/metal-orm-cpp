#pragma once

#include "metal/dml.hpp"
#include "metal/orm.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace metal {

using BulkRow = std::vector<DmlAssignment>;

enum class BulkStrategy {
    Individual,
    Batch,
    WhereIn
};

struct BulkResultMetadata {
    BulkStrategy strategy{BulkStrategy::Individual};
    std::string dialect{"sqlite"};
    bool has_returning_support{true};
};

struct BulkResult {
    std::size_t processed_rows{0};
    std::size_t chunks_executed{0};
    std::vector<Row> returning;
    std::optional<std::vector<double>> chunk_timings_ms;
    std::optional<BulkResultMetadata> metadata;
};

struct ChunkCompleteInfo {
    std::size_t chunk_index{0};
    std::size_t total_chunks{0};
    std::size_t rows_in_chunk{0};
    double elapsed_ms{0.0};
};

struct BulkBaseOptions {
    std::size_t chunk_size{500};
    std::size_t concurrency{1};
    bool transactional{true};
    bool timing{false};
    std::function<void(const ChunkCompleteInfo&)> on_chunk_complete;
};

class BulkColumns {
public:
    enum class Mode {
        Default,
        Selected,
        All
    };

    BulkColumns() = default;

    template <std::meta::info First, std::meta::info... Rest>
    static BulkColumns selected() {
        using Owner = reflect::owner_type_t<First>;
        static_assert(std::meta::is_nonstatic_data_member(First),
                      "MetalORM: bulk column must reflect a data member");
        static_assert(reflect::is_persistent_member<First>(),
                      "MetalORM: bulk column must be a persistent scalar member");
        static_assert((std::same_as<reflect::owner_type_t<Rest>, Owner> && ...),
                      "MetalORM: bulk columns must belong to the same entity");
        static_assert((reflect::is_persistent_member<Rest>() && ...),
                      "MetalORM: bulk columns must be persistent scalar members");

        std::vector<std::string> names{reflect::column_name<First>()};
        (names.push_back(reflect::column_name<Rest>()), ...);
        return BulkColumns{Mode::Selected, std::move(names)};
    }

    template <reflect::Mapped T>
    static BulkColumns all() {
        static_assert(reflect::validate_mapping<T>());
        std::vector<std::string> names;
        reflect::for_each_column<T>([&]<std::meta::info Member>() {
            names.push_back(reflect::column_name<Member>());
        });
        return BulkColumns{Mode::All, std::move(names)};
    }

    static BulkColumns none() {
        return BulkColumns{Mode::Selected, {}};
    }

    [[nodiscard]] Mode mode() const noexcept { return mode_; }
    [[nodiscard]] const std::vector<std::string>& names() const noexcept { return names_; }

private:
    BulkColumns(Mode mode, std::vector<std::string> names)
        : mode_(mode), names_(std::move(names)) {}

    Mode mode_{Mode::Default};
    std::vector<std::string> names_;
};

template <std::meta::info First, std::meta::info... Rest>
BulkColumns bulk_columns() {
    return BulkColumns::selected<First, Rest...>();
}

template <reflect::Mapped T>
BulkColumns bulk_all_columns() {
    return BulkColumns::all<T>();
}

inline BulkColumns bulk_no_columns() {
    return BulkColumns::none();
}

namespace bulk_detail {

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

template <typename V>
Value normalize_value(V&& value) {
    if constexpr (std::same_as<std::remove_cvref_t<V>, Value>) {
        return std::forward<V>(value);
    } else {
        return to_value(std::forward<V>(value));
    }
}

inline Value assignment_value(const DmlAssignment& assignment, std::string_view context) {
    if (const auto* value = std::get_if<Value>(&assignment.value)) return *value;
    throw std::invalid_argument(
        "MetalORM: excluded() cannot be used as a bulk row value in " + std::string(context));
}

template <reflect::Mapped T>
bool known_column(std::string_view name) {
    bool found = false;
    reflect::for_each_column<T>([&]<std::meta::info Member>() {
        if (reflect::column_name_view<Member>() == name) found = true;
    });
    return found;
}

template <reflect::Mapped T>
std::vector<std::string> mapped_columns() {
    std::vector<std::string> out;
    reflect::for_each_column<T>([&]<std::meta::info Member>() {
        out.push_back(reflect::column_name<Member>());
    });
    return out;
}

template <reflect::Entity T>
std::vector<std::string> resolve_by_columns(const BulkColumns& selection, bool single_only = false) {
    std::vector<std::string> result;
    if (selection.mode() == BulkColumns::Mode::Default) {
        result.push_back(reflect::primary_key_name<T>());
    } else if (selection.mode() == BulkColumns::Mode::All) {
        throw std::invalid_argument("MetalORM: bulk 'by' cannot select all columns");
    } else {
        result = selection.names();
    }

    if (result.empty()) {
        throw std::invalid_argument("MetalORM: bulk 'by' requires at least one reflected column");
    }
    if (single_only && result.size() != 1) {
        throw std::invalid_argument("MetalORM: this bulk operation requires exactly one reflected 'by' column");
    }
    for (const auto& name : result) {
        if (!known_column<T>(name)) {
            throw std::invalid_argument("MetalORM: bulk 'by' column is not mapped by the target entity");
        }
    }
    return result;
}

template <reflect::Mapped T>
std::vector<std::string> resolve_returning_columns(const BulkColumns& selection) {
    if (selection.mode() == BulkColumns::Mode::Default) return {};
    const auto result = selection.mode() == BulkColumns::Mode::All
        ? mapped_columns<T>()
        : selection.names();
    for (const auto& name : result) {
        if (!known_column<T>(name)) {
            throw std::invalid_argument("MetalORM: RETURNING column is not mapped by the target entity");
        }
    }
    return result;
}

inline void validate_base_options(const BulkBaseOptions& options) {
    if (options.chunk_size < 1) {
        throw std::invalid_argument("MetalORM: bulk chunk_size must be >= 1");
    }
    if (options.concurrency < 1) {
        throw std::invalid_argument("MetalORM: bulk concurrency must be >= 1");
    }
}

struct ChunkOutcome {
    std::size_t processed_rows{0};
    std::vector<Row> returning;
    double elapsed_ms{0.0};
};

template <typename ExecuteChunk>
std::vector<ChunkOutcome> run_chunks(
    std::size_t row_count,
    const BulkBaseOptions& options,
    ExecuteChunk&& execute_chunk) {
    validate_base_options(options);
    const std::size_t total_chunks =
        row_count == 0 ? 0 : (row_count + options.chunk_size - 1) / options.chunk_size;
    std::vector<ChunkOutcome> outcomes(total_chunks);

    const auto run_one = [&](std::size_t chunk_index) {
        const std::size_t begin = chunk_index * options.chunk_size;
        const std::size_t end = std::min(row_count, begin + options.chunk_size);
        const bool measure = options.timing || static_cast<bool>(options.on_chunk_complete);
        const auto started = measure ? std::chrono::steady_clock::now()
                                     : std::chrono::steady_clock::time_point{};

        auto outcome = execute_chunk(chunk_index, begin, end);
        if (measure) {
            const auto elapsed = std::chrono::steady_clock::now() - started;
            outcome.elapsed_ms =
                std::chrono::duration<double, std::milli>(elapsed).count();
        }

        if (options.on_chunk_complete) {
            options.on_chunk_complete(ChunkCompleteInfo{
                chunk_index,
                total_chunks,
                end - begin,
                outcome.elapsed_ms
            });
        }
        outcomes[chunk_index] = std::move(outcome);
    };

    if (total_chunks == 0) return outcomes;
    if (options.concurrency == 1 || total_chunks == 1) {
        for (std::size_t i = 0; i < total_chunks; ++i) run_one(i);
        return outcomes;
    }

    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};
    std::exception_ptr error;
    std::mutex error_mutex;
    const std::size_t worker_count = std::min(options.concurrency, total_chunks);

    {
        std::vector<std::jthread> workers;
        workers.reserve(worker_count);
        for (std::size_t worker = 0; worker < worker_count; ++worker) {
            workers.emplace_back([&] {
                while (!failed.load(std::memory_order_acquire)) {
                    const auto index = next.fetch_add(1, std::memory_order_relaxed);
                    if (index >= total_chunks) break;
                    try {
                        run_one(index);
                    } catch (...) {
                        {
                            std::lock_guard lock(error_mutex);
                            if (!error) error = std::current_exception();
                        }
                        failed.store(true, std::memory_order_release);
                        break;
                    }
                }
            });
        }
    }

    if (error) std::rethrow_exception(error);
    return outcomes;
}

inline BulkResult aggregate(
    std::vector<ChunkOutcome> outcomes,
    bool timing,
    BulkStrategy strategy) {
    BulkResult result;
    result.chunks_executed = outcomes.size();
    if (timing) result.chunk_timings_ms.emplace();

    for (auto& outcome : outcomes) {
        result.processed_rows += outcome.processed_rows;
        result.returning.insert(
            result.returning.end(),
            std::make_move_iterator(outcome.returning.begin()),
            std::make_move_iterator(outcome.returning.end()));
        if (result.chunk_timings_ms) {
            result.chunk_timings_ms->push_back(outcome.elapsed_ms);
        }
    }

    result.metadata = BulkResultMetadata{strategy, "sqlite", true};
    return result;
}

template <typename Fn>
BulkResult maybe_transaction(Session& session, bool transactional, Fn&& fn) {
    if (!transactional) return std::invoke(std::forward<Fn>(fn));
    return session.transaction([&](Session&) {
        return std::invoke(std::forward<Fn>(fn));
    });
}

inline void apply_conflict(InsertQueryBuilder& builder, const DmlConflictClause& clause) {
    auto conflict = builder.on_conflict(clause.columns);
    if (clause.do_nothing) {
        conflict.do_nothing();
    } else {
        conflict.do_update(clause.assignments, clause.predicates);
    }
}

} // namespace bulk_detail

template <reflect::Entity T>
class BulkRowBuilder {
public:
    template <std::meta::info Member, typename V>
    BulkRowBuilder& set(V&& value) {
        static_assert(std::same_as<reflect::owner_type_t<Member>, T>,
                      "MetalORM: bulk row member must belong to the target entity");
        static_assert(reflect::is_persistent_member<Member>(),
                      "MetalORM: bulk row member must be a persistent scalar column");
        using MemberType = reflect::member_type_t<Member>;
        static_assert(bulk_detail::value_compatible<MemberType, V>(),
                      "MetalORM: bulk row value is incompatible with the reflected member type");

        const auto name = reflect::column_name<Member>();
        Value converted;
        if constexpr (std::same_as<std::remove_cvref_t<V>, std::nullopt_t>) {
            converted = Value{nullptr};
        } else {
            converted = to_value(std::forward<V>(value));
        }

        auto found = std::find_if(row_.begin(), row_.end(), [&](const auto& assignment) {
            return assignment.column == name;
        });
        if (found == row_.end()) row_.push_back(DmlAssignment{name, std::move(converted)});
        else found->value = std::move(converted);
        return *this;
    }

    [[nodiscard]] BulkRow build() const & { return row_; }
    [[nodiscard]] BulkRow build() && { return std::move(row_); }

private:
    BulkRow row_;
};

template <reflect::Entity T>
BulkRowBuilder<T> bulk_row() {
    static_assert(reflect::validate_mapping<T>());
    return {};
}

struct BulkInsertOptions : BulkBaseOptions {
    BulkColumns returning;
    std::optional<DmlConflictClause> on_conflict;
};

template <reflect::Entity T>
struct BulkUpdateOptions : BulkBaseOptions {
    BulkColumns by;
    std::optional<Expression<T>> where;
    BulkColumns returning;
};

template <reflect::Entity T>
struct BulkDeleteOptions : BulkBaseOptions {
    BulkColumns by;
    std::optional<Expression<T>> where;
};

struct BulkUpsertOptions : BulkBaseOptions {
    BulkColumns conflict_columns;
    BulkColumns update_columns;
    BulkColumns returning;
};

struct BulkDeleteWhereOptions {
    bool transactional{false};
};

template <reflect::Entity T>
BulkResult bulk_insert(
    Session& session,
    const std::vector<BulkRow>& rows,
    const BulkInsertOptions& options = {}) {
    static_assert(reflect::validate_mapping<T>());
    if (rows.empty()) return {};

    const auto returning = bulk_detail::resolve_returning_columns<T>(options.returning);
    return bulk_detail::maybe_transaction(session, options.transactional, [&] {
        auto outcomes = bulk_detail::run_chunks(
            rows.size(), options,
            [&](std::size_t, std::size_t begin, std::size_t end) {
                std::vector<BulkRow> chunk(rows.begin() + static_cast<std::ptrdiff_t>(begin),
                                           rows.begin() + static_cast<std::ptrdiff_t>(end));
                auto builder = insert_into<T>();
                builder.values(std::move(chunk));
                if (options.on_conflict) {
                    bulk_detail::apply_conflict(builder, *options.on_conflict);
                }
                if (!returning.empty()) builder.returning(returning);
                const auto compiled = builder.compile(session.dialect());
                auto result = session.executor().execute(compiled.sql, compiled.params);
                return bulk_detail::ChunkOutcome{
                    end - begin,
                    std::move(result.rows),
                    0.0
                };
            });
        return bulk_detail::aggregate(std::move(outcomes), options.timing, BulkStrategy::Batch);
    });
}

template <reflect::Entity T>
BulkResult bulk_update(
    Session& session,
    const std::vector<BulkRow>& rows,
    const BulkUpdateOptions<T>& options = {}) {
    static_assert(reflect::validate_mapping<T>());
    if (rows.empty()) return {};

    const auto by_columns = bulk_detail::resolve_by_columns<T>(options.by);
    const auto returning = bulk_detail::resolve_returning_columns<T>(options.returning);

    return bulk_detail::maybe_transaction(session, options.transactional, [&] {
        auto outcomes = bulk_detail::run_chunks(
            rows.size(), options,
            [&](std::size_t, std::size_t begin, std::size_t end) {
                bulk_detail::ChunkOutcome outcome;
                outcome.processed_rows = end - begin;

                for (std::size_t i = begin; i < end; ++i) {
                    const auto& row = rows[i];
                    std::vector<DmlPredicate> predicates;
                    predicates.reserve(by_columns.size());

                    for (const auto& by : by_columns) {
                        const auto found = std::find_if(row.begin(), row.end(), [&](const auto& assignment) {
                            return assignment.column == by;
                        });
                        if (found == row.end()) {
                            throw std::invalid_argument(
                                "MetalORM: bulk_update row is missing a reflected identity column required by 'by'");
                        }
                        predicates.push_back(DmlPredicate{
                            by,
                            CompareOp::Eq,
                            bulk_detail::assignment_value(*found, "bulk_update")
                        });
                    }

                    std::vector<DmlAssignment> assignments;
                    for (const auto& assignment : row) {
                        if (std::find(by_columns.begin(), by_columns.end(), assignment.column) != by_columns.end()) {
                            continue;
                        }
                        if (bulk_detail::known_column<T>(assignment.column)) assignments.push_back(assignment);
                    }
                    if (assignments.empty()) continue;

                    auto builder = update<T>();
                    builder.set(std::move(assignments)).where(std::move(predicates));
                    if (options.where) builder.template and_where<T>(*options.where);
                    if (!returning.empty()) builder.returning(returning);
                    const auto compiled = builder.compile(session.dialect());
                    auto result = session.executor().execute(compiled.sql, compiled.params);
                    outcome.returning.insert(
                        outcome.returning.end(),
                        std::make_move_iterator(result.rows.begin()),
                        std::make_move_iterator(result.rows.end()));
                }
                return outcome;
            });
        return bulk_detail::aggregate(std::move(outcomes), options.timing, BulkStrategy::Individual);
    });
}

template <reflect::Entity T, typename Id>
BulkResult bulk_update_where(
    Session& session,
    const std::vector<Id>& ids,
    BulkRow set,
    const BulkUpdateOptions<T>& options = {}) {
    static_assert(reflect::validate_mapping<T>());
    if (ids.empty()) return {};

    const auto by_columns = bulk_detail::resolve_by_columns<T>(options.by, true);
    const auto returning = bulk_detail::resolve_returning_columns<T>(options.returning);
    const auto by = by_columns.front();

    set.erase(
        std::remove_if(set.begin(), set.end(), [&](const auto& assignment) {
            return !bulk_detail::known_column<T>(assignment.column);
        }),
        set.end());
    if (set.empty()) {
        throw std::invalid_argument("MetalORM: bulk_update_where requires at least one mapped SET column");
    }

    std::vector<Value> values;
    values.reserve(ids.size());
    for (const auto& id : ids) values.push_back(bulk_detail::normalize_value(id));

    return bulk_detail::maybe_transaction(session, options.transactional, [&] {
        auto outcomes = bulk_detail::run_chunks(
            values.size(), options,
            [&](std::size_t, std::size_t begin, std::size_t end) {
                std::vector<Value> chunk(values.begin() + static_cast<std::ptrdiff_t>(begin),
                                         values.begin() + static_cast<std::ptrdiff_t>(end));
                auto builder = update<T>();
                builder.set(set).where_in(by, std::move(chunk));
                if (options.where) builder.template and_where<T>(*options.where);
                if (!returning.empty()) builder.returning(returning);
                const auto compiled = builder.compile(session.dialect());
                auto result = session.executor().execute(compiled.sql, compiled.params);
                return bulk_detail::ChunkOutcome{
                    end - begin,
                    std::move(result.rows),
                    0.0
                };
            });
        return bulk_detail::aggregate(std::move(outcomes), options.timing, BulkStrategy::WhereIn);
    });
}

template <reflect::Entity T, typename Id>
BulkResult bulk_delete(
    Session& session,
    const std::vector<Id>& ids,
    const BulkDeleteOptions<T>& options = {}) {
    static_assert(reflect::validate_mapping<T>());
    if (ids.empty()) return {};

    const auto by_columns = bulk_detail::resolve_by_columns<T>(options.by, true);
    const auto by = by_columns.front();
    std::vector<Value> values;
    values.reserve(ids.size());
    for (const auto& id : ids) values.push_back(bulk_detail::normalize_value(id));

    return bulk_detail::maybe_transaction(session, options.transactional, [&] {
        auto outcomes = bulk_detail::run_chunks(
            values.size(), options,
            [&](std::size_t, std::size_t begin, std::size_t end) {
                std::vector<Value> chunk(values.begin() + static_cast<std::ptrdiff_t>(begin),
                                         values.begin() + static_cast<std::ptrdiff_t>(end));
                auto builder = delete_from<T>();
                builder.where_in(by, std::move(chunk));
                if (options.where) builder.template and_where<T>(*options.where);
                const auto compiled = builder.compile(session.dialect());
                (void)session.executor().execute(compiled.sql, compiled.params);
                return bulk_detail::ChunkOutcome{end - begin, {}, 0.0};
            });
        return bulk_detail::aggregate(std::move(outcomes), options.timing, BulkStrategy::WhereIn);
    });
}

template <reflect::Entity T>
BulkResult bulk_delete_where(
    Session& session,
    Expression<T> where,
    const BulkDeleteWhereOptions& options = {}) {
    static_assert(reflect::validate_mapping<T>());

    const auto execute = [&]() {
        auto builder = delete_from<T>();
        builder.template where<T>(std::move(where));
        const auto compiled = builder.compile(session.dialect());
        (void)session.executor().execute(compiled.sql, compiled.params);
        BulkResult result;
        result.chunks_executed = 1;
        result.metadata = BulkResultMetadata{BulkStrategy::WhereIn, "sqlite", true};
        return result;
    };

    return bulk_detail::maybe_transaction(session, options.transactional, execute);
}

template <reflect::Entity T>
BulkResult bulk_upsert(
    Session& session,
    const std::vector<BulkRow>& rows,
    const BulkUpsertOptions& options = {}) {
    static_assert(reflect::validate_mapping<T>());
    if (rows.empty()) return {};

    std::vector<std::string> conflict_columns;
    if (options.conflict_columns.mode() == BulkColumns::Mode::Default) {
        conflict_columns.push_back(reflect::primary_key_name<T>());
    } else if (options.conflict_columns.mode() == BulkColumns::Mode::All) {
        conflict_columns = bulk_detail::mapped_columns<T>();
    } else {
        conflict_columns = options.conflict_columns.names();
    }
    if (conflict_columns.empty()) {
        throw std::invalid_argument("MetalORM: bulk_upsert requires at least one conflict column");
    }
    for (const auto& name : conflict_columns) {
        if (!bulk_detail::known_column<T>(name)) {
            throw std::invalid_argument("MetalORM: bulk_upsert conflict column is not mapped");
        }
    }

    std::vector<std::string> update_columns;
    if (options.update_columns.mode() == BulkColumns::Mode::Default) {
        for (const auto& assignment : rows.front()) {
            if (!bulk_detail::known_column<T>(assignment.column)) continue;
            if (std::find(conflict_columns.begin(), conflict_columns.end(), assignment.column) ==
                conflict_columns.end()) {
                update_columns.push_back(assignment.column);
            }
        }
    } else if (options.update_columns.mode() == BulkColumns::Mode::All) {
        update_columns = bulk_detail::mapped_columns<T>();
    } else {
        update_columns = options.update_columns.names();
    }
    for (const auto& name : update_columns) {
        if (!bulk_detail::known_column<T>(name)) {
            throw std::invalid_argument("MetalORM: bulk_upsert update column is not mapped");
        }
    }

    const auto returning = bulk_detail::resolve_returning_columns<T>(options.returning);
    return bulk_detail::maybe_transaction(session, options.transactional, [&] {
        auto outcomes = bulk_detail::run_chunks(
            rows.size(), options,
            [&](std::size_t, std::size_t begin, std::size_t end) {
                std::vector<BulkRow> chunk(rows.begin() + static_cast<std::ptrdiff_t>(begin),
                                           rows.begin() + static_cast<std::ptrdiff_t>(end));
                auto builder = insert_into<T>();
                builder.values(std::move(chunk));
                auto conflict = builder.on_conflict(conflict_columns);
                if (update_columns.empty()) {
                    conflict.do_nothing();
                } else {
                    std::vector<DmlAssignment> assignments;
                    assignments.reserve(update_columns.size());
                    for (const auto& name : update_columns) {
                        assignments.push_back(DmlAssignment{name, excluded(name)});
                    }
                    conflict.do_update(std::move(assignments));
                }
                if (!returning.empty()) builder.returning(returning);
                const auto compiled = builder.compile(session.dialect());
                auto result = session.executor().execute(compiled.sql, compiled.params);
                return bulk_detail::ChunkOutcome{
                    end - begin,
                    std::move(result.rows),
                    0.0
                };
            });
        return bulk_detail::aggregate(std::move(outcomes), options.timing, BulkStrategy::Batch);
    });
}

} // namespace metal
