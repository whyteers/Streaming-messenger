















#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#define SDL_MAIN_HANDLED

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <timeapi.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>

#include <SDL.h>

#include <iostream>
#include <string>
#include <vector>
#include <queue>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <algorithm>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libswscale/swscale.h>
}

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include "nvEncodeAPI.h"




static const GUID NV_ENC_PRESET_COMPAT_LOW_LATENCY_DEFAULT_GUID =
{ 0x49df21c5, 0x6dfa, 0x4feb, { 0x97, 0x87, 0x6a, 0xcc, 0x9e, 0xff, 0xb7, 0x26 } };
static const GUID NV_ENC_PRESET_COMPAT_DEFAULT_GUID =
{ 0xb2dfb705, 0x4ebd, 0x4c49, { 0x9b, 0x5f, 0x24, 0xa7, 0x77, 0xd3, 0xe5, 0x87 } };

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")



static const char* VIDEO_VIEW_PORT = "8080";
static const char* VIDEO_BROADCAST_PORT = "8082";

static const int MAX_FRAME_SIZE = 4 * 1024 * 1024;
static const int FRAME_QUEUE_SIZE = 6;



static std::mutex g_LogMutex;
static std::vector<std::string> g_LogMessages;

static void Log(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(g_LogMutex);

    g_LogMessages.push_back(msg);

    if (g_LogMessages.size() > 300)
        g_LogMessages.erase(g_LogMessages.begin());

    std::cout << msg << std::endl;
}



static bool RecvAll(SOCKET sock, void* buf, int len)
{
    char* ptr = static_cast<char*>(buf);
    int received = 0;

    while (received < len)
    {
        int n = recv(sock, ptr + received, len - received, 0);
        if (n <= 0)
            return false;

        received += n;
    }

    return true;
}

static bool SendAll(SOCKET sock, const void* buf, int len)
{
    const char* ptr = static_cast<const char*>(buf);
    int sent = 0;

    while (sent < len)
    {
        int n = send(sock, ptr + sent, len - sent, 0);
        if (n <= 0)
            return false;

        sent += n;
    }

    return true;
}

static bool ConnectTCP(const std::string& ip, const std::string& port, SOCKET& outSock)
{
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* result = nullptr;

    int ret = getaddrinfo(ip.c_str(), port.c_str(), &hints, &result);
    if (ret != 0)
    {
        Log("[Net] getaddrinfo failed for " + ip + ":" + port);
        return false;
    }

    outSock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (outSock == INVALID_SOCKET)
    {
        freeaddrinfo(result);
        Log("[Net] socket() failed");
        return false;
    }

    ret = connect(outSock, result->ai_addr, static_cast<int>(result->ai_addrlen));
    freeaddrinfo(result);

    if (ret == SOCKET_ERROR)
    {
        closesocket(outSock);
        outSock = INVALID_SOCKET;
        Log("[Net] connect() failed to " + ip + ":" + port);
        return false;
    }

    int flag = 1;
    setsockopt(outSock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&flag), sizeof(flag));

    int sndbuf = 512 * 1024;
    setsockopt(outSock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&sndbuf), sizeof(sndbuf));

    Log("[Net] Connected to " + ip + ":" + port);

    return true;
}



class FrameQueue
{
public:
    ~FrameQueue()
    {
        Clear();
    }

    void Push(AVFrame* frame)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        while (static_cast<int>(m_Queue.size()) >= FRAME_QUEUE_SIZE)
        {
            AVFrame* old = m_Queue.front();
            m_Queue.pop();
            av_frame_free(&old);
        }

        AVFrame* copy = av_frame_alloc();
        if (!copy)
            return;

        av_frame_ref(copy, frame);
        m_Queue.push(copy);
    }

    AVFrame* Pop()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        if (m_Queue.empty())
            return nullptr;

        AVFrame* frame = m_Queue.front();
        m_Queue.pop();

        return frame;
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        while (!m_Queue.empty())
        {
            AVFrame* frame = m_Queue.front();
            m_Queue.pop();
            av_frame_free(&frame);
        }
    }

    int Size()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return static_cast<int>(m_Queue.size());
    }

private:
    std::queue<AVFrame*> m_Queue;
    mutable std::mutex m_Mutex;
};



class H264Decoder
{
public:
    ~H264Decoder()
    {
        Cleanup();
    }

    bool Init(FrameQueue* queue)
    {
        m_FrameQueue = queue;

        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) { Log("[Decoder] H264 not found"); return false; }

        m_CodecCtx = avcodec_alloc_context3(codec);
        if (!m_CodecCtx) return false;


        m_HWDeviceCtx = nullptr;
        int err = av_hwdevice_ctx_create(&m_HWDeviceCtx,
            AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
        if (err == 0 && m_HWDeviceCtx)
        {
            m_CodecCtx->hw_device_ctx = av_buffer_ref(m_HWDeviceCtx);
            m_CodecCtx->get_format = [](AVCodecContext*, const AVPixelFormat* fmts) -> AVPixelFormat {
                for (auto p = fmts; *p != AV_PIX_FMT_NONE; ++p)
                    if (*p == AV_PIX_FMT_D3D11) return *p;
                return fmts[0];
            };
            m_UseHW = true;
            Log("[Decoder] D3D11VA ON");
        }
        else
        {
            m_UseHW = false;
            Log("[Decoder] D3D11VA OFF, CPU decode");
        }

        m_CodecCtx->flags  |= AV_CODEC_FLAG_LOW_DELAY;
        m_CodecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
        m_CodecCtx->thread_count = 4;
        m_CodecCtx->thread_type  = FF_THREAD_SLICE;

        if (avcodec_open2(m_CodecCtx, codec, nullptr) < 0)
        {
            Log("[Decoder] open2 failed");
            Cleanup();
            return false;
        }

        m_Packet  = av_packet_alloc();
        m_SWFrame = av_frame_alloc();
        if (!m_Packet || !m_SWFrame) { Cleanup(); return false; }

        Log("[Decoder] ready, HW=" + std::string(m_UseHW ? "yes" : "no"));
        return true;
    }



    int Decode(const uint8_t* data, size_t size)
    {
        if (!m_CodecCtx || !m_Packet) return 0;

        m_Packet->data = const_cast<uint8_t*>(data);
        m_Packet->size = static_cast<int>(size);

        int ret = avcodec_send_packet(m_CodecCtx, m_Packet);
        if (ret < 0 && ret != AVERROR(EAGAIN)) return 0;

        int pushed = 0;
        AVFrame* frame = av_frame_alloc();
        if (!frame) return 0;

        while (avcodec_receive_frame(m_CodecCtx, frame) == 0)
        {
            AVFrame* out = nullptr;

            if (frame->format == AV_PIX_FMT_D3D11)
            {


                av_frame_unref(m_SWFrame);
                m_SWFrame->format = AV_PIX_FMT_NV12;
                if (av_hwframe_transfer_data(m_SWFrame, frame, 0) == 0)
                    out = m_SWFrame;
                else if (++m_TransferFails <= 5)
                    Log("[Decoder] hwframe_transfer_data failed");
            }
            else if (frame->format == AV_PIX_FMT_NV12 ||
                     frame->format == AV_PIX_FMT_YUV420P)
            {

                out = frame;
            }
            else if (++m_FormatWarns <= 5)
            {
                Log("[Decoder] unsupported sw format: " + std::to_string(frame->format));
            }

            if (out && m_FrameQueue)
            {
                m_FrameQueue->Push(out);
                ++pushed;
            }
        }

        av_frame_free(&frame);
        return pushed;
    }

    void Flush()
    {
        if (m_CodecCtx)
            avcodec_flush_buffers(m_CodecCtx);
    }

