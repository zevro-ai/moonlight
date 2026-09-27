#include "pyrowave.h"

#include "streaming/session.h"
#include "streaming/streamutils.h"

#include <Limelight.h>
#include <SDL.h>

#include <vulkan/vulkan.h>
#include <pyrowave/pyrowave.h>

#include <cstdint>
#include <cstdio>
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
    if (Session::get() != nullptr) {
        Session::get()->getOverlayManager().setOverlayRenderer(nullptr);
    }
    for (int i = 0; i < Overlay::OverlayMax; i++) {
        if (m_OverlayTextures[i] != nullptr) {
            SDL_DestroyTexture(m_OverlayTextures[i]);
        }
    }
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
    SDL_SetRenderDrawColor(m_Renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    if (Session::get() != nullptr) {
        Session::get()->getOverlayManager().setOverlayRenderer(this);
        Session::get()->flushWindowEvents();
    }
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

void PyrowaveVideoDecoder::noteReceivedFrame(PDECODE_UNIT du) {
    if (m_ActiveWndVideoStats.measurementStartUs == 0) {
        m_ActiveWndVideoStats.measurementStartUs = LiGetMicroseconds();
        m_LastFrameNumber = du->frameNumber;
    }
    else {
        m_ActiveWndVideoStats.networkDroppedFrames += du->frameNumber - (m_LastFrameNumber + 1);
        m_ActiveWndVideoStats.totalFrames += du->frameNumber - (m_LastFrameNumber + 1);
        m_LastFrameNumber = du->frameNumber;
    }

    m_BwTracker.AddBytes(du->fullLength);
    if (du->frameHostProcessingLatency != 0) {
        if (m_ActiveWndVideoStats.minHostProcessingLatency != 0) {
            m_ActiveWndVideoStats.minHostProcessingLatency = qMin(m_ActiveWndVideoStats.minHostProcessingLatency, du->frameHostProcessingLatency);
        }
        else {
            m_ActiveWndVideoStats.minHostProcessingLatency = du->frameHostProcessingLatency;
        }
        m_ActiveWndVideoStats.framesWithHostProcessingLatency++;
    }
    m_ActiveWndVideoStats.maxHostProcessingLatency = qMax(m_ActiveWndVideoStats.maxHostProcessingLatency, du->frameHostProcessingLatency);
    m_ActiveWndVideoStats.totalHostProcessingLatency += du->frameHostProcessingLatency;
    m_ActiveWndVideoStats.receivedFrames++;
    m_ActiveWndVideoStats.totalFrames++;

    if (LiGetMicroseconds() > m_ActiveWndVideoStats.measurementStartUs + 1000000) {
        publishStats();
    }
}

void PyrowaveVideoDecoder::publishStats() {
    VIDEO_STATS combined = m_LastWndVideoStats;
    combined.receivedFrames += m_ActiveWndVideoStats.receivedFrames;
    combined.decodedFrames += m_ActiveWndVideoStats.decodedFrames;
    combined.renderedFrames += m_ActiveWndVideoStats.renderedFrames;
    combined.totalFrames += m_ActiveWndVideoStats.totalFrames;
    combined.networkDroppedFrames += m_ActiveWndVideoStats.networkDroppedFrames;
    combined.totalDecodeTimeUs += m_ActiveWndVideoStats.totalDecodeTimeUs;
    combined.totalRenderTimeUs += m_ActiveWndVideoStats.totalRenderTimeUs;
    combined.totalHostProcessingLatency += m_ActiveWndVideoStats.totalHostProcessingLatency;
    combined.framesWithHostProcessingLatency += m_ActiveWndVideoStats.framesWithHostProcessingLatency;
    if (combined.minHostProcessingLatency == 0 ||
            (m_ActiveWndVideoStats.minHostProcessingLatency != 0 &&
             m_ActiveWndVideoStats.minHostProcessingLatency < combined.minHostProcessingLatency)) {
        combined.minHostProcessingLatency = m_ActiveWndVideoStats.minHostProcessingLatency;
    }
    combined.maxHostProcessingLatency = qMax(combined.maxHostProcessingLatency, m_ActiveWndVideoStats.maxHostProcessingLatency);
    if (combined.measurementStartUs == 0) {
        combined.measurementStartUs = m_ActiveWndVideoStats.measurementStartUs;
    }
    if (!LiGetEstimatedRttInfo(&combined.lastRtt, &combined.lastRttVariance)) {
        combined.lastRtt = 0;
        combined.lastRttVariance = 0;
    }

    uint64_t now = LiGetMicroseconds();
    double seconds = (double) (now - combined.measurementStartUs) / 1000000.0;
    if (seconds > 0) {
        combined.totalFps = (double) combined.totalFrames / seconds;
        combined.receivedFps = (double) combined.receivedFrames / seconds;
        combined.decodedFps = (double) combined.decodedFrames / seconds;
        combined.renderedFps = (double) combined.renderedFrames / seconds;
    }

    SDL_memcpy(&m_LastWndVideoStats, &m_ActiveWndVideoStats, sizeof(m_ActiveWndVideoStats));
    SDL_zero(m_ActiveWndVideoStats);
    m_ActiveWndVideoStats.measurementStartUs = now;

    if (Session::get() == nullptr || !Session::get()->getOverlayManager().isOverlayEnabled(Overlay::OverlayDebug)) {
        return;
    }

    char text[1024];
    char rtt[64] = "N/A";
    if (combined.lastRtt != 0) {
        snprintf(rtt, sizeof(rtt), "%u ms (variance: %u ms)", combined.lastRtt, combined.lastRttVariance);
    }
    double decodeMs = combined.decodedFrames ? (double) combined.totalDecodeTimeUs / 1000.0 / combined.decodedFrames : 0;
    double renderMs = combined.renderedFrames ? (double) combined.totalRenderTimeUs / 1000.0 / combined.renderedFrames : 0;
    double hostMs = combined.framesWithHostProcessingLatency ? (double) combined.totalHostProcessingLatency / 10.0 / combined.framesWithHostProcessingLatency : 0;
    double dropped = combined.totalFrames ? (double) combined.networkDroppedFrames / combined.totalFrames * 100.0 : 0;
    snprintf(text, sizeof(text),
             "Video stream: %dx%d %.2f FPS (Codec: PyroWave)\n"
             "Bitrate: %.1f Mbps, Peak (%us): %.1f\n"
             "Incoming frame rate from network: %.2f FPS\n"
             "Decoding frame rate: %.2f FPS\n"
             "Rendering frame rate: %.2f FPS\n"
             "Host processing latency min/max/average: %.1f/%.1f/%.1f ms\n"
             "Frames dropped by your network connection: %.2f%%\n"
             "Average network latency: %s\n"
             "Average decoding time: %.2f ms\n"
             "Average rendering time: %.2f ms\n",
             m_Width, m_Height, combined.totalFps,
             m_BwTracker.GetAverageMbps(), m_BwTracker.GetWindowSeconds(), m_BwTracker.GetPeakMbps(),
             combined.receivedFps, combined.decodedFps, combined.renderedFps,
             (float) combined.minHostProcessingLatency / 10, (float) combined.maxHostProcessingLatency / 10, hostMs,
             dropped, rtt, decodeMs, renderMs);

    Overlay::OverlayManager& overlay = Session::get()->getOverlayManager();
    snprintf(overlay.getOverlayText(Overlay::OverlayDebug), overlay.getOverlayMaxTextLength(), "%s", text);
    overlay.setOverlayTextUpdated(Overlay::OverlayDebug);
}

void PyrowaveVideoDecoder::notifyOverlayUpdated(Overlay::OverlayType) {
}

void PyrowaveVideoDecoder::renderOverlay(Overlay::OverlayType type) {
    if (Session::get() == nullptr || !Session::get()->getOverlayManager().isOverlayEnabled(type)) {
        return;
    }

    SDL_Surface* surface = Session::get()->getOverlayManager().getUpdatedOverlaySurface(type);
    if (surface != nullptr) {
        if (m_OverlayTextures[type] != nullptr) {
            SDL_DestroyTexture(m_OverlayTextures[type]);
        }
        if (type == Overlay::OverlayStatusUpdate) {
            SDL_Rect viewport;
            SDL_RenderGetViewport(m_Renderer, &viewport);
            m_OverlayRects[type].x = 0;
            m_OverlayRects[type].y = viewport.h - surface->h;
        }
        else {
            m_OverlayRects[type].x = 0;
            m_OverlayRects[type].y = 0;
        }
        m_OverlayRects[type].w = surface->w;
        m_OverlayRects[type].h = surface->h;
        m_OverlayTextures[type] = SDL_CreateTextureFromSurface(m_Renderer, surface);
        SDL_FreeSurface(surface);
        if (m_OverlayTextures[type] != nullptr) {
            SDL_SetTextureScaleMode(m_OverlayTextures[type], SDL_ScaleModeNearest);
        }
    }

    if (m_OverlayTextures[type] != nullptr) {
        SDL_RenderCopy(m_Renderer, m_OverlayTextures[type], nullptr, &m_OverlayRects[type]);
    }
}

int PyrowaveVideoDecoder::submitDecodeUnit(PDECODE_UNIT du) {
    if (m_TestOnly || m_Decoder == nullptr) {
        return DR_OK;
    }

    noteReceivedFrame(du);
    uint64_t decodeStart = LiGetMicroseconds();

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

    m_ActiveWndVideoStats.totalDecodeTimeUs += LiGetMicroseconds() - decodeStart;
    m_ActiveWndVideoStats.decodedFrames++;

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

    uint64_t renderStart = LiGetMicroseconds();
    SDL_RenderSetViewport(m_Renderer, nullptr);
    SDL_RenderClear(m_Renderer);

    SDL_Rect src = {0, 0, m_Width, m_Height};
    SDL_Rect dst = {};
    SDL_GetRendererOutputSize(m_Renderer, &dst.w, &dst.h);
    StreamUtils::scaleSourceToDestinationSurface(&src, &dst);
    SDL_RenderSetViewport(m_Renderer, &dst);
    SDL_RenderCopy(m_Renderer, m_Texture, nullptr, nullptr);

    SDL_RenderSetViewport(m_Renderer, nullptr);
    for (int i = 0; i < Overlay::OverlayMax; i++) {
        renderOverlay((Overlay::OverlayType) i);
    }
    SDL_RenderPresent(m_Renderer);

    m_ActiveWndVideoStats.totalRenderTimeUs += LiGetMicroseconds() - renderStart;
    m_ActiveWndVideoStats.renderedFrames++;
}

void PyrowaveVideoDecoder::setHdrMode(bool) {
}

bool PyrowaveVideoDecoder::notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO) {
    return true;
}
