// Include guard to prevent multiple inclusion
#ifndef H323ASKW_MAIN_H
#define H323ASKW_MAIN_H

/*
 * main.h
 *
 * H.323 Video Client (H323ASKW)
 *
 * Based on CallGen323 by:
 * Copyright (c) 2001 Benny L. Prijono <seventhson@theseventhson.freeserve.co.uk>
 * Copyright (c) 2008-2018 Jan Willamowius <jan@willamowius.de>
 *
 * Modified and enhanced for video conferencing by:
 * Copyright (c) 2024-2025 Yoshifumi Asakawa
 *
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.0 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * https://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 */


#include <ptlib/video.h>
#include <ptlib/sound.h>
#include <map>
#include <vector>
#include <memory>
#include <atomic>
#include <chrono>
#include <deque>
#include <set>
#include <cstring>
#include <string>

#include <h323.h>

// Phase 1 Stability Improvements: Thread-safe state management
#include "connection_state.h"
#include "h245_state_machine.h"
#include <h245.h>
#include <h323pdu.h>
#include <h323rtp.h>
#include <codec/opalplugin.h>  // 🎬 For PluginCodec_Video_FrameHeader (H323Plus include)

#if defined(H323_VIDEO) && defined(USE_QT6)
// 🎬 Preview callback (kept in sync with plugin ABI: two pointer-sized fields)
typedef struct PreviewCallback {
    void (*fn)(const PluginCodec_Video_FrameHeader*, const unsigned char*, unsigned, void*);
    void* userData;
} PreviewCallback;
static_assert(sizeof(PreviewCallback) == sizeof(void*) * 2,
              "PreviewCallback ABI mismatch – expected two pointer-sized fields");
#endif // defined(H323_VIDEO) && defined(USE_QT6)

// Qt6 video display support
#ifdef USE_QT6
#include "qt_video_window.h"
#endif

#if !defined(P_USE_STANDARD_CXX_BOOL) && !defined(P_USE_INTEGER_BOOL)
    typedef int PBoolean;
#endif

///////////////////////////////////////////////////////////////////////////////

#ifdef USE_QT6
#include "qt_video_window.h"

class Qt6VideoOutputDevice : public PVideoOutputDevice 
{
    PCLASSINFO(Qt6VideoOutputDevice, PVideoOutputDevice);
  public:
    Qt6VideoOutputDevice();
    virtual ~Qt6VideoOutputDevice();
    
    void SetIsContentDisplay(bool isContent) { m_isContentDisplay = isContent; }
    
    static PStringArray GetOutputDeviceNames();
    virtual PStringArray GetDeviceNames() const { return GetOutputDeviceNames(); }
    
    virtual PBoolean Open(const PString & deviceName, PBoolean startImmediate = true);
    virtual PBoolean IsOpen();
    virtual PBoolean Close();
    virtual PBoolean Start();
    virtual PBoolean Stop();
    virtual PBoolean SetFrameData(unsigned x, unsigned y, unsigned width, unsigned height,
                                  const BYTE * data, PBoolean endFrame = true);
    virtual PBoolean EndFrame();
    
    virtual PBoolean SetColourFormat(const PString & colourFormat);
    virtual PBoolean SetFrameSize(unsigned width, unsigned height);
    virtual PINDEX GetMaxFrameBytes();
    
    // デュアルウィンドウ対応: Local/Remoteの区別
    void SetIsRemoteDisplay(bool isRemote) { m_isRemoteDisplay = isRemote; }
    bool IsRemoteDisplay() const { return m_isRemoteDisplay; }
    
  protected:
    unsigned m_frameWidth, m_frameHeight;
    bool m_isStarted, m_isOpen;
    bool m_isRemoteDisplay;  // true=リモート受信用, false=ローカルプレビュー用
    bool m_isContentDisplay; // true=H.239コンテンツ用
    PTime m_lastFrame;
    unsigned m_frameCount;
    PMutex m_mutex;
    PString m_colourFormat;
};
#endif // USE_QT6

///////////////////////////////////////////////////////////////////////////////

// Custom RTP session to handle video packets in audio session
// Custom RTP session user data to handle video packets in MCU compatibility mode
class MyH323_RTP_UserData : public RTP_UserData
{
    PCLASSINFO(MyH323_RTP_UserData, RTP_UserData);
    
  public:
    MyH323_RTP_UserData(MyH323Connection* connection, unsigned sessionID);

    virtual void OnReceiveData(RTP_DataFrame & frame, const RTP_Session::ReceiverReportArray & reports);
    
    // Required for PTLib object cloning (called during H.245 OLC Ack processing)
    // CRITICAL: Return a proper copy with valid connection pointer
    virtual PObject * Clone() const override {
      // Deep copy: create new instance with same connection and session ID
      return new MyH323_RTP_UserData(m_connection, m_sessionID);
    }

  private:
    MyH323Connection* m_connection;
    unsigned m_sessionID;
};

///////////////////////////////////////////////////////////////////////////////
// MutableMicChannel - Microphone wrapper that mutes audio when m_localMicMuted is true
// This wraps a PSoundChannel and replaces audio data with silence when muted
// Forward declaration - implementation in main.cxx after MyH323Connection is defined
class MutableMicChannel : public PIndirectChannel
{
    PCLASSINFO(MutableMicChannel, PIndirectChannel);
    
  public:
    MutableMicChannel(PSoundChannel* soundChannel, MyH323Connection* connection);
    
    virtual PBoolean Read(void* buf, PINDEX len) override;
    
  private:
    MyH323Connection* m_connection;
};

// Wraps speaker PSoundChannel to capture audio data for spectrum analyzer
class SpectrumSpeakerChannel : public PIndirectChannel
{
    PCLASSINFO(SpectrumSpeakerChannel, PIndirectChannel);
    
  public:
    SpectrumSpeakerChannel(PSoundChannel* soundChannel, int sampleRate);
    
    virtual PBoolean Write(const void* buf, PINDEX len) override;
    
  private:
    int m_sampleRate;
};

// グローバル関数：スペクトラム更新（Qt6 UIへの橋渡し）
void UpdateLocalAudioSpectrum(const int16_t* pcmData, size_t sampleCount, int channels, int sampleRate);
void UpdateRemoteAudioSpectrum(const int16_t* pcmData, size_t sampleCount, int channels, int sampleRate);

// H.245 PDU解析の共通関数
void TraceH245Summary(const char* who, const H323ControlPDU & pdu);
PString AnalyzeOLCDetails(const PString& pduString, const PString& pduType);
PString AnalyzeOLCDetailsWithDump(const PString& pduString, const PString& pduType, const H323ControlPDU & pdu);
PString AnalyzeOpenLogicalChannel(const PString& pduString, const PString& pduType);

class MyH323EndPoint;

class RTPFuzzingChannel : public H323_ExternalRTPChannel
{
    PCLASSINFO(RTPFuzzingChannel, H323_ExternalRTPChannel);
public:
    RTPFuzzingChannel(MyH323EndPoint & ep, H323Connection & connection, const H323Capability & capability, Directions direction, unsigned sessionID, WORD rtpPort, WORD rtcpPort);
    virtual ~RTPFuzzingChannel();

    virtual PBoolean Start();
    PDECLARE_NOTIFIER(PTimer, RTPFuzzingChannel, TransmitRTP);
    PDECLARE_NOTIFIER(PTimer, RTPFuzzingChannel, TransmitRTCP);

protected:
    PUDPSocket m_rtpSocket;
    PUDPSocket m_rtcpSocket;
    RTP_DataFrame m_rtpPacket;
    PTimer m_rtpTransmitTimer;
    PTimer m_rtcpTransmitTimer;
    unsigned m_frameTime;
    unsigned m_frameTimeUnits;
    RTP_DataFrame::PayloadTypes m_payloadType;
    DWORD m_syncSource;
    DWORD m_timestamp;
    unsigned m_percentBadRTPHeader;
    unsigned m_percentBadRTPMedia;
    unsigned m_percentBadRTCP;
};

///////////////////////////////////////////////////////////////////////////////

struct CallDetail
{
  CallDetail()
    : openedTransmitMedia(0),
      openedReceiveMedia(0),
      receivedMedia(0),
      receivedAudio(false),
      receivedVideo(false)
    { }

  PTime                openedTransmitMedia;
  PTime                openedReceiveMedia;
  PTime                receivedMedia;
  bool                 receivedAudio;
  bool                 receivedVideo;
  H323TransportAddress mediaGateway;

  void Drop(H323Connection & connection);

  void OnRTPStatistics(const RTP_Session & session, const PString & token);
};


///////////////////////////////////////////////////////////////////////////////

// Forward declarations
class MyH323Connection;

#ifdef H323_VIDEO
// Custom video channel to hook frames for Qt6 display
class MyVideoChannel : public PVideoChannel
{
    PCLASSINFO(MyVideoChannel, PVideoChannel);
  public:
    MyVideoChannel(MyH323Connection * connection, PBoolean isEncoding = FALSE);
    virtual ~MyVideoChannel();
    
    virtual PBoolean Read(void * buf, PINDEX len);
    virtual PBoolean Write(const void * buf, PINDEX len);
    
  protected:
    MyH323Connection * m_connection;
    PBoolean m_isEncoding;  // TRUE for outgoing (encoding), FALSE for incoming (decoding)
    
