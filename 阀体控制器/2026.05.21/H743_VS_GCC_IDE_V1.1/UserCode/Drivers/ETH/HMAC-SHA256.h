#ifndef __HMAC_SHA256_H
#define __HMAC_SHA256_H

#ifdef __cplusplus
extern "C" {
#endif

#include "string.h"
#include "stdio.h"
#include <stdint.h>

// 密钥协议里的 PSK
#define HMAC_KEY    "1234567890ABCDEF1234567890ABCDEF"

// SHA256 基础算法
#define SHA256_BLOCK_SIZE  64
#define SHA256_DIGEST_SIZE 32

// 下面是完整SHA256实现
#define ROTR(x, n) ((x >> n) | (x << (32 - n)))
#define SHR(x, n)  (x >> n)
#define S0(x) (ROTR(x, 2) ^ ROTR(x,13) ^ ROTR(x,22))
#define S1(x) (ROTR(x, 6) ^ ROTR(x,11) ^ ROTR(x,25))
#define s0(x) (ROTR(x, 7) ^ ROTR(x,18) ^ SHR(x, 3))
#define s1(x) (ROTR(x,17) ^ ROTR(x,19) ^ SHR(x,10))

typedef struct {
    uint32_t h[8];
    uint8_t buf[SHA256_BLOCK_SIZE];
    uint64_t bitlen;
} SHA256_CTX;

void sha256_init(SHA256_CTX *ctx);
void sha256_update(SHA256_CTX *ctx, const uint8_t *data, uint32_t len);
void sha256_final(SHA256_CTX *ctx, uint8_t hash[]);
void HMAC_SHA256_Soft(const uint8_t *key, uint16_t key_len,
                      const uint8_t *data, uint16_t data_len,
                      uint8_t *out);






#ifdef __cplusplus
}
#endif

#endif