    void Cleanup()
    {
        if (m_Packet)      { av_packet_free(&m_Packet);         m_Packet = nullptr; }
        if (m_SWFrame)     { av_frame_free(&m_SWFrame);         m_SWFrame = nullptr; }
        if (m_CodecCtx)    { avcodec_free_context(&m_CodecCtx); m_CodecCtx = nullptr; }
        if (m_HWDeviceCtx) { av_buffer_unref(&m_HWDeviceCtx);   m_HWDeviceCtx = nullptr; }
    }

private:
    AVCodecContext* m_CodecCtx    = nullptr;
    AVPacket*       m_Packet      = nullptr;
    AVFrame*        m_SWFrame     = nullptr;
    AVBufferRef*    m_HWDeviceCtx = nullptr;
    bool            m_UseHW       = false;
    FrameQueue*     m_FrameQueue  = nullptr;
    int             m_TransferFails = 0;
    int             m_FormatWarns   = 0;
};



class VideoReceiver
{
public:
    std::atomic<bool> running{ false };
    std::atomic<bool> connected{ false };

    std::atomic<int> rxFps{ 0 };
    std::atomic<int> decodedFps{ 0 };
    std::atomic<int> rxKbps{ 0 };

    FrameQueue frameQueue;

    ~VideoReceiver()
    {
        Stop();
    }

    bool Connect(const std::string& ip)
    {
        return ConnectTCP(ip, VIDEO_VIEW_PORT, m_Sock);
    }

    void Start()
    {
        if (running.load())
            return;

        if (!m_Decoder.Init(&frameQueue))
        {
            Log("[VideoRx] Decoder init failed");
            return;
        }

        running = true;
        connected = true;

        m_NetThread = std::thread(&VideoReceiver::NetworkLoop, this);
        m_DecodeThread = std::thread(&VideoReceiver::DecodeLoop, this);

        Log("[VideoRx] Started");
    }

    void Stop()
    {
        running = false;
        m_CV.notify_all();

        if (m_Sock != INVALID_SOCKET)
        {
            shutdown(m_Sock, SD_BOTH);
            closesocket(m_Sock);
            m_Sock = INVALID_SOCKET;
        }

        if (m_NetThread.joinable())
            m_NetThread.join();

        if (m_DecodeThread.joinable())
            m_DecodeThread.join();

        connected = false;

        frameQueue.Clear();
        m_Decoder.Cleanup();

        {
            std::lock_guard<std::mutex> lk(m_QMutex);

            while (!m_PktQueue.empty())
                m_PktQueue.pop();
        }

        rxFps = 0;
        decodedFps = 0;
        rxKbps = 0;

        Log("[VideoRx] Stopped");
    }

    bool IsConnected() const
    {
        return connected.load();
    }

private:

    void NetworkLoop()
    {
        while (running.load())
        {
            uint32_t sz = 0;

            if (!RecvAll(m_Sock, &sz, 4))
            {
                Log("[Net] lost hdr");
                break;
            }

            if (sz == 0 || sz > MAX_FRAME_SIZE)
            {
                Log("[Net] bad size");
                break;
            }

            auto buf = std::make_shared<std::vector<uint8_t>>(sz);

            if (!RecvAll(m_Sock, buf->data(), static_cast<int>(sz)))
            {
                Log("[Net] lost payload");
                break;
            }

            {
                std::lock_guard<std::mutex> lk(m_QMutex);


                while (m_PktQueue.size() >= 8)
                    m_PktQueue.pop();

                m_PktQueue.push(std::move(buf));
            }

            m_CV.notify_one();
        }

        connected = false;
        m_CV.notify_all();
    }


    void DecodeLoop()
    {
        uint32_t rxCnt = 0;
        uint32_t decCnt = 0;
        uint64_t bytes = 0;

        auto tStats = std::chrono::steady_clock::now();

        while (running.load())
        {
            std::shared_ptr<std::vector<uint8_t>> pkt;

            {
                std::unique_lock<std::mutex> lk(m_QMutex);


                m_CV.wait_for(
                    lk,
                    std::chrono::milliseconds(2),
                    [&]
                    {
                        return !m_PktQueue.empty() || !running.load();
                    }
                );

                if (m_PktQueue.empty())
                    continue;

                pkt = std::move(m_PktQueue.front());
                m_PktQueue.pop();
            }

            ++rxCnt;
            bytes += 4 + pkt->size();

            int decoded = m_Decoder.Decode(pkt->data(), pkt->size());
            decCnt += decoded;

            if (decoded == 0 && rxCnt <= 5)
            {
                Log("[Decode] Frame " + std::to_string(rxCnt)
                    + ": " + std::to_string(pkt->size()) + " bytes -> 0 decoded");
            }

            auto now = std::chrono::steady_clock::now();

            if (now - tStats >= std::chrono::seconds(1))
            {
                rxFps = rxCnt;
                decodedFps = decCnt;
                rxKbps = static_cast<int>((bytes * 8) / 1000);

                rxCnt = 0;
                decCnt = 0;
                bytes = 0;

                tStats = now;
            }
        }
    }

private:
    SOCKET m_Sock = INVALID_SOCKET;
    std::thread m_NetThread;
    std::thread m_DecodeThread;
    H264Decoder m_Decoder;

    std::queue<std::shared_ptr<std::vector<uint8_t>>> m_PktQueue;
    std::mutex m_QMutex;
    std::condition_variable m_CV;
};



typedef NVENCSTATUS(NVENCAPI* PNVENCODEAPICREATEINSTANCE)(NV_ENCODE_API_FUNCTION_LIST*);
typedef NVENCSTATUS(NVENCAPI* PNVENCODEAPIGETMAXSUPPORTEDVERSION)(uint32_t*);

class HWEncoder
{
public:
    static const int NUM_SURFACES = 4;

    struct EncodeSurface
    {
        ID3D11Texture2D* texture = nullptr;
        NV_ENC_REGISTER_RESOURCE registeredResource = { NV_ENC_REGISTER_RESOURCE_VER };
        NV_ENC_MAP_INPUT_RESOURCE mappedResource = { NV_ENC_MAP_INPUT_RESOURCE_VER };
        NV_ENC_CREATE_BITSTREAM_BUFFER bitstreamBuffer = { NV_ENC_CREATE_BITSTREAM_BUFFER_VER };
        HANDLE completionEvent = nullptr;
    };

    HINSTANCE hInstNvEnc = nullptr;
    uint32_t driverApiVersion = 0;
    NV_ENCODE_API_FUNCTION_LIST nvenc = { NV_ENCODE_API_FUNCTION_LIST_VER };
    void* hEncoder = nullptr;
    NV_ENC_INITIALIZE_PARAMS initParams = { NV_ENC_INITIALIZE_PARAMS_VER };

    uint32_t originalWidth = 0;
    uint32_t originalHeight = 0;

    ~HWEncoder()
    {
        Clean();
    }

