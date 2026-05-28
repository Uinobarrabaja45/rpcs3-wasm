#include "Emu/Cell/SPURecompiler.h"
#include "Utilities/JIT.h"
#include "rpcs3_version.h"
#include "Input/pad_thread.h"
#include <functional>
#include <thread>
#include <chrono>
#include <cstring>

// qt_events_aware_op stub
void qt_events_aware_op(int repeat_duration_ms, std::function<bool()> wrapped_op)
{
    while (!wrapped_op())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(repeat_duration_ms > 0 ? repeat_duration_ms : 1));
    }
}

// rpcs3 version stubs
namespace rpcs3 {
    std::string get_verbose_version() { return "0.0.40-wasm-local"; }
    std::string get_version_and_branch() { return "0.0.40-wasm-local"; }
}

// report_fatal_error stub
[[noreturn]] void report_fatal_error(std::string_view text, bool is_html, bool include_help_text)
{
    printf("FATAL ERROR: %.*s\n", static_cast<int>(text.size()), text.data());
    std::abort();
}

// pad namespace globals stubs
namespace pad {
    atomic_t<pad_thread*> g_pad_thread = nullptr;
    shared_mutex g_pad_mutex;
    std::string g_title_id;
    atomic_t<bool> g_enabled = false;
    atomic_t<bool> g_reset = false;
    atomic_t<bool> g_started = false;
    atomic_t<bool> g_home_menu_requested = false;
}

// spu_thread and utilize_spu_data_segment stubs
void utilize_spu_data_segment(u32 vaddr, const void* ls_data_vaddr, u32 size) {}
std::vector<u32> spu_thread::discover_functions(u32 base_addr, std::span<const u8> ls, bool is_known_addr, u32 entry)
{
    return {};
}

// spu_runtime static members stubs
const spu_function_t spu_runtime::tr_dispatch = nullptr;
const spu_function_t spu_runtime::tr_branch = nullptr;
const spu_function_t spu_runtime::tr_interpreter = nullptr;
const spu_function_t spu_runtime::tr_all = nullptr;
std::array<atomic_t<spu_function_t>, (1 << 20)>* const spu_runtime::g_dispatcher = nullptr;
const spu_function_t spu_runtime::g_gateway = nullptr;
void(*const spu_runtime::g_escape)(spu_thread*) = nullptr;
void(*const spu_runtime::g_tail_escape)(spu_thread*, spu_function_t, u8*) = nullptr;
std::array<u64, 256> spu_runtime::g_interpreter_table = {};
spu_function_t spu_runtime::g_interpreter = nullptr;

// jit_announce stub
void jit_announce(uptr func, usz size, std::string_view name) {}

// jit_runtime stubs
jit_runtime::jit_runtime() {}
jit_runtime::~jit_runtime() {}
uchar* jit_runtime::_alloc(usz size, usz align) noexcept { return nullptr; }
u8* jit_runtime::alloc(usz size, usz align, bool exec) noexcept { return nullptr; }
u8* jit_runtime::peek(bool exec) noexcept { return nullptr; }
void jit_runtime::initialize() {}
void jit_runtime::finalize() noexcept {}

// jit_runtime_base stubs
const asmjit::Environment& jit_runtime_base::environment() const noexcept {
    static asmjit::Environment env;
    return env;
}
void* jit_runtime_base::_add(asmjit::CodeHolder* code, usz align) noexcept { return nullptr; }

namespace asmjit {
    jit_runtime_base& get_global_runtime() {
        static struct dummy_runtime : jit_runtime_base {
            uchar* _alloc(usz size, usz align) noexcept override { return nullptr; }
        } rt;
        return rt;
    }
}

// spu_cache stubs
spu_cache::spu_cache(const std::string& loc) {}
spu_cache::~spu_cache() {}
std::deque<spu_program> spu_cache::get() { return {}; }
void spu_cache::add(const spu_program& func) {}
void spu_cache::initialize(bool build_existing_cache) {}

// spu_program operators stubs
bool spu_program::operator==(const spu_program& rhs) const noexcept { return false; }
bool spu_program::operator<(const spu_program& rhs) const noexcept { return false; }

