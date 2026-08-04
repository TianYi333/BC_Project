#ifndef __HMAC_SHA256_H
#define __HMAC_SHA256_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

#define SHA256_BLOCKLEN  64ul
#define SHA256_DIGESTLEN 32ul
#define SHA256_DIGESTINT 8ul

/* SHA-256 上下文结构体 */
typedef struct sha256_ctx_t
{
    uint64_t len;                       // 已处理的消息长度
    uint32_t h[SHA256_DIGESTINT];       // 哈希状态
    uint8_t buf[SHA256_BLOCKLEN];       // 消息块缓冲区
} SHA256_CTX;

void sha256_init(SHA256_CTX *ctx);
void sha256_update(SHA256_CTX *ctx, const uint8_t *m, uint32_t mlen);
void sha256_final(SHA256_CTX *ctx, uint8_t *md);

/* HMAC-SHA256 上下文结构体 */
typedef struct hmac_sha256_ctx_t
{
    uint8_t buf[SHA256_BLOCKLEN];               // 密钥块缓冲区，初始化后不再需要
    uint32_t h_inner[SHA256_DIGESTINT];
    uint32_t h_outer[SHA256_DIGESTINT];
    SHA256_CTX sha;
} HMAC_SHA256_CTX;

void hmac_sha256_init(HMAC_SHA256_CTX *hmac, const uint8_t *key, uint32_t keylen);
void hmac_sha256_update(HMAC_SHA256_CTX *hmac, const uint8_t *m, uint32_t mlen);
void hmac_sha256_final(HMAC_SHA256_CTX *hmac, uint8_t *md);

#ifdef __cplusplus
}
#endif

#endif