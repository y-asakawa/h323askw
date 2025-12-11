/*
 * h245_state_machine.h
 *
 * H.245 Negotiation State Machine for H.323 calls
 *
 * Copyright (c) 2025
 * Part of CallGen323 Phase 1 stability improvements
 * 
 * 状態遷移図:
 * 
 *   [Initial] 
 *       ↓ startNegotiation()
 *   [AwaitingTCS]
 *       ↓ onTCSAckReceived()
 *   [AwaitingMSD]
 *       ↓ onMSDComplete()
 *   [AwaitingChannels]
 *       ↓ onChannelsOpened()
 *   [Established]
 *       ↓ close()
 *   [Closed]
 */

#ifndef H245_STATE_MACHINE_H
#define H245_STATE_MACHINE_H

#include <atomic>
#include <string>
#include <chrono>
#include <ptlib.h>

/**
 * H245StateMachine - H.245ネゴシエーションの状態管理
 * 
 * このクラスは、H.245プロトコルネゴシエーションの状態を
 * スレッドセーフに管理します。
 * 
 * 使用例:
 *   H245StateMachine h245State;
 *   h245State.startNegotiation();
 *   // TCS Ack受信時
 *   h245State.onTCSAckReceived();
 *   // MSD完了時
 *   h245State.onMSDComplete();
 */
class H245StateMachine {
public:
    /**
     * H.245ネゴシエーションの主要状態
     */
    enum class State {
        Initial,           // 初期状態
        AwaitingTCS,       // TCS送信済み、Ack待ち
        AwaitingMSD,       // MasterSlave決定待ち
        AwaitingChannels,  // チャネル開設待ち
        Established,       // 確立完了
        Closing,           // クローズ中
        Closed,            // 終了
        Error              // エラー状態
    };
    
    /**
     * ビデオチャネルの状態（サブ状態マシン）
     */
    enum class VideoChannelState {
        None,              // 未設定
        FastStartOpened,   // FastStartで開設済み
        OLCPending,        // OLC送信準備中
        OLCSent,           // OLC送信済み
        OLCAckReceived,    // OLC Ack受信
        Active,            // アクティブ（送受信可能）
        Rejected,          // 拒否された
        Closed             // クローズ済み
    };
    
    /**
     * オーディオチャネルの状態
     */
    enum class AudioChannelState {
        None,
        FastStartOpened,
        OLCSent,
        OLCAckReceived,
        Active,
        Rejected,
        Closed
    };

public:
    H245StateMachine() 
        : state_(State::Initial)
        , videoState_(VideoChannelState::None)
        , audioState_(AudioChannelState::None)
        , isMaster_(false)
        , fastStartUsed_(false)
        , h245TunnelActive_(false)
    {
        createdTime_ = std::chrono::steady_clock::now();
        PTRACE(3, "H245SM\tState machine created");
    }
    
    ~H245StateMachine() {
        PTRACE(3, "H245SM\tState machine destroyed (final state: " << stateString() << ")");
    }
    
    // コピー禁止
    H245StateMachine(const H245StateMachine&) = delete;
    H245StateMachine& operator=(const H245StateMachine&) = delete;

    // ============================================================
    // 主要状態遷移メソッド
    // ============================================================
    
    /**
     * ネゴシエーション開始
     * Initial -> AwaitingTCS
     */
    bool startNegotiation() {
        return transition(State::Initial, State::AwaitingTCS, "startNegotiation");
    }
    
    /**
     * TCS Ack受信時
     * AwaitingTCS -> AwaitingMSD
     */
    bool onTCSAckReceived() {
        if (transition(State::AwaitingTCS, State::AwaitingMSD, "onTCSAckReceived")) {
            tcsAckTime_ = std::chrono::steady_clock::now();
            return true;
        }
        // Already past TCS phase - that's ok
        State current = state_.load();
        if (current == State::AwaitingMSD || current == State::AwaitingChannels || 
            current == State::Established) {
            PTRACE(3, "H245SM\tonTCSAckReceived: Already past TCS phase (state=" << stateString() << ")");
            return true;
        }
        return false;
    }
    
