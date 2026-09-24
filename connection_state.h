/*
 * connection_state.h
 *
 * Thread-safe connection-scoped state management for H.323 calls
 *
 * Copyright (c) 2025 Yoshifumi Asakawa
 * Part of CallGen323 Phase 1 stability improvements
 * SPDX-License-Identifier: MPL-1.0
 */

#ifndef CONNECTION_STATE_H
#define CONNECTION_STATE_H

#include <mutex>
#include <set>
#include <map>
#include <atomic>
#include <utility>  // for std::pair
#include <ptlib.h>
#include <ptlib/sockets.h>

/**
 * ConnectionScopedState - 接続ごとの状態を管理するスレッドセーフなクラス
 * 
 * 設計原則:
 * - 全てのメンバーはmutexで保護
 * - コピー禁止（各接続に1インスタンス）
 * - 状態遷移は明示的なメソッドでのみ可能
 * 
 * 使用例:
 *   ConnectionScopedState state;
 *   state.channels().addVideo(2);
 *   if (state.channels().hasVideo(2)) { ... }
 */
class ConnectionScopedState {
public:
    /**
     * RTPセッション情報構造体
     */
    struct RtpSessionInfo {
        PIPSocket::Address localIP;
        WORD rtpPort = 0;
        WORD rtcpPort = 0;
        unsigned sessionID = 0;
        unsigned payloadType = 0;
        bool isActive = false;
        PString codecName;
        
        RtpSessionInfo() = default;
        
        RtpSessionInfo(const PIPSocket::Address& ip, WORD rtp, WORD rtcp, unsigned sid)
            : localIP(ip), rtpPort(rtp), rtcpPort(rtcp), sessionID(sid), 
              payloadType(0), isActive(false) {}
    };

    /**
     * AcceptedChannels - チャネル受け入れ状態を管理
     * 
     * スレッドセーフ: 全てのメソッドはミューテックスで保護
     */
    class AcceptedChannels {
    public:
        AcceptedChannels() = default;
        
        // Video channel management
        bool hasVideo(unsigned sessionID) const {
            std::lock_guard<std::mutex> lock(mutex_);
            return video_.find(sessionID) != video_.end();
        }
        
        void addVideo(unsigned sessionID) {
            std::lock_guard<std::mutex> lock(mutex_);
            video_.insert(sessionID);
            PTRACE(3, "ConnState\tVideo channel added: sessionID=" << sessionID 
                   << " (total=" << video_.size() << ")");
        }
        
        void removeVideo(unsigned sessionID) {
            std::lock_guard<std::mutex> lock(mutex_);
            video_.erase(sessionID);
            PTRACE(3, "ConnState\tVideo channel removed: sessionID=" << sessionID);
        }
        
        size_t videoCount() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return video_.size();
        }
        