    bool Init(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t initialBitrate)
    {
        originalWidth = width;
        originalHeight = height;
        m_FrameIndex = 0;

        uint32_t targetWidth = (width + 1) & ~1;
        uint32_t targetHeight = (height + 1) & ~1;

        hInstNvEnc = LoadLibraryA("nvEncodeAPI64.dll");
        if (!hInstNvEnc)
        {
            Log("[Encoder] nvEncodeAPI64.dll not found");
            return false;
        }

        auto getMaxVer =
            (PNVENCODEAPIGETMAXSUPPORTEDVERSION)GetProcAddress(hInstNvEnc, "NvEncodeAPIGetMaxSupportedVersion");

        auto createInst =
            (PNVENCODEAPICREATEINSTANCE)GetProcAddress(hInstNvEnc, "NvEncodeAPICreateInstance");

        if (!getMaxVer || !createInst)
            return false;

        uint32_t maxVer = 0;
        if (getMaxVer(&maxVer) != NV_ENC_SUCCESS)
            return false;

        driverApiVersion = (maxVer >> 4) | ((maxVer & 0xF) << 24);

        Log("[Encoder] NVENC header API=" + std::to_string(NVENCAPI_MAJOR_VERSION) + "." + std::to_string(NVENCAPI_MINOR_VERSION)
            + " driverMax=" + std::to_string(maxVer >> 4) + "." + std::to_string(maxVer & 0xF));

        nvenc.version = Patch(NV_ENCODE_API_FUNCTION_LIST_VER);

        if (createInst(&nvenc) != NV_ENC_SUCCESS)
            return false;

        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = {};
        sessionParams.version = Patch(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER);
        sessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        sessionParams.device = reinterpret_cast<void*>(static_cast<IUnknown*>(device));
        sessionParams.apiVersion = driverApiVersion;

        NVENCSTATUS status = nvenc.nvEncOpenEncodeSessionEx(&sessionParams, &hEncoder);

        if (status != NV_ENC_SUCCESS)
        {
            Log("[Encoder] OpenSession failed: " + std::to_string(static_cast<int>(status)));
            return false;
        }

        NV_ENC_PRESET_CONFIG presetConfig;



        struct PresetAttempt {
            GUID                guid;
            NV_ENC_TUNING_INFO  tuning;
            bool                useEx;
            const char*         label;
        };

        std::vector<PresetAttempt> attempts;

        if (nvenc.nvEncGetEncodePresetConfigEx)
        {


            attempts.push_back({ NV_ENC_PRESET_P1_GUID,
                                 NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY,
                                 true,  "P1+ULL" });

            attempts.push_back({ NV_ENC_PRESET_P4_GUID,
                                 NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY,
                                 true,  "P4+ULL" });

            attempts.push_back({ NV_ENC_PRESET_P1_GUID,
                                 NV_ENC_TUNING_INFO_LOW_LATENCY,
                                 true,  "P1+LL" });


            attempts.push_back({ NV_ENC_PRESET_COMPAT_LOW_LATENCY_DEFAULT_GUID,
                                 NV_ENC_TUNING_INFO_LOW_LATENCY,
                                 true,  "LowLatDefault+LL_Ex" });
        }


        attempts.push_back({ NV_ENC_PRESET_COMPAT_LOW_LATENCY_DEFAULT_GUID,
                             NV_ENC_TUNING_INFO_LOW_LATENCY,
                             false, "LowLatDefault_oldAPI" });

        attempts.push_back({ NV_ENC_PRESET_COMPAT_DEFAULT_GUID,
                             NV_ENC_TUNING_INFO_HIGH_QUALITY,
                             false, "Default_oldAPI" });


        GUID               presetToUse    = NV_ENC_PRESET_COMPAT_DEFAULT_GUID;
        NV_ENC_TUNING_INFO usedTuning     = NV_ENC_TUNING_INFO_UNDEFINED;
        bool               configGot      = false;
        NVENCSTATUS        lastErr        = NV_ENC_ERR_GENERIC;

        for (const auto& a : attempts) {
            memset(&presetConfig, 0, sizeof(presetConfig));
            presetConfig.version           = Patch(NV_ENC_PRESET_CONFIG_VER);
            presetConfig.presetCfg.version = Patch(NV_ENC_CONFIG_VER);

            if (a.useEx && nvenc.nvEncGetEncodePresetConfigEx) {
                lastErr = nvenc.nvEncGetEncodePresetConfigEx(
                    hEncoder, NV_ENC_CODEC_H264_GUID,
                    a.guid, a.tuning, &presetConfig);
            } else {
                lastErr = nvenc.nvEncGetEncodePresetConfig(
                    hEncoder, NV_ENC_CODEC_H264_GUID,
                    a.guid, &presetConfig);
            }

            if (lastErr == NV_ENC_SUCCESS) {
                configGot   = true;
                presetToUse = a.guid;


                bool isNewPreset =
                    memcmp(&a.guid, &NV_ENC_PRESET_P1_GUID, sizeof(GUID)) == 0 ||
                    memcmp(&a.guid, &NV_ENC_PRESET_P4_GUID, sizeof(GUID)) == 0 ||
                    memcmp(&a.guid, &NV_ENC_PRESET_P7_GUID, sizeof(GUID)) == 0;

                usedTuning = (a.useEx && isNewPreset)
                           ? a.tuning
                           : NV_ENC_TUNING_INFO_UNDEFINED;

                Log(std::string("[Encoder] Preset OK: ") + a.label
                    + " tuning=" + std::to_string(static_cast<int>(usedTuning)));
                break;
            }
            Log(std::string("[Encoder] Preset FAIL: ") + a.label
                + " err=" + std::to_string(static_cast<int>(lastErr)));
        }

        if (!configGot) {
            Log("[Encoder] All presets failed, last err="
                + std::to_string(static_cast<int>(lastErr)));
            return false;
        }


        encodeConfig = presetConfig.presetCfg;



        encodeConfig.frameIntervalP = 1;
        encodeConfig.gopLength      = 60;

        encodeConfig.encodeCodecConfig.h264Config.repeatSPSPPS     = 1;
        encodeConfig.encodeCodecConfig.h264Config.idrPeriod        = 60;
        encodeConfig.encodeCodecConfig.h264Config.entropyCodingMode =
            NV_ENC_H264_ENTROPY_CODING_MODE_CABAC;

        encodeConfig.rcParams.rateControlMode  = NV_ENC_PARAMS_RC_CBR;
        encodeConfig.rcParams.averageBitRate   = initialBitrate;
        encodeConfig.rcParams.maxBitRate       = initialBitrate;
        encodeConfig.rcParams.vbvBufferSize    = initialBitrate / 10;
        encodeConfig.rcParams.vbvInitialDelay  = initialBitrate / 10;

        encodeConfig.rcParams.enableAQ        = 0;
        encodeConfig.rcParams.aqStrength      = 0;
        encodeConfig.rcParams.enableTemporalAQ = 0;
        encodeConfig.rcParams.enableLookahead = 0;
        encodeConfig.rcParams.lookaheadDepth  = 0;
        encodeConfig.rcParams.multiPass       = NV_ENC_MULTI_PASS_DISABLED;

        encodeConfig.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;


        memset(&initParams, 0, sizeof(initParams));
        initParams.version    = Patch(NV_ENC_INITIALIZE_PARAMS_VER);
        initParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
        initParams.presetGUID = presetToUse;
        initParams.tuningInfo = usedTuning;

        initParams.encodeWidth  = targetWidth;
        initParams.encodeHeight = targetHeight;
        initParams.darWidth     = targetWidth;
        initParams.darHeight    = targetHeight;
        initParams.frameRateNum = 60;
        initParams.frameRateDen = 1;
        initParams.enablePTD         = 1;
        initParams.enableEncodeAsync = 1;



        {
            uint32_t drvMajor = driverApiVersion & 0xFFFF;
            if (drvMajor >= 10) {
                initParams.maxEncodeWidth  = originalWidth;
                initParams.maxEncodeHeight = originalHeight;
            } else {
                initParams.maxEncodeWidth  = 0;
                initParams.maxEncodeHeight = 0;
                Log("[Encoder] Driver < 10: maxEncode* zeroed");
            }
        }

        initParams.encodeConfig = &encodeConfig;


        status = nvenc.nvEncInitializeEncoder(hEncoder, &initParams);

        if (status != NV_ENC_SUCCESS) {
            Log("[Encoder] Init failed: " + std::to_string(static_cast<int>(status)));
            return false;
        }

        for (int i = 0; i < NUM_SURFACES; ++i)
        {
            surfaces[i].bitstreamBuffer.version = Patch(NV_ENC_CREATE_BITSTREAM_BUFFER_VER);
            surfaces[i].bitstreamBuffer.size = 4 * 1024 * 1024;
            surfaces[i].bitstreamBuffer.memoryHeap = NV_ENC_MEMORY_HEAP_SYSMEM_CACHED;

            if (nvenc.nvEncCreateBitstreamBuffer(hEncoder, &surfaces[i].bitstreamBuffer) != NV_ENC_SUCCESS)
            {
                Log("[Encoder] Failed to create bitstream buffer " + std::to_string(i));
                return false;
            }

            surfaces[i].completionEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

            NV_ENC_EVENT_PARAMS eventParams = {};
            eventParams.version = Patch(NV_ENC_EVENT_PARAMS_VER);
            eventParams.completionEvent = surfaces[i].completionEvent;

            if (nvenc.nvEncRegisterAsyncEvent(hEncoder, &eventParams) != NV_ENC_SUCCESS)
            {
                Log("[Encoder] Failed to register async event " + std::to_string(i));
                return false;
            }
        }

        Log("[Encoder] NVENC initialized: " + std::to_string(targetWidth) + "x" + std::to_string(targetHeight));

        return true;
    }

