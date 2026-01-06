/**
 * @file qt_video_window.cpp
 * @brief Qt6ベースのビデオ表示ウィンドウ実装
 */

#ifdef USE_QT6

#include "qt_video_window.h"
#include <QDebug>
#include <QThread>
#include <QCoreApplication>
#include <QPainterPath>
#include <QPointer>
#include <QResizeEvent>
#include <atomic>
#include <cmath>
#include <cstdlib>  // for _exit()
#include <algorithm>
#include "main.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// フォワード宣言 - main.hのフル インクルードを避ける
class MyH323Connection;
class MyH323EndPoint;

// PTLibのPTRACEを使用するためのインクルード
#include <ptlib.h>

// Qt向けの簡易ログマクロ - PTRACEを使用してログファイルに出力
#define QT_TRACE(level, msg) PTRACE(level, "QtVideo\t" << msg)

///////////////////////////////////////////////////////////////////////////////
// QtVideoWidget Implementation
///////////////////////////////////////////////////////////////////////////////

QtVideoWidget::QtVideoWidget(const QString& title, QWidget* parent)
    : QWidget(parent)
    , m_title(title)
    , m_frameWidth(640)
    , m_frameHeight(480)
    , m_muted(false)
    , m_cameraMuted(false)
    , m_showMuteIcon(true)
    , m_lastFrameTime(0)
    , m_frameUpdateCount(0)
    , m_paintCallCount(0)
    , m_timerCallCount(0)
    , m_needsRepaint(false)
{
    setMinimumSize(320, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setWindowTitle(title);
    
    // 背景色を設定
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    setPalette(pal);

    // タイマーベースの描画更新（33ms = 30fps）
    // QueuedConnectionのイベントオーバーフローを防ぐ
    m_repaintTimer = new QTimer(this);
    m_repaintTimer->setTimerType(Qt::PreciseTimer);
    connect(m_repaintTimer, &QTimer::timeout, this, [this]() {
        if (m_needsRepaint) {
            m_needsRepaint = false;
            // 📺 タイマー描画リクエストをログ（インスタンスごと）
            m_timerCallCount++;
            if (m_timerCallCount % 30 == 1) {
                QT_TRACE(1, "⏱️ Timer update() [" << m_title.toStdString() 
                         << "] visible=" << isVisible()
                         << " count=" << m_timerCallCount);
            }
            update();
        }
    });
    m_repaintTimer->start(33);  // 30fps

    QT_TRACE(1, "QtVideoWidget created: " << title.toStdString());
}

QtVideoWidget::~QtVideoWidget()
{
    QT_TRACE(1, "QtVideoWidget destroyed: " << m_title.toStdString());
}

void QtVideoWidget::updateFrameYUV420P(const unsigned char* yuvData, unsigned width, unsigned height)
{
    if (!yuvData || width == 0 || height == 0) {
        return;
    }

    // フレームレート制限: 過剰な再描画を防ぐ（25fpsに制限）
    qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
    qint64 elapsed = currentTime - m_lastFrameTime;
    
    // 初回は必ず更新（m_lastFrameTime == 0の場合）
    if (m_lastFrameTime != 0 && elapsed < 40) {  // 40ms = 25fps
        return;  // フレームをスキップ
    }
    
    // 📺 デバッグ: フレーム更新の統計（インスタンスごとにカウント）
    m_frameUpdateCount++;
    if (m_frameUpdateCount % 30 == 1) {
        QT_TRACE(1, "📺 updateFrameYUV420P[" << m_title.toStdString() << "]: frame #" << m_frameUpdateCount 
                 << " elapsed=" << elapsed << "ms"
                 << " (" << width << "x" << height << ")");
    }
    
    m_lastFrameTime = currentTime;

    QMutexLocker locker(&m_frameMutex);
    
    m_frameWidth = width;
    m_frameHeight = height;
    
    // 📺 フレームデータ変更検出（最初の16バイトのチェックサム）
    unsigned int checksum = 0;
    for (int i = 0; i < 16 && i < static_cast<int>(width * height); i++) {
        checksum += yuvData[i];
    }
    static unsigned int lastChecksum = 0;
    if (m_frameUpdateCount % 30 == 1) {
        QT_TRACE(1, "📺 YUV checksum[" << m_title.toStdString() << "]: " << checksum 
                 << " (prev=" << lastChecksum << ")"
                 << (checksum == lastChecksum ? " ⚠️ SAME" : " ✅ NEW"));
    }
    lastChecksum = checksum;
    
    // YUV420P → RGB24 変換
    convertYUV420PtoRGB(yuvData, width, height);

    // タイマーでの再描画をリクエスト（イベントオーバーフロー防止）
    m_needsRepaint = true;
}

void QtVideoWidget::updateFrameRGB24(const unsigned char* rgbData, unsigned width, unsigned height)
{
    if (!rgbData || width == 0 || height == 0) {
        return;
    }

    QMutexLocker locker(&m_frameMutex);
    
    m_frameWidth = width;
    m_frameHeight = height;
    
    // RGB24データをQImageに直接コピー（事前確保でcopy()を避ける）
    size_t rgbSize = width * height * 3;
    if (m_currentFrame.width() != static_cast<int>(width) || 
        m_currentFrame.height() != static_cast<int>(height)) {
        m_currentFrame = QImage(width, height, QImage::Format_RGB888);
    }
    memcpy(m_currentFrame.bits(), rgbData, rgbSize);

    // タイマーでの再描画をリクエスト
    m_needsRepaint = true;
}

void QtVideoWidget::convertYUV420PtoRGB(const unsigned char* yuvData, unsigned width, unsigned height)
{
    // YUV420Pのサイズ: Y = width*height, U = width*height/4, V = width*height/4
    size_t ySize = width * height;
    size_t uvSize = ySize / 4;
    
    const unsigned char* yPlane = yuvData;
    const unsigned char* uPlane = yuvData + ySize;
    const unsigned char* vPlane = yuvData + ySize + uvSize;

    // RGB24バッファを確保（既存バッファを再利用）
    size_t rgbSize = width * height * 3;
    if (m_rgbBuffer.size() != static_cast<int>(rgbSize)) {
        m_rgbBuffer.resize(rgbSize);
        // QImageも事前確保（ゼロコピー用）
        m_currentFrame = QImage(width, height, QImage::Format_RGB888);
    }
    unsigned char* rgbData = reinterpret_cast<unsigned char*>(m_rgbBuffer.data());

    // YUV420P → RGB24 変換（SIMD最適化版）
    // NOTE: macOS NV12変換後のI420では Cb=U, Cr=V の順序
    for (unsigned y = 0; y < height; ++y) {
        const unsigned char* yRow = yPlane + y * width;
        const unsigned char* uRow = uPlane + (y / 2) * (width / 2);
        const unsigned char* vRow = vPlane + (y / 2) * (width / 2);
        unsigned char* rgbRow = rgbData + y * width * 3;
        
        for (unsigned x = 0; x < width; ++x) {
            int Y = yRow[x];
            int U = uRow[x / 2] - 128;
            int V = vRow[x / 2] - 128;

            // YUV → RGB 変換（ITU-R BT.601）
            // 紫色問題の修正: U/Vの係数を正しく適用
            int R = Y + ((351 * V) >> 8);
            int G = Y - ((179 * V + 86 * U) >> 8);
            int B = Y + ((443 * U) >> 8);

            // クランプ
            R = qBound(0, R, 255);
            G = qBound(0, G, 255);
            B = qBound(0, B, 255);

            rgbRow[x * 3 + 0] = static_cast<unsigned char>(R);
            rgbRow[x * 3 + 1] = static_cast<unsigned char>(G);
            rgbRow[x * 3 + 2] = static_cast<unsigned char>(B);
        }
    }

    // 事前確保済みQImageにデータをコピー（copy()を避ける）
    if (m_currentFrame.width() == static_cast<int>(width) && 
        m_currentFrame.height() == static_cast<int>(height)) {
        // 直接メモリコピー（高速）
        memcpy(m_currentFrame.bits(), rgbData, rgbSize);
    } else {
        // サイズ変更時のみ再作成
        m_currentFrame = QImage(rgbData, width, height, width * 3, QImage::Format_RGB888).copy();
    }
}

void QtVideoWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    
    // 📺 paintEventログ（インスタンスごと）
    m_paintCallCount++;
    if (m_paintCallCount % 30 == 1) {
        QT_TRACE(1, "🎨 paintEvent[" << m_title.toStdString() << "]: count=" << m_paintCallCount
                 << " frame=" << (m_currentFrame.isNull() ? "NULL" : "OK")
                 << " size=" << m_currentFrame.width() << "x" << m_currentFrame.height());
    }
    
    QPainter painter(this);
    // 高速描画モード（スムーズ変換を無効化してパフォーマンス優先）
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);

    QMutexLocker locker(&m_frameMutex);
    
    // 📹 カメラミュート時は黒画面 + カメラOFFアイコン
    if (m_cameraMuted) {
        painter.fillRect(rect(), Qt::black);
        
        // カメラOFFアイコンを中央に描画
        int iconSize = qMin(width(), height()) / 4;
        int cx = width() / 2;
        int cy = height() / 2;
        
        // 背景円
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(QColor(100, 100, 100, 200));
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(cx - iconSize/2, cy - iconSize/2, iconSize, iconSize);
        
        // カメラアイコン（斜線付き）
        painter.setPen(QPen(Qt::white, 3));
        int camW = iconSize * 0.5;
        int camH = iconSize * 0.35;
        int camX = cx - camW/2 - iconSize*0.1;
        int camY = cy - camH/2;
        painter.drawRect(camX, camY, camW, camH);
        
        // レンズ部分（三角）
        QPolygon lens;
        lens << QPoint(camX + camW, cy - camH/3)
             << QPoint(camX + camW + iconSize*0.2, cy - camH/2)
             << QPoint(camX + camW + iconSize*0.2, cy + camH/2)
             << QPoint(camX + camW, cy + camH/3);
        painter.drawPolyline(lens);
        
        // 斜線（禁止マーク）
        painter.setPen(QPen(QColor(255, 80, 80), 4));
        int slashOffset = iconSize * 0.35;
        painter.drawLine(cx - slashOffset, cy - slashOffset, cx + slashOffset, cy + slashOffset);
        
        // テキスト
        painter.setPen(Qt::gray);
        QFont font = painter.font();
        font.setPointSize(12);
        painter.setFont(font);
        painter.drawText(rect().adjusted(0, cy + iconSize/2 + 10, 0, 0), Qt::AlignHCenter | Qt::AlignTop, "Camera OFF");
        return;
    }
    
    if (m_currentFrame.isNull()) {
        // フレームがない場合は黒背景 + タイトル表示
        painter.fillRect(rect(), Qt::black);
        painter.setPen(Qt::gray);
        painter.drawText(rect(), Qt::AlignCenter, m_title + "\n(No Signal)");
    } else {
        // 黒背景
        painter.fillRect(rect(), Qt::black);
        
        // キャッシュされた描画領域を使用（カクカク防止）
        // サイズが変わった場合のみ再計算
        QSize frameSize = m_currentFrame.size();
        QSize widgetSize = size();
        if (m_cachedFrameSize != frameSize || m_cachedWidgetSize != widgetSize) {
            updateTargetRect();
        }
        
        // drawImageでスケーリング（Qtが内部で最適化）
        painter.drawImage(m_targetRect, m_currentFrame);
    }
    
    // 🎙️ ミュートアイコンを左上に描画
    if (m_showMuteIcon && !m_currentFrame.isNull()) {
        drawMicrophoneIcon(painter, 10, 10, 36, m_muted);
    }
}

