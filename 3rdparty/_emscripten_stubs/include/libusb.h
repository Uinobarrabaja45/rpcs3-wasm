// Emscripten/WASM stub for libusb-1.0.
//
// Purpose: satisfy `#include <libusb.h>` and the symbol surface that
// rpcs3 (Emu/Io/Usb*) calls, with no runtime functionality. The browser
// has no USB device access, so every entry point returns
// "no device" / failure. Function bodies live in 3rdparty/_emscripten_stubs/libusb_stubs.cpp.
//
// Tracked in plan-poc-ps3-webgpu.md §4 — once the emucore links, the
// USB-dependent code paths (real-DS3 passthrough, USB HID input)
// must be #ifdef'd out at call sites or rerouted to Web Gamepad API.

#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

#define LIBUSB_CALL
#define LIBUSB_API_VERSION 0x01000109

// Opaque types — rpcs3 only passes pointers to these around.
typedef struct libusb_context        libusb_context;
typedef struct libusb_device         libusb_device;
typedef struct libusb_device_handle  libusb_device_handle;
typedef int                          libusb_hotplug_callback_handle;

struct libusb_device_descriptor {
	uint8_t  bLength;
	uint8_t  bDescriptorType;
	uint16_t bcdUSB;
	uint8_t  bDeviceClass;
	uint8_t  bDeviceSubClass;
	uint8_t  bDeviceProtocol;
	uint8_t  bMaxPacketSize0;
	uint16_t idVendor;
	uint16_t idProduct;
	uint16_t bcdDevice;
	uint8_t  iManufacturer;
	uint8_t  iProduct;
	uint8_t  iSerialNumber;
	uint8_t  bNumConfigurations;
};

// Transfer struct — rpcs3 inspects several fields, so model them.
struct libusb_iso_packet_descriptor {
	unsigned int length;
	unsigned int actual_length;
	int          status;
};

struct libusb_transfer;
typedef void (LIBUSB_CALL *libusb_transfer_cb_fn)(struct libusb_transfer*);

struct libusb_transfer {
	libusb_device_handle*       dev_handle;
	uint8_t                     flags;
	unsigned char               endpoint;
	unsigned char               type;
	unsigned int                timeout;
	int                         status;
	int                         length;
	int                         actual_length;
	libusb_transfer_cb_fn       callback;
	void*                       user_data;
	unsigned char*              buffer;
	int                         num_iso_packets;
	struct libusb_iso_packet_descriptor iso_packet_desc[1];
};

typedef int (LIBUSB_CALL *libusb_hotplug_callback_fn)(libusb_context*, libusb_device*, int /*event*/, void*);

// Constants — only the ones actually referenced in rpcs3 source.
enum {
	LIBUSB_SUCCESS                       = 0,
	LIBUSB_ERROR_BUSY                    = -6,
	LIBUSB_ERROR_NO_DEVICE               = -4,
};

enum libusb_log_level {
	LIBUSB_LOG_LEVEL_NONE     = 0,
	LIBUSB_LOG_LEVEL_ERROR    = 1,
	LIBUSB_LOG_LEVEL_WARNING  = 2,
	LIBUSB_LOG_LEVEL_INFO     = 3,
	LIBUSB_LOG_LEVEL_DEBUG    = 4,
};

enum libusb_init_option {
	LIBUSB_OPTION_LOG_LEVEL = 0,
	LIBUSB_OPTION_LOG_CB    = 1,
};

enum libusb_hotplug_flag {
	LIBUSB_HOTPLUG_NO_FLAGS = 0,
};

enum libusb_hotplug_event {
	LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED = 0x01,
	LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT    = 0x02,
};

#define LIBUSB_HOTPLUG_MATCH_ANY -1
#define LIBUSB_CAP_HAS_HOTPLUG    1
#define LIBUSB_DEBUG              LIBUSB_OPTION_LOG_LEVEL
#define LIBUSB_WINDOWS_HOTPLUG    1

#define LIBUSB_ENDPOINT_IN  0x80
#define LIBUSB_ENDPOINT_OUT 0x00

#define LIBUSB_REQUEST_GET_STATUS         0x00
#define LIBUSB_REQUEST_GET_CONFIGURATION  0x08
#define LIBUSB_REQUEST_SET_CONFIGURATION  0x09
#define LIBUSB_REQUEST_SET_INTERFACE      0x0B
#define LIBUSB_REQUEST_GET_DESCRIPTOR     0x06

#define LIBUSB_RECIPIENT_DEVICE     0x00
#define LIBUSB_RECIPIENT_INTERFACE  0x01
#define LIBUSB_RECIPIENT_ENDPOINT   0x02

#define LIBUSB_REQUEST_TYPE_STANDARD 0x00
#define LIBUSB_REQUEST_TYPE_CLASS    0x20

#define LIBUSB_TRANSFER_TYPE_CONTROL 0
#define LIBUSB_TRANSFER_TYPE_BULK    2
#define LIBUSB_TRANSFER_TYPE_MASK    0x3