// FFmpeg stubs to avoid linking libavcodec / libavformat / libavutil / libswresample / libswscale
extern "C" {
    void avcodec_free_context(void**) {}
    void* avcodec_alloc_context3(const void*) { return nullptr; }
    int avcodec_open2(void*, const void*, void**) { return -1; }
    void* av_buffer_create(void*, int, void*, void*, int) { return nullptr; }
    void* avcodec_find_decoder(int) { return nullptr; }
    void* av_packet_alloc() { return nullptr; }
    void* av_frame_alloc() { return nullptr; }
    int avcodec_send_packet(void*, void*) { return -1; }
    int av_strerror(int, char*, size_t) { return -1; }
    void av_frame_unref(void*) {}
    int avcodec_receive_frame(void*, void*) { return -1; }
    void av_packet_free(void**) {}
    void av_frame_free(void**) {}

    void av_log_set_callback(void*) {}
    void av_log_set_level(int) {}
    int av_dict_set(void**, const char*, const char*, int) { return -1; }
    void* avformat_alloc_context() { return nullptr; }
    int avformat_open_input(void**, const char*, void*, void**) { return -1; }
    void av_dict_free(void**) {}
    void avformat_free_context(void*) {}
    int avformat_find_stream_info(void*, void**) { return -1; }
    void avformat_close_input(void**) {}
    void* av_dict_get(const void*, const char*, const void*, int) { return nullptr; }
    int av_log_get_level() { return 0; }
    int av_log_format_line2(void*, int, const char*, void*, char*, int, int*) { return 0; }

    int swr_alloc_set_opts2(void**, void*, int, int, void*, int, int, int, void*) { return -1; }
    int swr_init(void*) { return -1; }
    int swr_is_initialized(void*) { return 0; }
    int av_read_frame(void*, void*) { return -1; }
    int av_samples_alloc(void*, int*, int, int, int, int) { return -1; }
    int swr_convert(void*, void*, int, void*, int) { return -1; }
    void av_freep(void*) {}
    void av_packet_unref(void*) {}
    void swr_free(void**) {}
    void sws_freeContext(void*) {}
    void* av_codec_iterate(void**) { return nullptr; }
    int avformat_alloc_output_context2(void**, void*, const char*, const char*) { return -1; }
    int avcodec_get_supported_config(const void*, const void*, int, int, const void**, int*) { return -1; }
    int av_channel_layout_copy(void*, const void*) { return -1; }
    const char* av_get_sample_fmt_name(int) { return nullptr; }

    int av_image_fill_linesizes(int*, int, int) { return -1; }
    int av_image_fill_pointers(uint8_t**, int, int, uint8_t*, const int*) { return -1; }
    void av_free(void*) {}
    int av_codec_is_decoder(const void*) { return 0; }
    int av_codec_is_encoder(const void*) { return 0; }
    int av_channel_layout_describe(const void*, char*, size_t) { return -1; }
    void* avcodec_find_encoder(int) { return nullptr; }
    int avcodec_parameters_from_context(void*, const void*) { return -1; }
    int avcodec_receive_packet(void*, void*) { return -1; }
    int avcodec_send_frame(void*, const void*) { return -1; }
    int avformat_query_codec(const void*, int, int) { return 0; }
    void* avformat_new_stream(void*, const void*) { return nullptr; }
    int avformat_write_header(void*, void**) { return -1; }
    int avio_open(void**, const char*, int) { return -1; }
    int avio_closep(void**) { return -1; }
    void* sws_getCachedContext(void*, int, int, int, int, int, int, int, void*, void*, void*) { return nullptr; }
    int sws_scale(void*, const uint8_t* const*, const int*, int, int, uint8_t* const*, const int*) { return -1; }
    void* sws_getContext(int, int, int, int, int, int, int, void*, void*, const double*) { return nullptr; }

    int av_frame_get_buffer(void*, int) { return -1; }
    void av_dump_format(void*, int, const char*, int) {}
    int av_sample_fmt_is_planar(int) { return 0; }
    int av_get_bytes_per_sample(int) { return 0; }
    int av_frame_make_writable(void*) { return -1; }
    int av_write_trailer(void*) { return -1; }
    void* av_muxer_iterate(void**) { return nullptr; }
    void av_packet_rescale_ts(void*, void*, void*) {}
    int av_interleaved_write_frame(void*, void*) { return -1; }

    // Additional FFmpeg stubs
    int av_image_get_buffer_size(int, int, int, int) { return 0; }
    void avcodec_flush_buffers(void*) {}
    const void* avcodec_descriptor_get(int) { return nullptr; }

    // libcurl stubs
    void* curl_easy_init() { return nullptr; }
    int curl_easy_setopt(void*, int, ...) { return 0; }
    void curl_easy_cleanup(void*) {}
    int curl_easy_perform(void*) { return 0; }
    const char* curl_easy_strerror(int) { return "stub"; }

    // wolfSSL stubs
    int wolfSSL_Init(void) { return 1; }
    int wolfSSL_Cleanup(void) { return 1; }
    void* wolfTLSv1_2_client_method(void) { return (void*)1; }
    void* wolfSSL_CTX_new(void* method) { return (void*)1; }
    void wolfSSL_CTX_free(void* ctx) {}
    void wolfSSL_CTX_set_verify(void* ctx, int mode, void* verify_cb) {}
    void* wolfSSL_new(void* ctx) { return (void*)1; }
    void wolfSSL_free(void* ssl) {}
    int wolfSSL_set_fd(void* ssl, int fd) { return 1; }
    int wolfSSL_set_using_nonblock(void* ssl, int on) { return 1; }
    int wolfSSL_connect(void* ssl) { return 1; }
    int wolfSSL_read(void* ssl, void* data, int sz) { return -1; }
    int wolfSSL_write(void* ssl, const void* data, int sz) { return -1; }
    void* wolfSSL_write_dup(void* ssl) { return nullptr; }
    int wolfSSL_get_error(void* ssl, int ret) { return 0; }
    int wolfSSL_want_read(void* ssl) { return 0; }
    int wolfSSL_want_write(void* ssl) { return 0; }
    char* wolfSSL_ERR_error_string(unsigned long errNumber, char* data)
    {
        if (data) { std::strcpy(data, "stub"); return data; }
        return (char*)"stub";
    }

    // Base64 Encode stub
    int Base64_Encode_NoNl(const unsigned char* in, unsigned int inLen, unsigned char* out, unsigned int* outLen)
    {
        static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        unsigned int len = 4 * ((inLen + 2) / 3);
        if (outLen)
        {
            if (out)
            {
                if (*outLen < len) return -1; // buffer too small
                unsigned int i = 0, j = 0;
                for (; i + 2 < inLen; i += 3)
                {
                    out[j++] = alphabet[in[i] >> 2];
                    out[j++] = alphabet[((in[i] & 0x03) << 4) | (in[i+1] >> 4)];
                    out[j++] = alphabet[((in[i+1] & 0x0f) << 2) | (in[i+2] >> 6)];
                    out[j++] = alphabet[in[i+2] & 0x3f];
                }
                if (i < inLen)
                {
                    out[j++] = alphabet[in[i] >> 2];
                    if (i + 1 == inLen)
                    {
                        out[j++] = alphabet[(in[i] & 0x03) << 4];
                        out[j++] = '=';
                        out[j++] = '=';
                    }
                    else
                    {
                        out[j++] = alphabet[((in[i] & 0x03) << 4) | (in[i+1] >> 4)];
                        out[j++] = alphabet[(in[i+1] & 0x0f) << 2];
                        out[j++] = '=';
                    }
                }
                out[j] = '\0';
            }
            *outLen = len;
        }
        return 0; // success
    }
}