void QtVideoWidget::updateTargetRect()
{
    QSize frameSize = m_currentFrame.size();
    QSize widgetSize = size();
    QSize scaledSize = frameSize.scaled(widgetSize, Qt::KeepAspectRatio);
    
    int x = (widgetSize.width() - scaledSize.width()) / 2;
    int y = (widgetSize.height() - scaledSize.height()) / 2;
    m_targetRect = QRect(x, y, scaledSize.width(), scaledSize.height());
    m_cachedFrameSize = frameSize;
    m_cachedWidgetSize = widgetSize;
}

void QtVideoWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    // リサイズ時に描画領域を再計算
    if (!m_currentFrame.isNull()) {
        updateTargetRect();
    }
}

QSize QtVideoWidget::sizeHint() const
{
    return QSize(m_frameWidth, m_frameHeight);
}

QSize QtVideoWidget::minimumSizeHint() const
{
    return QSize(320, 240);
}

void QtVideoWidget::setMuted(bool muted)
{
    if (m_muted != muted) {
        m_muted = muted;
        update();  // 再描画をトリガー
    }
}

void QtVideoWidget::setCameraMuted(bool muted)
{
    if (m_cameraMuted != muted) {
        m_cameraMuted = muted;
        QT_TRACE(1, "QtVideoWidget::setCameraMuted: " << (muted ? "ON (black screen)" : "OFF (normal)"));
        update();  // 再描画をトリガー
    }
}