#define LIBUSB_TRANSFER_COMPLETED 0
#define LIBUSB_TRANSFER_ERROR     1
#define LIBUSB_TRANSFER_TIMED_OUT 2
#define LIBUSB_TRANSFER_CANCELLED 3
#define LIBUSB_TRANSFER_STALL     4
#define LIBUSB_TRANSFER_NO_DEVICE 5
#define LIBUSB_TRANSFER_OVERFLOW  6

#define LIBUSB_CONTROL_SETUP_SIZE 8

int           libusb_init(libusb_context** ctx);
int           libusb_init_context(libusb_context** ctx, const void* options, int num_options);
void          libusb_exit(libusb_context* ctx);
ssize_t       libusb_get_device_list(libusb_context* ctx, libusb_device*** list);
void          libusb_free_device_list(libusb_device** list, int unref_devices);
libusb_device* libusb_ref_device(libusb_device* dev);
void          libusb_unref_device(libusb_device* dev);
int           libusb_get_device_descriptor(libusb_device* dev, struct libusb_device_descriptor* desc);
int           libusb_get_configuration(libusb_device_handle* dev, int* config);
int           libusb_set_configuration(libusb_device_handle* dev, int configuration);
int           libusb_claim_interface(libusb_device_handle* dev, int interface_number);
int           libusb_release_interface(libusb_device_handle* dev, int interface_number);
int           libusb_open(libusb_device* dev, libusb_device_handle** dev_handle);
void          libusb_close(libusb_device_handle* dev_handle);
int           libusb_set_auto_detach_kernel_driver(libusb_device_handle* dev, int enable);
int           libusb_control_transfer(libusb_device_handle* dev_handle, uint8_t request_type, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, unsigned char* data, uint16_t wLength, unsigned int timeout);
int           libusb_submit_transfer(struct libusb_transfer* transfer);
int           libusb_handle_events_timeout_completed(libusb_context* ctx, struct timeval* tv, int* completed);
int           libusb_hotplug_register_callback(libusb_context* ctx, int events, int flags, int vendor_id, int product_id, int dev_class, libusb_hotplug_callback_fn cb_fn, void* user_data, libusb_hotplug_callback_handle* callback_handle);
void          libusb_hotplug_deregister_callback(libusb_context* ctx, libusb_hotplug_callback_handle callback_handle);
uint8_t       libusb_get_port_number(libusb_device* dev);
uint8_t       libusb_get_device_address(libusb_device* dev);
const char*   libusb_error_name(int errcode);
struct libusb_transfer* libusb_alloc_transfer(int iso_packets);
void          libusb_free_transfer(struct libusb_transfer* transfer);

// fill_* are usually macros/inlines upstream; provide minimal inlines.
static inline void libusb_fill_control_setup(unsigned char* buf, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, uint16_t wLength) {
	(void)buf; (void)bmRequestType; (void)bRequest; (void)wValue; (void)wIndex; (void)wLength;
}
static inline void libusb_fill_control_transfer(struct libusb_transfer* transfer, libusb_device_handle* dev_handle, unsigned char* buffer, libusb_transfer_cb_fn callback, void* user_data, unsigned int timeout) {
	if (transfer) { transfer->dev_handle = dev_handle; transfer->buffer = buffer; transfer->callback = callback; transfer->user_data = user_data; transfer->timeout = timeout; }
}
static inline void libusb_fill_bulk_transfer(struct libusb_transfer* transfer, libusb_device_handle* dev_handle, unsigned char endpoint, unsigned char* buffer, int length, libusb_transfer_cb_fn callback, void* user_data, unsigned int timeout) {
	if (transfer) { transfer->dev_handle = dev_handle; transfer->endpoint = endpoint; transfer->buffer = buffer; transfer->length = length; transfer->callback = callback; transfer->user_data = user_data; transfer->timeout = timeout; }
}
static inline void libusb_fill_interrupt_transfer(struct libusb_transfer* transfer, libusb_device_handle* dev_handle, unsigned char endpoint, unsigned char* buffer, int length, libusb_transfer_cb_fn callback, void* user_data, unsigned int timeout) {
	libusb_fill_bulk_transfer(transfer, dev_handle, endpoint, buffer, length, callback, user_data, timeout);
}
static inline void libusb_fill_iso_transfer(struct libusb_transfer* transfer, libusb_device_handle* dev_handle, unsigned char endpoint, unsigned char* buffer, int length, int num_iso_packets, libusb_transfer_cb_fn callback, void* user_data, unsigned int timeout) {
	if (transfer) { transfer->dev_handle = dev_handle; transfer->endpoint = endpoint; transfer->buffer = buffer; transfer->length = length; transfer->num_iso_packets = num_iso_packets; transfer->callback = callback; transfer->user_data = user_data; transfer->timeout = timeout; }
}

#ifdef __cplusplus
} // extern "C"
#endif