    // 🛡️ THREAD SAFETY FIX: Instance-specific counters to prevent data races
    unsigned m_incomingFrameCount = 0;  // Frame count for this channel instance
    PTime m_lastFrameTime;              // Last frame timestamp for FPS calculation
    PINDEX m_totalBytes = 0;            // Total bytes received by this channel
};

#ifdef H323_H239
// H.239 Content video channel - displays in separate window
class MyContentVideoChannel : public PVideoChannel
{
    PCLASSINFO(MyContentVideoChannel, PVideoChannel);
  public:
    MyContentVideoChannel(MyH323Connection * connection, PBoolean isEncoding = FALSE);
    virtual ~MyContentVideoChannel();
    
    virtual PBoolean Read(void * buf, PINDEX len);
    virtual PBoolean Write(const void * buf, PINDEX len);
    
  protected:
    MyH323Connection * m_connection;
    PBoolean m_isEncoding;  // TRUE for outgoing (encoding), FALSE for incoming (decoding)
    
    unsigned m_contentFrameCount = 0;
    PTime m_lastContentFrameTime;
    PINDEX m_totalContentBytes = 0;
};
#endif // H323_H239

// 🎯 FIX: Custom RTP Channel to prevent callback crashes
class MyH323RTPChannel : public H323_RTPChannel
{
    PCLASSINFO(MyH323RTPChannel, H323_RTPChannel);
    
public:
    MyH323RTPChannel(H323Connection & connection,
                     const H323Capability & capability,
                     Directions direction,
                     RTP_Session & rtp);
    
    virtual ~MyH323RTPChannel();
    
    // Override callback methods to prevent crashes
    virtual PBoolean OnReceivedPDU(const H245_H2250LogicalChannelParameters & param, unsigned & errorCode);
    virtual PBoolean OnSendingPDU(H245_H2250LogicalChannelParameters & param) const;
    virtual PBoolean Start();
    virtual PBoolean OnReceivedAckPDU(const H245_OpenLogicalChannelAck & ack);  // ACK処理のトレース用
    
protected:
    // Safe callback handling
    void InitializeCallbacks();
};
#endif

///////////////////////////////////////////////////////////////////////////////

class MyH323EndPoint;

class MyH323Connection : public H323Connection
{
    // PCLASSINFO(MyH323Connection, H323Connection);  // Temporarily commented out due to PTLib version compatibility issue
  public:
    MyH323Connection(MyH323EndPoint & ep, unsigned callRef);
    virtual ~MyH323Connection();

#ifdef USE_QT6
    friend class MyVideoChannel;  // Allow MyVideoChannel to access protected members
#endif

    virtual PBoolean OnSendSignalSetup(H323SignalPDU & setupPDU);

    virtual PBoolean OpenAudioChannel(
      PBoolean isEncoding,          /// Direction of data flow
      unsigned bufferSize,          /// Size of each audio buffer
      H323AudioCodec & codec        /// codec that is doing the opening
    );

#ifdef H323_VIDEO
    virtual PBoolean OpenVideoChannel(PBoolean isEncoding, H323VideoCodec & codec);
    
    // 🎬 Register preview callback with H.264 encoder plugin
    void RegisterPreviewCallback(H323VideoCodec* codec);
    
    // 🎬 Static preview callback handler
    static void PreviewCallbackHandler(const PluginCodec_Video_FrameHeader* hdr,
                                      const uint8_t* yuv, unsigned bytes, void* user);
    
    // *** VIDEO PIPELINE HELPER: Ensure USB camera and encoder are active after OLC Ack ***
    void EnsureVideoPipelineActive(const unsigned & sessionID);
    
    // RTP polling flag for MCU compatibility
    bool m_rtpPollingActive;
    
    // Start RTP polling for MCU compatibility
    void StartRTPPolling();
    
    // RTP polling timer callback
    void PollRTPPacketsTick(PThread &, INT);
    
    virtual PBoolean OnOpenLogicalChannel(const H245_OpenLogicalChannel & openPDU, H245_OpenLogicalChannelAck & ackPDU, unsigned & errorCode, const unsigned & sessionID = 0);
    virtual void OnOpenLogicalChannelAck(const H245_OpenLogicalChannelAck & ackPDU, const unsigned & sessionID);
    virtual void OnLogicalChannelOpenFailed(const H323Capability & capability, const H323Channel::Directions direction = H323Channel::IsTransmitter);
    virtual void OnSelectLogicalChannels();
    
    // Override to use custom RTP channel for H.239 mediaChannel fix
    virtual H323Channel * CreateLogicalChannel(const H245_OpenLogicalChannel & open,
                                               PBoolean startingFast,
                                               unsigned & errorCode);
    
    // ==== H.245受信: 実装差異に対応するため複数の受け口を用意 ====
    virtual PBoolean OnReceivedControlPDU(const H323ControlPDU & pdu);              // A) const参照
    virtual PBoolean OnReceivedControlPDU(H323ControlPDU & pdu);                    // B) 非const参照
    virtual PBoolean OnReceivedControlPDU(const H323ControlPDU & pdu, unsigned);    // C) 追加引数あり
    
    // ==== H323Plus内部のH.245処理パスを強制フック ====
    virtual void OnReceivedPDU(H323ControlPDU & pdu);                              // H323Plus内部PDU処理
    virtual PBoolean HandleControlPDU(const H323ControlPDU & pdu);                 // コントロールPDUハンドラ
    virtual void ProcessControlPDU(const H323ControlPDU & pdu);                    // PDU処理エンジン
    
    // ==== H323Plus Selective Utilization Functions (Added 2025/10/10) ====
    bool IsSafeForH323Plus(const H323ControlPDU & pdu);                           // Safe PDUs that can use H323Plus directly
    bool IsConditionallyUnsafe(const H323ControlPDU & pdu);                       // PDUs that need exception handling
    bool IsKnownProblematic(const H323ControlPDU & pdu);                          // PDUs that need custom implementation
    PBoolean HandlePDU_WithExceptionHandling(const H323ControlPDU & pdu);         // H323Plus call with safety wrapper
    PBoolean HandlePDU_SafeFallback(const H323ControlPDU & pdu);                  // Safe fallback implementation
    bool ValidateOpenLogicalChannelRequest(const H245_RequestMessage & request);  // Validation for OLC requests
    
    // *** COMPREHENSIVE EVENT INTEGRATION: Fast Start Support ***
    virtual PBoolean OnReceivedPDU(const H323SignalPDU & pdu);
    
    // ==== (ADDED) RequestMode ACK 受け口（実装差異を吸収するため複数用意）====
    // 1) 引数なしバージョン（従来の一部フォークで使用）
    virtual PBoolean OnRequestModeAck();
    // 2) 引数ありバージョン（H.245構造体を受け取る実装）
    virtual void OnRequestModeAcknowledge(const H245_RequestModeAck & pdu);
    // 3) Reject も拾っておく
    virtual void OnRequestModeReject(const H245_RequestModeReject & pdu);
    // 4) 通話確立時のRequestMode送出用
    virtual void OnEstablished();
    
    // ==== RequestModeAck Reception Monitoring ====
    virtual PBoolean OnReceivedModeChangeAck();  // Monitor RequestModeAck reception
    virtual void OnReceivedModeChangeReject(unsigned cause);  // Monitor RequestModeReject
    
    // RequestMode and ModeRequest handling
    virtual PBoolean OnRequestModeReject(unsigned cause);
    virtual void OnModeChanged(const H323Capability & capability);
    void SendPolycomCompatibleCapabilitySet();  // Send conservative TCS for Polycom MCU compatibility
    void SendRoundTripDelayRequest();  // H.245 keepalive test
    virtual PBoolean OnRoundTripDelayRequest(unsigned sequenceNumber);  // Handle incoming RTD request
    virtual PBoolean OnRoundTripDelayResponse(unsigned sequenceNumber); // Handle incoming RTD response
    void scheduleDelayedVideoChannelOpening(); // H.264 channel delayed opening
    void scheduleVideoFallbackCheck(); // Check if H.264 failed and try fallback
    void scheduleBidirectionalVideoChannel(const unsigned & sessionID); // MCU sessionID-compliant bidirectional video
    
    // P4: reverseLogicalChannelParameters implementation
    virtual PBoolean BuildOpenLogicalChannel(const H323Capability & capability, unsigned sessionID, H245_OpenLogicalChannel & olc);
    virtual PBoolean OnSendingPDU(H245_OpenLogicalChannel & olc) const;  // Override to add reverseLogicalChannelParameters
    
    // *** H.245 PDU LEVEL OVERRIDE: Session ID Management ***
    virtual PBoolean OnSendingPDU(H245_MultimediaSystemControlMessage & pdu);
    
