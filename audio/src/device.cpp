#include "audio/device.h"

// miniaudio is one large single header; its implementation is compiled *here only*.
// Disable the subsystems we don't use (decoding/encoding/resource manager/node
// graph) — we drive a raw playback device — to keep this TU lean. It is included
// with angle brackets so MSVC treats it as external (/external:W0 in CMake), and we
// also clamp warnings to be safe across toolchain versions.
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <miniaudio.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace br::audio {

struct AudioDevice::Impl {
    ma_context context{};
    ma_device device{};
    bool context_inited = false;
    bool device_inited = false;
    AudioDevice::PullFn pull = nullptr;
    void* user = nullptr;
    uint32_t channels = 2;
};

namespace {
void data_callback(ma_device* dev, void* output, const void* /*input*/, ma_uint32 frame_count) {
    auto* impl = static_cast<AudioDevice::Impl*>(dev->pUserData);
    if (impl && impl->pull)
        impl->pull(impl->user, static_cast<float*>(output), frame_count, impl->channels);
}
}  // namespace

AudioDevice::AudioDevice() : impl_(std::make_unique<Impl>()) {}
AudioDevice::~AudioDevice() { stop(); }

bool AudioDevice::start(uint32_t sample_rate, uint32_t channels, PullFn pull, void* user,
                        bool use_null) {
    stop();
    impl_->pull = pull;
    impl_->user = user;
    impl_->channels = channels;

    ma_backend null_backends[1] = {ma_backend_null};
    ma_context_config ctx_cfg = ma_context_config_init();
    const ma_result cr = use_null
        ? ma_context_init(null_backends, 1, &ctx_cfg, &impl_->context)
        : ma_context_init(nullptr, 0, &ctx_cfg, &impl_->context);
    if (cr != MA_SUCCESS) { last_error_ = "ma_context_init failed"; return false; }
    impl_->context_inited = true;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = channels;
    cfg.sampleRate = sample_rate;
    cfg.dataCallback = data_callback;
    cfg.pUserData = impl_.get();

    if (ma_device_init(&impl_->context, &cfg, &impl_->device) != MA_SUCCESS) {
        last_error_ = "ma_device_init failed";
        ma_context_uninit(&impl_->context);
        impl_->context_inited = false;
        return false;
    }
    impl_->device_inited = true;
    backend_ = use_null ? "Null" : ma_get_backend_name(impl_->context.backend);

    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        last_error_ = "ma_device_start failed";
        stop();
        return false;
    }
    return true;
}

void AudioDevice::stop() {
    if (impl_->device_inited) {
        ma_device_uninit(&impl_->device);
        impl_->device_inited = false;
    }
    if (impl_->context_inited) {
        ma_context_uninit(&impl_->context);
        impl_->context_inited = false;
    }
}

bool AudioDevice::running() const noexcept {
    return impl_->device_inited && ma_device_is_started(&impl_->device) == MA_TRUE;
}

}  // namespace br::audio