    /**
     * MasterSlave決定完了時
     * AwaitingMSD -> AwaitingChannels
     */
    bool onMSDComplete(bool isMaster) {
        isMaster_.store(isMaster);
        if (transition(State::AwaitingMSD, State::AwaitingChannels, 
                      isMaster ? "onMSDComplete(Master)" : "onMSDComplete(Slave)")) {
            msdCompleteTime_ = std::chrono::steady_clock::now();
            return true;
        }
        // Already past MSD phase
        State current = state_.load();
        if (current == State::AwaitingChannels || current == State::Established) {
            isMaster_.store(isMaster);
            PTRACE(3, "H245SM\tonMSDComplete: Already past MSD phase");
            return true;
        }
        return false;
    }
    
    /**
     * チャネル開設完了時
     * AwaitingChannels -> Established
     */
    bool onChannelsOpened() {
        if (transition(State::AwaitingChannels, State::Established, "onChannelsOpened")) {
            establishedTime_ = std::chrono::steady_clock::now();
            return true;
        }
        return false;
    }
    
    /**
     * 接続クローズ開始
     */
    bool close() {
        State current = state_.load();
        if (current == State::Closed || current == State::Closing) {
            return false;
        }
        state_.store(State::Closing);
        PTRACE(2, "H245SM\t[close] " << stateToString(current) << " -> Closing");
        return true;
    }
    
    /**
     * クローズ完了
     */
    bool markClosed() {
        State current = state_.load();
        state_.store(State::Closed);
        videoState_.store(VideoChannelState::Closed);
        audioState_.store(AudioChannelState::Closed);
        PTRACE(2, "H245SM\t[markClosed] " << stateToString(current) << " -> Closed");
        return true;
    }
    
    /**
     * エラー状態に設定
     */
    void setError(const std::string& reason) {
        State prev = state_.exchange(State::Error);
        errorReason_ = reason;
        PTRACE(1, "H245SM\t[ERROR] " << stateToString(prev) << " -> Error: " << reason);
    }

    // ============================================================
    // ビデオチャネル状態遷移
    // ============================================================
    
    /**
     * FastStartでビデオチャネルが開いた
     */
    bool openVideoViaFastStart(unsigned sessionID) {
        VideoChannelState expected = VideoChannelState::None;
        if (videoState_.compare_exchange_strong(expected, VideoChannelState::FastStartOpened)) {
            fastStartUsed_.store(true);
            videoSessionID_ = sessionID;
            PTRACE(2, "H245SM\t[Video] None -> FastStartOpened (sessionID=" << sessionID << ")");
            return true;
        }
        // Already opened
        VideoChannelState current = videoState_.load();
        if (current == VideoChannelState::FastStartOpened || current == VideoChannelState::Active) {
            PTRACE(3, "H245SM\t[Video] FastStart skipped - already open");
            return true;
        }
        PTRACE(2, "H245SM\t[Video] FastStart rejected (current=" << videoStateToString(expected) << ")");
        return false;
    }
    
    /**
     * ビデオOLC送信可能かチェック
     */
    bool canSendVideoOLC() const {
        VideoChannelState v = videoState_.load();
        // FastStartで既に開いている場合は送信不要
        if (v == VideoChannelState::FastStartOpened || v == VideoChannelState::Active) {
            return false;
        }
        // OLC送信中または拒否済みの場合も不可
        if (v == VideoChannelState::OLCSent || v == VideoChannelState::OLCPending ||
            v == VideoChannelState::Rejected) {
            return false;
        }
        return v == VideoChannelState::None;
    }
    
    /**
     * ビデオOLC送信準備
     */
    bool prepareVideoOLC() {
        VideoChannelState expected = VideoChannelState::None;
        return videoState_.compare_exchange_strong(expected, VideoChannelState::OLCPending);
    }
    
    /**
     * ビデオOLC送信完了
     */
    bool markVideoOLCSent(unsigned sessionID) {
        VideoChannelState current = videoState_.load();
        // FastStartで既に開いている場合はスキップ
        if (current == VideoChannelState::FastStartOpened || 
            current == VideoChannelState::Active) {
            PTRACE(2, "H245SM\t[Video] OLC skipped (already open via FastStart)");
            return false;
        }
        if (current == VideoChannelState::None || current == VideoChannelState::OLCPending) {
            videoState_.store(VideoChannelState::OLCSent);
            videoSessionID_ = sessionID;
            PTRACE(2, "H245SM\t[Video] -> OLCSent (sessionID=" << sessionID << ")");
            return true;
        }
        PTRACE(2, "H245SM\t[Video] OLC send rejected (current=" << videoStateToString(current) << ")");
        return false;
    }
    