    // CRITICAL FIX: Override OnSendH245_OpenLogicalChannel to add reverseLogicalChannelParameters
    virtual void OnSendH245_OpenLogicalChannel(H245_OpenLogicalChannel & open, PBoolean forward);
    PBoolean AddReverseLogicalChannelParameters(H245_OpenLogicalChannel & olc, const H323Capability & capability, unsigned sessionID);
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, DelayedVideoChannelOpening);
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, VideoFallbackCheck);
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, AggressiveVideoReceiveRequest);
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, BidirectionalVideoChannelTimer);
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, RequestModeTimeout);  // RequestMode timeout handler
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, RequestModeFallbackTimeout);  // RequestMode fallback timeout handler
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, DelayedRequestModeAttempt);  // Delayed RequestMode attempt handler
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, MonitorRTPStatistics);  // Real-time RTP monitoring
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, MonitorRTPDiagnostics);  // MCU診断用RTP監視
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, ProactiveVideoChannelRequest);  // NEW: Proactive video channel request
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, SendBidirectionalVideoRequestMode);  // 🚀 NEW: Send RequestMode for bidirectional video
    
    // *** H.245 CHANNEL ESTABLISHMENT GUARANTEE ***
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, OnH245MonitorTimeout);  // H.245 negotiation monitoring
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, OnH245RetryTimeout);    // H.245 retry mechanism
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, SendActiveVideoOLC);     // Force Slow Start active video OLC
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, OnEnhancedOLCSequenceTimeout);  // Enhanced OLC timeout (Fix 2)
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, OnMSDTimeout);          // MSD timeout handler (Fix 2)
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, OnTCSResendTimeout);    // TCS resend timeout (Fix 2)
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, OnH245RetryDelay);      // H.245 retry delay (Fix 2)
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, ConnectionHealthCheck);  // Connection health monitoring
    
    // *** H.245 CHANNEL ESTABLISHMENT GUARANTEE ***
    void StartH245Guarantee();  // Start H.245 negotiation guarantee mechanism
    
    // P1: Complete sequence ordering methods
    virtual PBoolean OnReceivedTerminalCapabilitySetAck();
    virtual PBoolean OnReceivedMasterSlaveDeterminationAck(PBoolean isMaster);
    virtual PBoolean OnReceivedOpenLogicalChannelAck(const H245_OpenLogicalChannelAck & ack, unsigned & errorCode);
    
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, CooldownComplete);
    void StartOrderedRequestMode();
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, ForcedRequestModeExecution);  // Fallback handler
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, DelayedRequestMode);  // Correct H.245 flow RequestMode after logical channel
    
    // P3: FastUpdatePicture I-frame request methods
    void SendVideoFastUpdatePicture(unsigned logicalChannelNumber);
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, DelayedFastUpdatePicture);
    
    // P4: RTCP RR/SDES transmission methods
    void SendRTCPReceptionReport(const PIPSocket::Address& mcuIP, WORD mcuRTCPPort);
    void SendRTCPSourceDescription(const PIPSocket::Address& mcuIP, WORD mcuRTCPPort);
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, PeriodicRTCPTransmission);
    void ResendRequestModeWithSameSequence();
    void SendRequestModeWithNewSequence();
    void CreateRequestModeDescription(H245_RequestMode & requestMode, const PString & identifier);
    
    // ✅ CRITICAL FIX 2: H.245 Fallback Automation Functions
    void StartFastStartTimeout();                // Monitor FastStart success
    
    // *** FASTSTART STRATEGY A: FastStart-only approach methods ***
    virtual void SelectFastStartChannels(unsigned sessionID, PBoolean tx, PBoolean rx);
    void MarkFastStartOpened(unsigned sessionID);  // Mark channel as opened via FastStart
    bool HasFastStartChannel() const { return m_hasAnyFastStartChannel; }
    void ResetFastStartState();  // Reset FastStart flags on connection close
    void InitiateH245Fallback();                 // Switch to separate H.245
    void ExecuteH245Sequence();                  // MSD→TCS→RequestMode→OLC
    void ExecuteEnhancedH245Sequence();          // Enhanced H.245 sequence (Fix 2)
    void ProceedToTCSStep();                     // Proceed to TCS step after MSD
    void ForceH245ChannelEstablishment();        // Create separate H.245 channel
    bool CheckFastStartStatus();                 // Check if FastStart succeeded
    void HandleFastStartFailure();               // Handle FastStart failure
    
    // Custom video frame hook for Qt6 display
#ifdef USE_QT6
    void DisplayVideoFrame(const BYTE * frameData, PINDEX frameSize, unsigned width, unsigned height, PBoolean isOutgoing = TRUE);
#ifdef H323_H239
    void DisplayContentFrame(const BYTE * frameData, PINDEX frameSize, unsigned width, unsigned height);
#endif
    void InitializePreviewDecoder();  // Initialize H.264 decoder for outgoing preview
    PBoolean DecodePreviewFrame(const BYTE * data, PINDEX len, unsigned width, unsigned height);  // Decode H.264 for preview
    
    // NEW: Send video frame to H.264 encoder for transmission
    void SendVideoFrameToEncoder(const BYTE * frameData, PINDEX frameSize, unsigned width, unsigned height);
    
    // NEW: Get the video encoder codec for direct frame transmission
    H323VideoCodec* GetVideoEncoder();
    
    // NEW: Video functionality integrity check
    void CheckVideoFunctionalityIntegrity();
#endif
    
#ifdef H323_H239
    void StartH239Transmission();
    // H.239開始をPeople送信確立まで遅延させるラッパ
    void RequestH239StartWithRetry();
    void TryStartPendingH239();
    void StopH239Transmission();
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, StartH239TransmissionTrigger);
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, H239StartRetryTrigger);
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, StopH239TransmissionTrigger);
    virtual PBoolean SendH239GenericResponse(PBoolean response);
    virtual PBoolean OnInitialFlowRestriction(H323Channel & channel);
	virtual PBoolean OpenExtendedVideoChannel(PBoolean isEncoding, H323VideoCodec & codec);