    bool RegisterSurface(int index, ID3D11Texture2D* texture)
    {
        if (index < 0 || index >= NUM_SURFACES)
            return false;

        EncodeSurface& surf = surfaces[index];
        surf.texture = texture;

        surf.registeredResource.version = Patch(NV_ENC_REGISTER_RESOURCE_VER);
        surf.registeredResource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        surf.registeredResource.resourceToRegister = texture;
        surf.registeredResource.width = originalWidth;
        surf.registeredResource.height = originalHeight;
        surf.registeredResource.pitch = 0;
        surf.registeredResource.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
        surf.registeredResource.bufferUsage = NV_ENC_INPUT_IMAGE;

        if (nvenc.nvEncRegisterResource(hEncoder, &surf.registeredResource) != NV_ENC_SUCCESS)
        {
            Log("[Encoder] Failed to register surface");
            return false;
        }

        return true;
    }

    int GetEncodeIndex() const
    {
        return currentEncodeIdx;
    }

    bool IsSurfaceEncoding(int index) const
    {
        for (int pendingIdx : pendingEncodes)
        {
            if (pendingIdx == index)
                return true;
        }

        return false;
    }

    bool EncodeFrame(int index, bool forceIDR = false)
    {
        EncodeSurface& surf = surfaces[index];

        if (IsSurfaceEncoding(index))
            return false;

        surf.mappedResource.version = Patch(NV_ENC_MAP_INPUT_RESOURCE_VER);
        surf.mappedResource.registeredResource = surf.registeredResource.registeredResource;

        if (nvenc.nvEncMapInputResource(hEncoder, &surf.mappedResource) != NV_ENC_SUCCESS)
            return false;

        NV_ENC_PIC_PARAMS picParams = {};
        picParams.version = Patch(NV_ENC_PIC_PARAMS_VER);

        picParams.inputWidth = originalWidth;
        picParams.inputHeight = originalHeight;
        picParams.inputPitch = 0;

        picParams.inputBuffer = surf.mappedResource.mappedResource;
        picParams.outputBitstream = surf.bitstreamBuffer.bitstreamBuffer;




        picParams.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
        picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        picParams.completionEvent = surf.completionEvent;




        picParams.frameIdx = m_FrameIndex++;
        picParams.inputTimeStamp = picParams.frameIdx;

        if (forceIDR)
            picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;

        NVENCSTATUS status = nvenc.nvEncEncodePicture(hEncoder, &picParams);

        if (status == NV_ENC_SUCCESS || status == NV_ENC_ERR_NEED_MORE_INPUT)
        {
            pendingEncodes.push_back(index);
        }
        else
        {
            nvenc.nvEncUnmapInputResource(hEncoder, surf.mappedResource.mappedResource);
            Log("[Encoder] EncodePicture failed: " + std::to_string(static_cast<int>(status)));
            return false;
        }

        currentEncodeIdx = (currentEncodeIdx + 1) % NUM_SURFACES;

        return true;
    }

    bool PollFinishedFrame(uint8_t* outBuffer, size_t maxSize, size_t& outSize, bool& isKey, bool wait = false)
    {
        if (pendingEncodes.empty())
            return false;

        int idx = pendingEncodes.front();
        EncodeSurface& surf = surfaces[idx];

        DWORD waitTime = wait ? INFINITE : 0;

        if (WaitForSingleObject(surf.completionEvent, waitTime) != WAIT_OBJECT_0)
            return false;

        outSize = 0;
        isKey = false;

        NV_ENC_LOCK_BITSTREAM lockBitstream = {};
        lockBitstream.version = Patch(NV_ENC_LOCK_BITSTREAM_VER);
        lockBitstream.outputBitstream = surf.bitstreamBuffer.bitstreamBuffer;
        lockBitstream.doNotWait = 0;

        if (nvenc.nvEncLockBitstream(hEncoder, &lockBitstream) == NV_ENC_SUCCESS)
        {
            if (lockBitstream.bitstreamSizeInBytes > 0)
            {
                isKey =
                    lockBitstream.pictureType == NV_ENC_PIC_TYPE_IDR ||
                    lockBitstream.pictureType == NV_ENC_PIC_TYPE_I;

                outSize = lockBitstream.bitstreamSizeInBytes;

                if (outSize > maxSize)
                    outSize = maxSize;

                memcpy(outBuffer, lockBitstream.bitstreamBufferPtr, outSize);
            }

            nvenc.nvEncUnlockBitstream(hEncoder, surf.bitstreamBuffer.bitstreamBuffer);
        }

        nvenc.nvEncUnmapInputResource(hEncoder, surf.mappedResource.mappedResource);
        pendingEncodes.pop_front();

        return true;
    }

    void Clean()
    {
        if (!hEncoder)
            return;

        NV_ENC_PIC_PARAMS flushParams = {};
        flushParams.version = Patch(NV_ENC_PIC_PARAMS_VER);
        flushParams.encodePicFlags = NV_ENC_PIC_FLAG_EOS;

        nvenc.nvEncEncodePicture(hEncoder, &flushParams);

        while (!pendingEncodes.empty())
        {
            int idx = pendingEncodes.front();
            EncodeSurface& surf = surfaces[idx];

            WaitForSingleObject(surf.completionEvent, INFINITE);

            NV_ENC_LOCK_BITSTREAM lockBitstream = {};
            lockBitstream.version = Patch(NV_ENC_LOCK_BITSTREAM_VER);
            lockBitstream.outputBitstream = surf.bitstreamBuffer.bitstreamBuffer;
            lockBitstream.doNotWait = 0;

            nvenc.nvEncLockBitstream(hEncoder, &lockBitstream);
            nvenc.nvEncUnlockBitstream(hEncoder, surf.bitstreamBuffer.bitstreamBuffer);

            nvenc.nvEncUnmapInputResource(hEncoder, surf.mappedResource.mappedResource);

            pendingEncodes.pop_front();
        }

        for (int i = 0; i < NUM_SURFACES; ++i)
        {
            if (surfaces[i].registeredResource.registeredResource)
                nvenc.nvEncUnregisterResource(hEncoder, surfaces[i].registeredResource.registeredResource);

            if (surfaces[i].bitstreamBuffer.bitstreamBuffer)
                nvenc.nvEncDestroyBitstreamBuffer(hEncoder, surfaces[i].bitstreamBuffer.bitstreamBuffer);

            if (surfaces[i].completionEvent)
            {
                NV_ENC_EVENT_PARAMS eventParams = {};
                eventParams.version = Patch(NV_ENC_EVENT_PARAMS_VER);
                eventParams.completionEvent = surfaces[i].completionEvent;

                nvenc.nvEncUnregisterAsyncEvent(hEncoder, &eventParams);

                CloseHandle(surfaces[i].completionEvent);
            }
        }

        nvenc.nvEncDestroyEncoder(hEncoder);
        hEncoder = nullptr;

        if (hInstNvEnc)
        {
            FreeLibrary(hInstNvEnc);
            hInstNvEnc = nullptr;
        }
    }

private:
    static std::string to_hex(uint32_t val) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%08X", val);
        return std::string(buf);
    }

    static std::string guid_to_string(const GUID& g) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
            g.Data1, g.Data2, g.Data3,
            g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
            g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
        return std::string(buf);
    }

    bool IsDriverAtLeast(uint32_t major, uint32_t minor) const {
        uint32_t drvMajor = driverApiVersion & 0xFFFF;
        uint32_t drvMinor = (driverApiVersion >> 24) & 0x0F;
        return (drvMajor > major) || (drvMajor == major && drvMinor >= minor);
    }

