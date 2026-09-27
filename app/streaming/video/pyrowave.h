#pragma once

#include "decoder.h"
#include "overlaymanager.h"
#include "streaming/bandwidth.h"

class PyrowaveVideoDecoder : public IVideoDecoder, public Overlay::IOverlayRenderer {
public:
    PyrowaveVideoDecoder();
    virtual ~PyrowaveVideoDecoder() override;

    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool isHardwareAccelerated() override;
    virtual bool isAlwaysFullScreen() override;
    virtual bool isHdrSupported() override;
    virtual int getDecoderCapabilities() override;
    virtual int getDecoderColorspace() override;
    virtual int getDecoderColorRange() override;
    virtual QSize getDecoderMaxResolution() override;
    virtual int submitDecodeUnit(PDECODE_UNIT du) override;
    virtual void renderFrameOnMainThread() override;
    virtual void setHdrMode(bool enabled) override;
    virtual bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info) override;
    virtual void notifyOverlayUpdated(Overlay::OverlayType type) override;

private:
    void noteReceivedFrame(PDECODE_UNIT du);
    void publishStats();
    void renderOverlay(Overlay::OverlayType type);

    SDL_Renderer* m_Renderer;
    SDL_Texture* m_Texture;
    SDL_Texture* m_OverlayTextures[Overlay::OverlayMax] = {};
    SDL_Rect m_OverlayRects[Overlay::OverlayMax] = {};
    SDL_mutex* m_FrameLock;
    int m_Width;
    int m_Height;
    bool m_TestOnly;
    bool m_FrameReady;
    void* m_Device;
    void* m_Decoder;
    QByteArray m_Y;
    QByteArray m_U;
    QByteArray m_V;
    VIDEO_STATS m_ActiveWndVideoStats = {};
    VIDEO_STATS m_LastWndVideoStats = {};
    int m_LastFrameNumber = 0;
    BandwidthTracker m_BwTracker;
};
