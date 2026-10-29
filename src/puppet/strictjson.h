#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace murasaki::puppet::json {

// A strict JSON reader for the files of an untrusted model folder, without Qt. Stricter than the readers the rest of Murasaki uses: RFC 8259 and nothing more (no comments, no
// trailing commas, no leading zeros, no NaN or Infinity), the text must be valid UTF-8 (one leading byte order mark is skipped), a key repeated in one object is an error, a
// number must be finite, a \u escape must not be a lone surrogate, and the depth, the number of values and the length of a string are bounded before anything grows.
enum class Kind { Null, Bool, Number, String, Array, Object };

struct Value {
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<Value> items;                             // Array (unless packed)
    std::vector<double> numbers;                          // Array whose elements are all numbers is kept here, not in items: a motion file holds a million of them
    std::vector<std::pair<std::string, Value>> members;   // Object, in file order

    bool isObject() const { return kind == Kind::Object; }
    bool isArray() const { return kind == Kind::Array; }
    bool isString() const { return kind == Kind::String; }
    bool isNumber() const { return kind == Kind::Number; }
    bool isBool() const { return kind == Kind::Bool; }
    // Elements of an array, packed or not.
    size_t size() const { return items.empty() ? numbers.size() : items.size(); }
    // The member named `key` of an object, or null.
    const Value *find(std::string_view key) const;
};

struct Limits {
    long long maxDepth;
    long long maxValues;
    long long maxStringBytes;
};

struct ParseResult {
    bool ok = false;
    Value value;
    std::string error; // why it failed, with no file content in it
    size_t offset = 0; // where
};

ParseResult parse(std::string_view text, const Limits &limits);

// True when the bytes are well-formed UTF-8 (no overlong forms, no surrogates, nothing above U+10FFFF).
bool validUtf8(std::string_view bytes);

} // namespace murasaki::puppet::json