private:
    uint32_t Patch(uint32_t version) const
    {


        uint32_t headerMajor = version & 0xFFFF;
        uint32_t headerMinor = (version >> 24) & 0x0F;
        uint32_t structVer   = (version >> 16) & 0xFF;

        uint32_t driverMajor = driverApiVersion & 0xFFFF;
        uint32_t driverMinor = (driverApiVersion >> 24) & 0x0F;


        if (driverMajor > headerMajor || (driverMajor == headerMajor && driverMinor >= headerMinor))
        {
            return version;
        }







        if (structVer >= 8) structVer = 7;
        else if (structVer == 7) structVer = 4;
        else if (structVer == 6) structVer = 5;
        else if (structVer == 5) structVer = 3;
        else if (structVer == 2) structVer = 1;


        return (version & ~0x0FFF00FFu) | (driverApiVersion & 0x0F0000FFu) | (structVer << 16);
    }

private:
    NV_ENC_CONFIG encodeConfig = { NV_ENC_CONFIG_VER };
    EncodeSurface surfaces[NUM_SURFACES];

    int currentEncodeIdx = 0;
    uint32_t m_FrameIndex = 0;
    std::deque<int> pendingEncodes;
};



static const char* g_ScaleVS = R"hlsl(
struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexId)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    float2 pos = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);

    VSOut o;
    o.pos = float4(pos, 0.0, 1.0);
    o.uv = uv;

    return o;
}
)hlsl";

static const char* g_ScalePS = R"hlsl(
Texture2D tex : register(t0);
SamplerState samp : register(s0);

float4 PSMain(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target
{
    return tex.Sample(samp, uv);
}
)hlsl";

static ID3DBlob* CompileShader(const char* source, const char* entry, const char* profile)
{
    ID3DBlob* shaderBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;

    HRESULT hr = D3DCompile(
        source,
        strlen(source),
        nullptr,
        nullptr,
        nullptr,
        entry,
        profile,
        0,
        0,
        &shaderBlob,
        &errorBlob
    );

    if (FAILED(hr))
    {
        if (errorBlob)
            errorBlob->Release();

        return nullptr;
    }

    return shaderBlob;
}



struct EncodedPacket
{
    std::vector<uint8_t> data;
    bool isKey = false;
};

class SendQueue
{
public:
    void Start()
    {
        m_Running = true;
    }

    void Push(EncodedPacket&& pkt)
    {
        std::lock_guard<std::mutex> lk(m_Mtx);
        while (m_Q.size() >= 4) m_Q.pop();
        m_Q.push(std::move(pkt));
        m_CV.notify_one();
    }

    bool Pop(EncodedPacket& out, int timeoutMs)
    {
        std::unique_lock<std::mutex> lk(m_Mtx);
        if (!m_CV.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                [&]{ return !m_Q.empty() || !m_Running; }))
            return false;
        if (m_Q.empty()) return false;
        out = std::move(m_Q.front());
        m_Q.pop();
        return true;
    }

    void Stop()
    {
        m_Running = false;
        m_CV.notify_all();
    }

private:
    std::queue<EncodedPacket> m_Q;
    std::mutex m_Mtx;
    std::condition_variable m_CV;
    std::atomic<bool> m_Running{ true };
};

class ScreenBroadcaster
{
public:
    struct Config
    {
        int width = 1920;
        int height = 1080;
        int fps = 60;
        int bitrateKbps = 8000;
    };

    std::atomic<bool> running{ false };
    std::atomic<bool> connected{ false };

    std::atomic<int> capturedFps{ 0 };
    std::atomic<int> encodedFps{ 0 };
    std::atomic<int> sentFps{ 0 };
    std::atomic<int> sentKbps{ 0 };

    ~ScreenBroadcaster()
    {
        Stop();
    }

    bool Connect(const std::string& ip)
    {
        return ConnectTCP(ip, VIDEO_BROADCAST_PORT, m_Sock);
    }

    void Start(const Config& config)
    {
        if (running.load())
            return;

        m_Config = config;
        m_TargetWidth  = (config.width + 1) & ~1;
        m_TargetHeight = (config.height + 1) & ~1;
        m_Fps     = config.fps;
        m_Bitrate = config.bitrateKbps * 1000;

        m_EncodeBuffer.resize(4 * 1024 * 1024);

        running   = true;
        connected = true;

        m_SendQueue.Start();

        m_SendThread   = std::thread(&ScreenBroadcaster::SendLoop, this);
        m_Thread       = std::thread(&ScreenBroadcaster::BroadcastLoop, this);

        Log("[Broadcast] Started");
    }

    void Stop()
    {
        running = false;
        m_SendQueue.Stop();

        if (m_Sock != INVALID_SOCKET)
        {
            shutdown(m_Sock, SD_BOTH);
            closesocket(m_Sock);
            m_Sock = INVALID_SOCKET;
        }

        if (m_Thread.joinable())     m_Thread.join();
        if (m_SendThread.joinable()) m_SendThread.join();

        connected = false;
        CleanupDX();

        capturedFps = 0; encodedFps = 0; sentFps = 0; sentKbps = 0;
        Log("[Broadcast] Stopped");
    }