void QtVideoWidget::drawMicrophoneIcon(QPainter& painter, int x, int y, int size, bool muted)
{
    // 背景を半透明の丸で描画
    painter.setRenderHint(QPainter::Antialiasing);
    QColor bgColor = muted ? QColor(200, 0, 0, 180) : QColor(0, 180, 0, 180);
    painter.setBrush(bgColor);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(x, y, size, size);
    
    // マイクアイコンを描画
    QColor iconColor = Qt::white;
    painter.setPen(QPen(iconColor, 2));
    painter.setBrush(Qt::NoBrush);
    
    int cx = x + size / 2;  // 中心X
    int cy = y + size / 2;  // 中心Y
    
    // マイクのヘッド部分（楕円）
    int micWidth = size / 4;
    int micHeight = size / 3;
    QRect micHead(cx - micWidth / 2, cy - micHeight / 2 - 3, micWidth, micHeight);
    painter.drawRoundedRect(micHead, micWidth / 2, micWidth / 2);
    
    // マイクスタンド（U字型）
    int standY = cy + micHeight / 2 - 5;
    int standWidth = micWidth + 4;
    QPainterPath standPath;
    standPath.moveTo(cx - standWidth / 2, standY - 4);
    standPath.arcTo(cx - standWidth / 2, standY - 4, standWidth, standWidth / 2 + 6, 180, -180);
    painter.drawPath(standPath);
    
    // 支柱
    painter.drawLine(cx, standY + standWidth / 4 + 2, cx, cy + size / 3 - 2);
    
    // 台座
    int baseWidth = size / 3;
    painter.drawLine(cx - baseWidth / 2, cy + size / 3 - 2, cx + baseWidth / 2, cy + size / 3 - 2);
    
    // ミュート時は斜線を描画
    if (muted) {
        painter.setPen(QPen(Qt::white, 3));
        painter.drawLine(x + 4, y + size - 4, x + size - 4, y + 4);
        painter.setPen(QPen(QColor(200, 0, 0), 2));
        painter.drawLine(x + 4, y + size - 4, x + size - 4, y + 4);
    }
    
    // 白い枠線
    painter.setPen(QPen(Qt::white, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(x, y, size, size);
}

///////////////////////////////////////////////////////////////////////////////
// QtAudioSpectrumWidget Implementation
///////////////////////////////////////////////////////////////////////////////

QtAudioSpectrumWidget::QtAudioSpectrumWidget(QWidget* parent)
    : QWidget(parent)
    , m_bandCount(64)
    , m_barColor(QColor(0, 200, 100))      // 緑色のバー
    , m_peakColor(QColor(255, 100, 100))   // 赤色のピーク
    , m_bgColor(QColor(30, 30, 30))        // 暗いグレー背景
    , m_peakHoldEnabled(true)
    , m_timerId(0)
    , m_decayRate(0.92f)                   // 減衰率
    , m_peakHoldTime(15)                   // ピークホールド時間（フレーム数）
{
    // バンドデータの初期化
    m_bandLevels.resize(m_bandCount);
    m_peakLevels.resize(m_bandCount);
    m_peakHoldCounters.resize(m_bandCount);
    
    for (int i = 0; i < m_bandCount; i++) {
        m_bandLevels[i] = 0.0f;
        m_peakLevels[i] = 0.0f;
        m_peakHoldCounters[i] = 0;
    }
    
    // FFTバッファ初期化（512サンプル）
    const int fftSize = 512;
    m_fftSize = fftSize;
    m_fftBuffer.resize(fftSize);
    m_fftReal.resize(fftSize);
    m_fftImag.resize(fftSize);
    m_fftMagnitude.resize(fftSize / 2);
    m_sampleBuffer.resize(fftSize);
    m_sampleBufferPos = 0;
    
    // ハミング窓の初期化
    m_window.resize(fftSize);
    for (int i = 0; i < fftSize; i++) {
        m_window[i] = 0.54f - 0.46f * cos(2.0f * M_PI * i / (fftSize - 1));
    }
    
    // アニメーションタイマー開始（30fps）
    m_timerId = startTimer(33);
    
    // 最小サイズ設定
    setMinimumSize(200, 60);
    setMaximumHeight(80);
}

QtAudioSpectrumWidget::~QtAudioSpectrumWidget()
{
    if (m_timerId != 0) {
        killTimer(m_timerId);
    }
}

void QtAudioSpectrumWidget::setBandCount(int count)
{
    QMutexLocker locker(&m_dataMutex);
    m_bandCount = qBound(4, count, 64);
    m_bandLevels.resize(m_bandCount);
    m_peakLevels.resize(m_bandCount);
    m_peakHoldCounters.resize(m_bandCount);
    
    for (int i = 0; i < m_bandCount; i++) {
        m_bandLevels[i] = 0.0f;
        m_peakLevels[i] = 0.0f;
        m_peakHoldCounters[i] = 0;
    }
}

void QtAudioSpectrumWidget::updateSpectrum(const int16_t* pcmData, size_t sampleCount, int channels, int sampleRate)
{
    Q_UNUSED(sampleRate);
    
    if (!pcmData || sampleCount == 0) return;
    
    QMutexLocker locker(&m_dataMutex);
    
    // サンプルをバッファに蓄積
    for (size_t i = 0; i < sampleCount; i++) {
        // ステレオの場合は左チャンネルのみ使用
        int sampleIdx = i * channels;
        if (sampleIdx < (int)(sampleCount * channels)) {
            m_sampleBuffer[m_sampleBufferPos] = pcmData[sampleIdx];
            m_sampleBufferPos++;
            
            // バッファが埋まったらFFTを実行
            if (m_sampleBufferPos >= m_fftSize) {
                performFFT(m_sampleBuffer.data(), m_fftSize, 1);
                m_sampleBufferPos = 0;
            }
        }
    }
}

// Cooley-Tukey FFT (in-place, radix-2)
static void fftRadix2(float* real, float* imag, int n)
{
    // ビット反転並べ替え
    int j = 0;
    for (int i = 0; i < n - 1; i++) {
        if (i < j) {
            std::swap(real[i], real[j]);
            std::swap(imag[i], imag[j]);
        }
        int k = n >> 1;
        while (k <= j) {
            j -= k;
            k >>= 1;
        }
        j += k;
    }
    
    // FFT バタフライ演算
    for (int len = 2; len <= n; len <<= 1) {
        float angle = -2.0f * M_PI / len;
        float wReal = cos(angle);
        float wImag = sin(angle);
        
        for (int i = 0; i < n; i += len) {
            float curReal = 1.0f;
            float curImag = 0.0f;
            
            for (int k = 0; k < len / 2; k++) {
                int idx1 = i + k;
                int idx2 = i + k + len / 2;
                
                float tReal = curReal * real[idx2] - curImag * imag[idx2];
                float tImag = curReal * imag[idx2] + curImag * real[idx2];
                
                real[idx2] = real[idx1] - tReal;
                imag[idx2] = imag[idx1] - tImag;
                real[idx1] += tReal;
                imag[idx1] += tImag;
                
                float newReal = curReal * wReal - curImag * wImag;
                float newImag = curReal * wImag + curImag * wReal;
                curReal = newReal;
                curImag = newImag;
            }
        }
    }
}

void QtAudioSpectrumWidget::performFFT(const int16_t* pcmData, size_t sampleCount, int channels)
{
    // FFTサイズを確保（512サンプル）
    const int fftSize = 512;
    const int sampleRate = 16000;  // H.323音声は通常16kHz
    
    // バッファサイズ確認
    if (m_fftReal.size() != fftSize) {
        m_fftReal.resize(fftSize);
        m_fftImag.resize(fftSize);
        m_fftMagnitude.resize(fftSize / 2);
    }
    
    // PCMデータをFFTバッファにコピー（窓関数適用）
    size_t copySize = qMin(sampleCount, (size_t)fftSize);
    for (size_t i = 0; i < (size_t)fftSize; i++) {
        if (i < copySize) {
            int sampleIdx = i * channels;  // ステレオの場合は左チャンネル
            if (sampleIdx < (int)sampleCount) {
                m_fftReal[i] = (pcmData[sampleIdx] / 32768.0f) * m_window[i];
            } else {
                m_fftReal[i] = 0.0f;
            }
        } else {
            m_fftReal[i] = 0.0f;  // ゼロパディング
        }
        m_fftImag[i] = 0.0f;
    }
    
    // FFT実行
    fftRadix2(m_fftReal.data(), m_fftImag.data(), fftSize);
    
    // 振幅スペクトル計算（DC〜ナイキスト周波数）
    for (int i = 0; i < fftSize / 2; i++) {
        m_fftMagnitude[i] = sqrt(m_fftReal[i] * m_fftReal[i] + m_fftImag[i] * m_fftImag[i]);
    }
    
    // 各バンドの周波数範囲を計算（対数スケール）
    // 80Hz〜8kHz（ナイキスト周波数）を対数分割 - DC/低周波ノイズを除外
    float minFreq = 80.0f;   // 80Hz以下のDC/ノイズを除外
    float maxFreq = sampleRate / 2.0f;  // 8kHz
    
    for (int band = 0; band < m_bandCount; band++) {
        // 対数スケールで周波数範囲を計算
        float startFreq = minFreq * pow(maxFreq / minFreq, (float)band / m_bandCount);
        float endFreq = minFreq * pow(maxFreq / minFreq, (float)(band + 1) / m_bandCount);
        
        // 周波数をFFTビンインデックスに変換
        int startBin = (int)(startFreq * fftSize / sampleRate);
        int endBin = (int)(endFreq * fftSize / sampleRate);
        
        startBin = qBound(3, startBin, fftSize / 2 - 1);  // DC成分と低周波ノイズを除外（bin 0,1,2を除外）
        endBin = qBound(startBin + 1, endBin, fftSize / 2);
        
        // このバンドの平均振幅を計算
        float sum = 0.0f;
        int count = 0;
        for (int bin = startBin; bin < endBin; bin++) {
            sum += m_fftMagnitude[bin];
            count++;
        }
        
        if (count > 0) {
            float avgMagnitude = sum / count;
            // dBスケールに変換（振幅を2倍してFFT正規化）
            // ゲインブースト: 16倍 (24dB) に増幅して感度を上げる
            float db = 20.0f * log10f(qMax(avgMagnitude * 32.0f / fftSize, 0.00001f));
            // dBレンジを-36dB〜0dBに変更（さらに感度アップ）
            float normalized = (db + 36.0f) / 36.0f;  // -36dB to 0dB -> 0.0 to 1.0
            normalized = qBound(0.0f, normalized, 1.0f);
            
            // 上昇は即座に、下降は減衰
            if (normalized > m_bandLevels[band]) {
                m_bandLevels[band] = normalized;
            }
        }
    }
}

void QtAudioSpectrumWidget::updatePeaks()
{
    for (int i = 0; i < m_bandCount; i++) {
        if (m_bandLevels[i] >= m_peakLevels[i]) {
            m_peakLevels[i] = m_bandLevels[i];
            m_peakHoldCounters[i] = m_peakHoldTime;
        } else {
            if (m_peakHoldCounters[i] > 0) {
                m_peakHoldCounters[i]--;
            } else {
                m_peakLevels[i] *= m_decayRate;
            }
        }
    }
}

void QtAudioSpectrumWidget::decayBars()
{
    for (int i = 0; i < m_bandCount; i++) {
        m_bandLevels[i] *= m_decayRate;
    }
}

void QtAudioSpectrumWidget::timerEvent(QTimerEvent* event)
{
    if (event->timerId() == m_timerId) {
        QMutexLocker locker(&m_dataMutex);
        decayBars();
        updatePeaks();
        update();  // 再描画
    }
}

void QtAudioSpectrumWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    
    // 背景
    painter.fillRect(rect(), m_bgColor);
    
    QMutexLocker locker(&m_dataMutex);
    
    int w = width();
    int h = height();
    int margin = 2;
    int barSpacing = 2;
    int totalSpacing = (m_bandCount - 1) * barSpacing + margin * 2;
    int barWidth = (w - totalSpacing) / m_bandCount;
    
    if (barWidth < 2) barWidth = 2;
    
    for (int i = 0; i < m_bandCount; i++) {
        int x = margin + i * (barWidth + barSpacing);
        int barHeight = (int)(m_bandLevels[i] * (h - margin * 2));
        int y = h - margin - barHeight;
        
        // グラデーション（下から緑→黄→赤）暗めのトーン
        QLinearGradient gradient(x, h - margin, x, margin);
        gradient.setColorAt(0.0, QColor(0, 120, 60));    // 暗い緑（低レベル）
        gradient.setColorAt(0.5, QColor(180, 150, 0));   // 暗い黄色（中レベル）
        gradient.setColorAt(1.0, QColor(180, 30, 30));   // 暗い赤（高レベル）
        
        painter.setBrush(gradient);
        painter.setPen(Qt::NoPen);
        painter.drawRect(x, y, barWidth, barHeight);
        
        // ピークインジケーター
        if (m_peakHoldEnabled && m_peakLevels[i] > 0.01f) {
            int peakY = h - margin - (int)(m_peakLevels[i] * (h - margin * 2));
            painter.setBrush(m_peakColor);
            painter.drawRect(x, peakY - 2, barWidth, 3);
        }
    }
    
    // 枠線
    painter.setPen(QPen(QColor(60, 60, 60), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(0, 0, w - 1, h - 1);
}

///////////////////////////////////////////////////////////////////////////////
// QtVideoMainWindow Implementation
///////////////////////////////////////////////////////////////////////////////

QtVideoMainWindow::QtVideoMainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_localVideo(nullptr)
    , m_remoteVideo(nullptr)
    , m_videoSplitter(nullptr)
    , m_separateButton(nullptr)
    , m_windowsSeparated(false)
    , m_addressCombo(nullptr)
    , m_connectButton(nullptr)
    , m_disconnectButton(nullptr)
    , m_muteCheckbox(nullptr)
    , m_cameraCheckbox(nullptr)
    , m_contentButton(nullptr)
    , m_contentSendButton(nullptr)
    , m_micCombo(nullptr)
    , m_speakerCombo(nullptr)
    , m_cameraCombo(nullptr)
    , m_statusLabel(nullptr)
    , m_localSpectrum(nullptr)
    , m_remoteSpectrum(nullptr)
    , m_h323Connection(nullptr)
    , m_running(true)
{
    setWindowTitle("H323ASKW - Video Client");
    setMinimumSize(800, 600);

    setupUI();
    setupConnections();
    populateDeviceLists();

    QT_TRACE(1, "QtVideoMainWindow created");
}

QtVideoMainWindow::~QtVideoMainWindow()
{
    // 終了時に履歴を保存
    saveAddressHistory();
    m_running = false;
    QT_TRACE(1, "QtVideoMainWindow destroyed");
}

void QtVideoMainWindow::setupUI()
{
    QWidget* centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    QVBoxLayout* mainLayout = new QVBoxLayout(centralWidget);

    // ビデオ表示エリア（横並び）- QSplitterでサイズ調整可能
    m_videoSplitter = new QSplitter(Qt::Horizontal, this);
    m_videoSplitter->setChildrenCollapsible(false);  // 子ウィジェットを完全に折り畳まない
    
    m_localVideo = new QtVideoWidget("Local Preview", m_videoSplitter);
    m_remoteVideo = new QtVideoWidget("Remote Video", m_videoSplitter);
    
    m_videoSplitter->addWidget(m_localVideo);
    m_videoSplitter->addWidget(m_remoteVideo);
    
    // 初期サイズは同じ割合
    m_videoSplitter->setSizes({1, 1});
    
    // スプリッターのハンドルを大きくして操作しやすく
    m_videoSplitter->setHandleWidth(8);
    m_videoSplitter->setStyleSheet(
        "QSplitter::handle {"
        "    background-color: #555;"
        "    border-radius: 2px;"
        "}"
        "QSplitter::handle:hover {"
        "    background-color: #888;"
        "}"
    );
    
    mainLayout->addWidget(m_videoSplitter, 1);  // stretch factor = 1

    // オーディオスペクトラム表示エリア（横並び）
    QHBoxLayout* spectrumLayout = new QHBoxLayout();
    
    // ローカル音声（マイク入力）のスペクトラム
    QVBoxLayout* localSpectrumBox = new QVBoxLayout();
    QLabel* localSpecLabel = new QLabel("🎤 Mic Input", this);
    localSpecLabel->setStyleSheet("color: #888; font-size: 10px;");
    localSpecLabel->setAlignment(Qt::AlignCenter);
    m_localSpectrum = new QtAudioSpectrumWidget(this);
    m_localSpectrum->setBarColor(QColor(0, 180, 255));  // 青系
    localSpectrumBox->addWidget(localSpecLabel);
    localSpectrumBox->addWidget(m_localSpectrum);
    
    // リモート音声（相手の声）のスペクトラム
    QVBoxLayout* remoteSpectrumBox = new QVBoxLayout();
    QLabel* remoteSpecLabel = new QLabel("🔊 Remote Audio", this);
    remoteSpecLabel->setStyleSheet("color: #888; font-size: 10px;");
    remoteSpecLabel->setAlignment(Qt::AlignCenter);
    m_remoteSpectrum = new QtAudioSpectrumWidget(this);
    m_remoteSpectrum->setBarColor(QColor(0, 200, 100));  // 緑系
    remoteSpectrumBox->addWidget(remoteSpecLabel);
    remoteSpectrumBox->addWidget(m_remoteSpectrum);
    
    spectrumLayout->addLayout(localSpectrumBox);
    spectrumLayout->addLayout(remoteSpectrumBox);
    
    mainLayout->addLayout(spectrumLayout);

    // コントロールパネル
    QHBoxLayout* controlLayout = new QHBoxLayout();

    // 接続先入力（履歴付きコンボボックス）
    QLabel* addressLabel = new QLabel("Address:", this);
    m_addressCombo = new QComboBox(this);
    m_addressCombo->setEditable(true);
    m_addressCombo->setInsertPolicy(QComboBox::NoInsert);  // 自動挿入しない（手動管理）
    m_addressCombo->lineEdit()->setPlaceholderText("IP or H.323 alias");
    m_addressCombo->setMinimumWidth(250);
    m_addressCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    
    // 履歴を読み込み
    loadAddressHistory();

    m_connectButton = new QPushButton("Connect", this);
    m_connectButton->setObjectName("connectButton");
    m_disconnectButton = new QPushButton("Disconnect", this);
    m_disconnectButton->setObjectName("disconnectButton");
    m_disconnectButton->setEnabled(false);

    controlLayout->addWidget(addressLabel);
    controlLayout->addWidget(m_addressCombo);
    controlLayout->addWidget(m_connectButton);
    controlLayout->addWidget(m_disconnectButton);
    controlLayout->addStretch();

    // ミュート/カメラ
    m_muteCheckbox = new QCheckBox("Mute Mic", this);
    m_cameraCheckbox = new QCheckBox("Camera Off", this);
    m_muteCheckbox->setEnabled(false);
    m_cameraCheckbox->setEnabled(false);
    
    controlLayout->addWidget(m_muteCheckbox);
    controlLayout->addWidget(m_cameraCheckbox);

    // ウィンドウ分離ボタン
    m_separateButton = new QPushButton("↗ Separate", this);
    m_separateButton->setToolTip("Separate local and remote video into different windows");
    controlLayout->addWidget(m_separateButton);
    
    // コンテンツ再表示ボタン
    m_contentButton = new QPushButton("Content", this);
    m_contentButton->setToolTip("Show H.239 content window");
    m_contentButton->setEnabled(false); // 初期は無効（コンテンツ到着後に有効化）
    controlLayout->addWidget(m_contentButton);

    // コンテンツ送信ボタン
    m_contentSendButton = new QPushButton("Send Content", this);
    m_contentSendButton->setToolTip("Select a window to share via H.239");
    m_contentSendButton->setEnabled(false); // 接続後に有効化
    controlLayout->addWidget(m_contentSendButton);

    mainLayout->addLayout(controlLayout);

    // デバイス選択パネル
    QHBoxLayout* deviceLayout = new QHBoxLayout();

    deviceLayout->addWidget(new QLabel("Mic:", this));
    m_micCombo = new QComboBox(this);
    m_micCombo->setMinimumWidth(150);
    deviceLayout->addWidget(m_micCombo);

    deviceLayout->addWidget(new QLabel("Speaker:", this));
    m_speakerCombo = new QComboBox(this);
    m_speakerCombo->setMinimumWidth(150);
    deviceLayout->addWidget(m_speakerCombo);

    deviceLayout->addWidget(new QLabel("Camera:", this));
    m_cameraCombo = new QComboBox(this);
    m_cameraCombo->setMinimumWidth(150);
    deviceLayout->addWidget(m_cameraCombo);

    deviceLayout->addStretch();

    mainLayout->addLayout(deviceLayout);

    // ステータスバー
    m_statusLabel = new QLabel("Ready", this);
    statusBar()->addPermanentWidget(m_statusLabel);
}

void QtVideoMainWindow::setupConnections()
{
    // ボタン接続
    connect(m_connectButton, &QPushButton::clicked, this, &QtVideoMainWindow::onConnectClicked);
    connect(m_disconnectButton, &QPushButton::clicked, this, &QtVideoMainWindow::onDisconnectClicked);
    connect(m_addressCombo, QOverload<int>::of(&QComboBox::activated),
            this, &QtVideoMainWindow::onAddressActivated);
    
    // チェックボックス
    connect(m_muteCheckbox, &QCheckBox::toggled, this, &QtVideoMainWindow::onMuteToggled);
    connect(m_cameraCheckbox, &QCheckBox::toggled, this, &QtVideoMainWindow::onCameraToggled);

    // ウィンドウ分離ボタン
    connect(m_separateButton, &QPushButton::clicked, this, &QtVideoMainWindow::onSeparateWindowsClicked);
    // コンテンツ再表示ボタン
    connect(m_contentButton, &QPushButton::clicked, this, &QtVideoMainWindow::onContentWindowClicked);
    // コンテンツ送信ボタン
    connect(m_contentSendButton, &QPushButton::clicked, this, &QtVideoMainWindow::onContentSendClicked);

    // デバイス選択
    connect(m_micCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), 
            this, &QtVideoMainWindow::onMicDeviceChanged);
    connect(m_speakerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), 
            this, &QtVideoMainWindow::onSpeakerDeviceChanged);
    connect(m_cameraCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), 
            this, &QtVideoMainWindow::onCameraDeviceChanged);

    // フレーム受信（スレッドセーフ）
    connect(this, &QtVideoMainWindow::localFrameReady, 
            this, &QtVideoMainWindow::onLocalFrameReady, Qt::QueuedConnection);
    connect(this, &QtVideoMainWindow::remoteFrameReady, 
            this, &QtVideoMainWindow::onRemoteFrameReady, Qt::QueuedConnection);
}

