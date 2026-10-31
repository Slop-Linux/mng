/* Linux: CommonCrypto SHA-256 on OpenSSL's libcrypto. */
#pragma once
#include <openssl/sha.h>
typedef unsigned CC_LONG;
typedef SHA256_CTX CC_SHA256_CTX;
#define CC_SHA256_DIGEST_LENGTH SHA256_DIGEST_LENGTH
#define CC_SHA256(d, n, md) SHA256((const unsigned char *)(d), (n), (md))
#define CC_SHA256_Init SHA256_Init
#define CC_SHA256_Update SHA256_Update
#define CC_SHA256_Final SHA256_Final
