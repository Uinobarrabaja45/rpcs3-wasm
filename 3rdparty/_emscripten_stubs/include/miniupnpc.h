// Emscripten/WASM stub for miniupnpc.
// No UPnP in the browser. PSN/RPCN port-mapping is unsupported in the
// POC; the network code that calls these will silently fail. Bodies
// live in miniupnpc_stub.cpp.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct UPNPDev {
	struct UPNPDev* pNext;
	char* descURL;
	char* st;
	unsigned int scope_id;
	char* usn;
	char buffer[2];
};

struct UPNPUrls {
	char* controlURL;
	char* ipcondescURL;
	char* controlURL_CIF;
	char* controlURL_6FC;
	char* rootdescURL;
};

struct IGDdatas {
	char tmp[8];
};

#define UPNPCOMMAND_SUCCESS 0

struct UPNPDev* upnpDiscover(int delay, const char* multicastif, const char* minissdpdsock, int sameport, int ipv6, unsigned char ttl, int* error);
int GetUPNPUrls(struct UPNPUrls* urls, struct IGDdatas* data, const char* descURL, unsigned int scope_id);
void freeUPNPDevlist(struct UPNPDev* devlist);
void* miniwget(const char* url, int* size, unsigned int scope_id, int* status_code);
void parserootdesc(const char* buffer, int bufsize, struct IGDdatas* data);
int UPNP_AddPortMapping(const char* controlURL, const char* servicetype, const char* extPort, const char* inPort, const char* inClient, const char* desc, const char* proto, const char* remoteHost, const char* leaseDuration);
int UPNP_DeletePortMapping(const char* controlURL, const char* servicetype, const char* extPort, const char* proto, const char* remoteHost);

#ifdef __cplusplus
}
#endif