void QtVideoMainWindow::populateDeviceLists()
{
    QtVideoManager& manager = QtVideoManager::instance();

    auto setItems = [](QComboBox* combo, const QStringList& list, const QString& fallback) {
        combo->clear();
        if (!list.isEmpty()) {
            combo->addItems(list);
        } else {
            combo->addItem(fallback);
        }
    };

    QStringList micList;
    QStringList speakerList;
    QStringList cameraList;

    manager.getDeviceList(QT_DEVICE_TYPE_MIC, micList);
    manager.getDeviceList(QT_DEVICE_TYPE_SPEAKER, speakerList);
    manager.getDeviceList(QT_DEVICE_TYPE_CAMERA, cameraList);

    setItems(m_micCombo, micList, "Default Microphone");
    setItems(m_speakerCombo, speakerList, "Default Speaker");
    setItems(m_cameraCombo, cameraList, "Default Camera");

    QT_TRACE(1, "Device lists populated (Mic=" << micList.size()
             << ", Spk=" << speakerList.size()
             << ", Cam=" << cameraList.size() << ")");

    // 初期選択をエンドポイントに反映
    QtVideoManager::instance().applyDeviceSelection(
        m_micCombo->currentText(),
        m_speakerCombo->currentText(),
        m_cameraCombo->currentText());
}

void QtVideoMainWindow::setH323Connection(MyH323Connection* connection)
{
    // スレッドセーフ: H.245スレッドから呼ばれる可能性があるため、
    // UIウィジェットへのアクセスはメインスレッドで行う
    auto app = QCoreApplication::instance();
    if (!app) {
        QT_TRACE(1, "WARNING: QCoreApplication is null in setH323Connection");
        return;
    }
    
    if (QThread::currentThread() != app->thread()) {
        QT_TRACE(1, "setH323Connection called from non-main thread - using invokeMethod");
        // QPointerを使って安全にthisを参照
        QPointer<QtVideoMainWindow> safeThis = this;
        QMetaObject::invokeMethod(app, [safeThis, connection]() {
            if (!safeThis) {
                QT_TRACE(1, "WARNING: QtVideoMainWindow was deleted before invokeMethod executed");
                return;
            }
            safeThis->m_h323Connection = connection;
            if (connection) {
                QT_TRACE(1, "H323Connection set - UI controls enabled (via invokeMethod)");
                safeThis->m_disconnectButton->setEnabled(true);
                safeThis->m_connectButton->setEnabled(false);
                safeThis->m_muteCheckbox->setEnabled(true);
                safeThis->m_cameraCheckbox->setEnabled(true);
                if (safeThis->m_contentSendButton)
                    safeThis->m_contentSendButton->setEnabled(true);
            } else {
                QT_TRACE(1, "H323Connection cleared - UI controls reset (via invokeMethod)");
                safeThis->m_disconnectButton->setEnabled(false);
                safeThis->m_connectButton->setEnabled(true);
                safeThis->m_muteCheckbox->setEnabled(false);
                safeThis->m_cameraCheckbox->setEnabled(false);
                safeThis->m_muteCheckbox->setChecked(false);
                safeThis->m_cameraCheckbox->setChecked(false);
                if (safeThis->m_contentSendButton)
                    safeThis->m_contentSendButton->setEnabled(false);
            }
        }, Qt::QueuedConnection);
    } else {
        m_h323Connection = connection;
        if (connection) {
            QT_TRACE(1, "H323Connection set - UI controls enabled (direct call)");
            m_disconnectButton->setEnabled(true);
            m_connectButton->setEnabled(false);
            m_muteCheckbox->setEnabled(true);
            m_cameraCheckbox->setEnabled(true);
            if (m_contentSendButton)
                m_contentSendButton->setEnabled(true);
        } else {
            QT_TRACE(1, "H323Connection cleared - UI controls reset (direct call)");
            m_disconnectButton->setEnabled(false);
            m_connectButton->setEnabled(true);
            m_muteCheckbox->setEnabled(false);
            m_cameraCheckbox->setEnabled(false);
            m_muteCheckbox->setChecked(false);
            m_cameraCheckbox->setChecked(false);
            if (m_contentSendButton)
                m_contentSendButton->setEnabled(false);
        }
    }
}

void QtVideoMainWindow::enqueueLocalFrame(const unsigned char* yuvData, unsigned width, unsigned height)
{
    if (!yuvData || width == 0 || height == 0) return;
    
    // フレームレート制限（30fps = 33ms間隔）- カクカク防止
    static qint64 lastLocalFrameTime = 0;
    qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
    if (currentTime - lastLocalFrameTime < 33) {
        return;  // フレームをスキップ
    }
    lastLocalFrameTime = currentTime;
    
    // YUVデータをコピーしてシグナル発行
    size_t dataSize = width * height * 3 / 2;  // YUV420P
    QByteArray data(reinterpret_cast<const char*>(yuvData), dataSize);
    emit localFrameReady(data, width, height);
}

void QtVideoMainWindow::enqueueRemoteFrame(const unsigned char* yuvData, unsigned width, unsigned height)
{
    if (!yuvData || width == 0 || height == 0) return;
    
    // YUVデータをコピーしてシグナル発行
    size_t dataSize = width * height * 3 / 2;  // YUV420P
    QByteArray data(reinterpret_cast<const char*>(yuvData), dataSize);
    emit remoteFrameReady(data, width, height);
}

void QtVideoMainWindow::onLocalFrameReady(const QByteArray& yuvData, unsigned width, unsigned height)
{
    // 追加のフレームレート制限（キューに溜まった古いフレームをスキップ）
    static qint64 lastProcessedTime = 0;
    qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
    if (currentTime - lastProcessedTime < 30) {
        return;  // 古いフレームをスキップ
    }
    lastProcessedTime = currentTime;
    
    if (m_localVideo) {
        m_localVideo->updateFrameYUV420P(
            reinterpret_cast<const unsigned char*>(yuvData.constData()), 
            width, height);
    }
}

void QtVideoMainWindow::onRemoteFrameReady(const QByteArray& yuvData, unsigned width, unsigned height)
{
    // 📺 リモート映像処理のデバッグログ
    static int remoteFrameCounter = 0;
    remoteFrameCounter++;
    if (remoteFrameCounter % 30 == 1) {
        QT_TRACE(1, "📺 onRemoteFrameReady: frame #" << remoteFrameCounter 
                 << " (" << width << "x" << height << ")"
                 << " m_remoteVideo=" << (m_remoteVideo ? "OK" : "NULL"));
    }
    
    if (m_remoteVideo) {
        m_remoteVideo->updateFrameYUV420P(
            reinterpret_cast<const unsigned char*>(yuvData.constData()), 
            width, height);
    }
}

void QtVideoMainWindow::setConnectionStatus(const QString& status)
{
    if (m_statusLabel) {
        m_statusLabel->setText(status);
    }
    statusBar()->showMessage(status, 3000);
}

void QtVideoMainWindow::setContentButtonEnabled(bool enabled)
{
    if (!m_contentButton) {
        return;
    }
    m_contentButton->setEnabled(enabled);
    m_contentButton->setToolTip(enabled ? "Show H.239 content window" : "Content not available");
}

void QtVideoMainWindow::setLocalMuted(bool muted)
{
    if (m_localVideo) {
        m_localVideo->setMuted(muted);
    }
    // 外部イベントでミュート状態が変わったとき、チェックボックスも同期する
    if (m_muteCheckbox && m_muteCheckbox->isChecked() != muted) {
        QSignalBlocker blocker(m_muteCheckbox); // ループ防止
        m_muteCheckbox->setChecked(muted);
    }
}

void QtVideoMainWindow::setRemoteMuted(bool muted)
{
    if (m_remoteVideo) {
        m_remoteVideo->setMuted(muted);
    }
}

void QtVideoMainWindow::setLocalCameraMuted(bool muted)
{
    if (m_localVideo) {
        m_localVideo->setCameraMuted(muted);
    }
}

void QtVideoMainWindow::refreshDeviceLists()
{
    populateDeviceLists();
}

void QtVideoMainWindow::updateLocalAudioSpectrum(const int16_t* pcmData, size_t sampleCount, int channels, int sampleRate)
{
    if (m_localSpectrum) {
        m_localSpectrum->updateSpectrum(pcmData, sampleCount, channels, sampleRate);
    }
}

void QtVideoMainWindow::updateRemoteAudioSpectrum(const int16_t* pcmData, size_t sampleCount, int channels, int sampleRate)
{
    if (m_remoteSpectrum) {
        m_remoteSpectrum->updateSpectrum(pcmData, sampleCount, channels, sampleRate);
    }
}