    /**
     * ビデオOLC Ack受信
     */
    bool onVideoOLCAck(unsigned sessionID) {
        VideoChannelState current = videoState_.load();
        if (current == VideoChannelState::OLCSent) {
            videoState_.store(VideoChannelState::OLCAckReceived);
            PTRACE(2, "H245SM\t[Video] OLCSent -> OLCAckReceived (sessionID=" << sessionID << ")");
            return true;
        }
        // FastStartの場合はAckは不要
        if (current == VideoChannelState::FastStartOpened) {
            PTRACE(3, "H245SM\t[Video] OLC Ack ignored (FastStart mode)");
            return true;
        }
        PTRACE(2, "H245SM\t[Video] OLC Ack unexpected (current=" << videoStateToString(current) << ")");
        return false;
    }
    
    /**
     * ビデオチャネルをアクティブ化
     */
    bool activateVideo() {
        VideoChannelState current = videoState_.load();
        if (current == VideoChannelState::OLCAckReceived || 
            current == VideoChannelState::FastStartOpened) {
            videoState_.store(VideoChannelState::Active);
            PTRACE(2, "H245SM\t[Video] " << videoStateToString(current) << " -> Active");
            return true;
        }
        if (current == VideoChannelState::Active) {
            return true; // Already active
        }
        PTRACE(2, "H245SM\t[Video] Cannot activate from state " << videoStateToString(current));
        return false;
    }
    
    /**
     * ビデオOLC拒否された
     */
    bool rejectVideo(const std::string& cause) {
        VideoChannelState current = videoState_.load();
        if (current != VideoChannelState::Active && current != VideoChannelState::Closed) {
            videoState_.store(VideoChannelState::Rejected);
            videoRejectCause_ = cause;
            PTRACE(2, "H245SM\t[Video] " << videoStateToString(current) << " -> Rejected: " << cause);
            return true;
        }
        return false;
    }
    
    /**
     * ビデオチャネルクローズ
     */
    void closeVideo() {
        VideoChannelState prev = videoState_.exchange(VideoChannelState::Closed);
        PTRACE(2, "H245SM\t[Video] " << videoStateToString(prev) << " -> Closed");
    }

    // ============================================================
    // オーディオチャネル状態遷移
    // ============================================================
    
    bool openAudioViaFastStart(unsigned sessionID) {
        AudioChannelState expected = AudioChannelState::None;
        if (audioState_.compare_exchange_strong(expected, AudioChannelState::FastStartOpened)) {
            fastStartUsed_.store(true);
            audioSessionID_ = sessionID;
            PTRACE(2, "H245SM\t[Audio] None -> FastStartOpened (sessionID=" << sessionID << ")");
            return true;
        }
        return audioState_.load() == AudioChannelState::FastStartOpened || 
               audioState_.load() == AudioChannelState::Active;
    }
    
    bool markAudioOLCSent(unsigned sessionID) {
        AudioChannelState current = audioState_.load();
        if (current == AudioChannelState::FastStartOpened || 
            current == AudioChannelState::Active) {
            return false;
        }
        if (current == AudioChannelState::None) {
            audioState_.store(AudioChannelState::OLCSent);
            audioSessionID_ = sessionID;
            PTRACE(2, "H245SM\t[Audio] -> OLCSent (sessionID=" << sessionID << ")");
            return true;
        }
        return false;
    }
    
    bool onAudioOLCAck() {
        AudioChannelState expected = AudioChannelState::OLCSent;
        if (audioState_.compare_exchange_strong(expected, AudioChannelState::OLCAckReceived)) {
            PTRACE(2, "H245SM\t[Audio] OLCSent -> OLCAckReceived");
            return true;
        }
        return audioState_.load() == AudioChannelState::FastStartOpened;
    }
    
    bool activateAudio() {
        AudioChannelState current = audioState_.load();
        if (current == AudioChannelState::OLCAckReceived || 
            current == AudioChannelState::FastStartOpened) {
            audioState_.store(AudioChannelState::Active);
            PTRACE(2, "H245SM\t[Audio] -> Active");
            return true;
        }
        return current == AudioChannelState::Active;
    }
    
    void closeAudio() {
        AudioChannelState prev = audioState_.exchange(AudioChannelState::Closed);
        PTRACE(2, "H245SM\t[Audio] " << audioStateToString(prev) << " -> Closed");
    }

