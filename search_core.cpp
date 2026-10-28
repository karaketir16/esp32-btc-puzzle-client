#include "search_core.h"
#include "ripemd160.h"
#include <mbedtls/sha256.h>
#include <cstring>
namespace btc {

std::string hex(const uint8_t *data, size_t length) {
  static const char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(length * 2);
  for (size_t i = 0; i < length; ++i) {
    out += hex[data[i] >> 4];
    out += hex[data[i] & 0x0f];
  }
  return out;
}

bool decodeAddress(const char *text, uint8_t hash160[20]) {
  if (!text)
    return false;
  const std::string address(text);
  static const char alphabet[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
  uint8_t decoded[25] = {};
  for (size_t pos = 0; pos < address.length(); ++pos) {
    const char *digitPtr = strchr(alphabet, address[pos]);
    if (!digitPtr)
      return false;
    uint16_t carry = digitPtr - alphabet;
    for (int i = (int)sizeof(decoded) - 1; i >= 0; --i) {
      const uint16_t value = (uint16_t)decoded[i] * 58U + carry;
      decoded[i] = value & 0xff;
      carry = value >> 8;
    }
    if (carry)
      return false;
  }
  if (decoded[0] != 0x00)
    return false;
  uint8_t firstHash[32], checksum[32];
  if (mbedtls_sha256(decoded, 21, firstHash, 0) != 0 ||
      mbedtls_sha256(firstHash, sizeof(firstHash), checksum, 0) != 0 ||
      memcmp(checksum, decoded + 21, 4) != 0)
    return false;
  memcpy(hash160, decoded + 1, 20);
  return true;
}

static std::string base58Encode(const uint8_t *input, size_t length) {
  static const char alphabet[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
  size_t zeros = 0;
  while (zeros < length && input[zeros] == 0)
    ++zeros;
  uint8_t work[80];
  if (length > sizeof(work))
    return "";
  memcpy(work, input, length);
  char encoded[120];
  size_t out = sizeof(encoded);
  size_t start = zeros;
  while (start < length) {
    unsigned remainder = 0;
    for (size_t i = start; i < length; ++i) {
      unsigned value = (remainder << 8) | work[i];
      work[i] = value / 58;
      remainder = value % 58;
    }
    encoded[--out] = alphabet[remainder];
    while (start < length && work[start] == 0)
      ++start;
  }
  while (out < sizeof(encoded) && encoded[out] == alphabet[0])
    ++out;
  while (zeros--)
    encoded[--out] = alphabet[0];
  return std::string(encoded + out, sizeof(encoded) - out);
}

static std::string base58Check(const uint8_t *payload, size_t payloadLength) {
  uint8_t buffer[80];
  if (payloadLength + 4 > sizeof(buffer))
    return "";
  memcpy(buffer, payload, payloadLength);
  uint8_t firstHash[32], secondHash[32];
  if (mbedtls_sha256(payload, payloadLength, firstHash, 0) != 0 ||
      mbedtls_sha256(firstHash, sizeof(firstHash), secondHash, 0) != 0)
    return "";
  memcpy(buffer + payloadLength, secondHash, 4);
  return base58Encode(buffer, payloadLength + 4);
}

static bool hash160ForPublicKey(const uint8_t *pubkey, size_t pubkeyLength, uint8_t hash160[20]) {
  uint8_t sha[32];
  if (mbedtls_sha256(pubkey, pubkeyLength, sha, 0) != 0)
    return false;
  CRIPEMD160 ripemd;
  ripemd.Write(sha, sizeof(sha));
  ripemd.Finalize(hash160);
  return true;
}

std::string wif(const uint8_t key[32], bool compressed) {
  uint8_t payload[34];
  payload[0] = 0x80;
  memcpy(payload + 1, key, 32);
  size_t length = 33;
  if (compressed)
    payload[length++] = 0x01;
  return base58Check(payload, length);
}

bool randomKey(uint16_t bits, uint8_t key[32], Rng rng, void *context) {
  if (bits == 0 || bits > 160)
    return false;
  if (!rng || rng(context, key, 32) != 0)
    return false;
  const uint16_t bytes = (bits + 7) / 8;
  const uint16_t first = 32 - bytes;
  for (uint16_t i = 0; i < first; ++i)
    key[i] = 0;
  const uint8_t highBits = bits % 8;
  if (highBits) {
    const uint8_t mask = (1U << highBits) - 1;
    key[first] &= mask;
    key[first] |= 1U << (highBits - 1);
  } else {
    key[first] |= 0x80;
  }
  return true;
}

bool maximumKey(uint16_t bits, const uint8_t key[32]) {
  if (bits < 1 || bits > 160)
    return false;
  for (uint16_t bit = 0; bit < bits; ++bit) {
    const uint16_t byteIndex = 31 - bit / 8;
    if ((key[byteIndex] & (1U << (bit % 8))) == 0)
      return false;
  }
  for (uint16_t bit = bits; bit < 256; ++bit) {
    const uint16_t byteIndex = 31 - bit / 8;
    if ((key[byteIndex] & (1U << (bit % 8))) != 0)
      return false;
  }
  return true;
}

int Crypto::normalizeSmallModulo(mbedtls_mpi *value, const mbedtls_mpi *modulus) {
  // Call sites bound values to (-2p, 2p). Use separate outputs and bounded
  // corrections so a corrupted MPI value cannot trap the search in a loop.
  if (mbedtls_mpi_cmp_int(value, 0) < 0) {
    int result = mbedtls_mpi_add_mpi(&incrementTemp, value, modulus);
    if (result != 0 || (result = mbedtls_mpi_copy(value, &incrementTemp)) != 0)
      return result;
  }
  if (mbedtls_mpi_cmp_int(value, 0) < 0) {
    int result = mbedtls_mpi_add_mpi(&incrementTemp, value, modulus);
    if (result != 0 || (result = mbedtls_mpi_copy(value, &incrementTemp)) != 0)
      return result;
  }
  if (mbedtls_mpi_cmp_mpi(value, modulus) >= 0) {
    int result = mbedtls_mpi_sub_mpi(&incrementTemp, value, modulus);
    if (result != 0 || (result = mbedtls_mpi_copy(value, &incrementTemp)) != 0)
      return result;
  }
  return (mbedtls_mpi_cmp_int(value, 0) < 0 || mbedtls_mpi_cmp_mpi(value, modulus) >= 0)
             ? MBEDTLS_ERR_MPI_BAD_INPUT_DATA
             : 0;
}

bool Crypto::increment() {
  mbedtls_mpi *x1 = &publicPoint.MBEDTLS_PRIVATE(X);
  mbedtls_mpi *y1 = &publicPoint.MBEDTLS_PRIVATE(Y);
  const mbedtls_mpi *gx = &secpGroup.G.MBEDTLS_PRIVATE(X);
  const mbedtls_mpi *gy = &secpGroup.G.MBEDTLS_PRIVATE(Y);
  const mbedtls_mpi *prime = &secpGroup.P;
  const bool sameX = mbedtls_mpi_cmp_mpi(x1, gx) == 0;

  if (sameX) {
    if (mbedtls_mpi_cmp_mpi(y1, gy) != 0 || mbedtls_mpi_cmp_int(y1, 0) == 0) {
      mbedtls_mpi_lset(&publicPoint.MBEDTLS_PRIVATE(Z), 0);
      return false;
    }
    // Tangent slope for 2P: lambda = 3*x^2 / (2*y) mod p.
    if (mbedtls_mpi_mul_mpi(&incrementTemp, x1, x1) != 0 ||
        mbedtls_mpi_mul_int(&incrementNumerator, &incrementTemp, 3) != 0 ||
        mbedtls_mpi_mod_mpi(&incrementNumerator, &incrementNumerator, prime) != 0 ||
        mbedtls_mpi_mul_int(&incrementDenominator, y1, 2) != 0 ||
        mbedtls_mpi_mod_mpi(&incrementDenominator, &incrementDenominator, prime) != 0)
      return false;
    if (mbedtls_mpi_inv_mod(&incrementInverse, &incrementDenominator, prime) != 0 ||
        mbedtls_mpi_mul_mpi(&incrementSlope, &incrementNumerator, &incrementInverse) != 0 ||
        mbedtls_mpi_mod_mpi(&incrementSlope, &incrementSlope, prime) != 0 ||
        mbedtls_mpi_mul_mpi(&incrementX, &incrementSlope, &incrementSlope) != 0 ||
        mbedtls_mpi_mod_mpi(&incrementX, &incrementX, prime) != 0 ||
        mbedtls_mpi_mul_int(&incrementTemp, x1, 2) != 0 ||
        mbedtls_mpi_sub_mpi(&incrementX, &incrementX, &incrementTemp) != 0 ||
        mbedtls_mpi_mod_mpi(&incrementX, &incrementX, prime) != 0 ||
        mbedtls_mpi_sub_mpi(&incrementY, x1, &incrementX) != 0 ||
        mbedtls_mpi_mul_mpi(&incrementY, &incrementY, &incrementSlope) != 0 ||
        mbedtls_mpi_mod_mpi(&incrementY, &incrementY, prime) != 0 ||
        mbedtls_mpi_sub_mpi(&incrementY, &incrementY, y1) != 0 ||
        mbedtls_mpi_mod_mpi(&incrementY, &incrementY, prime) != 0)
      return false;
  } else {
    // Chord slope for P+G: lambda = (Gy-y) / (Gx-x) mod p.
    if (mbedtls_mpi_sub_mpi(&incrementNumerator, gy, y1) != 0 ||
        mbedtls_mpi_sub_mpi(&incrementDenominator, gx, x1) != 0 ||
        normalizeSmallModulo(&incrementNumerator, prime) != 0 ||
        normalizeSmallModulo(&incrementDenominator, prime) != 0)
      return false;
    if (mbedtls_mpi_inv_mod(&incrementInverse, &incrementDenominator, prime) != 0 ||
        mbedtls_mpi_mul_mpi(&incrementSlope, &incrementNumerator, &incrementInverse) != 0 ||
        mbedtls_mpi_mod_mpi(&incrementSlope, &incrementSlope, prime) != 0 ||
        mbedtls_mpi_mul_mpi(&incrementX, &incrementSlope, &incrementSlope) != 0 ||
        mbedtls_mpi_mod_mpi(&incrementX, &incrementX, prime) != 0 ||
        mbedtls_mpi_sub_mpi(&incrementX, &incrementX, x1) != 0 ||
        mbedtls_mpi_sub_mpi(&incrementX, &incrementX, gx) != 0 ||
        normalizeSmallModulo(&incrementX, prime) != 0 ||
        mbedtls_mpi_sub_mpi(&incrementY, x1, &incrementX) != 0 ||
        normalizeSmallModulo(&incrementY, prime) != 0 ||
        mbedtls_mpi_mul_mpi(&incrementY, &incrementY, &incrementSlope) != 0 ||
        mbedtls_mpi_mod_mpi(&incrementY, &incrementY, prime) != 0 ||
        mbedtls_mpi_sub_mpi(&incrementY, &incrementY, y1) != 0 ||
        normalizeSmallModulo(&incrementY, prime) != 0)
      return false;
  }

  if (mbedtls_mpi_copy(x1, &incrementX) != 0 || mbedtls_mpi_copy(y1, &incrementY) != 0 ||
      mbedtls_mpi_lset(&publicPoint.MBEDTLS_PRIVATE(Z), 1) != 0)
    return false;
  return true;
}

bool unhex(const std::string &text, uint8_t *out, size_t length) {
  if (text.size() != length * 2)
    return false;
  auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < length; ++i) {
    int a = digit(text[2 * i]), b = digit(text[2 * i + 1]);
    if (a < 0 || b < 0)
      return false;
    out[i] = uint8_t((a << 4) | b);
  }
  return true;
}
std::string sha256Hex(const std::string &data) {
  uint8_t digest[32];
  if (mbedtls_sha256(reinterpret_cast<const uint8_t *>(data.data()), data.size(), digest, 0) != 0)
    return {};
  return hex(digest, 32);
}
Crypto::Crypto() {
  mbedtls_ecp_group_init(&secpGroup);
  mbedtls_ecp_point_init(&publicPoint);
  for (auto p : {&privateScalar, &incrementSlope, &incrementNumerator, &incrementDenominator,
                 &incrementInverse, &incrementX, &incrementY, &incrementTemp})
    mbedtls_mpi_init(p);
}
Crypto::~Crypto() {
  for (auto p : {&privateScalar, &incrementSlope, &incrementNumerator, &incrementDenominator,
                 &incrementInverse, &incrementX, &incrementY, &incrementTemp})
    mbedtls_mpi_free(p);
  mbedtls_ecp_point_free(&publicPoint);
  mbedtls_ecp_group_free(&secpGroup);
}
bool Crypto::begin(Rng rng, void *context) {
  rng_ = rng;
  context_ = context;
  return rng_ && mbedtls_ecp_group_load(&secpGroup, MBEDTLS_ECP_DP_SECP256K1) == 0;
}
bool Crypto::derive(const uint8_t key[32]) {
  return rng_ && mbedtls_mpi_read_binary(&privateScalar, key, 32) == 0 &&
         mbedtls_ecp_mul(&secpGroup, &publicPoint, &privateScalar, &secpGroup.G, rng_, context_) ==
             0;
}
bool Crypto::serialize(bool compressed, uint8_t output[65], size_t &length) const {
  return mbedtls_ecp_point_write_binary(&secpGroup, &publicPoint,
                                        compressed ? MBEDTLS_ECP_PF_COMPRESSED
                                                   : MBEDTLS_ECP_PF_UNCOMPRESSED,
                                        &length, output, 65) == 0;
}
Check Crypto::checkCurrent(const uint8_t target[20], bool &compressed) {
  uint8_t encoded[65], hash[20];
  size_t length = 0;
  for (bool type : {true, false}) {
    if (!serialize(type, encoded, length) || !hash160ForPublicKey(encoded, length, hash))
      return Check::Error;
    if (std::memcmp(hash, target, 20) == 0) {
      compressed = type;
      return Check::Match;
    }
  }
  return Check::Miss;
}
Check Crypto::check(const uint8_t key[32], const uint8_t target[20], bool &compressed) {
  return derive(key) ? checkCurrent(target, compressed) : Check::Error;
}
} // namespace btc
