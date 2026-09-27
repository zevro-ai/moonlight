#include "pyrowave.h"

#include <Limelight.h>
#include <SDL.h>

#include <vulkan/vulkan.h>
#include <pyrowave/pyrowave.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {
constexpr uint32_t kPayloadMagic = 0x31575950u; // 'PYW1'

uint32_t read_u32(const uint8_t* data) {
    return (uint32_t) data[0] |
           ((uint32_t) data[1] << 8) |
           ((uint32_t) data[2] << 16) |
           ((uint32_t) data[3] << 24);
}
}

PyrowaveVideoDecoder::PyrowaveVideoDecoder()
    : m_Renderer(nullptr),
      m_Texture(nullptr),
      m_FrameLock(nullptr),
      m_Width(0),
      m_Height(0),
      m_TestOnly(false),
      m_FrameReady(false),
      m_Device(nullptr),
      m_Decoder(nullptr) {
}

PyrowaveVideoDecoder::~PyrowaveVideoDecoder() {
    if (m_Decoder != nullptr) {
        pyrowave_decoder_destroy(static_cast<pyrowave_decoder>(m_Decoder));
        m_Decoder = nullptr;
    }
    if (m_Device != nullptr) {
        pyrowave_device_destroy(static_cast<pyrowave_device>(m_Device));
        m_Device = nullptr;
    }
    if (m_Texture != nullptr) {
        SDL_DestroyTexture(m_Texture);
    }
    if (m_Renderer != nullptr) {
        SDL_DestroyRenderer(m_Renderer);
    }
    if (m_FrameLock != nullptr) {
        SDL_DestroyMutex(m_FrameLock);
    }
}

bool PyrowaveVideoDecoder::initialize(PDECODER_PARAMETERS params) {
    if (!(params->videoFormat & VIDEO_FORMAT_MASK_PYROWAVE)) {
        return false;
    }
    if ((params->width % 2) != 0 || (params->height % 2) != 0 || params->width < 2 || params->height < 2) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave needs positive even dimensions, got %dx%d",
                     params->width, params->height);
        return false;
    }

    pyrowave_device device = nullptr;
    if (pyrowave_create_default_device(&device) != PYROWAVE_SUCCESS || device == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave device creation failed");
        return false;
    }

    if (params->testOnly) {
        pyrowave_device_destroy(device);
        m_TestOnly = true;
        m_Width = params->width;
        m_Height = params->height;
        return true;
    }

    pyrowave_decoder_create_info info = {};
    info.device = device;
    info.width = params->width;
    info.height = params->height;
    info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;

    pyrowave_decoder decoder = nullptr;
    if (pyrowave_decoder_create(&info, &decoder) != PYROWAVE_SUCCESS || decoder == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave decoder creation failed");
        pyrowave_device_destroy(device);
        return false;
    }

    m_Renderer = SDL_CreateRenderer(params->window, -1, SDL_RENDERER_ACCELERATED);
    if (m_Renderer == nullptr) {
        m_Renderer = SDL_CreateRenderer(params->window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (m_Renderer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL renderer failed: %s", SDL_GetError());
        pyrowave_decoder_destroy(decoder);
        pyrowave_device_destroy(device);
        return false;
    }

    m_Texture = SDL_CreateTexture(m_Renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, params->width, params->height);
    if (m_Texture == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL YUV texture failed: %s", SDL_GetError());
        pyrowave_decoder_destroy(decoder);
        pyrowave_device_destroy(device);
        return false;
    }

    m_FrameLock = SDL_CreateMutex();
    m_Width = params->width;
    m_Height = params->height;
    m_Device = device;
    m_Decoder = decoder;
    m_Y.resize(m_Width * m_Height);
    m_U.resize((m_Width / 2) * (m_Height / 2));
    m_V.resize((m_Width / 2) * (m_Height / 2));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "PyroWave decoder %dx%d", m_Width, m_Height);
    return true;
}

bool PyrowaveVideoDecoder::isHardwareAccelerated() {
    return true;
}

bool PyrowaveVideoDecoder::isAlwaysFullScreen() {
    return false;
}

bool PyrowaveVideoDecoder::isHdrSupported() {
    return false;
}

int PyrowaveVideoDecoder::getDecoderCapabilities() {
    return 0;
}

int PyrowaveVideoDecoder::getDecoderColorspace() {
    return COLORSPACE_REC_709;
}

int PyrowaveVideoDecoder::getDecoderColorRange() {
    return 0;
}