    bool IsConnected() const
    {
        return connected.load();
    }

private:
    bool InitDXGI()
    {
        IDXGIFactory1* factory = nullptr;

        HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));

        if (FAILED(hr) || !factory)
        {
            Log("[Broadcast] CreateDXGIFactory1 failed");
            return false;
        }

        IDXGIAdapter1* chosenAdapter = nullptr;
        IDXGIAdapter1* adapter = nullptr;

        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);

            if (wcsstr(desc.Description, L"NVIDIA") || wcsstr(desc.Description, L"nvidia"))
            {
                chosenAdapter = adapter;
                chosenAdapter->AddRef();
                adapter->Release();
                break;
            }

            adapter->Release();
        }

        if (!chosenAdapter)
        {
            factory->EnumAdapters1(0, &chosenAdapter);
        }

        factory->Release();

        if (!chosenAdapter)
        {
            Log("[Broadcast] No DXGI adapter");
            return false;
        }

        D3D_FEATURE_LEVEL featureLevel;

        hr = D3D11CreateDevice(
            chosenAdapter,
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &m_Device,
            &featureLevel,
            &m_Context
        );

        if (FAILED(hr))
        {
            chosenAdapter->Release();
            Log("[Broadcast] D3D11CreateDevice failed");
            return false;
        }

        IDXGIOutput* output = nullptr;

        if (chosenAdapter->EnumOutputs(0, &output) == DXGI_ERROR_NOT_FOUND || !output)
        {
            chosenAdapter->Release();
            Log("[Broadcast] No DXGI output");
            return false;
        }

        output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&m_Output1));

        output->Release();
        chosenAdapter->Release();

        if (!m_Output1)
        {
            Log("[Broadcast] Failed to get IDXGIOutput1");
            return false;
        }

        if (!CreateDuplication())
            return false;

        if (!m_Encoder.Init(m_Device, m_TargetWidth, m_TargetHeight, m_Bitrate))
        {
            Log("[Broadcast] NVENC init failed");
            return false;
        }

        m_EncodeSurfaces.resize(HWEncoder::NUM_SURFACES, nullptr);

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = m_TargetWidth;
        desc.Height = m_TargetHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        for (int i = 0; i < HWEncoder::NUM_SURFACES; ++i)
        {
            if (FAILED(m_Device->CreateTexture2D(&desc, nullptr, &m_EncodeSurfaces[i])))
            {
                Log("[Broadcast] CreateTexture2D failed");
                return false;
            }

            if (!m_Encoder.RegisterSurface(i, m_EncodeSurfaces[i]))
            {
                Log("[Broadcast] RegisterSurface failed");
                return false;
            }
        }

        Log("[Broadcast] DXGI initialized");

        return true;
    }

    bool CreateDuplication()
    {
        if (m_Duplication)
        {
            m_Duplication->Release();
            m_Duplication = nullptr;
        }

        HRESULT hr = m_Output1->DuplicateOutput(m_Device, &m_Duplication);

        if (FAILED(hr) || !m_Duplication)
        {
            Log("[Broadcast] DuplicateOutput failed: " + std::to_string(hr));
            return false;
        }

        return true;
    }

    bool CompileScalerShaders()
    {
        if (m_ShadersReady)
            return true;

        ID3DBlob* vsBlob = CompileShader(g_ScaleVS, "VSMain", "vs_5_0");
        ID3DBlob* psBlob = CompileShader(g_ScalePS, "PSMain", "ps_5_0");

        if (!vsBlob || !psBlob)
        {
            if (vsBlob) vsBlob->Release();
            if (psBlob) psBlob->Release();

            Log("[Broadcast] Shader compile failed");
            return false;
        }

        HRESULT hr = m_Device->CreateVertexShader(
            vsBlob->GetBufferPointer(),
            vsBlob->GetBufferSize(),
            nullptr,
            &m_ScaleVS
        );

        vsBlob->Release();

        if (FAILED(hr))
        {
            psBlob->Release();
            return false;
        }

        hr = m_Device->CreatePixelShader(
            psBlob->GetBufferPointer(),
            psBlob->GetBufferSize(),
            nullptr,
            &m_ScalePS
        );

        psBlob->Release();

        if (FAILED(hr))
            return false;

        D3D11_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        samplerDesc.MinLOD = 0.0f;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

        hr = m_Device->CreateSamplerState(&samplerDesc, &m_ScaleSampler);

        if (FAILED(hr))
            return false;

        D3D11_RASTERIZER_DESC rasterDesc = {};
        rasterDesc.FillMode = D3D11_FILL_SOLID;
        rasterDesc.CullMode = D3D11_CULL_NONE;
        rasterDesc.DepthClipEnable = TRUE;

        hr = m_Device->CreateRasterizerState(&rasterDesc, &m_ScaleRaster);

        if (FAILED(hr))
            return false;

        m_ShadersReady = true;

        return true;
    }

    bool InitScaler(uint32_t desktopWidth, uint32_t desktopHeight, DXGI_FORMAT format)
    {
        if (!CompileScalerShaders())
            return false;

        if (m_IntermediateTex)
        {
            m_IntermediateTex->Release();
            m_IntermediateTex = nullptr;
        }

        if (m_IntermediateSRV)
        {
            m_IntermediateSRV->Release();
            m_IntermediateSRV = nullptr;
        }

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = desktopWidth;
        desc.Height = desktopHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = m_Device->CreateTexture2D(&desc, nullptr, &m_IntermediateTex);

        if (FAILED(hr))
            return false;

        hr = m_Device->CreateShaderResourceView(m_IntermediateTex, nullptr, &m_IntermediateSRV);

        if (FAILED(hr))
            return false;

        if (!m_RTViewsReady)
        {
            m_RTViews.resize(m_EncodeSurfaces.size(), nullptr);

            D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
            rtvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            rtvDesc.Texture2D.MipSlice = 0;

            for (size_t i = 0; i < m_EncodeSurfaces.size(); ++i)
            {
                hr = m_Device->CreateRenderTargetView(m_EncodeSurfaces[i], &rtvDesc, &m_RTViews[i]);

                if (FAILED(hr))
                    return false;
            }

            m_RTViewsReady = true;
        }

        m_DesktopWidth = desktopWidth;
        m_DesktopHeight = desktopHeight;
        m_DesktopFormat = format;

        m_ScalerInitialized = true;

        return true;
    }

    void ScaleDesktopToEncodeSurface(ID3D11Texture2D* desktopTex, int surfIdx)
    {
        m_Context->CopyResource(m_IntermediateTex, desktopTex);

        ID3D11RenderTargetView* rtv = m_RTViews[surfIdx];

        m_Context->OMSetRenderTargets(1, &rtv, nullptr);

        D3D11_VIEWPORT vp = {};
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = 0.0f;
        vp.Width = static_cast<float>(m_TargetWidth);
        vp.Height = static_cast<float>(m_TargetHeight);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;

        m_Context->RSSetViewports(1, &vp);
        m_Context->RSSetState(m_ScaleRaster);

        m_Context->VSSetShader(m_ScaleVS, nullptr, 0);
        m_Context->PSSetShader(m_ScalePS, nullptr, 0);

        m_Context->PSSetShaderResources(0, 1, &m_IntermediateSRV);
        m_Context->PSSetSamplers(0, 1, &m_ScaleSampler);

        m_Context->IASetInputLayout(nullptr);
        m_Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        m_Context->Draw(3, 0);

        ID3D11ShaderResourceView* nullSRV = nullptr;
        m_Context->PSSetShaderResources(0, 1, &nullSRV);

        ID3D11RenderTargetView* nullRTV = nullptr;
        m_Context->OMSetRenderTargets(1, &nullRTV, nullptr);
    }

    void CleanupDX()
    {
        for (auto* rtv : m_RTViews)
        {
            if (rtv)
                rtv->Release();
        }

        m_RTViews.clear();
        m_RTViewsReady = false;

        if (m_IntermediateSRV)
        {
            m_IntermediateSRV->Release();
            m_IntermediateSRV = nullptr;
        }

        if (m_IntermediateTex)
        {
            m_IntermediateTex->Release();
            m_IntermediateTex = nullptr;
        }

        if (m_ScaleRaster)
        {
            m_ScaleRaster->Release();
            m_ScaleRaster = nullptr;
        }

        if (m_ScaleSampler)
        {
            m_ScaleSampler->Release();
            m_ScaleSampler = nullptr;
        }

        if (m_ScalePS)
        {
            m_ScalePS->Release();
            m_ScalePS = nullptr;
        }

        if (m_ScaleVS)
        {
            m_ScaleVS->Release();
            m_ScaleVS = nullptr;
        }

        m_ShadersReady = false;
        m_ScalerInitialized = false;

        for (auto* tex : m_EncodeSurfaces)
        {
            if (tex)
                tex->Release();
        }

        m_EncodeSurfaces.clear();

        if (m_Duplication)
        {
            m_Duplication->Release();
            m_Duplication = nullptr;
        }

        if (m_Output1)
        {
            m_Output1->Release();
            m_Output1 = nullptr;
        }

        if (m_Context)
        {
            m_Context->Release();
            m_Context = nullptr;
        }

        if (m_Device)
        {
            m_Device->Release();
            m_Device = nullptr;
        }
    }



    void DrainFinishedFrames()
    {
        while (true)
        {
            size_t encodedSize = 0;
            bool isKey = false;

            if (!m_Encoder.PollFinishedFrame(
                    m_EncodeBuffer.data(), m_EncodeBuffer.size(),
                    encodedSize, isKey, false))
                break;

            if (encodedSize > 0)
            {
                EncodedPacket pkt;
                pkt.data.assign(m_EncodeBuffer.data(),
                                m_EncodeBuffer.data() + encodedSize);
                pkt.isKey = isKey;
                m_SendQueue.Push(std::move(pkt));
            }
        }
    }

    void SendLoop()
    {
        uint32_t sentFrames = 0;
        uint64_t sentBytes  = 0;
        auto lastStats = std::chrono::steady_clock::now();

        while (running.load())
        {
            EncodedPacket pkt;
            if (!m_SendQueue.Pop(pkt, 50)) continue;

            if (m_Sock == INVALID_SOCKET) continue;

            uint32_t payloadSize = static_cast<uint32_t>(pkt.data.size());

            WSABUF bufs[2];
            bufs[0].buf = reinterpret_cast<char*>(&payloadSize);
            bufs[0].len = 4;
            bufs[1].buf = reinterpret_cast<char*>(pkt.data.data());
            bufs[1].len = static_cast<ULONG>(pkt.data.size());

            DWORD bytesSent = 0;
            int res = WSASend(m_Sock, bufs, 2, &bytesSent, 0, nullptr, nullptr);

            if (res == SOCKET_ERROR || bytesSent != payloadSize + 4)
            {
                Log("[Broadcast] Send failed");
                running = false;
                break;
            }

            sentBytes += bytesSent;
            ++sentFrames;

            auto now = std::chrono::steady_clock::now();
            if (now - lastStats >= std::chrono::seconds(1))
            {
                sentFps   = sentFrames;
                sentKbps  = static_cast<int>((sentBytes * 8) / 1000);
                sentFrames = 0;
                sentBytes  = 0;
                lastStats  = now;
            }
        }
    }

    void BroadcastLoop()
    {
        if (!InitDXGI())
        {
            connected = false;
            running = false;
            return;
        }

        auto lastFrameTime = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        auto minInterval = std::chrono::milliseconds(1000 / std::max(1, m_Fps));

        bool firstFrame = true;
        auto lastIdrTime = std::chrono::steady_clock::now();

        uint32_t capturedCount = 0;
        uint32_t encodedCount = 0;

        auto lastStatsTime = std::chrono::steady_clock::now();

        Log("[Broadcast] Capture loop started");

        while (running.load())
        {
            auto now = std::chrono::steady_clock::now();

            if (now - lastFrameTime < minInterval)
            {
                Sleep(1);
                continue;
            }

            DXGI_OUTDUPL_FRAME_INFO frameInfo;
            IDXGIResource* desktopResource = nullptr;

            HRESULT hr = m_Duplication->AcquireNextFrame(16, &frameInfo, &desktopResource);

            if (hr == DXGI_ERROR_WAIT_TIMEOUT)
                continue;

            if (hr == DXGI_ERROR_ACCESS_LOST)
            {
                Log("[Broadcast] ACCESS_LOST, reinitializing");

                if (!CreateDuplication())
                {
                    Sleep(100);
                    continue;
                }

                continue;
            }

            if (FAILED(hr) || !desktopResource)
            {
                Sleep(1);
                continue;
            }

            lastFrameTime = now;
            ++capturedCount;

            ID3D11Texture2D* desktopTex = nullptr;

            desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&desktopTex));
            desktopResource->Release();

            if (!desktopTex)
            {
                m_Duplication->ReleaseFrame();
                continue;
            }

            int surfIdx = m_Encoder.GetEncodeIndex();

            if (m_Encoder.IsSurfaceEncoding(surfIdx))
            {

                DrainFinishedFrames();

                if (m_Encoder.IsSurfaceEncoding(surfIdx))
                {
                    desktopTex->Release();
                    m_Duplication->ReleaseFrame();
                    continue;
                }
            }

            D3D11_TEXTURE2D_DESC desktopDesc = {};
            desktopTex->GetDesc(&desktopDesc);

            bool sameSize =
                desktopDesc.Width == m_TargetWidth &&
                desktopDesc.Height == m_TargetHeight;

            bool sameFormat = desktopDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;

            if (sameSize && sameFormat)
            {
                m_Context->CopyResource(m_EncodeSurfaces[surfIdx], desktopTex);
            }
            else
            {
                if (!m_ScalerInitialized ||
                    desktopDesc.Width != m_DesktopWidth ||
                    desktopDesc.Height != m_DesktopHeight ||
                    desktopDesc.Format != m_DesktopFormat)
                {
                    if (!InitScaler(desktopDesc.Width, desktopDesc.Height, desktopDesc.Format))
                    {
                        Log("[Broadcast] Failed to init scaler");
                        desktopTex->Release();
                        m_Duplication->ReleaseFrame();
                        continue;
                    }
                }

                ScaleDesktopToEncodeSurface(desktopTex, surfIdx);
            }

            desktopTex->Release();
            m_Duplication->ReleaseFrame();

            bool forceIDR = false;

            if (firstFrame)
            {
                forceIDR = true;
                firstFrame = false;
            }
            else if (now - lastIdrTime >= std::chrono::seconds(2))
            {
                forceIDR = true;
            }

            if (forceIDR)
                lastIdrTime = now;

            if (m_Encoder.EncodeFrame(surfIdx, forceIDR))
            {
                ++encodedCount;
            }


            DrainFinishedFrames();

            if (!running.load())
                break;

            if (now - lastStatsTime >= std::chrono::seconds(1))
            {
                capturedFps = capturedCount;
                encodedFps  = encodedCount;

                capturedCount = 0;
                encodedCount  = 0;
                lastStatsTime = now;
            }
        }

        connected = false;
        running = false;
    }