        std::set<unsigned> videoChannels() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return video_;  // return copy for thread safety
        }
        
        // Audio channel management
        bool hasAudio(unsigned sessionID) const {
            std::lock_guard<std::mutex> lock(mutex_);
            return audio_.find(sessionID) != audio_.end();
        }
        
        void addAudio(unsigned sessionID) {
            std::lock_guard<std::mutex> lock(mutex_);
            audio_.insert(sessionID);
            PTRACE(3, "ConnState\tAudio channel added: sessionID=" << sessionID 
                   << " (total=" << audio_.size() << ")");
        }
        
        void removeAudio(unsigned sessionID) {
            std::lock_guard<std::mutex> lock(mutex_);
            audio_.erase(sessionID);
            PTRACE(3, "ConnState\tAudio channel removed: sessionID=" << sessionID);
        }
        
        size_t audioCount() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return audio_.size();
        }
        
        std::set<unsigned> audioChannels() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return audio_;  // return copy for thread safety
        }
        
        // Check if any channels exist
        bool hasAnyChannel() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return !audio_.empty() || !video_.empty();
        }
        
        // Check if a session ID exists in either audio or video
        bool hasSessionID(unsigned sessionID) const {
            std::lock_guard<std::mutex> lock(mutex_);
            return audio_.find(sessionID) != audio_.end() || 
                   video_.find(sessionID) != video_.end();
        }
        
        // Alias methods for compatibility with syncStateMachineFromLegacyFlags
        void addVideoChannel(unsigned sessionID) { addVideo(sessionID); }
        void addAudioChannel(unsigned sessionID) { addAudio(sessionID); }
        
        // Clear all channels
        void clear() {
            std::lock_guard<std::mutex> lock(mutex_);
            audio_.clear();
            video_.clear();
            PTRACE(3, "ConnState\tAll channels cleared");
        }
        
        // Debug dump
        void dump() const {
            std::lock_guard<std::mutex> lock(mutex_);
            PTRACE(2, "ConnState\t=== Channel State Dump ===");
            PTRACE(2, "ConnState\tAudio channels: " << audio_.size());
            for (auto sid : audio_) {
                PTRACE(2, "ConnState\t  - Audio sessionID=" << sid);
            }
            PTRACE(2, "ConnState\tVideo channels: " << video_.size());
            for (auto sid : video_) {
                PTRACE(2, "ConnState\t  - Video sessionID=" << sid);
            }
        }
        
    private:
        mutable std::mutex mutex_;
        std::set<unsigned> audio_;
        std::set<unsigned> video_;
    };

    /**
     * SessionMap - RTPセッションのマッピングを管理
     * 
     * スレッドセーフ: 全てのメソッドはミューテックスで保護
     */
    class SessionMap {
    public:
        SessionMap() = default;
        
        void registerSession(unsigned sessionID, const RtpSessionInfo& info) {
            std::lock_guard<std::mutex> lock(mutex_);
            sessions_[sessionID] = info;
            if (info.rtpPort != 0) {
                portToSession_[info.rtpPort] = sessionID;
            }
            PTRACE(3, "ConnState\tSession registered: ID=" << sessionID 
                   << " RTP=" << info.rtpPort << " RTCP=" << info.rtcpPort);
        }
        
        void updateSession(unsigned sessionID, const RtpSessionInfo& info) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = sessions_.find(sessionID);
            if (it != sessions_.end()) {
                // Remove old port mapping if port changed
                if (it->second.rtpPort != 0 && it->second.rtpPort != info.rtpPort) {
                    portToSession_.erase(it->second.rtpPort);
                }
            }
            sessions_[sessionID] = info;
            if (info.rtpPort != 0) {
                portToSession_[info.rtpPort] = sessionID;
            }
            PTRACE(3, "ConnState\tSession updated: ID=" << sessionID);
        }
        
        bool hasSession(unsigned sessionID) const {
            std::lock_guard<std::mutex> lock(mutex_);
            return sessions_.find(sessionID) != sessions_.end();
        }
        
        // Returns (found, RtpSessionInfo) - check first element before using second
        std::pair<bool, RtpSessionInfo> getSession(unsigned sessionID) const {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = sessions_.find(sessionID);
            if (it != sessions_.end()) {
                return std::make_pair(true, it->second);
            }
            return std::make_pair(false, RtpSessionInfo());
        }
        
        // Returns (found, sessionID) - check first element before using second
        std::pair<bool, unsigned> getSessionByPort(WORD port) const {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = portToSession_.find(port);
            if (it != portToSession_.end()) {
                return std::make_pair(true, it->second);
            }
            return std::make_pair(false, 0u);
        }
        
        void setSessionActive(unsigned sessionID, bool active) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = sessions_.find(sessionID);
            if (it != sessions_.end()) {
                it->second.isActive = active;
                PTRACE(3, "ConnState\tSession " << sessionID 
                       << " active=" << (active ? "true" : "false"));
            }
        }
        
        void removeSession(unsigned sessionID) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = sessions_.find(sessionID);
            if (it != sessions_.end()) {
                if (it->second.rtpPort != 0) {
                    portToSession_.erase(it->second.rtpPort);
                }
                sessions_.erase(it);
                PTRACE(3, "ConnState\tSession removed: ID=" << sessionID);
            }
        }
        
        void clear() {
            std::lock_guard<std::mutex> lock(mutex_);
            sessions_.clear();
            portToSession_.clear();
            PTRACE(3, "ConnState\tAll sessions cleared");
        }
        
        size_t count() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return sessions_.size();
        }
        
        // Get all session IDs
        std::vector<unsigned> getAllSessionIDs() const {
            std::lock_guard<std::mutex> lock(mutex_);
            std::vector<unsigned> ids;
            ids.reserve(sessions_.size());
            for (const auto& pair : sessions_) {
                ids.push_back(pair.first);
            }
            return ids;
        }
        
        // Debug dump
        void dump() const {
            std::lock_guard<std::mutex> lock(mutex_);
            PTRACE(2, "ConnState\t=== Session Map Dump ===");
            PTRACE(2, "ConnState\tTotal sessions: " << sessions_.size());
            for (const auto& pair : sessions_) {
                const RtpSessionInfo& info = pair.second;
                PTRACE(2, "ConnState\t  SessionID=" << pair.first
                       << " RTP=" << info.rtpPort
                       << " RTCP=" << info.rtcpPort
                       << " PT=" << info.payloadType
                       << " Active=" << (info.isActive ? "Y" : "N")
                       << " Codec=" << info.codecName);
            }
        }
        
    private:
        mutable std::mutex mutex_;
        std::map<unsigned, RtpSessionInfo> sessions_;
        std::map<WORD, unsigned> portToSession_;
    };

    /**
     * NegotiatedPayloadTypes - ペイロードタイプのマッピング
     */
    class NegotiatedPayloadTypes {
    public:
        void setPayloadType(unsigned sessionID, unsigned payloadType) {
            std::lock_guard<std::mutex> lock(mutex_);
            payloadTypes_[sessionID] = payloadType;
            PTRACE(3, "ConnState\tPayload type set: sessionID=" << sessionID 
                   << " PT=" << payloadType);
        }
        
        // Returns (found, payloadType) - check first element before using second
        std::pair<bool, unsigned> getPayloadType(unsigned sessionID) const {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = payloadTypes_.find(sessionID);
            if (it != payloadTypes_.end()) {
                return std::make_pair(true, it->second);
            }
            return std::make_pair(false, 0u);
        }
        
        void clear() {
            std::lock_guard<std::mutex> lock(mutex_);
            payloadTypes_.clear();
        }
        
    private:
        mutable std::mutex mutex_;
        std::map<unsigned, unsigned> payloadTypes_;
    };