#endif
#endif

    virtual H323Channel * CreateRealTimeLogicalChannel(const H323Capability & capability, H323Channel::Directions dir,
                                                       unsigned sessionID, const H245_H2250LogicalChannelParameters * param, RTP_QOS * rtpqos = NULL);

    virtual void OnRTPStatistics(const RTP_Session & session) const;

    // 🚀 ENHANCEMENT: Real-time RTP packet processing (proposed improvement)
    virtual PBoolean OnReceiveRTPPacket(RTP_Session & session, RTP_DataFrame & frame);
    
    // *** SIGNALING ENHANCEMENT: OpenLogicalChannel Direction Verification ***
    virtual PBoolean OnStartLogicalChannel(H323Channel & channel);
    H323VideoCodec* FindOpenTxVideoChannel(unsigned sessionID = 0);  // Helper method
    void ValidateChannelDirectionConsistency(const H323Channel & channel);
    
  // *** CRITICAL: Missing Event Handlers for Video Decode Processing ***
  virtual void OnLogicalChannelOpened(const H323Channel & channel, const H245_OpenLogicalChannel & olc);
  virtual void OnLogicalChannelClosed(const H323Channel & channel);
  virtual void OnOpenLogicalChannelReject(const unsigned & sessionID, const H245_OpenLogicalChannelReject_cause & cause);
  
  // *** ITU-T COMPLIANCE: Master/Slave OLC Competition Resolution ***
  virtual PBoolean OnConflictingLogicalChannel(H323Channel & channel);
  
  // Helper functions for H.245 OLC event processing
  void RecordOLCEvent(const H323Channel & channel, const H245_OpenLogicalChannel & olc, const char* eventName);
  void UpdateH245TruthFromOLC(const H245_OpenLogicalChannel & olc);
  void ResolveOLCCompetition(const H323Channel & channel, bool isMaster);
  
  // *** MCU診断強化機能 ***
  void DiagnoseMCUChannelState(const H323Channel & channel);
  void StartRTPStreamMonitoring();
  
  // ★★★ CRITICAL: Video TX State Machine for Duplicate Prevention ★★★
  enum class VideoTxState { Closed, Opening, Open };
  
  std::atomic<VideoTxState> m_videoTxState { VideoTxState::Closed };
  std::atomic<uint64_t>     m_videoTxLastOpenMs { 0 };   // 直近 open 試行時刻(ms)
  std::atomic<bool>         m_fastStartVideoTxAccepted { false }; // FS で既に開いたか
  
  // OLC Ack / Close で更新するためのヘルパ
  void MarkVideoTxOpened();
  void MarkVideoTxClosed();
  void OnFastStartVideoTxAccepted();
  
  // 唯一のオープン関数（ここ以外で開かない）
  // bool openVideoTxOnce(const H323Capability & cap, unsigned sessionID);  // Removed: converted to state variable
  
  // 内部実装: ビデオ送信チャネル開設
  bool OpenVideoTransmitChannel();
  
  // *** FIX 1: FastStartビデオチャンネルの積極的使用 ***
  bool HasActiveFastStartVideoChannel() const;
  void StartVideoProcessingFromFastStart();
  void SetVideoChannelEstablished(bool established);
  
  // ミリ秒時刻ユーティリティ
  static uint64_t NowMs();
  
  // *** ITU-T H.323 (2022) COMPLIANCE: OLC Direction & RTP Session Validation ***
  void ValidateOLCDirectionCompliance(H323Channel::Directions direction, unsigned sessionID);
  
  // *** CRITICAL: H.264 Event Processing Bridge Functions ***
  void ProcessLogicalChannelOpenedEvent(const H323Channel & channel, const H245_OpenLogicalChannel & olc);
  void InitiateQt6VideoDisplay(const H323Channel & channel);
  PString GetMediaTypeFromDataType(const H245_DataType & dataType);
  
  // *** ITU-T COMPLIANCE: Empty Capability Set Prevention ***
  virtual void OnSendCapabilitySet(H245_TerminalCapabilitySet & pdu);
  void ValidateCapabilitySetNotEmpty(H245_TerminalCapabilitySet & pdu);
  
  // *** ITU-T COMPLIANCE: Mandatory G.711 Audio & H.241 Profile Declaration ***
  void EnsureMandatoryG711Audio(H245_TerminalCapabilitySet & pdu);
  void ConfigureH241ProfileLevelId(H323VideoCapability * h264Cap);
  void SetH241MaxMBPSMaxFS(H323VideoCapability * h264Cap);
  
  // *** COMPREHENSIVE EVENT INTEGRATION: Additional Helper Functions ***
  void RecordOLCEvent(const H323Channel & channel, const char* eventName);
  void UpdateH245TruthFromChannel(const H323Channel & channel);
  void CleanupH245TruthForClosedChannel(unsigned sessionID, H323Channel::Directions direction);
  void ProcessEmbeddedH245Message(const H245_MultimediaSystemControlMessage & h245msg);
  
  // *** SESSION ID MANAGEMENT: Proposed Enhancement Functions ***
  void FixupSessionIDForOLC(H245_OpenLogicalChannel & olc, bool isVideo);
  void SetRtpAddrsToOLC(H245_H2250LogicalChannelParameters & h2250,
                       const PIPSocket::Address & ip, WORD rtp, WORD rtcp);
  enum MediaType { MEDIA_UNKNOWN, MEDIA_AUDIO, MEDIA_VIDEO };
  MyH323Connection::MediaType ClassifyIncomingRTP(WORD dstPort, BYTE rtpPT);
    void ProcessFastStartElements(const H323SignalPDU & pdu);  // 🚀 ENHANCEMENT: H.245 Control PDU monitoring
  
  // Port mapping registration helper
  void RegisterPortMapping(unsigned sessionID);
  
  // 🛡️ FastStart Session Protection Functions
  void ProtectVideoSession(unsigned sessionID = 2);     // Protect video session from early closure
  void UnprotectVideoSession();                         // Release video session protection
  bool IsVideoSessionProtected() const { return m_fastStartVideoSessionGuarded; }
  
  // *** COMPREHENSIVE EVENT INTEGRATION: Call Termination ***
  virtual void OnCleared();
    
    // 🚀 ENHANCEMENT: Channel lifecycle monitoring  
    virtual void OnClosedLogicalChannel(const H323Channel & channel);
    
    // 🚀 ENHANCEMENT: H.264 RTP processing for display
    void ProcessH264RTPForDisplay(const RTP_DataFrame & frame, unsigned sessionID);

    CallDetail details;
    
    // *** H.245 Reception Session Truth Table - FIXED IMPLEMENTATION ***
    struct H245SessionTruthEntry {
        unsigned sessionID;
        unsigned dynamicPayloadType;  // Negotiated PT from H.245 (NOT fixed 96!)
        PString remoteRTPAddress;     // IP:port for RTP
        PString remoteRTCPAddress;    // IP:port for RTCP
        DWORD remoteSSRC;            // Remote synchronization source
        PString codecName;           // H.264, H.263, etc.
        PString mediaType;           // "video", "audio"
        PBoolean isReceiver;
        PBoolean isTransmitter;
        PTime establishedTime;
        
        H245SessionTruthEntry() : sessionID(0), dynamicPayloadType(0), remoteSSRC(0), 
                                 isReceiver(FALSE), isTransmitter(FALSE) {}
    };
    
    // H.245 Session Truth Table
    std::map<unsigned, H245SessionTruthEntry> m_h245TruthTable;
    
    // ★★★ PayloadType同期: 相手からのRX OLCで取得したPTをTXでも使用 ★★★
    // Polycomなどの端末は、自分のRX OLCで指定したPTでパケットを受け取ることを期待する
    // 例: PolycomがRX OLC(Session 2)でPT 109を指定 → H323ASKWのTXでもPT 109を使用
    std::map<unsigned, int> m_remoteRxPayloadType;  // sessionID -> remote's RX PT
    int GetRemoteRxPayloadType(unsigned sessionID) const;
    void SetRemoteRxPayloadType(unsigned sessionID, int pt);
    
    // RFC 6184 H.264 Depacketizer Implementation
    class RFC6184Depacketizer {
    private:
        struct RTPPacketInfo {
            uint32_t timestamp;
            uint16_t sequenceNumber;
            bool markerBit;
            std::vector<uint8_t> payload;
            
            RTPPacketInfo() : timestamp(0), sequenceNumber(0), markerBit(false) {}
        };
        
        struct AccessUnit {
            uint32_t timestamp;
            std::vector<uint8_t> nalUnits;  // Complete NAL units with 00 00 00 01 start codes
            bool hasParams;                 // Has SPS/PPS
            bool hasSlice;                  // Has slice NAL
            
            AccessUnit() : timestamp(0), hasParams(false), hasSlice(false) {}
        };
        
        struct FragmentationUnit {
            uint8_t nalType;
            std::vector<uint8_t> nalData;
            bool isComplete;
            uint16_t startSeq;
            uint16_t lastSeq;
            
            FragmentationUnit() : nalType(0), isComplete(false), startSeq(0), lastSeq(0) {}
        };
        
        // Timestamp-based packet buffering (90kHz units)
        std::map<uint32_t, std::vector<RTPPacketInfo>> m_packetBuffer;
        
        // FU-A reassembly state
        std::map<uint32_t, FragmentationUnit> m_fuAssembly;
        
        // SPS/PPS cache for parameter sets (ENHANCED RFC 6184)
        std::vector<uint8_t> m_sps;
        std::vector<uint8_t> m_pps;
        bool m_spsValid;
        bool m_ppsValid;
        bool m_hasSPS;
        bool m_hasPPS;
        bool m_hasValidParams;
        bool insert_sps_pps_before_idr = true;  // IDRの直前にSPS/PPSを差し込む
        
        // Diagnostic metrics
        struct Metrics {
            uint32_t rtpPacketsReceived;
            uint32_t nalsAssembled;
            uint32_t ausCompleted;
            uint32_t packetsDiscarded;
            uint32_t fuDropped;
            uint32_t stapErrors;
            
            Metrics() : rtpPacketsReceived(0), nalsAssembled(0), ausCompleted(0),
                       packetsDiscarded(0), fuDropped(0), stapErrors(0) {}
        } m_metrics;
        
        PTime m_lastMetricsLog;
        unsigned m_sessionID;
        
    public:
        RFC6184Depacketizer(MyH323Connection* connection, unsigned sessionID);
        
        // Main processing function
        bool ProcessRTPPacket(const uint8_t* rtpPayload, size_t payloadSize, 
                             uint32_t timestamp, uint16_t sequenceNumber, bool markerBit);
        
        // Access unit processing
        bool ProcessAccessUnit(uint32_t timestamp);
        
        // Access unit completion check
        bool IsAccessUnitComplete(uint32_t timestamp);
        
        // Get completed access unit
        std::vector<uint8_t> GetCompletedAccessUnit(uint32_t timestamp);
        
        // Parameter set management
        void UpdateParameterSets(const std::vector<uint8_t>& nalUnit);
        std::vector<uint8_t> PrependParameterSets(const std::vector<uint8_t>& accessUnit);
        
        // Diagnostic functions
        void LogMetrics();
        void ResetMetrics();
        
    private:
        // NAL unit type detection
        uint8_t GetNALType(const uint8_t* nalData, size_t size);
        
        // Single NAL unit processing (types 1-23)
        void ProcessSingleNAL(const RTPPacketInfo& packet, std::vector<uint8_t>& auBuffer);
        
        // STAP-A processing (type 24)
        bool ProcessSTAPA(const RTPPacketInfo& packet, std::vector<uint8_t>& auBuffer);
        
        // FU-A processing (type 28)
        bool ProcessFUA(const RTPPacketInfo& packet, uint32_t timestamp, std::vector<uint8_t>& auBuffer);
        
        // Utility functions
        void AddStartCode(std::vector<uint8_t>& buffer);
        bool IsParameterSet(uint8_t nalType);
        bool IsSliceNAL(uint8_t nalType);
        bool IsIDRFrame(uint8_t nalType);
        void DiscardIncompleteAU(uint32_t timestamp, const char* reason);
        
        // *** TASK 3: SPS/PPS Parameter Set Management (ENHANCED) ***
        void ParseSPSInfo(const std::vector<uint8_t>& sps);
        void HandleIDRFrame();
        void RequestParameterSets();
        void cache_sps_pps_if_needed(uint8_t nal_type, const uint8_t* nal, size_t n);
        void maybe_prepend_sps_pps_for_idr();
        
        // *** TASK 4: H.264 Decoder Integration ***
        PBoolean DecodeAccessUnit(const std::vector<uint8_t>& accessUnit);
        void InitializeDecoderContext();
        PBoolean SendToQt6Display(const uint8_t* yuvData, unsigned width, unsigned height);
        
        // Connection reference for decoder access
        MyH323Connection* m_connection;
    };
    
    // RFC 6184 Depacketizer instance for this connection
    std::map<unsigned, std::unique_ptr<RFC6184Depacketizer>> m_h264Depacketizers;
    
    // OLD MediaSessionInfo struct (keeping for backward compatibility)
    struct MediaSessionInfo {
        unsigned sessionID;
        PString mediaType;          // "video", "audio", "data"
        PString codecName;          // "H.264", "H.263", "PCMA", etc.
        unsigned payloadType;       // Dynamic PT (96-127) for video
        PString remoteAddress;      // Remote RTP IP:port
        PBoolean isReceiver;        // TRUE for receive channel
        PBoolean isTransmitter;     // TRUE for transmit channel
        PTime establishedTime;      // When OLC was established
        
        MediaSessionInfo() : sessionID(0), payloadType(0), isReceiver(FALSE), isTransmitter(FALSE) {}
    };
    
    // Session mapping: sessionID -> MediaSessionInfo (Legacy)
    std::map<unsigned, MediaSessionInfo> m_sessionMap;
    
    // *** VIDEO SESSION TRACKING VARIABLES ***
    unsigned m_videoSessionID;
    PBoolean m_videoChannelActive;
    unsigned m_contentSessionID;
    PBoolean m_contentChannelActive;
    
    // *** H.245 Truth Table Management Methods ***
    void RecordH245SessionTruth(unsigned sessionID, unsigned dynamicPT, 
                               const PString& remoteRTP, const PString& remoteRTCP,
                               DWORD ssrc, const PString& codec, const PString& mediaType,
                               PBoolean isReceiver, PBoolean isTransmitter);
    
    void UpdateH245TruthFromOLC(const H245_OpenLogicalChannel & olc, unsigned sessionID);
    void UpdateH245TruthFromOLCAck(const H245_OpenLogicalChannelAck & ack, unsigned sessionID);
    
    H245SessionTruthEntry* FindVideoReceiveSession();
    H245SessionTruthEntry* FindSessionByID(unsigned sessionID);
    bool IsValidVideoPayloadType(unsigned payloadType) const;
    void DumpH245TruthTable() const;
    
    // *** RFC 6184 Integration Methods ***
    void InitializeH264Depacketizer(unsigned sessionID);
    void ProcessIncomingRTPWithDepacketizer(RTP_Session* rtpSession, unsigned sessionID);
    void OnH264AccessUnitReady(const std::vector<uint8_t>& accessUnit);
    
    // Helper methods for session management (Legacy compatibility)
    void RecordVideoSession(unsigned sessionID, const PString& codecName, unsigned payloadType, const PString& remoteAddr, PBoolean isReceiver);
    void DumpAllSessions() const;
    MediaSessionInfo* FindVideoSession();  // Find any video session
    PBoolean IsVideoSession(unsigned sessionID) const;
    
    // NEW: Video session tracking via OLC analysis
    void AnalyzeOpenLogicalChannel(const H245_OpenLogicalChannel & olc, unsigned sessionID);
    void AnalyzeOpenLogicalChannelAck(const H245_OpenLogicalChannelAck & ack, unsigned sessionID);
    
    // NEW: RTP session processing with payload type filtering
    void ProcessRTPSessionWithFiltering(RTP_Session* rtpSession, unsigned sessionID, unsigned expectedPT = 0);

    // *** TASK 3: COMPREHENSIVE SELF-TEST SYSTEM ***
    struct SelfTestMetrics {
        // H.245 negotiation verification
        bool videoOLCSent;
        bool videoOLCAckReceived;
        bool audioOLCSent;
        bool audioOLCAckReceived;
        PTime negotiationStart;
        PTime negotiationComplete;
        
        // Packet arrival confirmation
        uint32_t totalRTPPacketsReceived;
        uint32_t videoPacketsReceived;
        uint32_t audioPacketsReceived;
        uint32_t payloadTypeMismatches;
        PTime firstPacketTime;
        PTime lastPacketTime;
        
        // Payload type filter matching
        bool depacketizerActivated;
        uint32_t framesProcessed;
        uint32_t idrFramesFound;
        
        // IDR→decode→display timing
        PTime lastIDRReceived;
        PTime lastDecodeComplete;
        PTime lastDisplayTime;
        double avgDecodeTime;  // milliseconds
        double avgDisplayTime; // milliseconds
        uint32_t timingMeasurements;
        
        SelfTestMetrics() : videoOLCSent(false), videoOLCAckReceived(false),
                           audioOLCSent(false), audioOLCAckReceived(false),
                           totalRTPPacketsReceived(0), videoPacketsReceived(0),
                           audioPacketsReceived(0), payloadTypeMismatches(0),
                           depacketizerActivated(false), framesProcessed(0),
                           idrFramesFound(0), avgDecodeTime(0.0), avgDisplayTime(0.0),
                           timingMeasurements(0) {}
    } m_selfTestMetrics;
    
    // Self-test diagnostic functions
    void RecordOLCEvent(const PString& eventType, unsigned sessionID, const PString& mediaType);
    void RecordRTPPacket(unsigned payloadType, unsigned sessionID, size_t packetSize);
    PBoolean IsVideoPayloadType(unsigned payloadType, unsigned sessionID); // 包括的PayloadType判定
    void RecordDepacketizerActivation();
    void RecordFrameProcessing(bool isIDR);
    void RecordDecodeComplete();
    void RecordDisplayTime();
    void PrintSelfTestSummary();
    bool ValidatePipelineHealth();
    
    // *** RFC 4585 RTCP FEEDBACK SUPPORT - MINIMUM IMPLEMENTATION ***
    class RTCPFeedbackManager {
    public:
        RTCPFeedbackManager(MyH323Connection* connection);
        
        // Core RFC 4585 feedback functions
        void SendNACK(uint16_t lostSeq, uint16_t blpMask = 0);         // Generic NACK
        void SendPLI();                                                 // Picture Loss Indication  
        void SendFIR();                                                 // Full Intra Request
        
        // Packet loss detection and management
        void OnRTPPacketReceived(uint16_t seq, uint32_t timestamp);
        void DetectPacketLoss(uint16_t currentSeq);
        bool IsInLostSequenceRange(uint16_t seq);
        
        // Frame damage detection
        void OnFrameDecodeFailed(uint32_t timestamp, const char* reason);
        void OnAccessUnitIncomplete(uint32_t timestamp);
        
        // Rate limiting for feedback messages
        bool CanSendNACK();
        bool CanSendPLI();  
        bool CanSendFIR();
        
        // Statistics and logging
        void LogFeedbackStats();
        void ResetStats();
        
        // Configuration
        void SetNACKCooldown(unsigned ms) { m_nackCooldownMs = ms; }
        void SetPLICooldown(unsigned ms) { m_pliCooldownMs = ms; }
        void SetFIRCooldown(unsigned ms) { m_firCooldownMs = ms; }
        
    private:
        MyH323Connection* m_connection;
        
        // Sequence number tracking
        uint16_t m_lastReceivedSeq;
        std::set<uint16_t> m_lostSequences;
        uint32_t m_lastReceivedTimestamp;
        
        // Rate limiting timers
        PTime m_lastNACKSent;
        PTime m_lastPLISent;
        PTime m_lastFIRSent;
        
        // Cooldown periods (configurable)
        unsigned m_nackCooldownMs;
        unsigned m_pliCooldownMs;
        unsigned m_firCooldownMs;
        
        // Statistics
        struct FeedbackStats {
            unsigned nacksSent;
            unsigned plissSent;
            unsigned firsSent;
            unsigned packetsLost;
            unsigned framesLost;
            PTime startTime;
            
            FeedbackStats() : nacksSent(0), plissSent(0), firsSent(0), 
                             packetsLost(0), framesLost(0) {}
        } m_stats;
        
        // Helper methods
        uint16_t CalculateSequenceGap(uint16_t from, uint16_t to);
        uint16_t CreateBitmaskForLostPackets(uint16_t baseSeq);
        void LogNACK(uint16_t seq, uint16_t blp);
        void LogPLI();
        void LogFIR();
    };
    
    // RFC 4585 Feedback manager instance
    std::unique_ptr<RTCPFeedbackManager> m_rtcpFeedback;
    
    // RTP frame processing handled via OnReceiveRTPPacket

  protected:
    MyH323EndPoint & endpoint;
    PVideoChannel * videoChannelIn;
    PVideoChannel * contentChannelIn;   // H.239コンテンツ用受信チャンネル
    PVideoChannel * videoChannelOut;
