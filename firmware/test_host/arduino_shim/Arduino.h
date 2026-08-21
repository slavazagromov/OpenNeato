#ifndef ARDUINO_SHIM_H
#define ARDUINO_SHIM_H

#include <cstdio>
#include <cstdlib>
#include <string>

class String {
public:
    String() = default;
    String(const char *value) : value_(value ? value : "") {}
    String(const std::string& value) : value_(value) {}

    size_t length() const { return value_.length(); }
    char charAt(size_t index) const { return value_[index]; }
    const char *c_str() const { return value_.c_str(); }
    bool isEmpty() const { return value_.empty(); }

    String substring(size_t from, size_t to) const {
        if (from >= value_.length() || to <= from)
            return String("");
        if (to > value_.length())
            to = value_.length();
        return String(value_.substr(from, to - from));
    }

    float toFloat() const { return static_cast<float>(std::atof(value_.c_str())); }

    int indexOf(char character, size_t from = 0) const {
        size_t position = value_.find(character, from);
        return position == std::string::npos ? -1 : static_cast<int>(position);
    }

    int indexOf(const String& text, size_t from = 0) const {
        size_t position = value_.find(text.value_, from);
        return position == std::string::npos ? -1 : static_cast<int>(position);
    }

    String& operator+=(char character) {
        value_ += character;
        return *this;
    }

    String& operator+=(const char *text) {
        value_ += text;
        return *this;
    }

    bool operator==(const char *other) const { return value_ == other; }
    bool operator==(const String& other) const { return value_ == other.value_; }

    friend String operator+(String left, const String& right) {
        left.value_ += right.value_;
        return left;
    }

    friend String operator+(String left, const char *right) {
        left.value_ += right;
        return left;
    }

private:
    std::string value_;
};

#endif // ARDUINO_SHIM_H