#include "Input/ps_move_tracker.h"
#include "Input/ps_move_config.h"

// g_cfg_move stub
cfg_ps_moves g_cfg_move;

cfg_ps_moves::cfg_ps_moves()
    : cfg::node()
    , path("")
{
}

bool cfg_ps_moves::load()
{
    return false;
}

void cfg_ps_moves::save() const
{
}

// pad_thread::SetRumble stub
void pad_thread::SetRumble(unsigned int device_id, unsigned char large_motor, unsigned char small_motor)
{
}

// ps_move_tracker template stubs
template <bool DiagnosticsEnabled>
ps_move_tracker<DiagnosticsEnabled>::ps_move_tracker()
{
}

template <bool DiagnosticsEnabled>
ps_move_tracker<DiagnosticsEnabled>::~ps_move_tracker()
{
}

template <bool DiagnosticsEnabled>
void ps_move_tracker<DiagnosticsEnabled>::set_image_data(const void* buf, u64 size, u32 width, u32 height, s32 format)
{
}

template <bool DiagnosticsEnabled>
void ps_move_tracker<DiagnosticsEnabled>::init_workers()
{
}

template <bool DiagnosticsEnabled>
void ps_move_tracker<DiagnosticsEnabled>::process_image()
{
}

template <bool DiagnosticsEnabled>
void ps_move_tracker<DiagnosticsEnabled>::convert_image(s32 output_format)
{
}

template <bool DiagnosticsEnabled>
void ps_move_tracker<DiagnosticsEnabled>::process_hues()
{
}

template <bool DiagnosticsEnabled>
void ps_move_tracker<DiagnosticsEnabled>::process_contours(ps_move_info& info, u32 index)
{
}

template <bool DiagnosticsEnabled>
void ps_move_tracker<DiagnosticsEnabled>::set_active(u32 index, bool active)
{
}

template <bool DiagnosticsEnabled>
void ps_move_tracker<DiagnosticsEnabled>::set_hue(u32 index, u16 hue)
{
}

template <bool DiagnosticsEnabled>
void ps_move_tracker<DiagnosticsEnabled>::set_hue_threshold(u32 index, u16 threshold)
{
}

template <bool DiagnosticsEnabled>
void ps_move_tracker<DiagnosticsEnabled>::set_saturation_threshold(u32 index, u16 threshold)
{
}

template <bool DiagnosticsEnabled>
std::tuple<u8, u8, u8> ps_move_tracker<DiagnosticsEnabled>::hsv_to_rgb(u16 hue, f32 saturation, f32 value)
{
    return {0, 0, 0};
}

template <bool DiagnosticsEnabled>
std::tuple<s16, f32, f32> ps_move_tracker<DiagnosticsEnabled>::rgb_to_hsv(f32 r, f32 g, f32 b)
{
    return {0, 0.0f, 0.0f};
}

// Explicit instantiation for false and true
template class ps_move_tracker<false>;
template class ps_move_tracker<true>;

#include "Input/product_info.h"

// input stubs
namespace input {
    std::vector<product_info> get_products_by_class(int class_id)
    {
        return {};
    }
}

// pad_thread stubs
s32 pad_thread::AddLddPad()
{
    return -1;
}

void pad_thread::UnregisterLddPad(u32 handle)
{
}

void pad_thread::SetIntercepted(bool intercepted)
{
}