public:
    ConnectionScopedState() {
        PTRACE(4, "ConnState\tConnectionScopedState created");
    }
    
    ~ConnectionScopedState() {
        PTRACE(4, "ConnState\tConnectionScopedState destroyed");
    }
    
    // コピー・ムーブ禁止（各接続に1インスタンス）
    ConnectionScopedState(const ConnectionScopedState&) = delete;
    ConnectionScopedState& operator=(const ConnectionScopedState&) = delete;
    ConnectionScopedState(ConnectionScopedState&&) = delete;
    ConnectionScopedState& operator=(ConnectionScopedState&&) = delete;
    
    // アクセサ
    AcceptedChannels& channels() { return channels_; }
    const AcceptedChannels& channels() const { return channels_; }
    
    SessionMap& sessions() { return sessions_; }
    const SessionMap& sessions() const { return sessions_; }
    
    NegotiatedPayloadTypes& payloadTypes() { return payloadTypes_; }
    const NegotiatedPayloadTypes& payloadTypes() const { return payloadTypes_; }
    
    // 全状態をリセット
    void reset() {
        channels_.clear();
        sessions_.clear();
        payloadTypes_.clear();
        PTRACE(2, "ConnState\t*** Connection state fully reset ***");
    }
    
    // デバッグダンプ
    void dump() const {
        PTRACE(2, "ConnState\t========== Connection State Dump ==========");
        channels_.dump();
        sessions_.dump();
        PTRACE(2, "ConnState\t============================================");
    }

private:
    AcceptedChannels channels_;
    SessionMap sessions_;
    NegotiatedPayloadTypes payloadTypes_;
};

#endif // CONNECTION_STATE_H
