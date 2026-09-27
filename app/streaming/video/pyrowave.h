#pragma once

#include "decoder.h"

class PyrowaveVideoDecoder : public IVideoDecoder {
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

private:
    SDL_Renderer* m_Renderer;
    SDL_Texture* m_Texture;
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
};
