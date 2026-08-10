#pragma once

#include "metal/execution.hpp"
#include "metal/mapping.hpp"
#include "metal/query.hpp"

#include <concepts>
#include <cstddef>
#include <meta>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

namespace metal::mapping {

struct view {
    fixed_text<96> name;

    template <std::size_t N>
    consteval view(const char (&value)[N]) : name(value) {
        if (name.view().empty()) throw "MetalORM: view name cannot be empty";
    }
};

} // namespace metal::mapping

namespace metal::reflect {

template <typename T>
concept ViewMapped = std::is_class_v<T> && has<mapping::view>(^^T);

template <info Member>
consteval bool is_view_column() {
    using M = member_type_t<Member>;
    return (PersistableValue<M> || std::same_as<std::remove_cvref_t<M>, Value>) &&
           !has<mapping::ignore_t>(Member) &&
           !has_relation_annotation<Member>();
}

template <ViewMapped T, typename F>
constexpr void for_each_view_column(F&& fn) {
    template for (constexpr auto member : data_members<T>()) {
        if constexpr (is_view_column<member>()) fn.template operator()<member>();
    }
}

template <ViewMapped T>
consteval bool validate_view_mapping() {
    static_assert(std::meta::annotations_of_with_type(^^T, ^^mapping::view).size() == 1,
                  "MetalORM: mapped view must have exactly one view annotation");
    static_assert(!has<mapping::table>(^^T),
                  "MetalORM: a mapped type cannot be both a table and a view");

    template for (constexpr auto member : data_members<T>()) {
        using M = member_type_t<member>;
        if constexpr (has_relation_annotation<member>()) {
            static_assert(!has_relation_annotation<member>(),
                          "MetalORM: mapped views are read-only and cannot declare ORM relations");
        } else if constexpr (has<mapping::ignore_t>(member)) {
            static_assert(!has<mapping::column>(member) &&
                          !has<mapping::primary_key_t>(member) &&
                          !has<mapping::generated_t>(member),
                          "MetalORM: ignored view member cannot also carry column/PK/generated metadata");
        } else {
            static_assert(PersistableValue<M> || std::same_as<std::remove_cvref_t<M>, Value>,
                          "MetalORM: view columns must be scalar values, metal::Value, or [[=ignore]]");
            static_assert(!has<mapping::primary_key_t>(member),
                          "MetalORM: mapped views do not declare ORM primary keys");
            static_assert(!has<mapping::generated_t>(member),
                          "MetalORM: mapped views cannot declare generated columns");
        }
    }

    template for (constexpr auto left : data_members<T>()) {
        if constexpr (is_view_column<left>()) {
            template for (constexpr auto right : data_members<T>()) {
                if constexpr (left != right && is_view_column<right>()) {
                    static_assert(column_name_view<left>() != column_name_view<right>(),
                                  "MetalORM: duplicate mapped view column name");
                }
            }
        }
    }
    return true;
}

template <ViewMapped T>
inline constexpr auto view_mapping = annotation<mapping::view>(^^T);

template <ViewMapped T>
std::string view_name() {
    static_assert(validate_view_mapping<T>());
    return std::string(view_mapping<T>.name.view());
}

} // namespace metal::reflect

namespace metal {

struct ViewOrderSpec {
    std::string column;
    bool ascending{true};
};

template <reflect::ViewMapped T>
class ViewQuery {
public:
    ViewQuery(DbExecutor& executor, const Dialect& dialect)
        : executor_(executor), dialect_(dialect) {
        static_assert(reflect::validate_view_mapping<T>());
    }

    ViewQuery& where(Expression<T> expression) {
        where_ = expression.node();
        return *this;
    }

    template <std::meta::info Member>
    requires std::same_as<reflect::owner_type_t<Member>, T> && reflect::is_view_column<Member>()
    ViewQuery& order_by(bool ascending = true) {
        order_by_.push_back(ViewOrderSpec{reflect::column_name<Member>(), ascending});
        return *this;
    }

    ViewQuery& limit(std::size_t value) {
        limit_ = value;
        return *this;
    }

    ViewQuery& offset(std::size_t value) {
        offset_ = value;
        return *this;
    }

    [[nodiscard]] CompiledQuery compile() const {
        CompiledQuery out;
        const std::string alias{"v"};
        out.sql = "SELECT ";

        bool first = true;
        reflect::for_each_view_column<T>([&]<std::meta::info Member>() {
            if (!first) out.sql += ", ";
            first = false;
            const auto column = reflect::column_name<Member>();
            out.sql += dialect_.quote_identifier(alias) + "." + dialect_.quote_identifier(column) +
                       " AS " + dialect_.quote_identifier(column);
        });
        if (first) throw std::logic_error("MetalORM: mapped view has no readable columns");

        out.sql += " FROM " + dialect_.quote_identifier(reflect::view_name<T>()) +
                   " AS " + dialect_.quote_identifier(alias);

        CompileContext ctx{dialect_, {{std::type_index(typeid(T)), alias}}, out.params};
        if (where_) out.sql += " WHERE " + compile_expression(*where_, ctx);

        if (!order_by_.empty()) {
            out.sql += " ORDER BY ";
            for (std::size_t i = 0; i < order_by_.size(); ++i) {
                if (i) out.sql += ", ";
                out.sql += dialect_.quote_identifier(alias) + "." +
                           dialect_.quote_identifier(order_by_[i].column) +
                           (order_by_[i].ascending ? " ASC" : " DESC");
            }
        }
        if (limit_) out.sql += " LIMIT " + std::to_string(*limit_);
        if (offset_) {
            if (!limit_) out.sql += " LIMIT -1";
            out.sql += " OFFSET " + std::to_string(*offset_);
        }
        out.sql += ";";
        return out;
    }

    [[nodiscard]] std::vector<T> all() const {
        const auto compiled = compile();
        const auto result = executor_.execute(compiled.sql, compiled.params);
        std::vector<T> values;
        values.reserve(result.rows.size());
        for (const auto& row : result.rows) values.push_back(hydrate(row));
        return values;
    }

    [[nodiscard]] std::optional<T> first() const {
        auto copy = *this;
        copy.limit(1);
        auto values = copy.all();
        if (values.empty()) return std::nullopt;
        return std::move(values.front());
    }

private:
    static T hydrate(const Row& row) {
        T value{};
        reflect::for_each_view_column<T>([&]<std::meta::info Member>() {
            const auto found = row.find(reflect::column_name<Member>());
            if (found == row.end()) return;
            using M = reflect::member_type_t<Member>;
            if constexpr (std::same_as<std::remove_cvref_t<M>, Value>) {
                value.[:Member:] = found->second;
            } else {
                value.[:Member:] = from_value<M>(found->second);
            }
        });
        return value;
    }

    DbExecutor& executor_;
    const Dialect& dialect_;
    std::optional<ExprPtr> where_;
    std::vector<ViewOrderSpec> order_by_;
    std::optional<std::size_t> limit_;
    std::optional<std::size_t> offset_;
};

template <reflect::ViewMapped T>
ViewQuery<T> view_query(DbExecutor& executor, const Dialect& dialect) {
    return ViewQuery<T>{executor, dialect};
}

template <reflect::ViewMapped T, typename Context>
requires requires(Context& context) {
    { context.executor() } -> std::same_as<DbExecutor&>;
    { context.dialect() } -> std::same_as<const Dialect&>;
}
ViewQuery<T> view_query(Context& context) {
    return ViewQuery<T>{context.executor(), context.dialect()};
}

} // namespace metal
