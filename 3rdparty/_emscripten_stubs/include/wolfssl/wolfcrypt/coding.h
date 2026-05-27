// Emscripten/WASM stub for wolfssl/wolfcrypt/coding.h — provides the
// Base64 encode used by rpcs3 net code. Real wolfssl ships a robust
// impl; the stub returns failure. Bodies in wolfssl_stub.cpp.

#pragma once

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

int Base64_Encode_NoNl(const unsigned char* in, unsigned int inLen, unsigned char* out, unsigned int* outLen);

#ifdef __cplusplus
}
#endif