void QtVideoMainWindow::closeEvent(QCloseEvent* event)
{
    QT_TRACE(1, "Window close event received");
    m_running = false;
    emit disconnectRequested();
    event->accept();
}

void QtVideoMainWindow::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
        case Qt::Key_Escape:
            QT_TRACE(1, "ESC pressed - closing window");
            close();
            break;
        case Qt::Key_S:
            if (m_muteCheckbox->isEnabled()) {
                QT_TRACE(1, "'S' pressed - toggling mute");
                m_muteCheckbox->toggle();
            }
            break;
        case Qt::Key_C:
            if (m_cameraCheckbox->isEnabled()) {
                QT_TRACE(1, "'C' pressed - toggling camera");
                m_cameraCheckbox->toggle();
            }
            break;
        default:
            QMainWindow::keyPressEvent(event);
    }
}

void QtVideoMainWindow::onConnectClicked()
{
    QString address = m_addressCombo->currentText().trimmed();
    if (!address.isEmpty()) {
        QT_TRACE(1, "Connect requested to: " << address.toStdString());
        
        // 履歴に追加
        addToAddressHistory(address);
        
        setConnectionStatus("Connecting to " + address + "...");
        emit connectRequested(address);
    }
}

void QtVideoMainWindow::onDisconnectClicked()
{
    QT_TRACE(1, "Disconnect requested");
    setConnectionStatus("Disconnecting...");
    emit disconnectRequested();
}

void QtVideoMainWindow::onMuteToggled(bool checked)
{
    if (m_h323Connection == nullptr) {
        QSignalBlocker blocker(m_muteCheckbox);
        m_muteCheckbox->setChecked(false);
        return;
    }
    QT_TRACE(1, "Mute toggled: " << (checked ? "ON" : "OFF"));
    setConnectionStatus(checked ? "Microphone muted" : "Microphone unmuted");
    emit muteToggleRequested();
}

void QtVideoMainWindow::onCameraToggled(bool checked)
{
    if (m_h323Connection == nullptr) {
        QSignalBlocker blocker(m_cameraCheckbox);
        m_cameraCheckbox->setChecked(false);
        return;
    }
    QT_TRACE(1, "Camera toggled: " << (checked ? "OFF" : "ON"));
    setConnectionStatus(checked ? "Camera disabled" : "Camera enabled");
    emit cameraToggleRequested();
}

void QtVideoMainWindow::onMicDeviceChanged(int index)
{
    Q_UNUSED(index);
    QString device = m_micCombo->currentText();
    QT_TRACE(1, "Mic device changed: " << device.toStdString());
    QtVideoManager::instance().applyDeviceSelection(device, m_speakerCombo->currentText(), m_cameraCombo->currentText());
}

void QtVideoMainWindow::onSpeakerDeviceChanged(int index)
{
    Q_UNUSED(index);
    QString device = m_speakerCombo->currentText();
    QT_TRACE(1, "Speaker device changed: " << device.toStdString());
    QtVideoManager::instance().applyDeviceSelection(m_micCombo->currentText(), device, m_cameraCombo->currentText());
}

void QtVideoMainWindow::onCameraDeviceChanged(int index)
{
    Q_UNUSED(index);
    QString device = m_cameraCombo->currentText();
    QT_TRACE(1, "Camera device changed: " << device.toStdString());
    QtVideoManager::instance().applyDeviceSelection(m_micCombo->currentText(), m_speakerCombo->currentText(), device);
}

///////////////////////////////////////////////////////////////////////////////
// QtContentWindow Implementation
///////////////////////////////////////////////////////////////////////////////

QtContentWindow::QtContentWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_contentVideo(nullptr)
{
    setWindowTitle("H.239 Content");
    QWidget* central = new QWidget(this);
    setCentralWidget(central);

    QVBoxLayout* layout = new QVBoxLayout(central);
    m_contentVideo = new QtVideoWidget("Content Sharing", central);
    layout->addWidget(m_contentVideo);

    connect(this, &QtContentWindow::contentFrameReady,
            this, &QtContentWindow::onContentFrameReady,
            Qt::QueuedConnection);

    QT_TRACE(1, "QtContentWindow created");
}

QtContentWindow::~QtContentWindow() = default;

void QtContentWindow::enqueueContentFrame(const unsigned char* yuvData, unsigned width, unsigned height, size_t dataSize)
{
    if (!yuvData || dataSize == 0) {
        return;
    }
    QByteArray buffer(reinterpret_cast<const char*>(yuvData), static_cast<int>(dataSize));
    emit contentFrameReady(buffer, width, height);
}

void QtContentWindow::onContentFrameReady(const QByteArray& yuvData, unsigned width, unsigned height)
{
    if (!m_contentVideo) {
        return;
    }
    m_contentVideo->updateFrameYUV420P(reinterpret_cast<const unsigned char*>(yuvData.constData()), width, height);
}

///////////////////////////////////////////////////////////////////////////////
// QtVideoManager Implementation
///////////////////////////////////////////////////////////////////////////////

QtVideoManager& QtVideoManager::getInstance()
{
    static QtVideoManager instance;
    return instance;
}

QtVideoManager::QtVideoManager()
    : m_mainWindow(nullptr)
    , m_contentWindow(nullptr)
    , m_endpoint(nullptr)
    , m_initialized(false)
    , m_lastContentWidth(1280)
    , m_lastContentHeight(720)
    , m_contentAvailable(false)
    , m_contentSendTarget()
    , m_makeCallCb(nullptr)
    , m_makeCallUserData(nullptr)
    , m_hangupCallCb(nullptr)
    , m_hangupCallUserData(nullptr)
    , m_toggleMuteCb(nullptr)
    , m_toggleMuteUserData(nullptr)
    , m_toggleCameraCb(nullptr)
    , m_toggleCameraUserData(nullptr)
    , m_isMutedCb(nullptr)
    , m_isMutedUserData(nullptr)
    , m_getDeviceListCb(nullptr)
    , m_getDeviceListUserData(nullptr)
    , m_applyDeviceSelectionCb(nullptr)
    , m_applyDeviceSelectionUserData(nullptr)
{
}

QtVideoManager::~QtVideoManager()
{
    shutdown();
}

bool QtVideoManager::initialize()
{
    if (m_initialized) {
        QT_TRACE(1, "QtVideoManager already initialized");
        return true;
    }

    // QApplicationは外部で作成済みであることを確認
    if (!QCoreApplication::instance()) {
        QT_TRACE(1, "ERROR: QApplication not created - cannot initialize QtVideoManager");
        return false;
    }

    m_initialized = true;
    QT_TRACE(1, "QtVideoManager initialized successfully");
    return true;
}

void QtVideoManager::shutdown()
{
    if (!m_initialized) return;

    QT_TRACE(1, "QtVideoManager shutting down...");
    
    // コンテンツボタンを無効化しておく
    setContentAvailable(false);

    if (m_mainWindow) {
        m_mainWindow->close();
        delete m_mainWindow;
        m_mainWindow = nullptr;
    }
    if (m_contentWindow) {
        m_contentWindow->close();
        delete m_contentWindow;
        m_contentWindow = nullptr;
    }

    m_initialized = false;
    QT_TRACE(1, "QtVideoManager shutdown complete");
}

bool QtVideoManager::createWindow(int width, int height)
{
    return createLocalWindow(width, height);
}

bool QtVideoManager::createLocalWindow(int width, int height)
{
    if (!m_initialized) {
        QT_TRACE(1, "Cannot create window - not initialized");
        return false;
    }

    // 🎯 スレッドセーフ: ウィンドウ作成はメインスレッドでのみ実行可能
    if (QThread::currentThread() != QCoreApplication::instance()->thread()) {
        QT_TRACE(1, "createLocalWindow called from non-main thread - using invokeMethod");
        bool result = false;
        // 同期的に実行（ウィンドウが作成されてから続行する必要があるため）
        QMetaObject::invokeMethod(QCoreApplication::instance(), [this, width, height, &result]() {
            if (!m_mainWindow) {
                m_mainWindow = new QtVideoMainWindow();
                m_mainWindow->resize(width * 2 + 50, height + 150);
                m_mainWindow->show();
                QT_TRACE(1, "Main window created (via invokeMethod): " << width << "x" << height);
            }
            result = true;
        }, Qt::BlockingQueuedConnection);
        return result;
    }

    if (!m_mainWindow) {
        m_mainWindow = new QtVideoMainWindow();
        m_mainWindow->resize(width * 2 + 50, height + 150);  // 2つのビデオ + コントロール
        m_mainWindow->show();
        QT_TRACE(1, "Main window created: " << width << "x" << height);
    }

    return true;
}

bool QtVideoManager::createRemoteWindow(int width, int height)
{
    // リモートウィンドウはメインウィンドウ内に含まれる
    return createLocalWindow(width, height);
}

bool QtVideoManager::createContentWindow(int width, int height)
{
    if (!m_initialized) {
        QT_TRACE(1, "Cannot create content window - not initialized");
        return false;
    }

    if (QThread::currentThread() != QCoreApplication::instance()->thread()) {
        QT_TRACE(1, "createContentWindow called from non-main thread - using invokeMethod");
        bool result = false;
        QMetaObject::invokeMethod(QCoreApplication::instance(), [this, width, height, &result]() {
            result = createContentWindow(width, height);
        }, Qt::BlockingQueuedConnection);
        return result;
    }

    if (!m_contentWindow) {
        m_contentWindow = new QtContentWindow();
        // ウインドウが閉じられた／破棄されたときにポインタをクリアする
        QObject::connect(m_contentWindow, &QObject::destroyed, m_contentWindow, [this]() {
            QT_TRACE(1, "Content window destroyed - clearing pointer");
            m_contentWindow = nullptr;
        });
        m_contentWindow->resize(width, height);
        m_contentWindow->show();
        QT_TRACE(1, "Content window created: " << width << "x" << height);
    }
    
    // コンテンツボタンを有効化
    setContentAvailable(true);
    return true;
}

void QtVideoManager::closeContentWindow(bool disableAvailability)
{
    // コンテンツウィンドウが無ければ何もしない
    if (!m_contentWindow) {
        if (disableAvailability) {
            setContentAvailable(false);
        }
        return;
    }

    // メインスレッドでのみウィンドウを閉じる
    if (QThread::currentThread() != QCoreApplication::instance()->thread()) {
        // 終了処理中でもデッドロックしないよう非同期で依頼
        if (disableAvailability) {
            setContentAvailable(false);
        }
        QMetaObject::invokeMethod(QCoreApplication::instance(), [this, disableAvailability]() {
            closeContentWindow(disableAvailability);
        }, Qt::QueuedConnection);
        return;
    }

    PTRACE(1, "QtVideo\tClosing content window");
    m_contentWindow->close();
    delete m_contentWindow;
    m_contentWindow = nullptr;
    
    if (disableAvailability) {
        setContentAvailable(false);
    }
}