#if defined(USE_QT6)
    class Qt6VideoOutputDevice * outgoingVideoDisplay;   // For displaying outgoing video frames (Qt6)
    class Qt6VideoOutputDevice * incomingVideoDisplay;   // For displaying incoming video frames (Qt6)
#else
    PVideoOutputDevice * outgoingVideoDisplay;           // Generic video output
    PVideoOutputDevice * incomingVideoDisplay;           // Generic video output
#endif
#ifdef H323_VIDEO
    H323VideoCodec * m_activeVideoEncoder;  // Track active video encoder for frame transmission
    // H.264 preview decoder for outgoing video
    H323VideoCodec * m_previewDecoder;
    PBYTEArray m_previewBuffer;
#endif
    
    // 🛡️ THREAD SAFETY FIX: Instance-specific counters to prevent data races
    unsigned m_displayIncomingFrameCount = 0;  // Frame count for DisplayVideoFrame()
    unsigned m_rtcpSdesCounter = 0;            // RTCP SDES counter for PeriodicRTCPTransmission()
    
    map<unsigned, WORD> m_sessionPorts;
    bool m_isH239ready;
    bool m_haveStartedH239;
    unsigned m_h239TargetKbps = 540; // H.239送信用に直近で設定したターゲット帯域
    PTimer m_h239StartTimer;
    PTimer m_h239StopTimer;
    // H.239送信開始の遅延・リトライ制御（Peopleチャネル確立待ち）
    bool m_h239StartPending = false;
    unsigned m_h239StartRetryCount = 0;
    PTimer m_h239StartRetryTimer;
    
    // RequestMode state management
    unsigned m_requestModeSequence;
    bool m_requestModeInProgress;
    PTime m_requestModeStartTime;
    unsigned m_requestModeRetryCount;
    PTimer m_requestModeTimer;  // Timer for RequestMode timeout and retry
    
    // P1: Complete sequence ordering state management
    bool m_tcsAckPending;          // TCS Ack pending flag
    bool m_tcsAckReceived;         // TCS Ack received flag
    bool m_requestModeOrdered;     // CRITICAL: RequestMode ordered flag
    unsigned m_requestModeAttempt; // CRITICAL: RequestMode attempt counter
    
    // P3: FastUpdatePicture I-frame request state management
    PTimer m_fastUpdateTimer;       // Timer for delayed FastUpdatePicture sending
    unsigned m_fastUpdateLogicalChannel;  // Target logical channel for FastUpdatePicture
    
    // P4: RTCP RR/SDES transmission state management  
    PTimer m_rtcpTimer;             // Timer for periodic RTCP transmission
    PIPSocket::Address m_mcuRTCPAddress; // MCU RTCP address for transmission
    WORD m_mcuRTCPPort;             // MCU RTCP port for transmission
    bool m_rtcpTransmissionActive;  // RTCP transmission active flag
    bool m_masterSlaveComplete;    // Master/Slave determination complete
    PTime m_tcsAckReceivedTime;    // When TCS Ack was received
    PTimer m_cooldownTimer;        // Cooldown timer after TCS Ack
    PTimer m_fallbackTimer;        // CRITICAL FIX: Fallback timer for P2-P3 execution
    PTimer m_delayedVideoChannelTimer; // Timer for delayed H.264 channel opening
    PTimer m_videoFallbackTimer; // Timer for video codec fallback check
    PTimer m_aggressiveVideoRequestTimer; // Timer for aggressive video receive request
    PTimer m_forceSlowStartVideoTimer; // Timer for Force Slow Start active video OLC sending
    int m_proactiveVideoRetryCount; // Retry count for proactive video channel request
    PBoolean m_forceSlowStartMode; // Flag indicating Force Slow Start mode (-f option)
    DWORD m_lastVideoSSRC; // Last SSRC for video RTP activity detection
    PTimer m_bidirectionalVideoTimer; // Timer for MCU sessionID-compliant bidirectional video
    PTimer m_rtpMonitorTimer;      // Timer for periodic RTP statistics monitoring and video detection
    PTimer m_polycomRequestModeTimer; // 🚀 Timer for Polycom bidirectional video RequestMode
    PBoolean m_videoFallbackAttempted; // Flag to prevent multiple fallback attempts
    
    // *** CRITICAL: Store MCU-assigned sessionIDs for bidirectional use ***
    unsigned mcuAudioSessionID;  // Audio sessionID provided by MCU
    unsigned mcuVideoSessionID;  // Video sessionID provided by MCU
    PBoolean hasMCUVideoSessionID; // Flag to track if we received MCU's video sessionID
    
    // *** CRITICAL H.264 DECODER FIX: Store decoder reference ***
    H323VideoCodec* m_activeVideoDecoder;  // Active H.264 decoder for incoming video
    
    // *** RTP SESSION MONITORING: Direct RTP packet monitoring ***
    RTP_Session* m_videoRTPSession;        // RTP session for video monitoring
    
    // ==== Video OLC arrival watchdog (after RequestMode ACK) ====
    PTimer m_videoOlcWaitTimer;
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, VideoOlcWaitTimeout);
    
    // （任意）ACKを見たかの簡易フラグ（デバッグ補助）
    bool m_modeAckSeen;
    
    // ✅ CRITICAL FIX 2: H.245 Fallback Automation State Management
    bool m_fastStartSucceeded;           // Track FastStart success/failure
    bool m_h245TunnelEstablished;        // Track H.245 tunnel establishment
    bool m_needsH245Fallback;            // Flag when separate H.245 needed
    bool m_h245FallbackInProgress;       // Prevent multiple fallback attempts
    unsigned m_h245RetryCount;           // H.245 retry counter (Fix 2)
    unsigned m_maxH245Retries;           // Maximum H.245 retry attempts (Fix 2)
    PTimer m_fastStartTimeoutTimer;      // FastStart failure detection
    PTimer m_h245FallbackTimer;          // Automatic H.245 fallback
    PTimer m_olcSequenceTimer;           // OLC sequence enforcement
    PTimer m_msdTimeoutTimer;            // MSD timeout timer (Fix 2)
    PTimer m_tcsResendTimer;             // TCS resend timer (Fix 2)
    
    // *** OLC RETRY AND TIMEOUT MANAGEMENT ***
    unsigned m_olcRetryCount;            // OLC retry counter
    unsigned m_maxOlcRetries;            // Maximum OLC retry attempts
    PTimer m_olcRetryTimer;              // OLC retry timer
    PTimer m_olcTimeoutTimer;            // OLC response timeout timer
    bool m_olcRetryInProgress;           // Flag to prevent multiple OLC retries
    
    // Timer callback functions for automated fallback
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, OnFastStartTimeout);
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, OnH245FallbackTimeout);
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, OnOlcTimeout);
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, OnOlcRetryTimeout);
    
    // 直近に受信したOLC RequestをchannelNumber→種別(video/audio)で覚えておく（Reject/Ack対応づけ用）
    std::map<unsigned, PString> m_lastOlcKindByNumber;
    std::map<unsigned, unsigned> m_lastOlcSessionByNumber;
    std::map<unsigned, bool>     m_olcRetriedByNumber; // invalidSessionIDで1回だけ再送したか
    void RetryOpenLogicalChannelWithAlternateSession(unsigned chNo, const PString & kind);
    
    // FIR送り（即時＋バースト＋周期）
    PTimer m_fastUpdateKickTimer;
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, SendFastUpdateKick);
    unsigned m_firBurstCount = 0;
    static const unsigned kFirBurstMax = 3;     // 3発バースト

    // RequestMode リトライ（連続タイマ）
    PTimer m_modeResendTimer;
    unsigned m_modeResendCount = 0;
    static const unsigned kModeResendLimit = 3; // 2〜3回で十分
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, ModeResendTick);
    
    // TCS 定期キック（5秒毎）
    PTimer m_tcsKickTimer;
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, TcsKickTick);

    // *** RTP PACKET POLLING: Timer for polling RTP packets from audio session ***
    PTimer m_rtpPollTimer;
    PDECLARE_NOTIFIER(PTimer, MyH323Connection, PollRTPPacketsTick);

    // *** H.245 CHANNEL ESTABLISHMENT GUARANTEE: Timers for ensuring H.245 negotiation starts ***
    PTimer m_h245MonitorTimer;    // Monitors H.245 negotiation progress
    PTimer m_h245RetryTimer;      // Retries H.245 negotiation if needed
    PTimer m_connectionHealthTimer; // Connection health monitoring timer
    PTime m_h245StartTime;        // When H.245 negotiation was initiated
    
    // *** DUPLICATE OLC PREVENTION: Flag to prevent multiple Video TX OLC attempts ***
    bool m_videoTxOlcAttempted;   // Track if Video TX OLC has been attempted
    PMutex m_olcMutex;            // Mutex for OLC synchronization between threads
    
    void SendRequestModeVideoSendReceive();   // 明示的に video を要求
    void SendFastUpdatePicture();             // FIR（MiscellaneousCommand）を送る
    void SendTerminalCapabilitySetH264();     // TCSを明示送出（H.264/H.241込み）
    void StopAllKickers();                    // RX確立時に全停止
  public:
    static void AnalyzeMcuRejection(const PString& pduType, const H323ControlPDU& pdu); // MCU拒否理由解析 (static版)
    
    // *** Force Slow Start Mode Accessor ***
    PBoolean IsForceSlowStartMode() const { return m_forceSlowStartMode; }
  protected:

    // === Canonical RTP session IDs ===
    static const unsigned AUDIO_SESSION_ID = 1;
    static const unsigned VIDEO_SESSION_ID = 2;
    static const unsigned CONTENT_SESSION_ID = 32;  // H.239 Content/Presentation (RFC 4796)

    unsigned GetCanonicalSessionId(const H323Capability & cap, H323Channel::Directions dir) const;
    void ForceSessionId(H245_H2250LogicalChannelParameters & h2250, unsigned sessId, const char* whereTag);
    
    // ASN.1オブジェクトの生ダンプ用静的メソッド
