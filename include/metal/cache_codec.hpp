#pragma once

#include "metal/execution.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace metal {

class CacheCodecError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace cache_codec_detail {

inline constexpr std::string_view magic{"MORMC001"};
inline constexpr std::uint64_t max_collection_size = 1'000'000;

class Writer {
public:
    void byte(std::uint8_t value) {
        out_.push_back(static_cast<char>(value));
    }

    void u64(std::uint64_t value) {
        for (unsigned shift = 0; shift != 64; shift += 8) {
            byte(static_cast<std::uint8_t>((value >> shift) & 0xffu));
        }
    }

    void i64(std::int64_t value) {
        u64(std::bit_cast<std::uint64_t>(value));
    }

    void f64(double value) {
        u64(std::bit_cast<std::uint64_t>(value));
    }

    void bytes(std::string_view value) {
        u64(static_cast<std::uint64_t>(value.size()));
        out_.append(value.data(), value.size());
    }

    void blob(const Blob& value) {
        u64(static_cast<std::uint64_t>(value.size()));
        out_.reserve(out_.size() + value.size());
        for (const auto byte_value : value) {
            out_.push_back(static_cast<char>(std::to_integer<unsigned char>(byte_value)));
        }
    }

    void value(const Value& value) {
        std::visit([&](const auto& current) {
            using T = std::remove_cvref_t<decltype(current)>;
            if constexpr (std::same_as<T, std::nullptr_t>) {
                byte(0);
            } else if constexpr (std::same_as<T, std::int64_t>) {
                byte(1);
                i64(current);
            } else if constexpr (std::same_as<T, double>) {
                byte(2);
                f64(current);
            } else if constexpr (std::same_as<T, std::string>) {
                byte(3);
                bytes(current);
            } else if constexpr (std::same_as<T, bool>) {
                byte(4);
                byte(current ? 1 : 0);
            } else if constexpr (std::same_as<T, Blob>) {
                byte(5);
                blob(current);
            }
        }, value);
    }

    [[nodiscard]] std::string finish() && {
        return std::move(out_);
    }

private:
    std::string out_;
};

class Reader {
public:
    explicit Reader(std::string_view input) : input_(input) {}

    [[nodiscard]] bool done() const noexcept {
        return position_ == input_.size();
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return input_.size() - position_;
    }

    std::uint8_t byte() {
        require(1);
        return static_cast<std::uint8_t>(static_cast<unsigned char>(input_[position_++]));
    }

    std::uint64_t u64() {
        require(8);
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift != 64; shift += 8) {
            value |= static_cast<std::uint64_t>(byte()) << shift;
        }
        return value;
    }

    std::int64_t i64() {
        return std::bit_cast<std::int64_t>(u64());
    }

    double f64() {
        return std::bit_cast<double>(u64());
    }

    std::size_t count(std::string_view what) {
        const auto raw = u64();
        if (raw > max_collection_size) {
            throw CacheCodecError(
                "MetalORM: cached " + std::string(what) + " count exceeds safety limit");
        }
        return narrow_size(raw, what);
    }

    std::string bytes(std::string_view what) {
        const auto size = sized_length(what);
        std::string result(input_.substr(position_, size));
        position_ += size;
        return result;
    }

    Blob blob() {
        const auto size = sized_length("blob");
        Blob result;
        result.reserve(size);
        for (std::size_t i = 0; i < size; ++i) {
            result.push_back(std::byte{
                static_cast<unsigned char>(input_[position_ + i])});
        }
        position_ += size;
        return result;
    }

    Value value() {
        switch (byte()) {
            case 0: return Value{nullptr};
            case 1: return Value{i64()};
            case 2: return Value{f64()};
            case 3: return Value{bytes("string")};
            case 4: {
                const auto raw = byte();
                if (raw > 1) {
                    throw CacheCodecError("MetalORM: cached boolean has invalid payload");
                }
                return Value{raw == 1};
            }
            case 5: return Value{blob()};
            default:
                throw CacheCodecError("MetalORM: cached value has unknown type tag");
        }
    }

private:
    void require(std::size_t size) const {
        if (size > remaining()) {
            throw CacheCodecError("MetalORM: truncated cache payload");
        }
    }

    std::size_t narrow_size(std::uint64_t raw, std::string_view what) const {
        if (raw > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            throw CacheCodecError(
                "MetalORM: cached " + std::string(what) + " length is not representable");
        }
        return static_cast<std::size_t>(raw);
    }

    std::size_t sized_length(std::string_view what) {
        const auto size = narrow_size(u64(), what);
        require(size);
        return size;
    }

    std::string_view input_;
    std::size_t position_{};
};

} // namespace cache_codec_detail

[[nodiscard]] inline std::string encode_query_result(const QueryResult& result) {
    cache_codec_detail::Writer writer;
    for (const char c : cache_codec_detail::magic) {
        writer.byte(static_cast<std::uint8_t>(static_cast<unsigned char>(c)));
    }

    writer.u64(static_cast<std::uint64_t>(result.rows.size()));
    for (const auto& row : result.rows) {
        std::vector<const Row::value_type*> fields;
        fields.reserve(row.size());
        for (const auto& field : row) fields.push_back(&field);
        std::sort(fields.begin(), fields.end(), [](const auto* left, const auto* right) {
            return left->first < right->first;
        });

        writer.u64(static_cast<std::uint64_t>(fields.size()));
        for (const auto* field : fields) {
            writer.bytes(field->first);
            writer.value(field->second);
        }
    }
    writer.i64(result.affected_rows);
    writer.i64(result.last_insert_id);
    return std::move(writer).finish();
}

[[nodiscard]] inline QueryResult decode_query_result(std::string_view payload) {
    cache_codec_detail::Reader reader(payload);
    for (const char expected : cache_codec_detail::magic) {
        if (reader.byte() != static_cast<std::uint8_t>(static_cast<unsigned char>(expected))) {
            throw CacheCodecError("MetalORM: cache payload has unsupported format or version");
        }
    }

    QueryResult result;
    const auto row_count = reader.count("row");
    result.rows.reserve(row_count);
    for (std::size_t row_index = 0; row_index < row_count; ++row_index) {
        Row row;
        const auto field_count = reader.count("field");
        row.reserve(field_count);
        for (std::size_t field_index = 0; field_index < field_count; ++field_index) {
            auto name = reader.bytes("column name");
            auto [_, inserted] = row.emplace(std::move(name), reader.value());
            if (!inserted) {
                throw CacheCodecError("MetalORM: cache payload contains a duplicate column name");
            }
        }
        result.rows.push_back(std::move(row));
    }
    result.affected_rows = reader.i64();
    result.last_insert_id = reader.i64();
    if (!reader.done()) {
        throw CacheCodecError("MetalORM: cache payload contains trailing bytes");
    }
    return result;
}

} // namespace metal