QSize PyrowaveVideoDecoder::getDecoderMaxResolution() {
    return QSize(3840, 2160);
}

int PyrowaveVideoDecoder::submitDecodeUnit(PDECODE_UNIT du) {
    if (m_TestOnly || m_Decoder == nullptr) {
        return DR_OK;
    }

    std::vector<uint8_t> payload;
    payload.reserve(du->fullLength);
    for (PLENTRY entry = du->bufferList; entry != nullptr; entry = entry->next) {
        payload.insert(payload.end(), entry->data, entry->data + entry->length);
    }

    if (payload.size() < 8 || read_u32(payload.data()) != kPayloadMagic) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "PyroWave payload magic mismatch");
        return DR_NEED_IDR;
    }

    uint32_t packetCount = read_u32(payload.data() + 4);
    size_t headerBytes = 8 + (size_t) packetCount * 4;
    if (packetCount == 0 || packetCount > 4096 || payload.size() < headerBytes) {
        return DR_NEED_IDR;
    }

    pyrowave_decoder decoder = static_cast<pyrowave_decoder>(m_Decoder);
    pyrowave_decoder_clear(decoder);

    size_t offset = headerBytes;
    for (uint32_t i = 0; i < packetCount; i++) {
        uint32_t packetSize = read_u32(payload.data() + 8 + i * 4);
        if (offset + packetSize > payload.size()) {
            return DR_NEED_IDR;
        }
        if (pyrowave_decoder_push_packet(decoder, payload.data() + offset, packetSize) != PYROWAVE_SUCCESS) {
            return DR_NEED_IDR;
        }
        offset += packetSize;
    }

    if (!pyrowave_decoder_decode_is_ready(decoder, false)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "PyroWave frame is incomplete");
        return DR_NEED_IDR;
    }

    std::vector<uint8_t> y((size_t) m_Width * m_Height);
    std::vector<uint8_t> u((size_t) (m_Width / 2) * (m_Height / 2));
    std::vector<uint8_t> v((size_t) (m_Width / 2) * (m_Height / 2));

    pyrowave_cpu_buffer buffer = {};
    buffer.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
    buffer.width = m_Width;
    buffer.height = m_Height;
    buffer.data[0] = y.data();
    buffer.data[1] = u.data();
    buffer.data[2] = v.data();
    buffer.row_stride_in_bytes[0] = m_Width;
    buffer.row_stride_in_bytes[1] = m_Width / 2;
    buffer.row_stride_in_bytes[2] = m_Width / 2;
    buffer.plane_size_in_bytes[0] = y.size();
    buffer.plane_size_in_bytes[1] = u.size();
    buffer.plane_size_in_bytes[2] = v.size();

    if (pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &buffer) != PYROWAVE_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "PyroWave decode failed");
        return DR_NEED_IDR;
    }

    SDL_LockMutex(m_FrameLock);
    m_Y = QByteArray(reinterpret_cast<const char*>(y.data()), (int) y.size());
    m_U = QByteArray(reinterpret_cast<const char*>(u.data()), (int) u.size());
    m_V = QByteArray(reinterpret_cast<const char*>(v.data()), (int) v.size());
    m_FrameReady = true;
    SDL_UnlockMutex(m_FrameLock);

    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = SDL_CODE_FRAME_READY;
    SDL_PushEvent(&event);
    return DR_OK;
}

void PyrowaveVideoDecoder::renderFrameOnMainThread() {
    if (m_Renderer == nullptr || m_Texture == nullptr || m_FrameLock == nullptr) {
        return;
    }

    SDL_LockMutex(m_FrameLock);
    if (!m_FrameReady) {
        SDL_UnlockMutex(m_FrameLock);
        return;
    }

    SDL_UpdateYUVTexture(m_Texture, nullptr,
                         reinterpret_cast<const Uint8*>(m_Y.constData()), m_Width,
                         reinterpret_cast<const Uint8*>(m_U.constData()), m_Width / 2,
                         reinterpret_cast<const Uint8*>(m_V.constData()), m_Width / 2);
    m_FrameReady = false;
    SDL_UnlockMutex(m_FrameLock);

    SDL_RenderClear(m_Renderer);
    SDL_RenderCopy(m_Renderer, m_Texture, nullptr, nullptr);
    SDL_RenderPresent(m_Renderer);
}

void PyrowaveVideoDecoder::setHdrMode(bool) {
}

bool PyrowaveVideoDecoder::notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO) {
    return true;
}
