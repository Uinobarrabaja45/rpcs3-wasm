// Emscripten/WASM stub for libcurl. No network in the POC. Bodies in
// curl_stub.cpp. fetch() via Emscripten could be wired later if needed.

#pragma once

#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

typedef void CURL;

#define CURL_ERROR_SIZE 256

typedef enum {
	CURLE_OK = 0,
} CURLcode;

typedef enum {
	CURLOPT_URL,
	CURLOPT_WRITEFUNCTION,
	CURLOPT_WRITEDATA,
	CURLOPT_POSTFIELDS,
	CURLOPT_POSTFIELDSIZE,
	CURLOPT_POST,
	CURLOPT_ERRORBUFFER,
	CURLOPT_SSL_VERIFYPEER,
	CURLOPT_SSL_VERIFYHOST,
	CURLOPT_SSL_OPTIONS,
} CURLoption;

#define CURLSSLOPT_NATIVE_CA (1 << 4)

typedef size_t (*curl_write_callback)(char* ptr, size_t size, size_t nmemb, void* userdata);

CURL* curl_easy_init(void);
void  curl_easy_cleanup(CURL* handle);
CURLcode curl_easy_setopt(CURL* handle, CURLoption option, ...);
CURLcode curl_easy_perform(CURL* handle);
const char* curl_easy_strerror(CURLcode errornum);

#ifdef __cplusplus
}
#endif