private:
    SOCKET m_Sock = INVALID_SOCKET;
    std::thread m_Thread;
    std::thread m_SendThread;
    SendQueue m_SendQueue;

    HWEncoder m_Encoder;

    Config m_Config;

    uint32_t m_TargetWidth = 1280;
    uint32_t m_TargetHeight = 720;
    int m_Fps = 60;
    uint32_t m_Bitrate = 5000000;

    ID3D11Device* m_Device = nullptr;
    ID3D11DeviceContext* m_Context = nullptr;

    IDXGIOutput1* m_Output1 = nullptr;
    IDXGIOutputDuplication* m_Duplication = nullptr;

    std::vector<ID3D11Texture2D*> m_EncodeSurfaces;

    std::vector<uint8_t> m_EncodeBuffer;


    ID3D11Texture2D* m_IntermediateTex = nullptr;
    ID3D11ShaderResourceView* m_IntermediateSRV = nullptr;

    ID3D11VertexShader* m_ScaleVS = nullptr;
    ID3D11PixelShader* m_ScalePS = nullptr;
    ID3D11SamplerState* m_ScaleSampler = nullptr;
    ID3D11RasterizerState* m_ScaleRaster = nullptr;

    std::vector<ID3D11RenderTargetView*> m_RTViews;

    bool m_ShadersReady = false;
    bool m_ScalerInitialized = false;
    bool m_RTViewsReady = false;

    uint32_t m_DesktopWidth = 0;
    uint32_t m_DesktopHeight = 0;
    DXGI_FORMAT m_DesktopFormat = DXGI_FORMAT_UNKNOWN;
};



static void ApplyDarkTheme()
{
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 3.0f;

    style.WindowPadding = ImVec2(10, 10);
    style.FramePadding = ImVec2(8, 6);
    style.ItemSpacing = ImVec2(8, 6);

    ImVec4* colors = style.Colors;

    colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.11f, 0.97f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.06f, 0.06f, 0.09f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.10f, 0.14f, 1.00f);

    colors[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.13f, 0.17f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.19f, 0.19f, 0.25f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.23f, 0.23f, 0.31f, 1.00f);

    colors[ImGuiCol_Button] = ImVec4(0.16f, 0.16f, 0.22f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.25f, 0.35f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.31f, 0.31f, 0.43f, 1.00f);

    colors[ImGuiCol_Text] = ImVec4(0.92f, 0.92f, 0.93f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.48f, 0.48f, 0.50f, 1.00f);

    colors[ImGuiCol_Border] = ImVec4(0.20f, 0.20f, 0.27f, 0.50f);
}



enum class Role
{
    Idle,
    Viewer,
    Broadcaster
};

