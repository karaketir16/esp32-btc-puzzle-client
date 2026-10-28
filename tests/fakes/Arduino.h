#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <sstream>
#include <iomanip>
class String : public std::string {
public:
  using std::string::string;
  using std::string::operator=;
  String(const std::string &s) : std::string(s) {}
  String(int n) : std::string(std::to_string(n)) {}
  String(unsigned n) : std::string(std::to_string(n)) {}
  String(unsigned long n) : std::string(std::to_string(n)) {}
  String(unsigned long long n) : std::string(std::to_string(n)) {}
  String(double n, int decimals) : std::string(format(n, decimals)) {}
  static std::string format(double n, int d) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(d) << n;
    return os.str();
  }
  bool isEmpty() const { return empty(); }
  bool startsWith(const char *s) const { return rfind(s, 0) == 0; }
  void trim() {
    auto p = find_first_not_of(" \t\n\r");
    if (p == npos) {
      clear();
      return;
    }
    *this = substr(p, find_last_not_of(" \t\n\r") - p + 1);
  }
  int indexOf(char c) const {
    auto p = find(c);
    return p == npos ? -1 : int(p);
  }
  String substring(size_t a) const { return substr(a); }
  String substring(size_t a, size_t b) const { return substr(a, b - a); }
  void toLowerCase() {
    for (char &c : *this)
      c = std::tolower(static_cast<unsigned char>(c));
  }
};
extern uint32_t fakeMillis;
inline uint32_t millis() { return fakeMillis; }
inline void delay(uint32_t ms) { fakeMillis += ms; }
void esp_fill_random(void *, size_t);
struct FakeSerial {
  std::vector<String> lines;
  void begin(int) {}
  void println(const String &s) { lines.push_back(s); }
  int available() { return 0; }
  int read() { return -1; }
};
extern FakeSerial Serial;
