// Emscripten/WASM stub for wolfssl/ssl.h.
//
// IMPORTANT: This is a *compile-only* stub. Bodies live in
// wolfssl_stub.cpp and return failure for every call. SELF/EDAT
// decryption (which depends on real crypto) will fail at runtime,
// blocking BootGame.
//
// To make BootGame actually run a PS3 ELF you need REAL crypto. Two
// realistic paths:
//   1. Port wolfssl proper to emcc (most of it should compile, the
//      networking layer is the wrinkle).
//   2. Substitute Emscripten's mbedtls port for the AES/ECDSA primitives
//      rpcs3 actually uses for SELF decryption (a few hundred lines in
//      Emu/Crypto/).
//
// Tracked in plan-poc-ps3-webgpu.md §3 (next blockers, source-level).

#pragma once

#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WOLFSSL     WOLFSSL;
typedef struct WOLFSSL_CTX WOLFSSL_CTX;
typedef struct WOLFSSL_METHOD WOLFSSL_METHOD;

#define WOLFSSL_SUCCESS    1
#define WOLFSSL_FAILURE    0
#define SSL_VERIFY_NONE    0
#define WOLFSSL_MAX_ERROR_SZ 80

int  wolfSSL_Init(void);
int  wolfSSL_Cleanup(void);

WOLFSSL_METHOD* wolfTLSv1_2_client_method(void);

WOLFSSL_CTX* wolfSSL_CTX_new(WOLFSSL_METHOD* method);
void         wolfSSL_CTX_free(WOLFSSL_CTX* ctx);
void         wolfSSL_CTX_set_verify(WOLFSSL_CTX* ctx, int mode, void* verify_cb);

WOLFSSL* wolfSSL_new(WOLFSSL_CTX* ctx);
void     wolfSSL_free(WOLFSSL* ssl);
int      wolfSSL_set_fd(WOLFSSL* ssl, int fd);
int      wolfSSL_set_using_nonblock(WOLFSSL* ssl, int on);

int  wolfSSL_connect(WOLFSSL* ssl);
int  wolfSSL_read(WOLFSSL* ssl, void* data, int sz);
int  wolfSSL_write(WOLFSSL* ssl, const void* data, int sz);
int  wolfSSL_write_dup(WOLFSSL* ssl, const void* data, int sz);

int  wolfSSL_get_error(WOLFSSL* ssl, int ret);
int  wolfSSL_want_read(WOLFSSL* ssl);
int  wolfSSL_want_write(WOLFSSL* ssl);
char* wolfSSL_ERR_error_string(unsigned long errNumber, char* data);

#ifdef __cplusplus
}
#endif
