#include "HMAC-SHA256.h"

static const uint32_t K[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6551,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391e6968,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256_transform(SHA256_CTX *ctx, const uint8_t data[]);

//=====================================================================
// HMAC‑SHA256 封装
//=====================================================================
void HMAC_SHA256_Soft(const uint8_t *key, uint16_t key_len,
                      const uint8_t *data, uint16_t data_len,
                      uint8_t *out)
{
    uint8_t k_ipad[64], k_opad[64];
    uint8_t tmp_hash[32];
    SHA256_CTX ctx;
    int i;

    // 密钥填充
    memset(k_ipad, 0x36, 64);
    memset(k_opad, 0x5C, 64);
    for(i=0; i<key_len && i<64; i++){
        k_ipad[i] ^= key[i];
        k_opad[i] ^= key[i];
    }

    // 内层哈希
    sha256_init(&ctx);
    sha256_update(&ctx, k_ipad, 64);
    sha256_update(&ctx, data, data_len);
    sha256_final(&ctx, tmp_hash);

    // 外层哈希
    sha256_init(&ctx);
    sha256_update(&ctx, k_opad, 64);
    sha256_update(&ctx, tmp_hash, 32);
    sha256_final(&ctx, out);
}

static void sha256_transform(SHA256_CTX *ctx, const uint8_t data[])
{
    uint32_t a,b,c,d,e,f,g,h,t1,t2,w[64];
    int i,j;

    for (i = 0, j = 0; i < 16; i++, j += 4)
        w[i] = (data[j] << 24) | (data[j+1] << 16) | (data[j+2] << 8) | data[j+3];

    for (i = 16; i < 64; i++)
        w[i] = s1(w[i-2]) + w[i-7] + s0(w[i-15]) + w[i-16];

    a = ctx->h[0]; b = ctx->h[1]; c = ctx->h[2]; d = ctx->h[3];
    e = ctx->h[4]; f = ctx->h[5]; g = ctx->h[6]; h = ctx->h[7];

    for (i = 0; i < 64; i++) {
        t1 = h + S1(e) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        t2 = S0(a) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

void sha256_init(SHA256_CTX *ctx)
{
    ctx->h[0] = 0x6a09e667; ctx->h[1] = 0xbb67ae85;
    ctx->h[2] = 0x3c6ef372; ctx->h[3] = 0xa54ff53a;
    ctx->h[4] = 0x510e527f; ctx->h[5] = 0x9b05688c;
    ctx->h[6] = 0x1f83d9ab; ctx->h[7] = 0x5be0cd19;
    ctx->bitlen = 0;
    memset(ctx->buf, 0, 64);
}

void sha256_update(SHA256_CTX *ctx, const uint8_t *data, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++) {
        ctx->buf[ctx->bitlen%64]=data[i];
        ctx->bitlen++;
        if ((ctx->bitlen % 64) == 0)
        {
            sha256_transform(ctx, ctx->buf);
        }
    }
}

void sha256_final(SHA256_CTX *ctx, uint8_t hash[])
{
    uint64_t i = ctx->bitlen<<3;
    uint32_t j;
    ctx->buf[(ctx->bitlen)%64]=0x80;
    if(((ctx->bitlen)%64)>55){
        sha256_transform(ctx,ctx->buf);
        memset(ctx->buf,0,56);
    }
    for(j=0;j<8;j++) 
    {
        ctx->buf[56+j] = (i >> (56-j*8)) & 0xFF;
    }
    sha256_transform(ctx,ctx->buf);
    for(j=0;j<8;j++){
        hash[j * 4]     = (ctx->h[j] >> 24) & 0xFF;
        hash[j * 4 + 1] = (ctx->h[j] >> 16) & 0xFF;
        hash[j * 4 + 2] = (ctx->h[j] >> 8) & 0xFF;
        hash[j * 4 + 3] = ctx->h[j] & 0xFF;
    }
}