public:
    static void DumpPASN(const char* who, const PASN_Object& obj);
    
    // 🚨 CRITICAL FIX: State machine variables for OLC management  
    bool openVideoTxOnce = false;        // True when video TX OLC has been attempted
    bool videoTxReady = false;           // True when video TX OLC ACK received
    bool videoTxRejected = false;        // True when video TX OLC rejected  
    PString videoTxRejectedCause = "";   // Reject cause description
    
    // *** FASTSTART MANAGEMENT: FastStart状態管理 ***
    bool m_fastStartAudioOpened = false;  // Audio channel opened via FastStart
    bool m_fastStartVideoOpened = false;  // Video channel opened via FastStart
    bool m_hasAnyFastStartChannel = false; // Any FastStart channel opened (prevents Slow-Start)
    bool m_fastStartSelected = false;      // FastStart channels were selected (for Strategy A)
    
    // *** THREAD SAFETY: RTP session access synchronization ***
    std::mutex m_rtpMutex;  // Mutex for synchronizing RTP session access
    
    // *** RTP PACKET POLLING: Poll RTP packets from audio session for video processing ***
    void PollRTPPackets();
    void PollSessionPackets(RTP_Session* session, const char* sessionType);
    
    // Track registered sessions per connection to avoid data races
    std::set<unsigned> m_registeredSessions;
    
    // *** CONNECTION-SPECIFIC STATE VARIABLES: Global state moved to per-connection ***
    bool m_videoEstablishedByRTP = false;           // Video channel established by RTP detection
    mutable PBoolean m_forceDecoderTriggered = FALSE; // Force decoder trigger flag (mutable for const functions)
    bool m_pt0VideoDetected = false;                // G.711でビデオを誤送信されたか
    mutable unsigned m_videoStatCount = 0;          // RTP統計の表示カウンタ (mutable for const functions)
    
    // 🛡️ FastStart Session Protection Variables
    bool m_fastStartVideoSessionGuarded = false;   // Flag to track if video session 2 is protected
    RTP_Session* m_protectedVideoSession = nullptr; // Pointer to protected video session
    bool m_videoSessionSecured = false;             // Additional flag for extra protection tracking
    
    // ============================================================
    // Phase 1 Stability Improvements: Unified State Management
    // ============================================================
    
    /**
     * m_h245State - H.245ネゴシエーション状態マシン
     * 
     * 従来の多数のboolフラグを統一的に管理:
     * - m_tcsAckReceived -> m_h245State.state() >= AwaitingMSD
     * - m_fastStartVideoOpened -> m_h245State.videoState() == FastStartOpened
     * - openVideoTxOnce -> m_h245State.isVideoOLCSent()
     * - videoTxReady -> m_h245State.isVideoReady()
     */
    H245StateMachine m_h245State;
    
    /**
     * m_connState - 接続スコープの状態管理
     * 
     * グローバル変数を接続ごとの状態に置き換え:
     * - g_acceptedVideoChannels -> m_connState.channels().hasVideo()
     * - g_acceptedAudioChannels -> m_connState.channels().hasAudio()
     * - gSessionMap -> m_connState.sessions()
     */
    ConnectionScopedState m_connState;
    
    // Helper methods for state machine integration
    void syncLegacyFlagsFromStateMachine();
    void syncStateMachineFromLegacyFlags();
    
    // ============================================================
    // H.245 Mute Feature: logicalChannelActive/Inactive signaling
    // ============================================================
    
    /**
     * m_localMicMuted - ローカルマイクのミュート状態
     * true = ミュート中 (logicalChannelInactive送信済み)
     * false = アンミュート中 (logicalChannelActive送信済み)
     */
    std::atomic<bool> m_localMicMuted{false};
    
    /**
     * m_remoteMicMuted - 相手側マイクのミュート状態
     * true = 相手がミュート中 (logicalChannelInactive受信)
     * false = 相手がアンミュート中 (logicalChannelActive受信)
     */
    std::atomic<bool> m_remoteMicMuted{false};
    
    /**
     * m_localCameraMuted - ローカルカメラのミュート状態
     * true = カメラ停止中 (黒画面を送信)
     * false = カメラ有効 (通常映像を送信)
     */
    std::atomic<bool> m_localCameraMuted{false};
    