void QtVideoManager::showWindow()
{
    QT_TRACE(1, ">>> showWindow() ENTER - m_mainWindow=" << (void*)m_mainWindow);
    
    if (!m_mainWindow) {
        QT_TRACE(1, "WARNING: showWindow called but m_mainWindow is null");
        return;
    }
    
    // フラグで重複呼び出しを防ぐ（スレッドセーフにするためatomic使用）
    static std::atomic<bool> windowShown{false};
    if (windowShown.exchange(true)) {
        QT_TRACE(1, "showWindow: already shown, skipping");
        return;
    }
    
    // 🎯 スレッドセーフ: Qt GUIオペレーションはメインスレッドでのみ実行可能
    // H.245スレッドなど別スレッドから呼ばれる可能性があるため、
    // QMetaObject::invokeMethod を使用してメインスレッドで実行
    auto app = QCoreApplication::instance();
    QT_TRACE(1, "showWindow: QCoreApplication=" << (void*)app);
    
    if (!app) {
        QT_TRACE(1, "WARNING: QCoreApplication is null in showWindow");
        windowShown = false;  // リセット
        return;
    }
    
    QThread* currentThread = QThread::currentThread();
    QThread* appThread = app->thread();
    QT_TRACE(1, "showWindow: currentThread=" << (void*)currentThread << " appThread=" << (void*)appThread);
    
    if (currentThread != appThread) {
        QT_TRACE(1, "showWindow called from non-main thread - using invokeMethod");
        
        // m_mainWindowをQObjectとして渡し、そのコンテキストでラムダを実行
        // これにより、m_mainWindowのスレッド（メインスレッド）で実行される
        QPointer<QtVideoMainWindow> safeWindow = m_mainWindow;
        QT_TRACE(1, "showWindow: invoking method on app...");
        bool result = QMetaObject::invokeMethod(app, [safeWindow]() {
            QT_TRACE(1, "showWindow lambda ENTER - safeWindow=" << (void*)safeWindow.data());
            if (safeWindow) {
                safeWindow->show();
                safeWindow->raise();
                safeWindow->activateWindow();
                QT_TRACE(1, "Window shown and activated (via invokeMethod)");
            } else {
                QT_TRACE(1, "WARNING: Window was deleted before invokeMethod executed");
            }
            QT_TRACE(1, "showWindow lambda EXIT");
        }, Qt::QueuedConnection);
        QT_TRACE(1, "showWindow: invokeMethod returned " << result);
    } else {
        m_mainWindow->show();
        m_mainWindow->raise();
        m_mainWindow->activateWindow();
        QT_TRACE(1, "Window shown and activated (direct call)");
    }
    QT_TRACE(1, "<<< showWindow() EXIT");
}

void QtVideoManager::enqueueLocalFrame(const unsigned char* yuvData, unsigned width, unsigned height, size_t dataSize)
{
    Q_UNUSED(dataSize);
    if (m_mainWindow && yuvData && width > 0 && height > 0) {
        // enqueueLocalFrameはシグナル経由でQueuedConnectionを使うのでスレッドセーフ
        m_mainWindow->enqueueLocalFrame(yuvData, width, height);
    }
}

void QtVideoManager::enqueueRemoteFrame(const unsigned char* yuvData, unsigned width, unsigned height, size_t dataSize)
{
    Q_UNUSED(dataSize);
    if (m_mainWindow && yuvData && width > 0 && height > 0) {
        // enqueueRemoteFrameはシグナル経由でQueuedConnectionを使うのでスレッドセーフ
        m_mainWindow->enqueueRemoteFrame(yuvData, width, height);
    }
}

void QtVideoManager::enqueueContentFrame(const unsigned char* yuvData, unsigned width, unsigned height, size_t dataSize)
{
    if (!yuvData || width == 0 || height == 0) {
        return;
    }
    
    // 最新サイズを記録し、ボタンを有効化
    m_lastContentWidth = static_cast<int>(width);
    m_lastContentHeight = static_cast<int>(height);
    setContentAvailable(true);

    // コンテンツ用ウィンドウを遅延生成
    if (!m_contentWindow) {
        createContentWindow(static_cast<int>(width), static_cast<int>(height));
    }

    if (m_contentWindow) {
        m_contentWindow->enqueueContentFrame(yuvData, width, height, dataSize);
    }
}

void QtVideoManager::setH323Connection(MyH323Connection* connection)
{
    if (m_mainWindow) {
        m_mainWindow->setH323Connection(connection);
    }
}

MyH323Connection* QtVideoManager::getH323Connection() const
{
    return m_mainWindow ? m_mainWindow->getH323Connection() : nullptr;
}

void QtVideoManager::updateMuteState(bool localMuted, bool remoteMuted)
{
    if (!m_mainWindow) {
        return;
    }
    
    auto app = QCoreApplication::instance();
    if (!app) {
        QT_TRACE(1, "WARNING: QCoreApplication is null in updateMuteState");
        return;
    }
    
    // スレッドセーフ: ウィジェットへのアクセスはメインスレッドで行う
    if (QThread::currentThread() != app->thread()) {
        QPointer<QtVideoMainWindow> safeWindow = m_mainWindow;
        QMetaObject::invokeMethod(app, [safeWindow, localMuted, remoteMuted]() {
            if (safeWindow) {
                safeWindow->setLocalMuted(localMuted);
                safeWindow->setRemoteMuted(remoteMuted);
            }
        }, Qt::QueuedConnection);
    } else {
        m_mainWindow->setLocalMuted(localMuted);
        m_mainWindow->setRemoteMuted(remoteMuted);
    }
}

void QtVideoManager::setContentAvailable(bool available)
{
    m_contentAvailable = available;
    
    if (!m_mainWindow) {
        return;
    }
    
    auto app = QCoreApplication::instance();
    if (!app) {
        return;
    }
    
    if (QThread::currentThread() != app->thread()) {
        QMetaObject::invokeMethod(app, [this, available]() {
            if (m_mainWindow) {
                m_mainWindow->setContentButtonEnabled(available);
            }
        }, Qt::QueuedConnection);
    } else {
        m_mainWindow->setContentButtonEnabled(available);
    }
}

void QtVideoManager::openContentWindow()
{
    // デフォルトサイズは最後のコンテンツサイズを使用
    int cw = (m_lastContentWidth > 0) ? m_lastContentWidth : 1280;
    int ch = (m_lastContentHeight > 0) ? m_lastContentHeight : 720;
    
    if (QThread::currentThread() != QCoreApplication::instance()->thread()) {
        QMetaObject::invokeMethod(QCoreApplication::instance(), [this, cw, ch]() {
            if (createContentWindow(cw, ch) && m_contentWindow) {
                m_contentWindow->show();
                m_contentWindow->raise();
                m_contentWindow->activateWindow();
            }
        }, Qt::BlockingQueuedConnection);
        return;
    }
    
    if (createContentWindow(cw, ch) && m_contentWindow) {
        m_contentWindow->show();
        m_contentWindow->raise();
        m_contentWindow->activateWindow();
    }
}

void QtVideoManager::startContentCapture(const QString& targetTitle)
{
    m_contentSendTarget = targetTitle;
    PTRACE(2, "QtVideo\tContent capture enabled for target=" << targetTitle.toStdString());
    setContentAvailable(true);
    
    // Start periodic frame capture in main thread using timer
    m_contentCaptureTimer.setInterval(100); // 10fps (100ms interval)
    QObject::connect(&m_contentCaptureTimer, &QTimer::timeout, [this]() { this->captureContentFrame(); });
    m_contentCaptureTimer.start();
    PTRACE(2, "QtVideo\tTimer started for periodic capture (10fps)");

    // プレイヤー初期化: ターゲット解像度の黒フレームを用意して幅/高さを明示
    const int initW = 1280;
    const int initH = 720;
    {
        QMutexLocker locker(&m_contentFrameMutex);
        m_captureWidth = initW;
        m_captureHeight = initH;
        const int initSize = (initW * initH * 3) / 2;
        m_lastCapturedFrame.resize(initSize);
        memset(m_lastCapturedFrame.data(), 16, initW * initH); // Y
        memset(m_lastCapturedFrame.data() + initW * initH, 128, initSize - initW * initH); // UV
    }
}

void QtVideoManager::stopContentCapture()
{
    PTRACE(2, "QtVideo\tStopping content capture");
    m_contentCaptureTimer.stop();
}

