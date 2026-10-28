#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <mbedtls/ecp.h>
namespace btc {
using Rng = int (*)(void *, unsigned char *, size_t);
enum class Check { Error, Miss, Match };
bool randomKey(uint16_t bits, uint8_t key[32], Rng rng, void *context = nullptr);
bool maximumKey(uint16_t bits, const uint8_t key[32]);
bool decodeAddress(const char *address, uint8_t hash160[20]);
std::string hex(const uint8_t *data, size_t length);
bool unhex(const std::string &text, uint8_t *out, size_t length);
std::string wif(const uint8_t key[32], bool compressed);
std::string sha256Hex(const std::string &data);
class Crypto {
public:
  Crypto();
  ~Crypto();
  Crypto(const Crypto &) = delete;
  Crypto &operator=(const Crypto &) = delete;
  bool begin(Rng rng, void *context = nullptr);
  bool derive(const uint8_t key[32]);
  bool increment();
  Check check(const uint8_t key[32], const uint8_t target[20], bool &compressed);
  Check checkCurrent(const uint8_t target[20], bool &compressed);
  bool serialize(bool compressed, uint8_t output[65], size_t &length) const;

private:
  int normalizeSmallModulo(mbedtls_mpi *value, const mbedtls_mpi *modulus);
  mbedtls_ecp_group secpGroup;
  mbedtls_ecp_point publicPoint;
  mbedtls_mpi privateScalar, incrementSlope, incrementNumerator, incrementDenominator;
  mbedtls_mpi incrementInverse, incrementX, incrementY, incrementTemp;
  Rng rng_ = nullptr;
  void *context_ = nullptr;
};
} // namespace btc