    // ============================================================
    // 状態クエリ
    // ============================================================
    
    State state() const { return state_.load(); }
    VideoChannelState videoState() const { return videoState_.load(); }
    AudioChannelState audioState() const { return audioState_.load(); }
    
    bool isInitial() const { return state_.load() == State::Initial; }
    bool isNegotiating() const {
        State s = state_.load();
        return s == State::AwaitingTCS || s == State::AwaitingMSD || s == State::AwaitingChannels;
    }
    bool isEstablished() const { return state_.load() == State::Established; }
    bool isClosed() const { 
        State s = state_.load();
        return s == State::Closed || s == State::Closing;
    }
    bool hasError() const { return state_.load() == State::Error; }
    
    bool isMaster() const { return isMaster_.load(); }
    bool isFastStartUsed() const { return fastStartUsed_.load(); }
    bool isH245TunnelActive() const { return h245TunnelActive_.load(); }
    
    bool isVideoActive() const { 
        VideoChannelState v = videoState_.load();
        return v == VideoChannelState::Active || v == VideoChannelState::FastStartOpened;
    }
    bool isVideoReady() const {
        VideoChannelState v = videoState_.load();
        return v == VideoChannelState::Active || 
               v == VideoChannelState::FastStartOpened ||
               v == VideoChannelState::OLCAckReceived;
    }
    bool isVideoRejected() const { return videoState_.load() == VideoChannelState::Rejected; }
    bool isVideoOLCSent() const { return videoState_.load() == VideoChannelState::OLCSent; }
    
    bool isAudioActive() const {
        AudioChannelState a = audioState_.load();
        return a == AudioChannelState::Active || a == AudioChannelState::FastStartOpened;
    }
    
    unsigned getVideoSessionID() const { return videoSessionID_; }
    unsigned getAudioSessionID() const { return audioSessionID_; }
    
    // ============================================================
    // 設定メソッド
    // ============================================================
    
    void setH245TunnelActive(bool active) {
        h245TunnelActive_.store(active);
        PTRACE(3, "H245SM\tH.245 Tunnel: " << (active ? "Active" : "Inactive"));
    }
    
    void setFastStartUsed(bool used) {
        fastStartUsed_.store(used);
    }
    
    // ============================================================
    // レガシーフラグからの同期用メソッド（移行期間中のみ使用）
    // ============================================================
    
    /**
     * ビデオがFastStartで開設済みとして設定
     */
    void setVideoFastStartOpened() {
        VideoChannelState current = videoState_.load();
        if (current == VideoChannelState::None || current == VideoChannelState::OLCPending) {
            videoState_.store(VideoChannelState::FastStartOpened);
            fastStartUsed_.store(true);
            PTRACE(3, "H245SM\t[Sync] Video set to FastStartOpened");
        }
    }
    
    /**
     * ビデオOLC Ack受信として設定
     */
    void setVideoOLCAcknowledged() {
        VideoChannelState current = videoState_.load();
        if (current == VideoChannelState::OLCSent || current == VideoChannelState::OLCPending) {
            videoState_.store(VideoChannelState::OLCAckReceived);
            PTRACE(3, "H245SM\t[Sync] Video set to OLCAckReceived");
        }
    }
    
    /**
     * ビデオOLC拒否として設定
     */
    void setVideoOLCRejected() {
        VideoChannelState current = videoState_.load();
        if (current != VideoChannelState::Active && 
            current != VideoChannelState::FastStartOpened &&
            current != VideoChannelState::Closed) {
            videoState_.store(VideoChannelState::Rejected);
            PTRACE(3, "H245SM\t[Sync] Video set to Rejected");
        }
    }

    // ============================================================
    // デバッグ用
    // ============================================================
    
    std::string stateString() const {
        return stateToString(state_.load());
    }
    
    std::string videoStateString() const {
        return videoStateToString(videoState_.load());
    }
    
    std::string audioStateString() const {
        return audioStateToString(audioState_.load());
    }
    
    const std::string& errorReason() const { return errorReason_; }
    const std::string& videoRejectCause() const { return videoRejectCause_; }
    