int main(int argc, char* argv[])
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::string ip = "127.0.0.1";

    if (argc > 1)
        ip = argv[1];

    WSADATA wsaData;

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::cerr << "WSAStartup failed" << std::endl;
        return 1;
    }

    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    SDL_SetMainReady();

    if (SDL_Init(SDL_INIT_VIDEO) < 0)
    {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
        WSACleanup();
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "DXGI NVENC Stream Client",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        1280,
        720,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );

    if (!window)
    {
        std::cerr << "SDL_CreateWindow failed" << std::endl;
        SDL_Quit();
        WSACleanup();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(
        window,
        -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );

    if (!renderer)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);

    if (!renderer)
    {
        std::cerr << "SDL_CreateRenderer failed" << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        WSACleanup();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImFont* font = io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\arial.ttf",
        16.0f,
        nullptr,
        io.Fonts->GetGlyphRangesCyrillic()
    );

    if (!font)
        io.Fonts->AddFontDefault();

    ApplyDarkTheme();

    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    VideoReceiver videoReceiver;
    ScreenBroadcaster broadcaster;

    Role role = Role::Idle;

    char ipBuffer[256];
    strncpy(ipBuffer, ip.c_str(), sizeof(ipBuffer) - 1);
    ipBuffer[sizeof(ipBuffer) - 1] = '\0';

    int resolutionIndex = 1;
    int bitrateKbps = 8000;
    int fps = 60;

    SDL_Texture* videoTexture = nullptr;
    int videoWidth = 0;
    int videoHeight = 0;
    int videoFormat = -1;

    auto StopAll = [&]()
        {
            videoReceiver.Stop();
            broadcaster.Stop();

            role = Role::Idle;
        };

    auto StartViewer = [&]()
        {
            StopAll();

            if (videoReceiver.Connect(ipBuffer))
            {
                videoReceiver.Start();
                role = Role::Viewer;
            }
            else
            {
                Log("[Main] Failed to connect as viewer");
            }
        };

    auto StartBroadcast = [&]()
        {
            StopAll();

            ScreenBroadcaster::Config cfg;

            if (resolutionIndex == 0)
            {
                cfg.width = 1280;
                cfg.height = 720;
            }
            else if (resolutionIndex == 1)
            {
                cfg.width = 1920;
                cfg.height = 1080;
            }
            else
            {
                cfg.width = 2560;
                cfg.height = 1440;
            }

            cfg.fps = fps;
            cfg.bitrateKbps = bitrateKbps;

            if (broadcaster.Connect(ipBuffer))
            {
                broadcaster.Start(cfg);
                role = Role::Broadcaster;
            }
            else
            {
                Log("[Main] Failed to connect as broadcaster");
            }
        };

    Log("[Main] Client started");

    while (true)
    {
        SDL_Event event;

        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL2_ProcessEvent(&event);

            if (event.type == SDL_QUIT)
                goto shutdown;

            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)
                goto shutdown;
        }



        {
            AVFrame* latest = nullptr;

            while (true)
            {
                AVFrame* f = videoReceiver.frameQueue.Pop();
                if (!f)
                    break;

                if (latest)
                    av_frame_free(&latest);

                latest = f;
            }

            if (latest)
            {
                bool needRecreate =
                    latest->width != videoWidth ||
                    latest->height != videoHeight ||
                    latest->format != videoFormat;

                if (needRecreate)
                {
                    if (videoTexture)
                    {
                        SDL_DestroyTexture(videoTexture);
                        videoTexture = nullptr;
                    }

                    videoWidth = latest->width;
                    videoHeight = latest->height;
                    videoFormat = latest->format;

                    Uint32 sdlFmt = (videoFormat == AV_PIX_FMT_NV12)
                        ? SDL_PIXELFORMAT_NV12
                        : SDL_PIXELFORMAT_IYUV;

                    videoTexture = SDL_CreateTexture(
                        renderer,
                        sdlFmt,
                        SDL_TEXTUREACCESS_STREAMING,
                        videoWidth,
                        videoHeight
                    );

                    if (videoTexture)
                    {
                        Log("[Render] Texture " + std::to_string(videoWidth) + "x"
                            + std::to_string(videoHeight)
                            + (videoFormat == AV_PIX_FMT_NV12 ? " NV12" : " YUV420P"));
                    }
                }

                if (videoTexture)
                {
                    if (videoFormat == AV_PIX_FMT_NV12)
                    {
                        SDL_UpdateNVTexture(
                            videoTexture,
                            nullptr,
                            latest->data[0],
                            latest->linesize[0],
                            latest->data[1],
                            latest->linesize[1]
                        );
                    }
                    else
                    {
                        SDL_UpdateYUVTexture(
                            videoTexture,
                            nullptr,
                            latest->data[0],
                            latest->linesize[0],
                            latest->data[1],
                            latest->linesize[1],
                            latest->data[2],
                            latest->linesize[2]
                        );
                    }
                }

                av_frame_free(&latest);
            }
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();


        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(380, 420), ImGuiCond_FirstUseEver);

        ImGui::Begin("Control");

        ImGui::InputText("Server IP", ipBuffer, sizeof(ipBuffer));

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Combo("Resolution", &resolutionIndex, "1280x720\01920x1080\02560x1440\0");
        ImGui::SliderInt("FPS", &fps, 15, 60);
        ImGui::SliderInt("Bitrate kbps", &bitrateKbps, 1000, 20000);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (role == Role::Idle)
        {
            if (ImGui::Button("Start Viewer", ImVec2(-1, 40)))
                StartViewer();

            ImGui::Spacing();

            if (ImGui::Button("Start Broadcast", ImVec2(-1, 40)))
                StartBroadcast();
        }
        else if (role == Role::Viewer)
        {
            ImGui::Text("Role: Viewer");

            ImGui::Spacing();

            if (ImGui::Button("Stop", ImVec2(-1, 36)))
                StopAll();

            ImGui::Spacing();

            if (ImGui::Button("Switch to Broadcast", ImVec2(-1, 30)))
                StartBroadcast();
        }
        else if (role == Role::Broadcaster)
        {
            ImGui::Text("Role: Broadcaster");

            ImGui::Spacing();

            if (ImGui::Button("Stop", ImVec2(-1, 36)))
                StopAll();

            ImGui::Spacing();

            if (ImGui::Button("Switch to Viewer", ImVec2(-1, 30)))
                StartViewer();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (role == Role::Viewer)
        {
            ImGui::Text("Viewer stats:");
            ImGui::Text("Rx FPS: %d", videoReceiver.rxFps.load());
            ImGui::Text("Decoded FPS: %d", videoReceiver.decodedFps.load());
            ImGui::Text("Rx bitrate: %d kbps", videoReceiver.rxKbps.load());
            ImGui::Text("Queue: %d", videoReceiver.frameQueue.Size());
        }
        else if (role == Role::Broadcaster)
        {
            ImGui::Text("Broadcaster stats:");
            ImGui::Text("Captured FPS: %d", broadcaster.capturedFps.load());
            ImGui::Text("Encoded FPS: %d", broadcaster.encodedFps.load());
            ImGui::Text("Sent FPS: %d", broadcaster.sentFps.load());
            ImGui::Text("Sent bitrate: %d kbps", broadcaster.sentKbps.load());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Log:");

        ImGui::BeginChild("Log", ImVec2(0, 120), true);

        {
            std::lock_guard<std::mutex> lock(g_LogMutex);

            for (const auto& msg : g_LogMessages)
                ImGui::TextWrapped("%s", msg.c_str());

            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10.0f)
                ImGui::SetScrollHereY(1.0f);
        }

        ImGui::EndChild();

        ImGui::End();


        ImGui::SetNextWindowPos(ImVec2(410, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);

        ImGui::Begin("Video");

        if (role == Role::Viewer && videoTexture && videoWidth > 0 && videoHeight > 0)
        {
            ImVec2 avail = ImGui::GetContentRegionAvail();

            float aspect = static_cast<float>(videoWidth) / static_cast<float>(videoHeight);

            float drawW = avail.x;
            float drawH = drawW / aspect;

            if (drawH > avail.y)
            {
                drawH = avail.y;
                drawW = drawH * aspect;
            }

            float offsetX = (avail.x - drawW) * 0.5f;
            float offsetY = (avail.y - drawH) * 0.5f;

            if (offsetX > 0.0f)
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);

            if (offsetY > 0.0f)
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offsetY);

            ImGui::Image(reinterpret_cast<ImTextureID>(videoTexture), ImVec2(drawW, drawH));
        }
        else if (role == Role::Broadcaster)
        {
            ImGui::Text("Broadcasting...");
        }
        else
        {
            ImGui::Text("No video");
        }

        ImGui::End();

        ImGui::Render();

        SDL_SetRenderDrawColor(renderer, 12, 12, 16, 255);
        SDL_RenderClear(renderer);

        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);

        SDL_RenderPresent(renderer);
    }

shutdown:

    videoReceiver.Stop();
    broadcaster.Stop();

    if (videoTexture)
        SDL_DestroyTexture(videoTexture);

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    SDL_Quit();

    timeEndPeriod(1);
    WSACleanup();

    return 0;
}