// Timer callback - runs in main thread periodically to capture screen
void QtVideoManager::captureContentFrame()
{
    if (!m_contentSendTarget.isEmpty()) {
        // This runs in main thread, safe to use Qt GUI operations
        QWindow* targetWin = nullptr;
        const auto wins = QGuiApplication::topLevelWindows();
        for (auto* w : wins) {
            if (w && w->title() == m_contentSendTarget) {
                targetWin = w;
                break;
            }
        }
        
        QScreen* screen = QGuiApplication::primaryScreen();
        if (!screen) return;
        
        QPixmap grab;
        if (targetWin) {
            grab = screen->grabWindow(targetWin->winId());
        } else {
            grab = screen->grabWindow(0);
        }
        
        QImage img = grab.toImage().convertToFormat(QImage::Format_RGB32);
        if (img.isNull()) return;
        
        // Scale to 720P (1280x720)
        // 🎬 HIGH QUALITY: Use 720P for better content sharing experience
        // H.264 encoder expects exactly 1280x720, so we must provide that size
        const int targetW = 1280;
        const int targetH = 720;
        img = img.scaled(targetW, targetH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        
        int w = img.width() & ~1;
        int h = img.height() & ~1;
        if (img.width() != w || img.height() != h) {
            img = img.copy(0, 0, w, h);
        }
        if (w < 16 || h < 16) return;
        
        PTRACE(4, "QtVideo\tCaptured " << w << "x" << h << " frame in main thread");

        QMutexLocker locker(&m_contentFrameMutex);

        m_captureWidth = w;
        m_captureHeight = h;

        // RGB32 → YUV420P conversion
        const int ySize = w * h;
        const int uvSize = (w * h) / 4;
        m_lastCapturedFrame.resize(ySize + uvSize * 2);
        unsigned char* yuv = reinterpret_cast<unsigned char*>(m_lastCapturedFrame.data());
        unsigned char* yPlane = yuv;
        unsigned char* uPlane = yuv + ySize;
        unsigned char* vPlane = yuv + ySize + uvSize;
        
        const uchar* rgb = img.bits();
        const int stride = img.bytesPerLine();
        
        // Convert Y plane
        for (int j = 0; j < h; j++) {
            for (int i = 0; i < w; i++) {
                const int rgbIdx = j * stride + i * 4;
                const int r = rgb[rgbIdx + 2];
                const int g = rgb[rgbIdx + 1];
                const int b = rgb[rgbIdx + 0];
                
                int y = static_cast<int>(0.257 * r + 0.504 * g + 0.098 * b + 16);
                yPlane[j * w + i] = static_cast<unsigned char>(std::clamp(y, 0, 255));
            }
        }
        
        // Convert UV planes (subsampled 2x2)
        for (int j = 0; j < h; j += 2) {
            for (int i = 0; i < w; i += 2) {
                int rSum = 0, gSum = 0, bSum = 0;
                for (int dy = 0; dy < 2 && (j + dy) < h; dy++) {
                    for (int dx = 0; dx < 2 && (i + dx) < w; dx++) {
                        const int rgbIdx = (j + dy) * stride + (i + dx) * 4;
                        rSum += rgb[rgbIdx + 2];
                        gSum += rgb[rgbIdx + 1];
                        bSum += rgb[rgbIdx + 0];
                    }
                }
                rSum /= 4;
                gSum /= 4;
                bSum /= 4;
                
                int u = static_cast<int>(-0.148 * rSum - 0.291 * gSum + 0.439 * bSum + 128);
                int v = static_cast<int>(0.439 * rSum - 0.368 * gSum - 0.071 * bSum + 128);
                
                const int uvIdx = (j / 2) * (w / 2) + (i / 2);
                uPlane[uvIdx] = static_cast<unsigned char>(std::clamp(u, 0, 255));
                vPlane[uvIdx] = static_cast<unsigned char>(std::clamp(v, 0, 255));
            }
        }
    }
}

void QtVideoManager::captureContentFrameNow()
{
    // 🔧 SOLUTION: Don't call Qt GUI operations from encoder thread!
    // Instead, just return the last captured frame from buffer
    // (Frame buffer is updated periodically by timer in main thread)
    
    PTRACE(4, "QtVideo\tcaptureContentFrameNow() - using buffered frame");
    
    // Safety check: if timer hasn't started yet, fill with black frame
    {
        QMutexLocker locker(&m_contentFrameMutex);
        if (m_lastCapturedFrame.isEmpty()) {
            PTRACE(2, "QtVideo\tWARNING: Timer not started yet, using black frame");
            int w = 1280;
            int h = 720;
            m_captureWidth = w;
            m_captureHeight = h;
            int yuvSize = (w * h * 3) / 2;
            m_lastCapturedFrame.resize(yuvSize);
            memset(m_lastCapturedFrame.data(), 16, w * h);      // Y plane (black)
            memset(m_lastCapturedFrame.data() + w*h, 128, (w*h)/2); // UV planes (neutral)
        }
    }
    
    // Frame is already in m_lastCapturedFrame buffer from timer-based capture
    // No need to do anything here - encoder will read from buffer
    return;
}

bool QtVideoManager::getLatestContentFrame(QByteArray& outFrame, unsigned& width, unsigned& height)
{
    QMutexLocker locker(&m_contentFrameMutex);
    if (m_lastCapturedFrame.isEmpty()) {
        return false;
    }
    
    // Make a deep copy to ensure data stability
    outFrame = m_lastCapturedFrame;
    width = static_cast<unsigned>(m_captureWidth);
    height = static_cast<unsigned>(m_captureHeight);
    
    // Validate before returning
    if (width < 2 || height < 2 || outFrame.size() != (width * height * 3 / 2)) {
        PTRACE(2, "QtVideo\tgetLatestContentFrame: Invalid data - w=" << width 
               << " h=" << height << " size=" << outFrame.size());
        return false;
    }
    
    return true;
}

void QtVideoManager::requestContentSend(const QString& targetTitle)
{
    m_contentSendTarget = targetTitle;
    PTRACE(1, "QtVideo\tContent send target set to: " << targetTitle.toStdString());
    
    if (m_mainWindow) {
        m_mainWindow->setConnectionStatus("Content source selected: " + targetTitle);
    }
    
    // コンテンツキャプチャ開始
    startContentCapture(targetTitle);
    
#ifndef H323_H239
    PTRACE(1, "QtVideo\tH.239 not compiled in this build - cannot start content TX");
    return;
#else
    // H.239送信チャネル開始（接続中のとき）
    MyH323Connection* conn = getH323Connection();
    MyH323EndPoint* ep = getH323Endpoint();
    PTRACE(1, "QtVideo\tH.239 TX preflight - endpoint=" << (void*)ep << " conn=" << (void*)conn);
    if (ep) {
        ep->SetStartH239(true); // UIから送信を許可
        PTRACE(1, "QtVideo\tH.239 start flag set on endpoint");
    } else {
        PTRACE(1, "QtVideo\tEndpoint not set - cannot set H.239 start flag");
    }
    if (conn) {
        PTRACE(1, "QtVideo\tRequesting H.239 transmission start");
        conn->StartH239Transmission();
    } else {
        PTRACE(1, "QtVideo\tNo active connection - cannot start H.239 transmission");
    }
#endif
}

void QtVideoManager::updateCameraState(bool cameraMuted)
{
    if (!m_mainWindow) {
        return;
    }
    
    auto app = QCoreApplication::instance();
    if (!app) {
        QT_TRACE(1, "WARNING: QCoreApplication is null in updateCameraState");
        return;
    }
    
    // スレッドセーフ: UIスレッドで状態を更新（黒画面表示）
    if (QThread::currentThread() != app->thread()) {
        QPointer<QtVideoMainWindow> safeWindow = m_mainWindow;
        QMetaObject::invokeMethod(app, [safeWindow, cameraMuted]() {
            if (safeWindow) {
                safeWindow->setLocalCameraMuted(cameraMuted);
                safeWindow->setConnectionStatus(cameraMuted ? "📹 Camera OFF" : "📹 Camera ON");
            }
        }, Qt::QueuedConnection);
    } else {
        m_mainWindow->setLocalCameraMuted(cameraMuted);
        m_mainWindow->setConnectionStatus(cameraMuted ? "📹 Camera OFF" : "📹 Camera ON");
    }
    
    QT_TRACE(1, "Camera state updated: " << (cameraMuted ? "OFF (black screen)" : "ON"));
}

void QtVideoManager::refreshDeviceLists()
{
    if (!m_mainWindow) {
        return;
    }
    
    auto app = QCoreApplication::instance();
    if (app && QThread::currentThread() != app->thread()) {
        QPointer<QtVideoMainWindow> safeWindow = m_mainWindow;
        QMetaObject::invokeMethod(app, [safeWindow]() {
            if (safeWindow) {
                safeWindow->refreshDeviceLists();
            }
        }, Qt::QueuedConnection);
    } else {
        m_mainWindow->refreshDeviceLists();
    }
}

void QtVideoManager::setH323Endpoint(MyH323EndPoint* endpoint)
{
    m_endpoint = endpoint;
    QT_TRACE(1, "H323Endpoint set: " << (void*)endpoint);
    
    // シグナル接続を設定
    if (m_mainWindow && m_endpoint) {
        setupSignalConnections();
    }
}

MyH323EndPoint* QtVideoManager::getH323Endpoint() const
{
    return m_endpoint;
}

void QtVideoManager::setupSignalConnections()
{
    if (!m_mainWindow) return;
    
    // 接続リクエスト
    QObject::connect(m_mainWindow, &QtVideoMainWindow::connectRequested,
        [this](const QString& address) {
            makeCall(address);
        });
    
    // 切断リクエスト
    QObject::connect(m_mainWindow, &QtVideoMainWindow::disconnectRequested,
        [this]() {
            hangupCall();
        });
    
    // ミュートトグル
    QObject::connect(m_mainWindow, &QtVideoMainWindow::muteToggleRequested,
        [this]() {
            toggleMute();
        });
    
    // カメラトグル
    QObject::connect(m_mainWindow, &QtVideoMainWindow::cameraToggleRequested,
        [this]() {
            toggleCamera();
        });
    
    QT_TRACE(1, "Signal connections established between Qt UI and H.323");
}

void QtVideoManager::setMakeCallCallback(MakeCallCallback cb, void* userData)
{
    m_makeCallCb = cb;
    m_makeCallUserData = userData;
    QT_TRACE(1, "MakeCallCallback registered, userData=" << userData);
}

void QtVideoManager::setHangupCallCallback(HangupCallCallback cb, void* userData)
{
    m_hangupCallCb = cb;
    m_hangupCallUserData = userData;
    QT_TRACE(1, "HangupCallCallback registered, userData=" << userData);
}

void QtVideoManager::setToggleMuteCallback(ToggleMuteCallback cb, void* userData)
{
    m_toggleMuteCb = cb;
    m_toggleMuteUserData = userData;
}

void QtVideoManager::setToggleCameraCallback(ToggleCameraCallback cb, void* userData)
{
    m_toggleCameraCb = cb;
    m_toggleCameraUserData = userData;
}

void QtVideoManager::setIsMutedCallback(IsMutedCallback cb, void* userData)
{
    m_isMutedCb = cb;
    m_isMutedUserData = userData;
}

void QtVideoManager::setGetDeviceListCallback(GetDeviceListCallback cb, void* userData)
{
    m_getDeviceListCb = cb;
    m_getDeviceListUserData = userData;
}

void QtVideoManager::setApplyDeviceSelectionCallback(ApplyDeviceSelectionCallback cb, void* userData)
{
    m_applyDeviceSelectionCb = cb;
    m_applyDeviceSelectionUserData = userData;
}

bool QtVideoManager::getDeviceList(int deviceType, QStringList& outList)
{
    if (!m_getDeviceListCb) {
        return false;
    }
    outList.clear();
    return m_getDeviceListCb(deviceType, outList, m_getDeviceListUserData);
}

void QtVideoManager::applyDeviceSelection(const QString& mic, const QString& speaker, const QString& camera)
{
    if (m_applyDeviceSelectionCb) {
        m_applyDeviceSelectionCb(mic, speaker, camera, m_applyDeviceSelectionUserData);
    }
}

void QtVideoManager::makeCall(const QString& address)
{
    QT_TRACE(1, "makeCall ENTRY - address: " << address.toStdString());
    QT_TRACE(1, "makeCall - m_makeCallCb=" << (void*)m_makeCallCb << " m_makeCallUserData=" << m_makeCallUserData);
    
    if (!m_makeCallCb) {
        QT_TRACE(1, "ERROR: Cannot make call - no callback set");
        setConnectionStatus("Error: Call function not initialized");
        return;
    }
    
    if (!m_makeCallUserData) {
        QT_TRACE(1, "ERROR: Cannot make call - userData is nullptr (endpoint not set)");
        setConnectionStatus("Error: H.323 endpoint not initialized");
        return;
    }
    
    QT_TRACE(1, "Making call to: " << address.toStdString());
    setConnectionStatus("Calling " + address + "...");
    
    // UIを更新 - Connectボタン無効化、Disconnectボタン有効化
    if (m_mainWindow) {
        auto app = QCoreApplication::instance();
        if (app) {
            QPointer<QtVideoMainWindow> safeWindow = m_mainWindow;
            QMetaObject::invokeMethod(app, [safeWindow]() {
                if (safeWindow) {
                    safeWindow->m_connectButton->setEnabled(false);
                    safeWindow->m_disconnectButton->setEnabled(true);
                }
            }, Qt::QueuedConnection);
        }
    }
    
    // コールバックを通じてH.323接続を開始
    std::string addr = address.toStdString();
    QT_TRACE(1, "Invoking m_makeCallCb with addr=" << addr << " userData=" << m_makeCallUserData);
    if (m_makeCallCb(addr.c_str(), m_makeCallUserData)) {
        QT_TRACE(1, "Call initiated successfully");
    } else {
        QT_TRACE(1, "Failed to make call to: " << addr);
        setConnectionStatus("Call failed");
        // 失敗時はUIを元に戻す
        if (m_mainWindow) {
            auto app = QCoreApplication::instance();
            if (app) {
                QPointer<QtVideoMainWindow> safeWindow = m_mainWindow;
                QMetaObject::invokeMethod(app, [safeWindow]() {
                    if (safeWindow) {
                        safeWindow->m_connectButton->setEnabled(true);
                        safeWindow->m_disconnectButton->setEnabled(false);
                    }
                }, Qt::QueuedConnection);
            }
        }
    }
}

void QtVideoManager::hangupCall()
{
    QT_TRACE(1, "Hanging up call - m_hangupCallCb=" << (void*)m_hangupCallCb << " userData=" << m_hangupCallUserData);
    
    if (m_hangupCallCb) {
        m_hangupCallCb(m_hangupCallUserData);
    }
    
    setConnectionStatus("Disconnected - Exiting...");
    
    // UIを更新
    if (m_mainWindow) {
        auto app = QCoreApplication::instance();
        if (app && QThread::currentThread() != app->thread()) {
            QPointer<QtVideoMainWindow> safeWindow = m_mainWindow;
            QMetaObject::invokeMethod(app, [safeWindow]() {
                if (safeWindow) {
                    safeWindow->setH323Connection(nullptr);
                }
            }, Qt::QueuedConnection);
        } else if (m_mainWindow) {
            m_mainWindow->setH323Connection(nullptr);
        }
    }
    
    // 🎯 切断後、短い遅延を挟んでアプリケーションを終了
    // H.323の終了処理が完了するのを待ってから終了
    QTimer::singleShot(1500, []() {
        QT_TRACE(1, "Initiating application shutdown after disconnect");
        auto app = QCoreApplication::instance();
        if (app) {
            // 強制終了（H.323スレッドがブロックする可能性があるため）
            QT_TRACE(1, "Calling _exit(0) to force clean shutdown");
            _exit(0);
        }
    });
}

void QtVideoManager::toggleMute()
{
    if (m_toggleMuteCb) {
        m_toggleMuteCb(m_toggleMuteUserData);
        QT_TRACE(1, "Mute toggled via callback");
        
        // 現在のミュート状態を取得してUIに反映
        if (m_isMutedCb) {
            bool isMuted = m_isMutedCb(m_isMutedUserData);
            updateMuteState(isMuted, false);
        }
    } else {
        QT_TRACE(1, "Cannot toggle mute - no callback set");
    }
}

void QtVideoManager::toggleCamera()
{
    if (m_toggleCameraCb) {
        m_toggleCameraCb(m_toggleCameraUserData);
        QT_TRACE(1, "Camera toggled via callback");
    } else {
        QT_TRACE(1, "Camera toggle not yet implemented");
        setConnectionStatus("Camera toggle - not yet implemented");
    }
}

void QtVideoManager::setConnectionStatus(const QString& status)
{
    if (!m_mainWindow) return;
    
    auto app = QCoreApplication::instance();
    if (!app) return;
    
    if (QThread::currentThread() != app->thread()) {
        QPointer<QtVideoMainWindow> safeWindow = m_mainWindow;
        QString statusCopy = status;
        QMetaObject::invokeMethod(app, [safeWindow, statusCopy]() {
            if (safeWindow) {
                safeWindow->setConnectionStatus(statusCopy);
            }
        }, Qt::QueuedConnection);
    } else {
        m_mainWindow->setConnectionStatus(status);
    }
}

///////////////////////////////////////////////////////////////////////////////
// Address History Management
///////////////////////////////////////////////////////////////////////////////

void QtVideoMainWindow::loadAddressHistory()
{
    QSettings settings("H323ASKW", "VideoClient");
    QStringList history = settings.value("addressHistory").toStringList();
    
    m_addressCombo->clear();
    for (const QString& addr : history) {
        if (!addr.isEmpty()) {
            m_addressCombo->addItem(addr);
        }
    }

    appendClearHistoryItem();
    
    // 最新の履歴を選択状態に（空の場合は入力可能）
    if (m_addressCombo->count() > 0) {
        m_addressCombo->setCurrentIndex(0);
    }
    
    QT_TRACE(1, "Loaded " << history.size() << " address history entries");
}

void QtVideoMainWindow::saveAddressHistory()
{
    QSettings settings("H323ASKW", "VideoClient");
    QStringList history;

    int saved = 0;
    for (int i = 0; i < m_addressCombo->count() && saved < MAX_ADDRESS_HISTORY; i++) {
        if (isClearHistoryItem(i))
            continue;
        QString addr = m_addressCombo->itemText(i);
        if (!addr.isEmpty()) {
            history.append(addr);
            ++saved;
        }
    }
    
    settings.setValue("addressHistory", history);
    settings.sync();
    
    QT_TRACE(1, "Saved " << history.size() << " address history entries");
}

void QtVideoMainWindow::clearAddressHistory()
{
    QSettings settings("H323ASKW", "VideoClient");
    settings.remove("addressHistory");
    settings.sync();
    
    m_addressCombo->clear();
    m_addressCombo->setEditText(QString());
    appendClearHistoryItem();
    
    QT_TRACE(1, "Cleared address history");
}

void QtVideoMainWindow::addToAddressHistory(const QString& address)
{
    if (address.isEmpty()) return;
    
    // 既存の同じアドレスを削除（重複防止）
    int existingIndex = m_addressCombo->findText(address);
    if (existingIndex >= 0) {
        m_addressCombo->removeItem(existingIndex);
    }
    
    // 先頭に追加
    m_addressCombo->insertItem(0, address);
    m_addressCombo->setCurrentIndex(0);
    
    // 最大数を超えたら古いものを削除（クリア項目は除外）
    int nonClearCount = 0;
    for (int i = 0; i < m_addressCombo->count(); ++i) {
        if (!isClearHistoryItem(i))
            ++nonClearCount;
    }
    for (int i = m_addressCombo->count() - 1; nonClearCount > MAX_ADDRESS_HISTORY && i >= 0; --i) {
        if (isClearHistoryItem(i))
            continue;
        m_addressCombo->removeItem(i);
        --nonClearCount;
    }

    appendClearHistoryItem();
    
    // 即座に保存
    saveAddressHistory();
    
    QT_TRACE(1, "Added to address history: " << address.toStdString());
}

void QtVideoMainWindow::onAddressActivated(int index)
{
    if (isClearHistoryItem(index)) {
        clearAddressHistory();
    }
}

void QtVideoMainWindow::onSeparateWindowsClicked()
{
    if (!m_windowsSeparated) {
        // === 分離モード ===
        // 現在のサイズ比を保存
        QList<int> sizes = m_videoSplitter->sizes();
        
        // リモートビデオをスプリッターから外す
        m_remoteVideo->setParent(nullptr);
        m_remoteVideo->setWindowFlags(Qt::Window);
        m_remoteVideo->setWindowTitle("Remote Video - H323ASKW");
        m_remoteVideo->setMinimumSize(320, 240);
        m_remoteVideo->resize(640, 480);
        
        // メインウィンドウの右側に配置
        QPoint mainPos = this->pos();
        int mainWidth = this->width();
        m_remoteVideo->move(mainPos.x() + mainWidth + 20, mainPos.y());
        
        m_remoteVideo->show();
        
        // ボタンテキスト変更
        m_separateButton->setText("⇦ Combine");
        m_separateButton->setToolTip("Combine local and remote video back into single window");
        
        m_windowsSeparated = true;
        
        QT_TRACE(1, "Video windows separated");
    } else {
        // === 結合モード ===
        // リモートビデオウィンドウを閉じてスプリッターに戻す
        m_remoteVideo->hide();
        m_remoteVideo->setWindowFlags(Qt::Widget);
        m_remoteVideo->setParent(m_videoSplitter);
        
        // スプリッターに追加（ローカルの右側）
        m_videoSplitter->addWidget(m_remoteVideo);
        m_remoteVideo->show();
        
        // サイズを均等に戻す
        m_videoSplitter->setSizes({1, 1});
        
        // ボタンテキスト変更
        m_separateButton->setText("↗ Separate");
        m_separateButton->setToolTip("Separate local and remote video into different windows");
        
        m_windowsSeparated = false;
        
        QT_TRACE(1, "Video windows combined");
    }
}

void QtVideoMainWindow::onContentWindowClicked()
{
    QT_TRACE(1, "Content button clicked - requesting content window");
    QtVideoManager::instance().openContentWindow();
}

void QtVideoMainWindow::onContentSendClicked()
{
    QT_TRACE(1, "Content send button clicked - selecting window to share");

    if (!m_h323Connection) {
        setConnectionStatus("Connect first to send content");
        return;
    }

    QStringList windowTitles;
    // 取得できるトップレベルウインドウのタイトルを列挙
    const auto windows = QGuiApplication::topLevelWindows();
    for (auto* win : windows) {
        if (win) {
            const QString title = win->title();
            if (!title.isEmpty()) {
                windowTitles << title;
            }
        }
    }

    // フォールバック
    if (windowTitles.isEmpty()) {
        windowTitles << "Current Application Window";
    }

    bool ok = false;
    const QString selected = QInputDialog::getItem(
        this,
        tr("Select content window"),
        tr("Window to share:"),
        windowTitles,
        0,
        false,
        &ok
    );

    if (!ok || selected.isEmpty()) {
        QT_TRACE(1, "Content send cancelled or no selection");
        return;
    }

    QtVideoManager::instance().requestContentSend(selected);
    setConnectionStatus("Content source selected: " + selected);
    QMessageBox::information(this,
                             tr("Content source set"),
                             tr("Sharing source: %1\n(送信パイプラインはこの後のステップで実装)").arg(selected));
}

void QtVideoMainWindow::onRemoteWindowClosed()
{
    // リモートウィンドウが閉じられた場合、結合モードに戻す
    if (m_windowsSeparated) {
        m_windowsSeparated = false;
        
        // リモートビデオをスプリッターに戻す
        m_remoteVideo->setWindowFlags(Qt::Widget);
        m_remoteVideo->setParent(m_videoSplitter);
        m_videoSplitter->addWidget(m_remoteVideo);
        m_remoteVideo->show();
        
        // サイズを均等に
        m_videoSplitter->setSizes({1, 1});
        
        // ボタンテキスト変更
        m_separateButton->setText("↗ Separate");
        m_separateButton->setToolTip("Separate local and remote video into different windows");
        
        QT_TRACE(1, "Remote window closed, combined back");
    }
}

void QtVideoMainWindow::appendClearHistoryItem()
{
    // 既に追加済みなら何もしない
    int existing = m_addressCombo->findData(true, Qt::UserRole + 1);
    if (existing >= 0)
        return;

    // 区別しやすいプレースホルダ
    m_addressCombo->addItem("Clear history...");
    int idx = m_addressCombo->count() - 1;
    m_addressCombo->setItemData(idx, true, Qt::UserRole + 1);
}

bool QtVideoMainWindow::isClearHistoryItem(int index) const
{
    if (index < 0 || index >= m_addressCombo->count())
        return false;
    QVariant marker = m_addressCombo->itemData(index, Qt::UserRole + 1);
    return marker.isValid() && marker.toBool();
}

#endif // USE_QT6