    void dump() const {
        PTRACE(2, "H245SM\t========== State Machine Dump ==========");
        PTRACE(2, "H245SM\tMain State: " << stateString());
        PTRACE(2, "H245SM\tVideo State: " << videoStateString());
        PTRACE(2, "H245SM\tAudio State: " << audioStateString());
        PTRACE(2, "H245SM\tMaster: " << (isMaster_.load() ? "Yes" : "No"));
        PTRACE(2, "H245SM\tFastStart: " << (fastStartUsed_.load() ? "Used" : "Not used"));
        PTRACE(2, "H245SM\tH.245 Tunnel: " << (h245TunnelActive_.load() ? "Active" : "Inactive"));
        PTRACE(2, "H245SM\tVideo SessionID: " << videoSessionID_);
        PTRACE(2, "H245SM\tAudio SessionID: " << audioSessionID_);
        if (!errorReason_.empty()) {
            PTRACE(2, "H245SM\tError: " << errorReason_);
        }
        if (!videoRejectCause_.empty()) {
            PTRACE(2, "H245SM\tVideo Reject: " << videoRejectCause_);
        }
        PTRACE(2, "H245SM\t=========================================");
    }
    
    // 状態リセット（新しい通話用）
    void reset() {
        state_.store(State::Initial);
        videoState_.store(VideoChannelState::None);
        audioState_.store(AudioChannelState::None);
        isMaster_.store(false);
        fastStartUsed_.store(false);
        h245TunnelActive_.store(false);
        videoSessionID_ = 0;
        audioSessionID_ = 0;
        errorReason_.clear();
        videoRejectCause_.clear();
        createdTime_ = std::chrono::steady_clock::now();
        PTRACE(2, "H245SM\t*** State machine reset ***");
    }

private:
    bool transition(State from, State to, const char* trigger) {
        if (state_.compare_exchange_strong(from, to)) {
            PTRACE(2, "H245SM\t[" << trigger << "] " << stateToString(from) << " -> " << stateToString(to));
            return true;
        }
        State actual = state_.load();
        PTRACE(3, "H245SM\tTransition rejected: " << trigger 
               << " (expected=" << stateToString(from) 
               << ", actual=" << stateToString(actual) << ")");
        return false;
    }
    
    static const char* stateToString(State s) {
        switch (s) {
            case State::Initial: return "Initial";
            case State::AwaitingTCS: return "AwaitingTCS";
            case State::AwaitingMSD: return "AwaitingMSD";
            case State::AwaitingChannels: return "AwaitingChannels";
            case State::Established: return "Established";
            case State::Closing: return "Closing";
            case State::Closed: return "Closed";
            case State::Error: return "Error";
            default: return "Unknown";
        }
    }
    
    static const char* videoStateToString(VideoChannelState s) {
        switch (s) {
            case VideoChannelState::None: return "None";
            case VideoChannelState::FastStartOpened: return "FastStartOpened";
            case VideoChannelState::OLCPending: return "OLCPending";
            case VideoChannelState::OLCSent: return "OLCSent";
            case VideoChannelState::OLCAckReceived: return "OLCAckReceived";
            case VideoChannelState::Active: return "Active";
            case VideoChannelState::Rejected: return "Rejected";
            case VideoChannelState::Closed: return "Closed";
            default: return "Unknown";
        }
    }
    
    static const char* audioStateToString(AudioChannelState s) {
        switch (s) {
            case AudioChannelState::None: return "None";
            case AudioChannelState::FastStartOpened: return "FastStartOpened";
            case AudioChannelState::OLCSent: return "OLCSent";
            case AudioChannelState::OLCAckReceived: return "OLCAckReceived";
            case AudioChannelState::Active: return "Active";
            case AudioChannelState::Rejected: return "Rejected";
            case AudioChannelState::Closed: return "Closed";
            default: return "Unknown";
        }
    }

private:
    std::atomic<State> state_;
    std::atomic<VideoChannelState> videoState_;
    std::atomic<AudioChannelState> audioState_;
    
    std::atomic<bool> isMaster_;
    std::atomic<bool> fastStartUsed_;
    std::atomic<bool> h245TunnelActive_;
    
    unsigned videoSessionID_ = 0;
    unsigned audioSessionID_ = 0;
    
    std::string errorReason_;
    std::string videoRejectCause_;
    
    // タイミング情報
    std::chrono::steady_clock::time_point createdTime_;
    std::chrono::steady_clock::time_point tcsAckTime_;
    std::chrono::steady_clock::time_point msdCompleteTime_;
    std::chrono::steady_clock::time_point establishedTime_;
};

#endif // H245_STATE_MACHINE_H