public:
    /**
     * SendLogicalChannelActivity - H.245 MiscellaneousIndication送信
     * 
     * @param active true = logicalChannelActive, false = logicalChannelInactive
     * @return 送信成功の場合true
     */
    bool SendLogicalChannelActivity(bool active);
    
    /**
     * ToggleMicMute - マイクミュート状態の切り替え
     * 
     * 現在の状態を反転し、H.245 Indicationを送信
     */
    void ToggleMicMute();
    
    /**
     * SetMicMuteState - マイクミュート状態を設定
     * 
     * @param newMuted 設定するミュート状態
     * @param signalH245 trueの場合H.245を送信、falseの場合ローカル状態のみ更新
     */
    void SetMicMuteState(bool newMuted, bool signalH245 = true);
    
    /**
     * ToggleCameraMute - カメラミュート状態の切り替え
     * 
     * 現在の状態を反転し、ビデオ送信を停止/再開
     */
    void ToggleCameraMute();
    
    /**
     * ミュート状態アクセサ
     */
    bool IsLocalMicMuted() const { return m_localMicMuted.load(); }
    bool IsRemoteMicMuted() const { return m_remoteMicMuted.load(); }
    bool IsLocalCameraMuted() const { return m_localCameraMuted.load(); }
    
private:
};

///////////////////////////////////////////////////////////////////////////////

class MyH323EndPoint : public H323EndPoint
{
    PCLASSINFO(MyH323EndPoint, H323EndPoint);
  public:
    MyH323EndPoint();
    
    // Find connection by token
    H323Connection* FindConnection(const PString& token) {
      H323ConnectionDict& connDict = GetConnections();
      return connDict.Contains(token) ? &connDict[token] : NULL;
    }
    
    // 一部実装では EndPoint でPDUを横取りできるため、ここでも要約ログを出す
    virtual void OnReceivedControlPDU(
        const H323Connection & connection,
        const H323ControlPDU & pdu
    );

    // override from H323EndPoint
    virtual H323Connection * CreateConnection(unsigned callReference);

    virtual void OnConnectionEstablished(
      H323Connection & connection,    /// Connection that was established
      const PString & token           /// Token for identifying connection
    );
    virtual void OnConnectionCleared(
      H323Connection & connection,    /// Connection that was established
      const PString & token           /// Token for identifying connection
    );
    virtual PBoolean OnStartLogicalChannel(H323Connection & connection, H323Channel & PTRACE_channel);
    virtual PBoolean SetVideoFrameSize(H323Capability::CapabilityFrameSize frameSize, int frameUnits = 1);
    virtual H323Capability::CapabilityFrameSize GetMaxFrameSize() const { return m_maxFrameSize; }

    // TODO: include in codec negotiations, only sets bearer capabilities right now
    void SetPerCallBandwidth(unsigned bw) { m_rateMultiplier = ceil((float)bw / 64); }
    BYTE GetRateMultiplier() const { return m_rateMultiplier; }

    void SetVideoPattern(const PString & pattern, bool isH239 = false) { if (isH239) m_h239videoPattern = pattern; else m_videoPattern = pattern; }
    PString GetVideoPattern(bool isH239) const { return isH239 ? m_h239videoPattern : m_videoPattern; }

    void SetFrameRate(unsigned fps) { m_frameRate = fps; }
    unsigned GetFrameRate() const { return m_frameRate; }

    void SetFuzzing(bool val) { m_fuzzing = val; }
    bool IsFuzzing() const { return m_fuzzing; }
    void SetPercentBadRTPHeader(unsigned val) { m_percentBadRTPHeader = val; }
    unsigned GetPercentBadRTPHeader() const { return m_percentBadRTPHeader; }
    void SetPercentBadRTPMedia(unsigned val) { m_percentBadRTPMedia = val; }
    unsigned GetPercentBadRTPMedia() const { return m_percentBadRTPMedia; }
    void SetPercentBadRTCP(unsigned val) { m_percentBadRTCP = val; }
    unsigned GetPercentBadRTCP() const { return m_percentBadRTCP; }

    void SetStartH239(bool start) { m_startH239 = start; }
    bool IsStartH239() const { return m_startH239; }

    void SetH239Delay(int delay) { m_h239delay = delay; }
    int GetH239Delay() { return m_h239delay; }
    void SetH239Duration(int duration) { m_h239duration = duration; }
    int GetH239Duration() { return m_h239duration; }

    // USB Camera and Qt6 video support
#ifdef H323_VIDEO
    void SetUseUSBCamera(bool enable) { m_useUSBCamera = enable; }
    bool IsUsingUSBCamera() const { return m_useUSBCamera; }
    void SetUSBCameraDeviceName(const PString & deviceName) { m_usbCameraDevice = deviceName; }
    PString GetUSBCameraDeviceName() const { return m_usbCameraDevice; }
    
    // Enhanced Video Display System Support (Qt6)
    void SetUseQt6Display(bool enable) { m_useQt6Display = enable; }
    bool IsUsingQt6Display() const { return m_useQt6Display; }
    
