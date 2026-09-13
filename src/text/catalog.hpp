/// @file catalog.hpp
/// @brief Compile-time reader for text catalogs: a JSON object whose values
/// are strings or arrays of string fragments, concatenated without separators.
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pigpen::text {
namespace detail {

using Fields = std::vector<std::pair<std::string, std::string>>;

/// Only the text-catalog subset of JSON is accepted. No runtime parsing is
/// used by the application; constexpr also lets tests exercise diagnostics.
class Reader {
public:
  explicit constexpr Reader(std::string_view source) : source_(source) {}

  [[nodiscard]] constexpr Fields read() {
    Fields fields;
    expect('{');
    if (!take('}')) {
      do {
        auto key = string();
        for (const auto &field : fields) {
          if (field.first == key) {
            throw std::invalid_argument{"duplicate text catalog key"};
          }
        }
        expect(':');
        std::string value;
        if (take('[')) {
          if (!take(']')) {
            do {
              value += string();
            } while (take(','));
            expect(']');
          }
        } else {
          value = string();
        }
        fields.emplace_back(std::move(key), std::move(value));
      } while (take(','));
      expect('}');
    }
    whitespace();
    if (position_ != source_.size()) {
      throw std::invalid_argument{"trailing text after JSON catalog"};
    }
    return fields;
  }

private:
  constexpr void whitespace() {
    while (position_ < source_.size() &&
           (source_[position_] == ' ' || source_[position_] == '\n' ||
            source_[position_] == '\r' || source_[position_] == '\t')) {
      ++position_;
    }
  }

  constexpr bool take(char token) {
    whitespace();
    if (position_ < source_.size() && source_[position_] == token) {
      ++position_;
      return true;
    }
    return false;
  }

  constexpr void expect(char token) {
    if (!take(token)) {
      throw std::invalid_argument{"unexpected token in JSON text catalog"};
    }
  }

  constexpr unsigned char next() {
    if (position_ == source_.size()) {
      throw std::invalid_argument{"truncated JSON text catalog"};
    }
    return static_cast<unsigned char>(source_[position_++]);
  }

  constexpr std::uint32_t hex_quad() {
    std::uint32_t value{};
    for (int i = 0; i < 4; ++i) {
      const auto c = next();
      value *= 16;
      if (c >= '0' && c <= '9') {
        value += c - '0';
      } else if (c >= 'a' && c <= 'f') {
        value += c - 'a' + 10U;
      } else if (c >= 'A' && c <= 'F') {
        value += c - 'A' + 10U;
      } else {
        throw std::invalid_argument{"invalid JSON Unicode escape"};
      }
    }
    return value;
  }

  static constexpr void append_byte(std::string &out, std::uint32_t byte) {
    out += std::bit_cast<char>(static_cast<unsigned char>(byte));
  }

  static constexpr void append_unicode(std::string &out, std::uint32_t cp) {
    if (cp <= 0x7f) {
      append_byte(out, cp);
    } else if (cp <= 0x7ff) {
      append_byte(out, 0xc0U | (cp >> 6));
      append_byte(out, 0x80U | (cp & 0x3fU));
    } else if (cp <= 0xffff) {
      append_byte(out, 0xe0U | (cp >> 12));
      append_byte(out, 0x80U | ((cp >> 6) & 0x3fU));
      append_byte(out, 0x80U | (cp & 0x3fU));
    } else {
      append_byte(out, 0xf0U | (cp >> 18));
      append_byte(out, 0x80U | ((cp >> 12) & 0x3fU));
      append_byte(out, 0x80U | ((cp >> 6) & 0x3fU));
      append_byte(out, 0x80U | (cp & 0x3fU));
    }
  }

  constexpr void escaped(std::string &out) {
    switch (next()) {
    case '"':
      out += '"';
      break;
    case '\\':
      out += '\\';
      break;
    case '/':
      out += '/';
      break;
    case 'b':
      out += '\b';
      break;
    case 'f':
      out += '\f';
      break;
    case 'n':
      out += '\n';
      break;
    case 'r':
      out += '\r';
      break;
    case 't':
      out += '\t';
      break;
    case 'u': {
      auto cp = hex_quad();
      if (cp >= 0xd800 && cp <= 0xdbff) {
        if (next() != '\\' || next() != 'u') {
          throw std::invalid_argument{"missing JSON low surrogate"};
        }
        const auto low = hex_quad();
        if (low < 0xdc00 || low > 0xdfff) {
          throw std::invalid_argument{"invalid JSON low surrogate"};
        }
        cp = 0x10000U + ((cp - 0xd800U) << 10) + low - 0xdc00U;
      } else if (cp >= 0xdc00 && cp <= 0xdfff) {
        throw std::invalid_argument{"unpaired JSON low surrogate"};
      }
      append_unicode(out, cp);
      break;
    }
    default:
      throw std::invalid_argument{"invalid JSON string escape"};
    }
  }

  constexpr std::string string() {
    expect('"');
    std::string out;
    for (;;) {
      const auto c = next();
      if (c == '"') {
        return out;
      }
      if (c == '\\') {
        escaped(out);
      } else if (c < 0x20) {
        throw std::invalid_argument{"unescaped JSON control character"};
      } else if (c < 0x80) {
        append_byte(out, c);
      } else {
        // Validate raw UTF-8 too, including overlong encodings and surrogates.
        const int count = c >= 0xc2 && c <= 0xdf   ? 1
                          : c >= 0xe0 && c <= 0xef ? 2
                          : c >= 0xf0 && c <= 0xf4 ? 3
                                                   : 0;
        if (count == 0) {
          throw std::invalid_argument{"invalid UTF-8 in text catalog"};
        }
        std::uint32_t cp = c & (0x7fU >> count);
        for (int i = 0; i < count; ++i) {
          const auto continuation = next();
          if ((continuation & 0xc0U) != 0x80U) {
            throw std::invalid_argument{"invalid UTF-8 continuation"};
          }
          cp = (cp << 6) | (continuation & 0x3fU);
        }
        if ((count == 1 && cp < 0x80) || (count == 2 && cp < 0x800) ||
            (count == 3 && cp < 0x10000) || cp > 0x10ffff ||
            (cp >= 0xd800 && cp <= 0xdfff)) {
          throw std::invalid_argument{"invalid UTF-8 code point"};
        }
        append_unicode(out, cp);
      }
    }
  }

  std::string_view source_;
  std::size_t position_{};
};

template <auto &Source> constexpr Fields read_embedded() {
  std::string source;
  for (const auto byte : Source) {
    source += std::bit_cast<char>(byte);
  }
  return Reader{source}.read();
}

} // namespace detail

/// Owns decoded text and offsets, so copying the catalog never dangles views.
/// Each field is null-terminated as well as length-delimited.
template <std::size_t Size, std::size_t Count> struct Catalog {
  struct Entry {
    std::size_t key_offset{}, key_size{}, value_offset{}, value_size{};
  };
  std::array<char, Size> data{};
  std::array<Entry, Count> entries{};

  [[nodiscard]] constexpr std::string_view get(std::string_view key) const {
    for (const auto &entry : entries) {
      if (std::string_view{data.data() + entry.key_offset, entry.key_size} ==
          key) {
        return {data.data() + entry.value_offset, entry.value_size};
      }
    }
    throw std::invalid_argument{"missing text catalog key"};
  }
};

/// Decode #embed bytes during constant evaluation. Temporary allocations are
/// released before returning; the resulting catalog contains only fixed arrays.
template <auto &Source> consteval auto make_catalog() {
  constexpr auto count = detail::read_embedded<Source>().size();
  Catalog<sizeof(Source), count> catalog;
  std::size_t offset{};
  std::size_t index{};
  for (const auto &[key, value] : detail::read_embedded<Source>()) {
    auto &entry = catalog.entries[index++];
    entry.key_offset = offset;
    entry.key_size = key.size();
    for (const char c : key) {
      catalog.data.at(offset++) = c;
    }
    catalog.data.at(offset++) = '\0';
    entry.value_offset = offset;
    entry.value_size = value.size();
    for (const char c : value) {
      catalog.data.at(offset++) = c;
    }
    catalog.data.at(offset++) = '\0';
  }
  return catalog;
}

} // namespace pigpen::text
