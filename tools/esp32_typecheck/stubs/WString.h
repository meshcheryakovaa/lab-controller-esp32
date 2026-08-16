// Declaration-only Arduino String, good enough to type-check firmware that
// touches the web server.  Backed by std::string; every member here exists on
// the real thing with the same meaning.
#pragma once

#include <cstdio>
#include <cstring>
#include <string>

class String {
 public:
  String() = default;
  String(const char* text) : s_(text != nullptr ? text : "") {}
  String(const std::string& text) : s_(text) {}
  explicit String(char c) : s_(1, c) {}
  explicit String(int value) : s_(std::to_string(value)) {}
  explicit String(unsigned value) : s_(std::to_string(value)) {}
  explicit String(long value) : s_(std::to_string(value)) {}
  explicit String(unsigned long value) : s_(std::to_string(value)) {}
  explicit String(double value, int = 2) : s_(std::to_string(value)) {}

  const char* c_str() const { return s_.c_str(); }
  unsigned length() const { return static_cast<unsigned>(s_.size()); }
  bool isEmpty() const { return s_.empty(); }
  void clear() { s_.clear(); }
  void reserve(unsigned n) { s_.reserve(n); }

  int indexOf(char c, unsigned from = 0) const {
    const std::size_t at = s_.find(c, from);
    return at == std::string::npos ? -1 : static_cast<int>(at);
  }
  int indexOf(const String& other, unsigned from = 0) const {
    const std::size_t at = s_.find(other.s_, from);
    return at == std::string::npos ? -1 : static_cast<int>(at);
  }
  int lastIndexOf(char c) const {
    const std::size_t at = s_.rfind(c);
    return at == std::string::npos ? -1 : static_cast<int>(at);
  }
  String substring(unsigned from) const { return String(s_.substr(from)); }
  String substring(unsigned from, unsigned to) const {
    return String(s_.substr(from, to - from));
  }
  bool startsWith(const String& p) const { return s_.rfind(p.s_, 0) == 0; }
  bool endsWith(const String& p) const {
    return s_.size() >= p.s_.size() &&
           s_.compare(s_.size() - p.s_.size(), p.s_.size(), p.s_) == 0;
  }
  bool equals(const String& other) const { return s_ == other.s_; }
  bool equalsIgnoreCase(const String& other) const {
    if (s_.size() != other.s_.size()) return false;
    for (std::size_t i = 0; i < s_.size(); ++i) {
      if (std::tolower(s_[i]) != std::tolower(other.s_[i])) return false;
    }
    return true;
  }
  void toLowerCase() {
    for (char& c : s_) c = static_cast<char>(std::tolower(c));
  }
  int toInt() const { return std::atoi(s_.c_str()); }
  float toFloat() const { return static_cast<float>(std::atof(s_.c_str())); }
  double toDouble() const { return std::atof(s_.c_str()); }
  char charAt(unsigned i) const { return s_[i]; }
  char operator[](unsigned i) const { return s_[i]; }
  operator const std::string&() const { return s_; }

  bool concat(const String& other) {
    s_ += other.s_;
    return true;
  }
  bool concat(const char* other) {
    if (other != nullptr) s_ += other;
    return true;
  }
  bool concat(char c) {
    s_ += c;
    return true;
  }
  void getBytes(unsigned char* buffer, unsigned length) const {
    std::snprintf(reinterpret_cast<char*>(buffer), length, "%s", s_.c_str());
  }
  void remove(unsigned index) { s_.erase(index); }
  void remove(unsigned index, unsigned count) { s_.erase(index, count); }
  void trim() {}

  String& operator+=(const String& other) {
    s_ += other.s_;
    return *this;
  }
  String& operator+=(const char* other) {
    if (other != nullptr) s_ += other;
    return *this;
  }
  String& operator+=(char c) {
    s_ += c;
    return *this;
  }

  friend String operator+(String lhs, const String& rhs) { return lhs += rhs; }
  friend String operator+(String lhs, const char* rhs) { return lhs += rhs; }
  friend String operator+(const char* lhs, const String& rhs) {
    return String(lhs) += rhs;
  }
  friend bool operator==(const String& a, const String& b) { return a.s_ == b.s_; }
  friend bool operator!=(const String& a, const String& b) { return a.s_ != b.s_; }
  friend bool operator<(const String& a, const String& b) { return a.s_ < b.s_; }

 private:
  std::string s_;
};

extern const String emptyString;