    void SetUseConsoleDisplay(bool enable) { m_useConsoleDisplay = enable; }
    bool IsUsingConsoleDisplay() const { return m_useConsoleDisplay; }
    
    void SetUseMetalDisplay(bool enable) { m_useMetalDisplay = enable; }
    bool IsUsingMetalDisplay() const { return m_useMetalDisplay; }
    
    void SetPreferredVideoDisplaySystem(const PString& system) { m_preferredVideoDisplaySystem = system; }
    PString GetPreferredVideoDisplaySystem() const { return m_preferredVideoDisplaySystem; }
    
    // Auto-detect best available display system
    PString GetBestAvailableDisplaySystem() const;
    
    bool HasVideoSupport() const { return true; }  // Always true when H323_VIDEO is enabled
    
    // Update H.323 codec resolution after USB camera setup
    void UpdateCodecResolutionAfterUSBSetup();
#endif

    // ============================================================
    // Audio Device Configuration
    // ============================================================
    void SetAudioDevices(const PString& inDev, const PString& outDev, bool disable) {
        m_audioInputDevice = inDev;
        m_audioOutputDevice = outDev;
        m_disableAudio = disable;
    }
    const PString& GetAudioInputDevice() const { return m_audioInputDevice; }
    const PString& GetAudioOutputDevice() const { return m_audioOutputDevice; }
    bool IsAudioDisabled() const { return m_disableAudio; }

    // *** SIGNALING ENHANCEMENT: H.241 H.264 Capability Declaration ***
    void BuildH241H264Capabilities();
    
    // H.241 H.264 capability parameters accessors
    unsigned GetH264ProfileIDC() const { return m_h264_profile_idc; }
    unsigned GetH264LevelIDC() const { return m_h264_level_idc; }
    unsigned GetH264MaxFS() const { return m_h264_maxFS; }
    unsigned GetH264MaxMBPS() const { return m_h264_maxMBPS; }
    unsigned GetH264MaxBR() const { return m_h264_maxBR; }

  protected:
    BYTE m_rateMultiplier;
    PString m_videoPattern;
    PString m_h239videoPattern;
    unsigned m_frameRate;
    H323Capability::CapabilityFrameSize m_maxFrameSize;
    bool m_fuzzing;
    unsigned m_percentBadRTPHeader;
    unsigned m_percentBadRTPMedia;
    unsigned m_percentBadRTCP;
    bool m_startH239;
    int m_h239delay;
    int m_h239duration;
#ifdef H323_VIDEO
    bool m_useUSBCamera;
    PString m_usbCameraDevice;
    bool m_useQt6Display;
    bool m_useConsoleDisplay;
    bool m_useMetalDisplay;
    PString m_preferredVideoDisplaySystem;
#endif
    
    // *** Audio Device Configuration ***
    PString m_audioInputDevice;     // Microphone device name
    PString m_audioOutputDevice;    // Speaker device name
    bool m_disableAudio = false;    // Disable audio completely
    
    // *** SIGNALING ENHANCEMENT: H.241 H.264 Capability Parameters ***
    unsigned m_h264_profile_idc;    // H.264 profile identifier (66 = Baseline)
    unsigned m_h264_level_idc;      // H.264 level identifier (31 = Level 3.1)
    unsigned m_h264_maxFS;          // Maximum Frame Size in macroblocks
    unsigned m_h264_maxMBPS;        // Maximum Macroblock Processing Rate
    unsigned m_h264_maxBR;          // Maximum Bit Rate (in 100s of bits/s)
    
    // *** AUTO-LISTENING MODE: Switch to listening after remote disconnect ***
public:
    void SetAutoListenOnDisconnect(bool enable) { m_autoListenOnDisconnect = enable; }
    bool IsAutoListenOnDisconnect() const { return m_autoListenOnDisconnect; }
    void SetListeningMode(bool enable) { m_listeningMode = enable; }
    bool IsListeningMode() const { return m_listeningMode; }
    void SetExitAfterCall(bool enable) { m_exitAfterCall = enable; }  // Exit program after call ends
    bool IsExitAfterCall() const { return m_exitAfterCall; }
    void NotifyDisconnectedByRemote();  // Called when remote party disconnects
    void RequestProgramExit();  // Request clean program exit
    
protected:
    bool m_autoListenOnDisconnect = false;  // Disabled: H.264 plugin has memory issues on reconnect
    std::atomic<bool> m_listeningMode{false};  // Currently in listening mode
    bool m_exitAfterCall = false;  // Exit program after call ends (for -l mode)
};

///////////////////////////////////////////////////////////////////////////////

class H323ASKW;

struct CallParams
{
  CallParams(H323ASKW & app)
    : callgen(app), repeat(0) { }

  H323ASKW & callgen;

  unsigned repeat;
  PTimeInterval tmax_est;
  PTimeInterval tmin_call;
  PTimeInterval tmax_call;
  PTimeInterval tmin_wait;
  PTimeInterval tmax_wait;
};


///////////////////////////////////////////////////////////////////////////////

class CallThread : public PThread
{
  PCLASSINFO(CallThread, PThread);
  public:
    CallThread(
      unsigned index,
      const PStringArray & destinations,
      const CallParams & params
    );
    void Main();
    void Stop();

  protected:
    PStringArray destinations;
    unsigned     index;
    CallParams   params;
    PSyncPoint   exit;
};

PLIST(CallThreadList, CallThread);


///////////////////////////////////////////////////////////////////////////////

class H323ASKW : public PProcess
{
  PCLASSINFO(H323ASKW, PProcess)

  public:
    H323ASKW();
    void Main();
    static H323ASKW & Current() { return (H323ASKW&)PProcess::Current(); }

    PTextFile  cdrFile;

    PSyncPoint threadEnded;
    unsigned   totalAttempts;
    unsigned   totalEstablished;
    PMutex     coutMutex;

  MyH323EndPoint * h323;

  PBoolean Start(const PString & destination, PString & token) {
    return h323->MakeCall(destination, token) != NULL;
  }
  PBoolean Exists(const PString & token) {
    return h323->HasConnection(token);
  }
  PBoolean IsEstablished(const PString & token) {
    return h323->IsConnectionEstablished(token);
  }
  PBoolean Clear(PString & token) {
    return h323->ClearCallSynchronous(token);
  }
  void ClearAll() {
    h323->ClearAllCalls();
  }

  protected:
    PDECLARE_NOTIFIER(PThread, H323ASKW, Cancel);
    PConsoleChannel console;
    CallThreadList threadList;
};

// P4統合: Strategy Pattern for reverseLogicalChannelParameters
namespace P4Strategy {
    
    // 基底戦略インターフェース
    class ReverseChannelParameterStrategy {
    public:
        virtual ~ReverseChannelParameterStrategy() = default;
        virtual bool configureReverseParameters(H245_OpenLogicalChannel& olc, 
                                               const H323Capability& capability, 
                                               unsigned sessionID,
                                               class MyH323Connection* connection) = 0;
        virtual const char* getStrategyName() const = 0;
    };
    
    // 標準ビデオ戦略
    class StandardVideoStrategy : public ReverseChannelParameterStrategy {
    public:
        bool configureReverseParameters(H245_OpenLogicalChannel& olc, 
                                       const H323Capability& capability, 
                                       unsigned sessionID,
                                       class MyH323Connection* connection) override;
        const char* getStrategyName() const override { return "StandardVideo"; }
    };
    
    // 拡張ビデオ戦略（将来の拡張用）
    class EnhancedVideoStrategy : public ReverseChannelParameterStrategy {
    public:
        bool configureReverseParameters(H245_OpenLogicalChannel& olc, 
                                       const H323Capability& capability, 
                                       unsigned sessionID,
                                       class MyH323Connection* connection) override;
        const char* getStrategyName() const override { return "EnhancedVideo"; }
    };
    
    // 音声用戦略
    class AudioStrategy : public ReverseChannelParameterStrategy {
    public:
        bool configureReverseParameters(H245_OpenLogicalChannel& olc, 
                                       const H323Capability& capability, 
                                       unsigned sessionID,
                                       class MyH323Connection* connection) override;
        const char* getStrategyName() const override { return "Audio"; }
    };
    
    // Strategy Pattern管理クラス
    class ParameterStrategyManager {
    public:
        static ParameterStrategyManager& getInstance();
        
        // 戦略の登録と選択
        void registerStrategy(const std::string& name, std::unique_ptr<ReverseChannelParameterStrategy> strategy);
        ReverseChannelParameterStrategy* getStrategy(const H323Capability& capability);
        void setDefaultVideoStrategy(const std::string& strategyName);
        void setDefaultAudioStrategy(const std::string& strategyName);
        
        // 設定ベースの戦略選択
        bool configureParameters(H245_OpenLogicalChannel& olc, 
                                const H323Capability& capability, 
                                unsigned sessionID,
                                class MyH323Connection* connection);
        
    private:
        ParameterStrategyManager();
        ~ParameterStrategyManager() = default;
        
        std::map<std::string, std::unique_ptr<ReverseChannelParameterStrategy>> strategies_;
        std::string defaultVideoStrategy_ = "StandardVideo";
        std::string defaultAudioStrategy_ = "Audio";
    };
}

///////////////////////////////////////////////////////////////////////////////

#endif // H323ASKW_MAIN_H
