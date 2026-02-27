/**
 * @file qt_video_window.cpp
 * @brief Qt6ベースのビデオ表示ウィンドウ実装
 */

#ifdef USE_QT6

#ifdef nil
#undef nil
#endif

#include "qt_video_window.h"
#include <QDebug>
#include <QThread>
#include <QCoreApplication>
#include <QPainterPath>
#include <QPointer>
#include <QList>
#include <QResizeEvent>
#include <QSet>
#include <QTimer>       // Phase 1: Audio Visualizer Timer
#include <QMessageBox>  // Phase 1: Multi-Device Audio UI
#include <QFileDialog>
#include <QStandardPaths>
#include <QFileInfo>
#include <QGroupBox>    // Phase 1: Multi-Device Audio UI
#include <QScrollArea>  // Phase 1: Multi-Device Audio UI - スクロール対応
#ifdef Q_OS_MAC
#include <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
#  ifdef nil
#    undef nil
#  endif
#endif
#include <atomic>
#include <cmath>
#include <algorithm>
#include <vector>
#include "main.h"

namespace {
constexpr int kGainSteps = 10;
const int kDbTable[kGainSteps]   = {-12, -6, -3, 0, 3, 6, 9, 12, 15, 18};
const double kLinTable[kGainSteps] = {0.25, 0.50, 0.71, 1.00, 1.41, 2.00, 2.82, 4.00, 5.62, 7.94};
constexpr int kAudioProfileMin = 0;
constexpr int kAudioProfileMax = 3;
constexpr int kAudioProfileDefault = 2;

int NormalizeAudioProfileIndex(int index)
{
    if (index < kAudioProfileMin) {
        return kAudioProfileMin;
    }
    if (index > kAudioProfileMax) {
        return kAudioProfileMax;
    }
    return index;
}

const char* AudioProfileNameFromIndex(int index)
{
    switch (NormalizeAudioProfileIndex(index)) {
        case 0: return "Low";
        case 1: return "High";
        case 2: return "Middle";
        case 3: return "MAX";
        default: return "Middle";
    }
}

const char* AudioProfileShortNameFromIndex(int index)
{
    switch (NormalizeAudioProfileIndex(index)) {
        case 0: return "Low";
        case 1: return "High";
        case 2: return "Middle";
        case 3: return "MAX";
        default: return "Middle";
    }
}

[[maybe_unused]] double DbToLinear(double db)
{
    return std::pow(10.0, db / 20.0);
}

[[maybe_unused]] double LinearToDb(double linear)
{
    if (linear <= 0.0) {
        return -120.0;
    }
    return 20.0 * std::log10(linear);
}

int FindNearestGainIndex(double linearGain)
{
    int bestIndex = 0;
    double bestDiff = std::abs(kLinTable[0] - linearGain);
    for (int i = 1; i < kGainSteps; i++) {
        double diff = std::abs(kLinTable[i] - linearGain);
        if (diff < bestDiff) {
            bestDiff = diff;
            bestIndex = i;
        }
    }
    return bestIndex;
}
}

#ifndef kCGNullWindowID
#define kCGNullWindowID 0
#endif

#ifdef Q_OS_MAC
// Convert CGImageRef to QImage using public CoreGraphics API
static QImage QImageFromCGImageRef(CGImageRef image)
{
    if (!image) {
        return QImage();
    }

    const size_t width = CGImageGetWidth(image);
    const size_t height = CGImageGetHeight(image);
    if (width == 0 || height == 0) {
        return QImage();
    }

    QImage img(static_cast<int>(width), static_cast<int>(height), QImage::Format_ARGB32_Premultiplied);
    if (img.isNull()) {
        return QImage();
    }

    CGContextRef ctx = CGBitmapContextCreate(
        img.bits(),
        width,
        height,
        8,
        static_cast<size_t>(img.bytesPerLine()),
        CGImageGetColorSpace(image),
        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host);

    if (!ctx) {
        return QImage();
    }

    CGContextDrawImage(ctx, CGRectMake(0, 0, width, height), image);
    CGContextRelease(ctx);
    return img;
}

// macOS 15 SDK marks CGWindowListCreateImage unavailable; resolve it dynamically.
static CGImageRef CallCGWindowListCreateImage(CGRect bounds,
                                              CGWindowListOption listOpts,
                                              CGWindowID windowID,
                                              CGWindowImageOption opts)
{
    using CGWindowListCreateImageFunc =
        CGImageRef (*)(CGRect, CGWindowListOption, CGWindowID, CGWindowImageOption);
    static CGWindowListCreateImageFunc func = nullptr;
    static bool resolved = false;

    if (!resolved) {
        // Try RTLD_DEFAULT first; CoreGraphics is already loaded in most cases.
        func = reinterpret_cast<CGWindowListCreateImageFunc>(
            dlsym(RTLD_DEFAULT, "CGWindowListCreateImage"));
        if (!func) {
            void* handle = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_LAZY);
            if (handle) {
                func = reinterpret_cast<CGWindowListCreateImageFunc>(
                    dlsym(handle, "CGWindowListCreateImage"));
            }
        }
        resolved = true;
    }

    if (!func) {
        PTRACE(1, "QtVideo\tCGWindowListCreateImage symbol not available (macOS 15 SDK); returning null");
        return nullptr;
    }

    return func(bounds, listOpts, windowID, opts);
}

// Request/verify Screen Recording permission to avoid black frames
static bool EnsureScreenCaptureAccess()
{
    using ScreenCaptureAccessFunc = bool (*)(void);

    static bool checked = false;
    static bool allowed = true;

    if (checked)
        return allowed;

    checked = true;

    ScreenCaptureAccessFunc preflight = reinterpret_cast<ScreenCaptureAccessFunc>(
        dlsym(RTLD_DEFAULT, "CGPreflightScreenCaptureAccess"));
    ScreenCaptureAccessFunc request = reinterpret_cast<ScreenCaptureAccessFunc>(
        dlsym(RTLD_DEFAULT, "CGRequestScreenCaptureAccess"));

    if (!preflight) {
        // Older SDKs may not have these symbols; assume allowed and proceed
        return allowed;
    }

    allowed = preflight();
    if (!allowed && request) {
        allowed = request();
    }

    if (!allowed) {
        PTRACE(1, "QtVideo\tScreen Recording permission denied; H.239 content capture will be black. "
                   "Enable screen recording for this app in macOS Privacy & Security.");
    } else {
        PTRACE(2, "QtVideo\tScreen Recording permission granted for content capture");
    }

    return allowed;
}
#endif

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

void QtVideoWidget::clearFrameToBlack(unsigned width, unsigned height)
{
    QMutexLocker locker(&m_frameMutex);

    unsigned targetWidth = width;
    unsigned targetHeight = height;
    if (targetWidth == 0 || targetHeight == 0) {
        if (m_frameWidth > 0 && m_frameHeight > 0) {
            targetWidth = m_frameWidth;
            targetHeight = m_frameHeight;
        } else {
            targetWidth = 640;
            targetHeight = 480;
        }
    }

    if (m_currentFrame.width() != static_cast<int>(targetWidth) ||
        m_currentFrame.height() != static_cast<int>(targetHeight)) {
        m_currentFrame = QImage(targetWidth, targetHeight, QImage::Format_RGB888);
    }
    m_currentFrame.fill(Qt::black);
    m_frameWidth = targetWidth;
    m_frameHeight = targetHeight;

    // 切断時はアイコン表示状態も通常に戻す
    m_cameraMuted = false;
    m_muted = false;

    updateTargetRect();
    m_needsRepaint = true;
    update();
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
// AudioDeviceRowWidget Implementation (Phase 1)
///////////////////////////////////////////////////////////////////////////////

AudioDeviceRowWidget::AudioDeviceRowWidget(int deviceType, 
                                           const QStringList& availableDevices,
                                           QWidget* parent)
    : QWidget(parent)
    , m_deviceType(deviceType)
    , m_deviceCombo(nullptr)
    , m_gainSlider(nullptr)
    , m_gainLabel(nullptr)
    , m_muteCheckbox(nullptr)
    , m_removeButton(nullptr)
    , m_spectrum(nullptr)
{
    setupUI(availableDevices);
}

AudioDeviceRowWidget::~AudioDeviceRowWidget()
{
    // Qt親子関係で自動削除されるため、明示的なdeleteは不要
}

void AudioDeviceRowWidget::setupUI(const QStringList& availableDevices)
{
    // メインレイアウト（縦方向）
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // 旧式ビジュアライザー（上段）
    m_spectrum = new QtAudioSpectrumWidget(this);
    // 旧式と同じ高さ（トップのスペクトラムと同サイズ）
    m_spectrum->setMinimumHeight(60);
    m_spectrum->setMaximumHeight(80);
    m_spectrum->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    if (m_deviceType == QT_DEVICE_TYPE_MIC) {
        m_spectrum->setBarColor(QColor(0, 180, 255));  // 旧式と同系色
    } else {
        m_spectrum->setBarColor(QColor(0, 200, 100));  // スピーカー系
    }
    mainLayout->addWidget(m_spectrum);

    // コントロール行（下段）
    QHBoxLayout* layout = new QHBoxLayout();
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    // デバイス選択コンボボックス
    m_deviceCombo = new QComboBox(this);
    m_deviceCombo->addItems(availableDevices);
    m_deviceCombo->setMinimumWidth(200);
    layout->addWidget(m_deviceCombo);

    // 旧式ゲインUI (MIN - slider - MAX - dB label)
    QLabel* gainLabel = new QLabel(m_deviceType == QT_DEVICE_TYPE_MIC ? "Gain:" : "Volume:", this);
    layout->addWidget(gainLabel);

    QLabel* minL = new QLabel("MIN", this);
    minL->setStyleSheet("color: #888; font-size: 10px;");
    QLabel* maxL = new QLabel("MAX", this);
    maxL->setStyleSheet("color: #888; font-size: 10px;");

    m_gainSlider = new QSlider(Qt::Horizontal, this);
    m_gainSlider->setRange(0, kGainSteps - 1);   // 0=-12dB, 9=+18dB
    m_gainSlider->setTickInterval(1);
    m_gainSlider->setTickPosition(QSlider::TicksBelow);
    m_gainSlider->setPageStep(1);
    m_gainSlider->setValue(3); // 0 dB 初期値
    m_gainSlider->setMinimumWidth(150);
    m_gainSlider->setMaximumWidth(200);

    m_gainLabel = new QLabel("0 dB", this);
    m_gainLabel->setMinimumWidth(60);

    layout->addWidget(minL);
    layout->addWidget(m_gainSlider);
    layout->addWidget(maxL);
    layout->addWidget(m_gainLabel);

    // ミュートチェックボックス（全体ミュートのみのため非表示）
    m_muteCheckbox = new QCheckBox("Mute", this);
    m_muteCheckbox->setVisible(false);

    // 削除ボタン
    m_removeButton = new QPushButton("×", this);
    m_removeButton->setMaximumWidth(30);
    m_removeButton->setToolTip("Remove this device");
    layout->addWidget(m_removeButton);

    layout->addStretch();
    mainLayout->addLayout(layout);

    // シグナル接続
    connect(m_deviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AudioDeviceRowWidget::onDeviceComboChanged);
    connect(m_gainSlider, &QSlider::valueChanged,
            this, &AudioDeviceRowWidget::onGainSliderChanged);
    connect(m_removeButton, &QPushButton::clicked,
            this, &AudioDeviceRowWidget::onRemoveClicked);
}

QString AudioDeviceRowWidget::formatGainLabel(double gainDb) const
{
    if (gainDb >= 0.0) {
        return QString("+%1 dB").arg(gainDb, 0, 'f', 1);
    } else {
        return QString("%1 dB").arg(gainDb, 0, 'f', 1);
    }
}

AudioDeviceEntry AudioDeviceRowWidget::getDeviceEntry() const
{
    AudioDeviceEntry entry;
    entry.name = m_deviceCombo->currentText();
    const int idx = m_gainSlider->value();
    entry.gain = kLinTable[idx];
    entry.muted = false;
    return entry;
}

void AudioDeviceRowWidget::setDeviceEntry(const AudioDeviceEntry& entry)
{
    // デバイス名で検索してコンボボックスをセット
    int index = m_deviceCombo->findText(entry.name);
    if (index >= 0) {
        m_deviceCombo->setCurrentIndex(index);
    }

    // ゲイン設定 (-12dB ~ +18dB)
    const int gainIndex = FindNearestGainIndex(entry.gain);
    m_gainSlider->setValue(gainIndex);
}

void AudioDeviceRowWidget::updateDeviceList(const QStringList& devices)
{
    QString currentDevice = m_deviceCombo->currentText();
    m_deviceCombo->clear();
    m_deviceCombo->addItems(devices);

    // 以前選択していたデバイスを復元
    int index = m_deviceCombo->findText(currentDevice);
    if (index >= 0) {
        m_deviceCombo->setCurrentIndex(index);
    }
}

void AudioDeviceRowWidget::setRemoveButtonVisible(bool show)
{
    m_removeButton->setVisible(show);
}

void AudioDeviceRowWidget::setMuteButtonVisible(bool show)
{
    if (m_muteCheckbox) {
        m_muteCheckbox->setVisible(show);
    }
}

void AudioDeviceRowWidget::setDeviceSelectionEnabled(bool enabled)
{
    if (m_deviceCombo) {
        m_deviceCombo->setEnabled(enabled);
    }
    if (m_removeButton) {
        m_removeButton->setEnabled(enabled);
    }
}

void AudioDeviceRowWidget::onDeviceComboChanged(int index)
{
    PTRACE(0, "QtVideo\t🟣 AudioDeviceRowWidget::onDeviceComboChanged() index=" << index);
    PTRACE(0, "QtVideo\t🟣 Selected device: " << m_deviceCombo->currentText().toStdString());
    Q_UNUSED(index);
    PTRACE(0, "QtVideo\t🟣 Emitting deviceChanged signal...");
    emit deviceChanged();
    PTRACE(0, "QtVideo\t🟣 deviceChanged signal emitted");
}

void AudioDeviceRowWidget::onGainSliderChanged(int value)
{
    int idx = qBound(0, value, kGainSteps - 1);
    const int db = kDbTable[idx];
    const QString label = QString("%1%2 dB").arg(db > 0 ? "+" : "").arg(db);
    m_gainLabel->setText(label);
    emit gainChanged();  // ゲイン専用シグナル
}

void AudioDeviceRowWidget::onMuteToggled(bool checked)
{
    Q_UNUSED(checked);
    emit gainChanged();  // ミュートもゲイン設定の一部
}

void AudioDeviceRowWidget::onRemoveClicked()
{
    emit removeRequested(this);
}

void AudioDeviceRowWidget::updateSpectrum(const int16_t* pcmData, size_t sampleCount, int channels, int sampleRate)
{
    if (!m_spectrum) {
        return;
    }
    if (!pcmData || sampleCount == 0) {
        resetSpectrum(sampleCount, sampleRate);
        return;
    }
    m_spectrum->updateSpectrum(pcmData, sampleCount, channels, sampleRate);
}

void AudioDeviceRowWidget::resetSpectrum(size_t sampleCount, int sampleRate)
{
    if (!m_spectrum) {
        return;
    }
    const size_t fallbackSamples = (sampleCount > 0) ? sampleCount : 160;
    std::vector<int16_t> silence(fallbackSamples, 0);
    m_spectrum->updateSpectrum(silence.data(), silence.size(), 1, sampleRate);
}

///////////////////////////////////////////////////////////////////////////////
// VideoDeviceRowWidget Implementation (Phase 2)
///////////////////////////////////////////////////////////////////////////////

VideoDeviceRowWidget::VideoDeviceRowWidget(const QStringList& availableDevices,
                                           QWidget* parent)
    : QWidget(parent)
    , m_deviceCombo(nullptr)
    , m_muteCheckbox(nullptr)
    , m_removeButton(nullptr)
{
    setupUI(availableDevices);
}

VideoDeviceRowWidget::~VideoDeviceRowWidget()
{
    // Qt親子関係で自動削除されるため、明示的なdeleteは不要
}

void VideoDeviceRowWidget::setupUI(const QStringList& availableDevices)
{
    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(8);

    m_deviceCombo = new QComboBox(this);
    m_deviceCombo->addItems(availableDevices);
    m_deviceCombo->setMinimumWidth(240);
    layout->addWidget(m_deviceCombo);

    m_muteCheckbox = new QCheckBox("Mute", this);
    layout->addWidget(m_muteCheckbox);

    m_removeButton = new QPushButton("×", this);
    m_removeButton->setMaximumWidth(30);
    m_removeButton->setToolTip("Remove this camera");
    layout->addWidget(m_removeButton);

    layout->addStretch();

    connect(m_deviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &VideoDeviceRowWidget::onDeviceComboChanged);
    connect(m_muteCheckbox, &QCheckBox::toggled,
            this, &VideoDeviceRowWidget::onMuteToggled);
    connect(m_removeButton, &QPushButton::clicked,
            this, &VideoDeviceRowWidget::onRemoveClicked);
}

VideoDeviceEntry VideoDeviceRowWidget::getDeviceEntry() const
{
    VideoDeviceEntry entry;
    entry.name = m_deviceCombo->currentText();
    entry.muted = m_muteCheckbox ? m_muteCheckbox->isChecked() : false;
    return entry;
}

void VideoDeviceRowWidget::setDeviceEntry(const VideoDeviceEntry& entry)
{
    int index = m_deviceCombo->findText(entry.name);
    if (index >= 0) {
        m_deviceCombo->setCurrentIndex(index);
    }
    if (m_muteCheckbox) {
        m_muteCheckbox->setChecked(entry.muted);
    }
}

void VideoDeviceRowWidget::updateDeviceList(const QStringList& devices)
{
    QString currentDevice = m_deviceCombo->currentText();
    m_deviceCombo->clear();
    m_deviceCombo->addItems(devices);
    int index = m_deviceCombo->findText(currentDevice);
    if (index >= 0) {
        m_deviceCombo->setCurrentIndex(index);
    }
}

void VideoDeviceRowWidget::setRemoveButtonVisible(bool show)
{
    m_removeButton->setVisible(show);
}

void VideoDeviceRowWidget::setMuteButtonVisible(bool show)
{
    if (m_muteCheckbox) {
        m_muteCheckbox->setVisible(show);
    }
}

void VideoDeviceRowWidget::onDeviceComboChanged(int index)
{
    Q_UNUSED(index);
    emit deviceChanged();
}

void VideoDeviceRowWidget::onMuteToggled(bool checked)
{
    Q_UNUSED(checked);
    emit deviceChanged();
}

void VideoDeviceRowWidget::onRemoveClicked()
{
    emit removeRequested(this);
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
    , m_multiDeviceButton(nullptr)
    , m_windowsSeparated(false)
    , m_addressCombo(nullptr)
    , m_connectButton(nullptr)
    , m_disconnectButton(nullptr)
    , m_exitButton(nullptr)
    , m_recordButton(nullptr)
    , m_muteCheckbox(nullptr)
    , m_cameraCheckbox(nullptr)
    , m_audioProfileSlider(nullptr)
    , m_audioProfileNameLabel(nullptr)
    , m_audioProfileIndex(kAudioProfileDefault)
    , m_contentButton(nullptr)
    , m_contentSendButton(nullptr)
    , m_gainRowWidget(nullptr)
    , m_gainSlider(nullptr)
    , m_gainValueLabel(nullptr)
    , m_spkGainSlider(nullptr)
    , m_spkGainValueLabel(nullptr)
    , m_baseMicGainIndex(3)
    , m_baseSpeakerGainIndex(3)
    , m_micCombo(nullptr)
    , m_speakerCombo(nullptr)
    , m_cameraCombo(nullptr)
    , m_overlayTextButton(nullptr)
    , m_backgroundBlurButton(nullptr)
    , m_statusLabel(nullptr)
    , m_overlayTextDialog(nullptr)
    , m_overlayTextLineEdit(nullptr)
    , m_overlayText()
    , m_overlayTextEnabled(false)
    , m_backgroundBlurDialog(nullptr)
    , m_backgroundBlurEnableCheck(nullptr)
    , m_backgroundBlurStrengthCombo(nullptr)
    , m_backgroundBlurEnabled(false)
    , m_backgroundBlurStrength(1)
    , m_localSpectrum(nullptr)
    , m_remoteSpectrum(nullptr)
    , m_h323Connection(nullptr)
    , m_running(true)
    , m_micRowsLayout(nullptr)           // Phase 1
    , m_speakerRowsLayout(nullptr)       // Phase 1
    , m_addMicButton(nullptr)            // Phase 1
    , m_addSpeakerButton(nullptr)        // Phase 1
    , m_audioVisualizerTimer(nullptr)    // Phase 1
    , m_connectBlinkTimer(nullptr)
    , m_connectBlinkOn(false)
    , m_connectBlinkActive(false)
    , m_connectEstablished(false)
    , m_recordingActive(false)
    , m_multiDeviceScrollArea(nullptr)   // Phase 1
    , m_multiDeviceWindow(nullptr)
    , m_cameraRowsLayout(nullptr)
    , m_addCameraButton(nullptr)
{
    PTRACE(0, "QtVideo\t🟨 QtVideoMainWindow constructor ENTER");
    
    setWindowTitle("H323ASKW - Video Client");
    setMinimumSize(1000, 700);

    setupUI();
    setupConnections();
    populateDeviceLists();
    
    // オーディオビジュアライザータイマーを初期化（75ms間隔）
    m_audioVisualizerTimer = new QTimer(this);
    m_audioVisualizerTimer->setInterval(75);
    connect(m_audioVisualizerTimer, &QTimer::timeout, this, &QtVideoMainWindow::updateAudioVisualizers);
    // タイマーはonConnectClicked()で開始、onDisconnectClicked()で停止

    m_connectBlinkTimer = new QTimer(this);
    m_connectBlinkTimer->setInterval(500);
    connect(m_connectBlinkTimer, &QTimer::timeout, this, [this]() {
        if (!m_connectBlinkActive)
            return;
        m_connectBlinkOn = !m_connectBlinkOn;
        updateConnectButtonStyle();
    });

    QT_TRACE(1, "QtVideoMainWindow created");
    PTRACE(0, "QtVideo\t🟨 QtVideoMainWindow constructor EXIT");
}

QtVideoMainWindow::~QtVideoMainWindow()
{
    QT_TRACE(1, "QtVideoMainWindow destructor ENTER");
    
    // 通話フラグをクリア（タイマーコールバックを停止）
    m_isCallActive.store(false, std::memory_order_release);
    
    // タイマーを停止
    if (m_audioVisualizerTimer) {
        m_audioVisualizerTimer->stop();
        m_audioVisualizerTimer->deleteLater();
        m_audioVisualizerTimer = nullptr;
    }
    if (m_connectBlinkTimer) {
        m_connectBlinkTimer->stop();
        m_connectBlinkTimer->deleteLater();
        m_connectBlinkTimer = nullptr;
    }
    
    // 親子関係で自動削除される前に、手動でクリア（安全のため）
    m_micRows.clear();
    m_speakerRows.clear();
    
    // 終了時に履歴を保存
    saveAddressHistory();
    m_running = false;
    
    QT_TRACE(1, "QtVideoMainWindow destroyed");
}

void QtVideoMainWindow::updateConnectButtonStyle()
{
    if (!m_connectButton)
        return;

    QString borderColor = "#2e7d32";
    QString backgroundColor = "transparent";
    QString textColor = "#1b5e20";
    QString disabledBorderColor = "#9e9e9e";
    QString disabledTextColor = "#7a7a7a";

    if (m_connectEstablished) {
        // 接続完了時は色反転（塗りつぶし＋白文字）
        borderColor = "#2e7d32";
        backgroundColor = "#2e7d32";
        textColor = "#ffffff";
        disabledBorderColor = borderColor;
        disabledTextColor = "#ffffff";
    } else if (m_connectBlinkActive) {
        borderColor = m_connectBlinkOn ? "#2e7d32" : "#9e9e9e";
        backgroundColor = m_connectBlinkOn ? "#e8f5e9" : "transparent";
        textColor = m_connectBlinkOn ? "#1b5e20" : "#4a4a4a";
        disabledBorderColor = borderColor;
        disabledTextColor = textColor;
    }

    m_connectButton->setStyleSheet(QString(
        "QPushButton#connectButton {"
        "  border: 3px solid %1;"
        "  border-radius: 4px;"
        "  padding: 3px 10px;"
        "  background-color: %2;"
        "  color: %3;"
        "}"
        "QPushButton#connectButton:disabled {"
        "  border: 3px solid %4;"
        "  border-radius: 4px;"
        "  padding: 3px 10px;"
        "  background-color: %2;"
        "  color: %5;"
        "}"
    ).arg(borderColor, backgroundColor, textColor, disabledBorderColor, disabledTextColor));
}

void QtVideoMainWindow::updateDisconnectButtonStyle()
{
    if (!m_disconnectButton)
        return;

    m_disconnectButton->setStyleSheet(
        "QPushButton#disconnectButton {"
        "  border: 3px solid #c62828;"
        "  border-radius: 4px;"
        "  padding: 3px 10px;"
        "}"
        "QPushButton#disconnectButton:disabled {"
        "  border-color: #9e9e9e;"
        "}"
    );
}

void QtVideoMainWindow::updateRecordButtonStyle()
{
    if (!m_recordButton)
        return;

    if (m_recordingActive) {
        m_recordButton->setStyleSheet(
            "QPushButton#recordButton {"
            "  border: 3px solid #8b0000;"
            "  border-radius: 4px;"
            "  padding: 3px 10px;"
            "  background-color: #c62828;"
            "  color: white;"
            "  font-weight: 700;"
            "}"
            "QPushButton#recordButton:disabled {"
            "  border-color: #9e9e9e;"
            "  background-color: #cccccc;"
            "  color: #777777;"
            "}"
        );
    } else {
        m_recordButton->setStyleSheet(
            "QPushButton#recordButton {"
            "  border: 3px solid #c62828;"
            "  border-radius: 4px;"
            "  padding: 3px 10px;"
            "  background-color: transparent;"
            "  color: #8b0000;"
            "  font-weight: 600;"
            "}"
            "QPushButton#recordButton:disabled {"
            "  border-color: #9e9e9e;"
            "  color: #7a7a7a;"
            "}"
        );
    }
}

void QtVideoMainWindow::setRecordingUiState(bool recording)
{
    m_recordingActive = recording;
    if (m_recordButton) {
        m_recordButton->setText(recording ? "■ Stop REC" : "● REC");
        m_recordButton->setToolTip(recording ? "Stop recording" : "Start recording");
    }
    updateRecordButtonStyle();
}

void QtVideoMainWindow::startConnectBlink()
{
    if (m_connectEstablished)
        return;

    if (m_connectBlinkActive)
        return;

    m_connectBlinkActive = true;
    m_connectBlinkOn = true;

    if (m_connectBlinkTimer && !m_connectBlinkTimer->isActive())
        m_connectBlinkTimer->start();

    updateConnectButtonStyle();
}

void QtVideoMainWindow::stopConnectBlink()
{
    m_connectBlinkActive = false;
    m_connectBlinkOn = false;

    if (m_connectBlinkTimer && m_connectBlinkTimer->isActive())
        m_connectBlinkTimer->stop();

    updateConnectButtonStyle();
}

void QtVideoMainWindow::setupUI()
{
    PTRACE(0, "QtVideo\t🟨 setupUI() ENTER");
    
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

    // 入出力ゲインスライダー（Mic / Speaker を横並び）
    m_gainRowWidget = new QWidget(this);
    QHBoxLayout* gainRow = new QHBoxLayout(m_gainRowWidget);

    auto buildGainBox = [this](const QString& title,
                               QSlider** sliderOut,
                               QLabel** valueLabelOut) -> QVBoxLayout* {
        QVBoxLayout* box = new QVBoxLayout();
        QLabel* titleLabel = new QLabel(title, this);
        titleLabel->setAlignment(Qt::AlignCenter);
        titleLabel->setStyleSheet("font-weight: 600; color: #666;");
        box->addWidget(titleLabel);

        QHBoxLayout* row = new QHBoxLayout();
        QLabel* minL = new QLabel("MIN", this);
        minL->setStyleSheet("color: #888; font-size: 10px;");
        QLabel* maxL = new QLabel("MAX", this);
        maxL->setStyleSheet("color: #888; font-size: 10px;");

        QSlider* slider = new QSlider(Qt::Horizontal, this);
        slider->setRange(0, kGainSteps - 1);   // 0=-12dB, 9=+18dB
        slider->setTickInterval(1);
        slider->setTickPosition(QSlider::TicksBelow);
        slider->setPageStep(1);
        slider->setValue(3); // 0 dB 初期値

        QLabel* val = new QLabel("0 dB", this);
        val->setMinimumWidth(50);
        val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        row->addWidget(minL);
        row->addWidget(slider, 1);
        row->addWidget(maxL);
        row->addWidget(val);

        box->addLayout(row);
        *sliderOut = slider;
        *valueLabelOut = val;
        return box;
    };

    gainRow->addLayout(buildGainBox("Mic Gain", &m_gainSlider, &m_gainValueLabel));
    
    // Mic GainとSpeaker Gainの間に区切り線
    QFrame* gainSeparator = new QFrame(this);
    gainSeparator->setFrameShape(QFrame::VLine);
    gainSeparator->setFrameShadow(QFrame::Sunken);
    gainSeparator->setLineWidth(2);
    gainRow->addWidget(gainSeparator);
    
    gainRow->addLayout(buildGainBox("Speaker Gain", &m_spkGainSlider, &m_spkGainValueLabel));

    mainLayout->addWidget(m_gainRowWidget);

    // コントロールパネル
    QHBoxLayout* controlLayout = new QHBoxLayout();

    // 接続先入力（履歴付きコンボボックス）
    QLabel* addressLabel = new QLabel("Address:", this);
    m_addressCombo = new QComboBox(this);
    m_addressCombo->setEditable(true);
    m_addressCombo->setInsertPolicy(QComboBox::NoInsert);  // 自動挿入しない（手動管理）
    m_addressCombo->lineEdit()->setPlaceholderText("IP or H.323 alias");
    m_addressCombo->setMinimumWidth(180);
    m_addressCombo->setMaximumWidth(240);
    m_addressCombo->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    
    // 履歴を読み込み
    loadAddressHistory();

    m_connectButton = new QPushButton("Connect", this);
    m_connectButton->setObjectName("connectButton");
    m_disconnectButton = new QPushButton("Disconnect", this);
    m_disconnectButton->setObjectName("disconnectButton");
    m_exitButton = new QPushButton("EXIT", this);
    m_exitButton->setObjectName("exitButton");
    m_recordButton = new QPushButton("● REC", this);
    m_recordButton->setObjectName("recordButton");
    updateConnectButtonStyle();
    updateDisconnectButtonStyle();
    setRecordingUiState(false);
    m_disconnectButton->setEnabled(false);
    m_recordButton->setEnabled(false);

    controlLayout->addWidget(addressLabel);
    controlLayout->addWidget(m_addressCombo);
    controlLayout->addWidget(m_connectButton);
    controlLayout->addWidget(m_disconnectButton);
    controlLayout->addWidget(m_exitButton);
    controlLayout->addWidget(m_recordButton);

    // 音声品質プロファイル（Mute Micの左隣に横スライダー）
    QWidget* audioProfileWidget = new QWidget(this);
    QVBoxLayout* audioProfileLayout = new QVBoxLayout(audioProfileWidget);
    audioProfileLayout->setContentsMargins(0, 0, 0, 0);
    audioProfileLayout->setSpacing(1);

    m_audioProfileNameLabel = new QLabel(AudioProfileShortNameFromIndex(m_audioProfileIndex), this);
    m_audioProfileNameLabel->setStyleSheet("font-size: 8px; color: #444; font-weight: 600;");
    m_audioProfileNameLabel->setAlignment(Qt::AlignHCenter | Qt::AlignBottom);
    audioProfileLayout->addWidget(m_audioProfileNameLabel, 0, Qt::AlignHCenter);

    m_audioProfileSlider = new QSlider(Qt::Horizontal, this);
    m_audioProfileSlider->setRange(kAudioProfileMin, kAudioProfileMax);
    m_audioProfileSlider->setTickInterval(1);
    m_audioProfileSlider->setTickPosition(QSlider::NoTicks);
    m_audioProfileSlider->setSingleStep(1);
    m_audioProfileSlider->setPageStep(1);
    m_audioProfileSlider->setValue(m_audioProfileIndex);
    m_audioProfileSlider->setFixedSize(44, 14);
    audioProfileLayout->addWidget(m_audioProfileSlider, 0, Qt::AlignHCenter);
    m_audioProfileNameLabel->setMinimumWidth(44);
    audioProfileWidget->setFixedSize(48, 32);

    // ミュート/カメラ
    m_muteCheckbox = new QCheckBox("Mute Mic", this);
    m_cameraCheckbox = new QCheckBox("Camera Off", this);
    m_muteCheckbox->setEnabled(false);
    m_cameraCheckbox->setEnabled(false);
    
    controlLayout->addWidget(audioProfileWidget);
    controlLayout->addWidget(m_muteCheckbox);
    controlLayout->addWidget(m_cameraCheckbox);

    // ウィンドウ分離ボタン
    m_separateButton = new QPushButton("↗ Separate", this);
    m_separateButton->setToolTip("Separate local and remote video into different windows");
    controlLayout->addWidget(m_separateButton);

    // マルチデバイス設定ウィンドウ表示ボタン
    m_multiDeviceButton = new QPushButton("Multi-Device", this);
    m_multiDeviceButton->setToolTip("Show Multi-Device Audio and Multi-Camera configuration");
    controlLayout->addWidget(m_multiDeviceButton);
    
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
    controlLayout->addStretch();

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

    m_overlayTextButton = new QPushButton("BG Text", this);
    m_overlayTextButton->setToolTip("Open background text input window");
    deviceLayout->addWidget(m_overlayTextButton);
    m_backgroundBlurButton = new QPushButton("BG Blur", this);
    m_backgroundBlurButton->setToolTip("Open background blur settings");
    deviceLayout->addWidget(m_backgroundBlurButton);

    deviceLayout->addStretch();

    mainLayout->addLayout(deviceLayout);

    setupMultiDeviceWindow();

    // ステータスバー
    m_statusLabel = new QLabel("Ready", this);
    statusBar()->addPermanentWidget(m_statusLabel);
}

void QtVideoMainWindow::setupMultiDeviceWindow()
{
    if (m_multiDeviceWindow != nullptr) {
        return;
    }

    m_multiDeviceWindow = new QDialog(this);
    m_multiDeviceWindow->setWindowFlags(Qt::Window);
    m_multiDeviceWindow->setWindowTitle("Multi-Device Configuration - H323ASKW");
    m_multiDeviceWindow->setMinimumSize(1180, 560);
    m_multiDeviceWindow->resize(1260, 680);

    QVBoxLayout* layout = new QVBoxLayout(m_multiDeviceWindow);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    setupMultiDeviceAudioUI(layout);
    setupMultiDeviceCameraUI(layout);
}

void QtVideoMainWindow::setupConnections()
{
    // ボタン接続
    connect(m_connectButton, &QPushButton::clicked, this, &QtVideoMainWindow::onConnectClicked);
    connect(m_disconnectButton, &QPushButton::clicked, this, &QtVideoMainWindow::onDisconnectClicked);
    connect(m_exitButton, &QPushButton::clicked, this, &QtVideoMainWindow::onExitClicked);
    connect(m_recordButton, &QPushButton::clicked, this, &QtVideoMainWindow::onRecordClicked);
    connect(m_addressCombo, QOverload<int>::of(&QComboBox::activated),
            this, &QtVideoMainWindow::onAddressActivated);
    
    // チェックボックス
    connect(m_muteCheckbox, &QCheckBox::toggled, this, &QtVideoMainWindow::onMuteToggled);
    connect(m_cameraCheckbox, &QCheckBox::toggled, this, &QtVideoMainWindow::onCameraToggled);
    if (m_audioProfileSlider) {
        connect(m_audioProfileSlider, &QSlider::valueChanged, this, &QtVideoMainWindow::onAudioProfileSliderChanged);
    }

    // ウィンドウ分離ボタン
    connect(m_separateButton, &QPushButton::clicked, this, &QtVideoMainWindow::onSeparateWindowsClicked);
    connect(m_multiDeviceButton, &QPushButton::clicked, this, &QtVideoMainWindow::onMultiDeviceWindowClicked);
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
    if (m_overlayTextButton) {
        connect(m_overlayTextButton, &QPushButton::clicked,
                this, &QtVideoMainWindow::onOverlayTextClicked);
    }
    if (m_backgroundBlurButton) {
        connect(m_backgroundBlurButton, &QPushButton::clicked,
                this, &QtVideoMainWindow::onBackgroundBlurClicked);
    }

    // フレーム受信（スレッドセーフ）
    connect(this, &QtVideoMainWindow::localFrameReady, 
            this, &QtVideoMainWindow::onLocalFrameReady, Qt::QueuedConnection);
    connect(this, &QtVideoMainWindow::remoteFrameReady, 
            this, &QtVideoMainWindow::onRemoteFrameReady, Qt::QueuedConnection);

    // マイクゲインスライダー
    auto applyGainIndex = [this](int idx) {
        if (idx < 0) idx = 0;
        if (idx >= kGainSteps) idx = kGainSteps - 1;
        m_baseMicGainIndex = idx;
        const int db = kDbTable[idx];
        const QString label = QString("%1%2 dB").arg(db > 0 ? "+" : "").arg(db);
        m_gainValueLabel->setText(label);
        onGainChanged();
    };
    auto applySpeakerGainIndex = [this](int idx) {
        if (idx < 0) idx = 0;
        if (idx >= kGainSteps) idx = kGainSteps - 1;
        m_baseSpeakerGainIndex = idx;
        const int db = kDbTable[idx];
        const QString label = QString("%1%2 dB").arg(db > 0 ? "+" : "").arg(db);
        m_spkGainValueLabel->setText(label);
        onGainChanged();
    };

    connect(m_gainSlider, &QSlider::valueChanged, this, applyGainIndex);
    connect(m_spkGainSlider, &QSlider::valueChanged, this, applySpeakerGainIndex);
    applyGainIndex(m_gainSlider->value());  // 初期値反映
    applySpeakerGainIndex(m_spkGainSlider->value());
    onAudioProfileSliderChanged(m_audioProfileIndex);

    // ==================== Phase 1: Multi-Device Audio UI Connections ====================
    PTRACE(0, "QtVideo\t🟦 Connecting m_addMicButton signal...");
    connect(m_addMicButton, &QPushButton::clicked, this, &QtVideoMainWindow::onAddMicClicked);
    PTRACE(0, "QtVideo\t🟦 Connecting m_addSpeakerButton signal...");
    connect(m_addSpeakerButton, &QPushButton::clicked, this, &QtVideoMainWindow::onAddSpeakerClicked);
    PTRACE(0, "QtVideo\t🟦 Phase 1 signals connected successfully");
    // ==================== End of Phase 1 Audio UI Connections ====================
    
    // ==================== Phase 2: Multi-Device Camera UI Connections ====================
    if (m_addCameraButton) {
        connect(m_addCameraButton, &QPushButton::clicked, this, &QtVideoMainWindow::onAddCameraClicked);
    }
    // ==================== End of Phase 2 Camera UI Connections ====================
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
    for (auto* row : m_cameraRows) {
        if (row) {
            row->updateDeviceList(cameraList);
        }
    }
    updateCameraRowControls();

    QT_TRACE(1, "Device lists populated (Mic=" << micList.size()
             << ", Spk=" << speakerList.size()
             << ", Cam=" << cameraList.size() << ")");

    // 初期選択をエンドポイントに反映
    QtVideoManager::instance().applyDeviceSelection(
        m_micCombo->currentText(),
        m_speakerCombo->currentText(),
        m_cameraCombo->currentText());

    // マルチデバイス設定も同期（旧式ベースデバイス含む）
    onDeviceRowChanged();
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
                safeThis->m_connectEstablished = true;
                safeThis->stopConnectBlink();
                if (safeThis->m_recordButton) {
                    safeThis->m_recordButton->setEnabled(true);
                }
                safeThis->setRecordingUiState(connection->IsRecording());
                safeThis->m_muteCheckbox->setEnabled(true);
                safeThis->m_cameraCheckbox->setEnabled(true);
                if (safeThis->m_micCombo)
                    safeThis->m_micCombo->setEnabled(false);
                if (safeThis->m_speakerCombo)
                    safeThis->m_speakerCombo->setEnabled(false);
                if (safeThis->m_addMicButton)
                    safeThis->m_addMicButton->setEnabled(false);
                if (safeThis->m_addSpeakerButton)
                    safeThis->m_addSpeakerButton->setEnabled(false);
                safeThis->updateDeviceRowControls(safeThis->m_micRows);
                safeThis->updateDeviceRowControls(safeThis->m_speakerRows);
                if (safeThis->m_contentSendButton)
                    safeThis->m_contentSendButton->setEnabled(true);
            } else {
                QT_TRACE(1, "H323Connection cleared - UI controls reset (via invokeMethod)");
                safeThis->m_disconnectButton->setEnabled(false);
                safeThis->m_connectButton->setEnabled(true);
                safeThis->m_connectEstablished = false;
                safeThis->stopConnectBlink();
                if (safeThis->m_recordButton) {
                    safeThis->m_recordButton->setEnabled(false);
                }
                safeThis->setRecordingUiState(false);
                safeThis->m_muteCheckbox->setEnabled(false);
                safeThis->m_cameraCheckbox->setEnabled(false);
                if (safeThis->m_micCombo)
                    safeThis->m_micCombo->setEnabled(true);
                if (safeThis->m_speakerCombo)
                    safeThis->m_speakerCombo->setEnabled(true);
                if (safeThis->m_addMicButton)
                    safeThis->m_addMicButton->setEnabled(true);
                if (safeThis->m_addSpeakerButton)
                    safeThis->m_addSpeakerButton->setEnabled(true);
                safeThis->updateDeviceRowControls(safeThis->m_micRows);
                safeThis->updateDeviceRowControls(safeThis->m_speakerRows);
                safeThis->m_muteCheckbox->setChecked(false);
                safeThis->m_cameraCheckbox->setChecked(false);
                if (safeThis->m_contentSendButton)
                    safeThis->m_contentSendButton->setEnabled(false);
                safeThis->clearVideoFramesToBlack();
            }
        }, Qt::QueuedConnection);
    } else {
        m_h323Connection = connection;
        if (connection) {
            QT_TRACE(1, "H323Connection set - UI controls enabled (direct call)");
            m_disconnectButton->setEnabled(true);
            m_connectButton->setEnabled(false);
            m_connectEstablished = true;
            stopConnectBlink();
            if (m_recordButton) {
                m_recordButton->setEnabled(true);
            }
            setRecordingUiState(connection->IsRecording());
            m_muteCheckbox->setEnabled(true);
            m_cameraCheckbox->setEnabled(true);
            if (m_micCombo)
                m_micCombo->setEnabled(false);
            if (m_speakerCombo)
                m_speakerCombo->setEnabled(false);
            if (m_addMicButton)
                m_addMicButton->setEnabled(false);
            if (m_addSpeakerButton)
                m_addSpeakerButton->setEnabled(false);
            updateDeviceRowControls(m_micRows);
            updateDeviceRowControls(m_speakerRows);
            if (m_contentSendButton)
                m_contentSendButton->setEnabled(true);
        } else {
            QT_TRACE(1, "H323Connection cleared - UI controls reset (direct call)");
            m_disconnectButton->setEnabled(false);
            m_connectButton->setEnabled(true);
            m_connectEstablished = false;
            stopConnectBlink();
            if (m_recordButton) {
                m_recordButton->setEnabled(false);
            }
            setRecordingUiState(false);
            m_muteCheckbox->setEnabled(false);
            m_cameraCheckbox->setEnabled(false);
            if (m_micCombo)
                m_micCombo->setEnabled(true);
            if (m_speakerCombo)
                m_speakerCombo->setEnabled(true);
            if (m_addMicButton)
                m_addMicButton->setEnabled(true);
            if (m_addSpeakerButton)
                m_addSpeakerButton->setEnabled(true);
            updateDeviceRowControls(m_micRows);
            updateDeviceRowControls(m_speakerRows);
            m_muteCheckbox->setChecked(false);
            m_cameraCheckbox->setChecked(false);
            if (m_contentSendButton)
                m_contentSendButton->setEnabled(false);
            clearVideoFramesToBlack();
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
    if (m_h323Connection == nullptr || !m_connectEstablished) {
        return;
    }

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
    if (m_h323Connection == nullptr || !m_connectEstablished) {
        return;
    }

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
    if (status.startsWith("Calling ") || status.startsWith("Connecting ")) {
        m_connectEstablished = false;
        startConnectBlink();
    } else if (status.startsWith("Call failed") || status.startsWith("Disconnected") || status.startsWith("Disconnecting")) {
        m_connectEstablished = false;
        stopConnectBlink();
    }

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

void QtVideoMainWindow::clearVideoFramesToBlack()
{
    if (m_localVideo) {
        m_localVideo->clearFrameToBlack();
    }
    if (m_remoteVideo) {
        m_remoteVideo->clearFrameToBlack();
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
    emit exitRequested();
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
        
        // 通話アクティブフラグを設定（接続確立前に設定）
        m_isCallActive.store(true, std::memory_order_release);
        m_connectEstablished = false;
        startConnectBlink();
        
        emit connectRequested(address);
        
        // オーディオビジュアライザータイマーを開始
        if (m_audioVisualizerTimer && !m_audioVisualizerTimer->isActive()) {
            m_audioVisualizerTimer->start();
            QT_TRACE(2, "Audio visualizer timer started");
        }
    }
}

void QtVideoMainWindow::onDisconnectClicked()
{
    QT_TRACE(1, "Disconnect requested");
    
    // 通話アクティブフラグを最初にクリア（タイマーコールバック停止）
    m_isCallActive.store(false, std::memory_order_release);
    m_connectEstablished = false;
    stopConnectBlink();
    clearVideoFramesToBlack();
    
    setConnectionStatus("Disconnecting...");
    emit disconnectRequested();
    
    // オーディオビジュアライザータイマーを停止
    if (m_audioVisualizerTimer && m_audioVisualizerTimer->isActive()) {
        m_audioVisualizerTimer->stop();
        QT_TRACE(2, "Audio visualizer timer stopped");
        
        // ビジュアライザーをリセット
        for (auto* row : m_micRows) {
            if (row) {
                row->resetSpectrum(160, 16000);
            }
        }
        for (auto* row : m_speakerRows) {
            if (row) {
                row->resetSpectrum(160, 16000);
            }
        }
    }
}

void QtVideoMainWindow::onExitClicked()
{
    QT_TRACE(1, "Exit requested");
    setConnectionStatus("Exiting...");
    emit exitRequested();
}

void QtVideoMainWindow::onRecordClicked()
{
    if (m_h323Connection == nullptr) {
        setConnectionStatus("No active call");
        return;
    }

    if (m_h323Connection->IsRecording()) {
        m_h323Connection->StopRecording();
        setRecordingUiState(false);
        setConnectionStatus("Recording stopped");
        return;
    }

    QString defaultDir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (defaultDir.isEmpty()) {
        defaultDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    }

    QSettings settings("h323askw", "QtVideoMainWindow");
    const QString lastDir = settings.value("recording/lastDir", defaultDir).toString();
    const QString defaultName = QString("call_%1.mp4").arg(
        QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));

    QString selectedPath = QFileDialog::getSaveFileName(
        this,
        tr("Save Recording"),
        QFileInfo(lastDir, defaultName).absoluteFilePath(),
        tr("MP4 files (*.mp4);;All files (*)"));

    if (selectedPath.isEmpty()) {
        return;
    }

    if (!selectedPath.endsWith(".mp4", Qt::CaseInsensitive)) {
        selectedPath += ".mp4";
    }

    settings.setValue("recording/lastDir", QFileInfo(selectedPath).absolutePath());

    if (!m_h323Connection->StartRecording(selectedPath.toUtf8().constData(), "mixed")) {
        QMessageBox::warning(this, tr("Recording"),
                             tr("Failed to start recording."));
        setRecordingUiState(false);
        return;
    }

    setRecordingUiState(true);
    setConnectionStatus("Recording: " + QFileInfo(selectedPath).fileName());
}

void QtVideoMainWindow::onAudioProfileSliderChanged(int value)
{
    const int index = NormalizeAudioProfileIndex(value);
    if (m_audioProfileSlider && m_audioProfileSlider->value() != index) {
        QSignalBlocker blocker(m_audioProfileSlider);
        m_audioProfileSlider->setValue(index);
    }

    m_audioProfileIndex = index;
    const QString name = QString::fromUtf8(AudioProfileNameFromIndex(index));
    const QString shortName = QString::fromUtf8(AudioProfileShortNameFromIndex(index));
    if (m_audioProfileNameLabel) {
        m_audioProfileNameLabel->setText(shortName);
    }

    PTRACE(1, "QtVideo\tAudio profile changed: index=" << index << " name=" << name.toStdString());
    emit audioProfileChanged(index);
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
    onDeviceRowChanged();  // マルチデバイス設定も更新
}

void QtVideoMainWindow::onSpeakerDeviceChanged(int index)
{
    Q_UNUSED(index);
    QString device = m_speakerCombo->currentText();
    QT_TRACE(1, "Speaker device changed: " << device.toStdString());
    QtVideoManager::instance().applyDeviceSelection(m_micCombo->currentText(), device, m_cameraCombo->currentText());
    onDeviceRowChanged();  // マルチデバイス設定も更新
}

void QtVideoMainWindow::onCameraDeviceChanged(int index)
{
    Q_UNUSED(index);
    QString device = m_cameraCombo->currentText();
    QT_TRACE(1, "Camera device changed: " << device.toStdString());
    if (m_cameraRows.isEmpty()) {
        QtVideoManager::instance().applyDeviceSelection(m_micCombo->currentText(), m_speakerCombo->currentText(), device);
    }
    onDeviceRowChanged();  // マルチカメラ設定も更新
}

void QtVideoMainWindow::onOverlayTextClicked()
{
    if (!m_overlayTextDialog) {
        m_overlayTextDialog = new QDialog(this);
        m_overlayTextDialog->setWindowFlags(Qt::Window);
        m_overlayTextDialog->setWindowTitle("Background Text");
        m_overlayTextDialog->setMinimumWidth(460);
        m_overlayTextDialog->resize(520, 120);

        QVBoxLayout* layout = new QVBoxLayout(m_overlayTextDialog);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);

        QLabel* hint = new QLabel("Enter text to overlay on local camera video background:", m_overlayTextDialog);
        layout->addWidget(hint);

        m_overlayTextLineEdit = new QLineEdit(m_overlayTextDialog);
        m_overlayTextLineEdit->setPlaceholderText("Type text and click Send");
        m_overlayTextLineEdit->setText(m_overlayText);
        layout->addWidget(m_overlayTextLineEdit);

        QHBoxLayout* buttonRow = new QHBoxLayout();
        QPushButton* sendButton = new QPushButton("Send", m_overlayTextDialog);
        QPushButton* clearButton = new QPushButton("Clear", m_overlayTextDialog);
        QPushButton* closeButton = new QPushButton("Close", m_overlayTextDialog);
        buttonRow->addWidget(sendButton);
        buttonRow->addWidget(clearButton);
        buttonRow->addStretch();
        buttonRow->addWidget(closeButton);
        layout->addLayout(buttonRow);

        connect(sendButton, &QPushButton::clicked, this, &QtVideoMainWindow::onOverlayTextSendClicked);
        connect(clearButton, &QPushButton::clicked, this, &QtVideoMainWindow::onOverlayTextClearClicked);
        connect(closeButton, &QPushButton::clicked, m_overlayTextDialog, &QDialog::hide);
        connect(m_overlayTextLineEdit, &QLineEdit::returnPressed, this, &QtVideoMainWindow::onOverlayTextSendClicked);
    } else if (m_overlayTextLineEdit) {
        m_overlayTextLineEdit->setText(m_overlayText);
    }

    m_overlayTextDialog->show();
    m_overlayTextDialog->raise();
    m_overlayTextDialog->activateWindow();
}

void QtVideoMainWindow::onOverlayTextSendClicked()
{
    if (!m_overlayTextLineEdit) {
        return;
    }

    m_overlayText = m_overlayTextLineEdit->text();
    m_overlayTextEnabled = !m_overlayText.trimmed().isEmpty();
    emit overlayTextChanged(m_overlayText, m_overlayTextEnabled);
    setConnectionStatus(m_overlayTextEnabled ? "Background text updated" : "Background text cleared");
}

void QtVideoMainWindow::onOverlayTextClearClicked()
{
    if (m_overlayTextLineEdit) {
        m_overlayTextLineEdit->clear();
    }

    m_overlayText.clear();
    m_overlayTextEnabled = false;
    emit overlayTextChanged(QString(), false);
    setConnectionStatus("Background text cleared");
}

void QtVideoMainWindow::onBackgroundBlurClicked()
{
    if (!m_backgroundBlurDialog) {
        m_backgroundBlurDialog = new QDialog(this);
        m_backgroundBlurDialog->setWindowFlags(Qt::Window);
        m_backgroundBlurDialog->setWindowTitle("Background Blur");
        m_backgroundBlurDialog->setMinimumWidth(360);
        m_backgroundBlurDialog->resize(420, 150);

        QVBoxLayout* layout = new QVBoxLayout(m_backgroundBlurDialog);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);

        QLabel* hint = new QLabel("Lightweight blur keeps center area sharper.", m_backgroundBlurDialog);
        layout->addWidget(hint);

        m_backgroundBlurEnableCheck = new QCheckBox("Enable blur", m_backgroundBlurDialog);
        m_backgroundBlurEnableCheck->setChecked(m_backgroundBlurEnabled);
        layout->addWidget(m_backgroundBlurEnableCheck);

        QHBoxLayout* strengthRow = new QHBoxLayout();
        strengthRow->addWidget(new QLabel("Strength:", m_backgroundBlurDialog));
        m_backgroundBlurStrengthCombo = new QComboBox(m_backgroundBlurDialog);
        m_backgroundBlurStrengthCombo->addItem("Low");
        m_backgroundBlurStrengthCombo->addItem("Medium");
        m_backgroundBlurStrengthCombo->addItem("High");
        m_backgroundBlurStrengthCombo->setCurrentIndex(m_backgroundBlurStrength);
        strengthRow->addWidget(m_backgroundBlurStrengthCombo);
        strengthRow->addStretch();
        layout->addLayout(strengthRow);

        QHBoxLayout* buttonRow = new QHBoxLayout();
        QPushButton* applyButton = new QPushButton("Apply", m_backgroundBlurDialog);
        QPushButton* clearButton = new QPushButton("Disable", m_backgroundBlurDialog);
        QPushButton* closeButton = new QPushButton("Close", m_backgroundBlurDialog);
        buttonRow->addWidget(applyButton);
        buttonRow->addWidget(clearButton);
        buttonRow->addStretch();
        buttonRow->addWidget(closeButton);
        layout->addLayout(buttonRow);

        connect(applyButton, &QPushButton::clicked, this, &QtVideoMainWindow::onBackgroundBlurApplyClicked);
        connect(clearButton, &QPushButton::clicked, this, &QtVideoMainWindow::onBackgroundBlurClearClicked);
        connect(closeButton, &QPushButton::clicked, m_backgroundBlurDialog, &QDialog::hide);
    } else {
        if (m_backgroundBlurEnableCheck) {
            m_backgroundBlurEnableCheck->setChecked(m_backgroundBlurEnabled);
        }
        if (m_backgroundBlurStrengthCombo) {
            m_backgroundBlurStrengthCombo->setCurrentIndex(m_backgroundBlurStrength);
        }
    }

    m_backgroundBlurDialog->show();
    m_backgroundBlurDialog->raise();
    m_backgroundBlurDialog->activateWindow();
}

void QtVideoMainWindow::onBackgroundBlurApplyClicked()
{
    if (m_backgroundBlurEnableCheck) {
        m_backgroundBlurEnabled = m_backgroundBlurEnableCheck->isChecked();
    }
    if (m_backgroundBlurStrengthCombo) {
        m_backgroundBlurStrength = m_backgroundBlurStrengthCombo->currentIndex();
    }

    if (m_backgroundBlurStrength < 0) {
        m_backgroundBlurStrength = 0;
    } else if (m_backgroundBlurStrength > 2) {
        m_backgroundBlurStrength = 2;
    }

    emit backgroundBlurChanged(m_backgroundBlurEnabled, m_backgroundBlurStrength);
    setConnectionStatus(m_backgroundBlurEnabled ? "Background blur updated" : "Background blur disabled");
}

void QtVideoMainWindow::onBackgroundBlurClearClicked()
{
    m_backgroundBlurEnabled = false;
    if (m_backgroundBlurEnableCheck) {
        m_backgroundBlurEnableCheck->setChecked(false);
    }
    emit backgroundBlurChanged(false, m_backgroundBlurStrength);
    setConnectionStatus("Background blur disabled");
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
    , m_localContentWindow(nullptr)
    , m_endpoint(nullptr)
    , m_initialized(false)
    , m_lastContentWidth(1280)
    , m_lastContentHeight(720)
    , m_contentAvailable(false)
    , m_contentWindowId(kCGNullWindowID)
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
    , m_applyAudioDeviceSelectionCb(nullptr)  // Phase 1
    , m_applyAudioDeviceSelectionUserData(nullptr)  // Phase 1
    , m_applyAudioProfileCb(nullptr)
    , m_applyAudioProfileUserData(nullptr)
    , m_setOverlayTextCb(nullptr)
    , m_setOverlayTextUserData(nullptr)
    , m_setBackgroundBlurCb(nullptr)
    , m_setBackgroundBlurUserData(nullptr)
{
}

QtVideoManager::~QtVideoManager()
{
    // Shutdown is performed explicitly in main() before QApplication destruction.
    // Avoid touching QWidget objects from static-destructor context.
    m_mainWindow = nullptr;
    m_contentWindow = nullptr;
    m_localContentWindow = nullptr;
    m_initialized = false;
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
        // Programmatic shutdown must not re-enter exit flow via closeEvent().
        QObject::disconnect(m_mainWindow, &QtVideoMainWindow::exitRequested, nullptr, nullptr);
        m_mainWindow->close();
        delete m_mainWindow;
        m_mainWindow = nullptr;
    }
    if (m_contentWindow) {
        m_contentWindow->close();
        delete m_contentWindow;
        m_contentWindow = nullptr;
    }
    if (m_localContentWindow) {
        m_localContentWindow->close();
        delete m_localContentWindow;
        m_localContentWindow = nullptr;
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
    PTRACE(0, "QtVideo\t🟨 createLocalWindow() ENTER - width=" << width << " height=" << height);
    
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
                PTRACE(0, "QtVideo\t🟨 Creating QtVideoMainWindow via invokeMethod...");
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
        PTRACE(0, "QtVideo\t🟨 Creating QtVideoMainWindow directly...");
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
        MyH323Connection* conn = getH323Connection();
        if (conn && conn->IsRecording()) {
            conn->RecordVideoFrame(yuvData, width, height, true);
        }
        // enqueueLocalFrameはシグナル経由でQueuedConnectionを使うのでスレッドセーフ
        m_mainWindow->enqueueLocalFrame(yuvData, width, height);
    }
}

void QtVideoManager::enqueueRemoteFrame(const unsigned char* yuvData, unsigned width, unsigned height, size_t dataSize)
{
    Q_UNUSED(dataSize);
    if (m_mainWindow && yuvData && width > 0 && height > 0) {
        MyH323Connection* conn = getH323Connection();
        if (conn && conn->IsRecording()) {
            conn->RecordVideoFrame(yuvData, width, height, false);
        }
        // enqueueRemoteFrameはシグナル経由でQueuedConnectionを使うのでスレッドセーフ
        m_mainWindow->enqueueRemoteFrame(yuvData, width, height);
    }
}

void QtVideoManager::enqueueContentFrame(const unsigned char* yuvData, unsigned width, unsigned height, size_t dataSize)
{
    if (!yuvData || width == 0 || height == 0) {
        return;
    }

    MyH323Connection* conn = getH323Connection();
    if (conn && conn->IsRecording()) {
        conn->RecordContentFrame(yuvData, width, height, false);
    }
    
    // 最新サイズを記録し、ボタンを有効化
    m_lastContentWidth = static_cast<int>(width);
    m_lastContentHeight = static_cast<int>(height);
    setContentAvailable(true);

    // フレームデータを安全にUIスレッドへ渡すためコピーを保持
    const QByteArray frameCopy(reinterpret_cast<const char*>(yuvData),
                               static_cast<int>(dataSize));
    auto showAndEnqueue = [this, frameCopy, width, height]() {
        if (!m_contentWindow) {
            createContentWindow(static_cast<int>(width), static_cast<int>(height));
        }
        if (!m_contentWindow)
            return;

        // 表示だけ行い、前面への強制はしない
        m_contentWindow->show();
        m_contentWindow->enqueueContentFrame(
            reinterpret_cast<const unsigned char*>(frameCopy.constData()),
            width,
            height,
            static_cast<size_t>(frameCopy.size()));
    };

    // GUI操作は必ずメインスレッドで
    if (QThread::currentThread() == qApp->thread()) {
        showAndEnqueue();
    } else {
        QMetaObject::invokeMethod(
            qApp,
            showAndEnqueue,
            Qt::QueuedConnection);
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

void QtVideoManager::startContentCapture(CGWindowID windowId, const QString& label)
{
    m_contentWindowId = windowId;
    m_contentSendTarget = label;
    PTRACE(2, "QtVideo\tContent capture enabled for target=" << label.toStdString()
           << " (windowId=" << windowId << ")");
    setContentAvailable(true);
    
    // Reset any previous timer connections to avoid duplicate callbacks
    m_contentCaptureTimer.stop();
    m_contentCaptureTimer.disconnect();

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
    m_contentWindowId = kCGNullWindowID;
    m_contentSendTarget.clear();
    
#ifndef H323_H239
    PTRACE(1, "QtVideo\tH.239 not compiled in this build - skipping content TX stop");
#else
    // Stop H.239 transmission so remote can take the presentation token.
    MyH323Connection* conn = getH323Connection();
    MyH323EndPoint* ep = getH323Endpoint();
    PTRACE(1, "QtVideo\tH.239 TX stop preflight - endpoint=" << (void*)ep << " conn=" << (void*)conn);
    if (conn) {
        PTRACE(1, "QtVideo\tRequesting H.239 transmission stop");
        conn->StopH239Transmission();
    } else {
        PTRACE(1, "QtVideo\tNo active connection - cannot stop H.239 transmission");
    }
    if (ep) {
        ep->SetStartH239(false);
        PTRACE(1, "QtVideo\tH.239 start flag cleared on endpoint");
    } else {
        PTRACE(1, "QtVideo\tEndpoint not set - cannot clear H.239 start flag");
    }
#endif

    // ローカルプレビューウィンドウを閉じる
    closeLocalContentPreview();
    
    // メインウィンドウのボタンテキストをリセット
    if (m_mainWindow && m_mainWindow->m_contentSendButton) {
        m_mainWindow->m_contentSendButton->setText("Send Content");
        m_mainWindow->m_contentSendButton->setToolTip("Select a window to share via H.239");
    }
}

// Timer callback - runs in main thread periodically to capture screen
void QtVideoManager::captureContentFrame()
{
    if (m_contentSendTarget.isEmpty() && m_contentWindowId == kCGNullWindowID) {
        return;
    }

    QImage img;

#ifdef Q_OS_MAC
    const CGWindowID targetId = m_contentWindowId;
    const CGWindowImageOption imgOpts = kCGWindowImageBoundsIgnoreFraming | kCGWindowImageShouldBeOpaque;
    const CGWindowListOption listOpts = (targetId == kCGNullWindowID)
        ? kCGWindowListOptionOnScreenOnly
        : kCGWindowListOptionIncludingWindow;

    if (!EnsureScreenCaptureAccess()) {
        // Permission not granted; keep sending black frames (handled by fallback)
        return;
    }

    // CGRectInfinite captures the full desktop; CGRectNull with includingWindow captures only the target window
    CGImageRef cgImg = (targetId == kCGNullWindowID)
        ? CallCGWindowListCreateImage(CGRectInfinite, listOpts, kCGNullWindowID, imgOpts)
        : CallCGWindowListCreateImage(CGRectNull, listOpts, targetId, imgOpts);

    if (cgImg) {
        img = QImageFromCGImageRef(cgImg).convertToFormat(QImage::Format_RGB32);
        CGImageRelease(cgImg);
    } else {
        PTRACE(1, "QtVideo\tFailed to capture CGWindow image for windowId=" << targetId
                  << " (no screen recording permission or invalid window)");
        return;
    }
#else
    QScreen* screen = QGuiApplication::primaryScreen();
    if (!screen) return;
    QPixmap grab = screen->grabWindow(0);
    img = grab.toImage().convertToFormat(QImage::Format_RGB32);
    if (img.isNull()) return;
#endif

    if (img.isNull()) {
        return;
    }

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
    
    // ローカルプレビューウィンドウを更新
    QByteArray frameCopy = m_lastCapturedFrame;  // mutexの外でコピー
    locker.unlock();
    MyH323Connection* conn = getH323Connection();
    if (conn && conn->IsRecording()) {
        conn->RecordContentFrame(
            reinterpret_cast<const unsigned char*>(frameCopy.constData()),
            static_cast<unsigned>(w),
            static_cast<unsigned>(h),
            true);
    }
    updateLocalContentPreview(frameCopy, w, h);
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

void QtVideoManager::requestContentSend(const QString& label, CGWindowID windowId)
{
    m_contentWindowId = windowId;
    m_contentSendTarget = label;
    PTRACE(1, "QtVideo\tContent send target set to: " << label.toStdString()
           << " (windowId=" << windowId << ")");
    
    if (m_mainWindow) {
        m_mainWindow->setConnectionStatus("Content source selected: " + label);
    }
    
    // コンテンツキャプチャ開始
    startContentCapture(windowId, label);
    
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

    QObject::connect(m_mainWindow, &QtVideoMainWindow::exitRequested,
        [this]() {
            requestProgramExit();
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

    QObject::connect(m_mainWindow, &QtVideoMainWindow::audioProfileChanged,
        [this](int index) {
            applyAudioProfile(index);
        });

    QObject::connect(m_mainWindow, &QtVideoMainWindow::overlayTextChanged,
        [this](const QString& text, bool enabled) {
            applyOverlayText(text, enabled);
        });

    QObject::connect(m_mainWindow, &QtVideoMainWindow::backgroundBlurChanged,
        [this](bool enabled, int strength) {
            applyBackgroundBlur(enabled, strength);
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

// ==================== Phase 1: Multi-Device Audio Implementation ====================

void QtVideoManager::setApplyAudioDeviceSelectionCallback(ApplyAudioDeviceSelectionCallback cb, void* userData)
{
    m_applyAudioDeviceSelectionCb = cb;
    m_applyAudioDeviceSelectionUserData = userData;
    QT_TRACE(2, "ApplyAudioDeviceSelectionCallback set: cb=" << (void*)cb << " userData=" << userData);
}

void QtVideoManager::applyAudioDeviceSelection(const AudioDeviceSelection& selection)
{
    PTRACE(0, "QtVideo\t🟣 QtVideoManager::applyAudioDeviceSelection() ENTER");
    PTRACE(0, "QtVideo\t🟣   inputDevices: " << selection.inputDevices.size());
    PTRACE(0, "QtVideo\t🟣   outputDevices: " << selection.outputDevices.size());
    PTRACE(0, "QtVideo\t🟣   cameraDevices: " << selection.cameraDevices.size());
    
    if (m_applyAudioDeviceSelectionCb) {
        PTRACE(0, "QtVideo\t🟣 Callback exists - calling it...");
        QT_TRACE(1, "Calling ApplyAudioDeviceSelectionCallback: "
                 << selection.inputDevices.size() << " mics, "
                 << selection.outputDevices.size() << " speakers, "
                 << selection.cameraDevices.size() << " cameras");
        m_applyAudioDeviceSelectionCb(selection, m_applyAudioDeviceSelectionUserData);
        PTRACE(0, "QtVideo\t🟣 Callback returned");
    } else {
        PTRACE(0, "QtVideo\t🟣 WARNING: Callback is NULL!");
        QT_TRACE(1, "WARNING: ApplyAudioDeviceSelectionCallback not set");
    }
}

void QtVideoManager::applyAudioGainSettings(const AudioDeviceSelection& selection)
{
    QT_TRACE(2, "Applying gain settings: "
             << selection.inputDevices.size() << " mics, "
             << selection.outputDevices.size() << " speakers");
    
    // 接続中のゲイン更新
    MyH323Connection* conn = getH323Connection();
    if (conn) {
        // CoreAudioDeviceConfigに変換
        CoreAudioDeviceConfig config;
        
        for (const AudioDeviceEntry& entry : selection.inputDevices) {
            CoreAudioDeviceEntry coreEntry;
            coreEntry.name = PString((const char*)entry.name.toUtf8());
            coreEntry.gain = entry.gain;
            coreEntry.muted = entry.muted;
            config.inputs.push_back(coreEntry);
        }
        
        for (const AudioDeviceEntry& entry : selection.outputDevices) {
            CoreAudioDeviceEntry coreEntry;
            coreEntry.name = PString((const char*)entry.name.toUtf8());
            coreEntry.gain = entry.gain;
            coreEntry.muted = entry.muted;
            config.outputs.push_back(coreEntry);
        }
        
        conn->UpdateAudioGainSettings(config);
        QT_TRACE(2, "Gain settings applied to active connection");
    } else {
        QT_TRACE(3, "No active connection - gain will be applied on next call");
    }
}

void QtVideoManager::setApplyAudioProfileCallback(ApplyAudioProfileCallback cb, void* userData)
{
    m_applyAudioProfileCb = cb;
    m_applyAudioProfileUserData = userData;
    QT_TRACE(2, "ApplyAudioProfileCallback set: cb=" << (void*)cb << " userData=" << userData);
}

void QtVideoManager::applyAudioProfile(int index)
{
    if (!m_applyAudioProfileCb) {
        QT_TRACE(1, "WARNING: ApplyAudioProfileCallback not set");
        return;
    }

    const int normalized = NormalizeAudioProfileIndex(index);
    m_applyAudioProfileCb(normalized, m_applyAudioProfileUserData);
}

void QtVideoManager::setOverlayTextCallback(SetOverlayTextCallback cb, void* userData)
{
    m_setOverlayTextCb = cb;
    m_setOverlayTextUserData = userData;
}

void QtVideoManager::applyOverlayText(const QString& text, bool enabled)
{
    if (!m_setOverlayTextCb) {
        return;
    }
    m_setOverlayTextCb(text, enabled, m_setOverlayTextUserData);
}

void QtVideoManager::setBackgroundBlurCallback(SetBackgroundBlurCallback cb, void* userData)
{
    m_setBackgroundBlurCb = cb;
    m_setBackgroundBlurUserData = userData;
}

void QtVideoManager::applyBackgroundBlur(bool enabled, int strength)
{
    if (!m_setBackgroundBlurCb) {
        return;
    }
    m_setBackgroundBlurCb(enabled, strength, m_setBackgroundBlurUserData);
}

// ==================== End of Phase 1 Audio Implementation ====================

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
    
    setConnectionStatus("Disconnected - Listening for incoming calls");
    
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
}

void QtVideoManager::requestProgramExit()
{
    QT_TRACE(1, "Program exit requested from UI");

    if (m_endpoint) {
        // Endpoint側で終了フラグを立てた上で全通話を切断する
        m_endpoint->RequestProgramExit();
    } else {
        // Endpointが無い場合のみ、UI側で切断処理して終了を試みる
        hangupCall();
        auto app = QCoreApplication::instance();
        if (app) {
            app->quit();
        }
    }

    setConnectionStatus("Exiting...");
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

void QtVideoMainWindow::onMultiDeviceWindowClicked()
{
    if (!m_multiDeviceWindow) {
        return;
    }

    updateMultiDevicePanelHeight();
    if (m_multiDeviceWindow->width() < m_multiDeviceWindow->minimumWidth()) {
        m_multiDeviceWindow->resize(m_multiDeviceWindow->minimumWidth(),
                                    m_multiDeviceWindow->height());
    }

    if (!m_multiDeviceWindow->isVisible()) {
        QPoint mainPos = this->pos();
        m_multiDeviceWindow->move(mainPos.x() + 40, mainPos.y() + 40);
    }

    m_multiDeviceWindow->show();
    m_multiDeviceWindow->raise();
    m_multiDeviceWindow->activateWindow();
}

void QtVideoMainWindow::onContentWindowClicked()
{
    QT_TRACE(1, "Content button clicked - requesting content window");
    QtVideoManager::instance().openContentWindow();
}

void QtVideoMainWindow::onContentSendClicked()
{
    QT_TRACE(1, "Content send button clicked");

    if (!m_h323Connection) {
        setConnectionStatus("Connect first to send content");
        return;
    }

    // コンテンツ送信中の場合は停止
    if (QtVideoManager::instance().isContentSending()) {
        QT_TRACE(1, "Stopping content send");
        QtVideoManager::instance().stopContentCapture();
        m_contentSendButton->setText("Send Content");
        m_contentSendButton->setToolTip("Select a window to share via H.239");
        setConnectionStatus("Content sharing stopped");
        return;
    }

    // コンテンツ送信開始
    QT_TRACE(1, "Selecting window to share");

#ifdef Q_OS_MAC
    QStringList windowLabels;
    QList<CGWindowID> windowIds;

    // Always offer full desktop capture
    windowLabels << tr("Entire Screen");
    windowIds << kCGNullWindowID;

    CFArrayRef windowList = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID);

    if (windowList) {
        const CFIndex count = CFArrayGetCount(windowList);
        for (CFIndex i = 0; i < count; ++i) {
            CFDictionaryRef info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(windowList, i));
            if (!info) continue;

            CFNumberRef windowIdRef = static_cast<CFNumberRef>(CFDictionaryGetValue(info, kCGWindowNumber));
            CFStringRef ownerRef = static_cast<CFStringRef>(CFDictionaryGetValue(info, kCGWindowOwnerName));
            CFStringRef nameRef = static_cast<CFStringRef>(CFDictionaryGetValue(info, kCGWindowName));
            CFNumberRef layerRef = static_cast<CFNumberRef>(CFDictionaryGetValue(info, kCGWindowLayer));

            if (!windowIdRef || !ownerRef) {
                continue;
            }

            // Skip non-app layers (menubar, dock, etc.)
            int layer = 0;
            if (layerRef) {
                CFNumberGetValue(layerRef, kCFNumberIntType, &layer);
                if (layer != 0)
                    continue;
            }

            CGWindowID winId = kCGNullWindowID;
            if (!CFNumberGetValue(windowIdRef, kCGWindowIDCFNumberType, &winId)) {
                continue;
            }

            QString owner = QString::fromCFString(ownerRef).trimmed();
            QString title = nameRef ? QString::fromCFString(nameRef).trimmed() : QString();
            if (title.isEmpty())
                title = tr("(Untitled)");

            QString label = owner;
            if (!title.isEmpty())
                label += " — " + title;

            windowLabels << label;
            windowIds << winId;
        }
        CFRelease(windowList);
    }

    // フォールバック
    if (windowLabels.isEmpty()) {
        windowLabels << tr("Entire Screen");
        windowIds << kCGNullWindowID;
    }
#else
    QStringList windowLabels;
    QList<CGWindowID> windowIds;
    windowLabels << tr("Entire Screen");
    windowIds << 0;
#endif

    bool ok = false;
    const QString selected = QInputDialog::getItem(
        this,
        tr("Select content window"),
        tr("Window to share:"),
        windowLabels,
        0,
        false,
        &ok
    );

    if (!ok || selected.isEmpty()) {
        QT_TRACE(1, "Content send cancelled or no selection");
        return;
    }

#ifdef Q_OS_MAC
    CGWindowID selectedId = kCGNullWindowID;
    int idx = windowLabels.indexOf(selected);
    if (idx >= 0 && idx < windowIds.size()) {
        selectedId = windowIds[idx];
    }
#else
    CGWindowID selectedId = 0;
#endif

    QtVideoManager::instance().requestContentSend(selected, selectedId);
    
    // ボタンテキストを変更
    m_contentSendButton->setText("Stop Content");
    m_contentSendButton->setToolTip("Stop sharing content");
    setConnectionStatus("Content source selected: " + selected);
    QMessageBox::information(this,
                             tr("Content source set"),
                             tr("Sharing source: %1").arg(selected));
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

void QtVideoManager::updateLocalContentPreview(const QByteArray& frame, unsigned width, unsigned height)
{
    if (!m_localContentWindow) {
        m_localContentWindow = new QtContentWindow();
        m_localContentWindow->setWindowTitle("Local Content (Sending)");
        m_localContentWindow->show();
    }
    m_localContentWindow->enqueueContentFrame(
        reinterpret_cast<const unsigned char*>(frame.constData()),
        width, 
        height, 
        static_cast<size_t>(frame.size())
    );
}

void QtVideoManager::closeLocalContentPreview()
{
    if (m_localContentWindow) {
        m_localContentWindow->close();
        delete m_localContentWindow;
        m_localContentWindow = nullptr;
    }
}

///////////////////////////////////////////////////////////////////////////////
// Phase 1: Multi-Device Audio UI Implementation
///////////////////////////////////////////////////////////////////////////////

void QtVideoMainWindow::setupMultiDeviceAudioUI(QVBoxLayout* mainLayout)
{
    PTRACE(0, "QtVideo\t🔵 setupMultiDeviceAudioUI() ENTER");
    
    // マルチデバイスオーディオ設定パネル
    QGroupBox* multiDeviceBox = new QGroupBox("Multi-Audio Configuration", this);
    multiDeviceBox->setMinimumWidth(1120);
    QVBoxLayout* multiDeviceLayout = new QVBoxLayout(multiDeviceBox);

    // 横並びレイアウト: 左=マイク、右=スピーカー
    QHBoxLayout* deviceColumnsLayout = new QHBoxLayout();
    deviceColumnsLayout->setSpacing(12);
    
    // ========== 左列: マイク設定 ==========
    QVBoxLayout* micColumn = new QVBoxLayout();
    
    QLabel* micTitle = new QLabel("<b>Microphones (Up to 4)</b>", this);
    micColumn->addWidget(micTitle);

    m_micRowsLayout = new QVBoxLayout();
    m_micRowsLayout->setSpacing(4);
    micColumn->addLayout(m_micRowsLayout);

    m_addMicButton = new QPushButton("+ Add Microphone", this);
    m_addMicButton->setMaximumWidth(150);
    micColumn->addWidget(m_addMicButton, 0, Qt::AlignLeft);
    PTRACE(0, "QtVideo\t🟦 m_addMicButton created and added to layout");
    
    micColumn->addStretch();  // 下に余白
    
    deviceColumnsLayout->addLayout(micColumn);
    
    // ========== 縦の区切り線 ==========
    QFrame* verticalLine = new QFrame(this);
    verticalLine->setFrameShape(QFrame::VLine);
    verticalLine->setFrameShadow(QFrame::Sunken);
    verticalLine->setLineWidth(2);
    deviceColumnsLayout->addWidget(verticalLine);
    
    // ========== 右列: スピーカー設定 ==========
    QVBoxLayout* speakerColumn = new QVBoxLayout();
    
    QLabel* speakerTitle = new QLabel("<b>Speakers (Up to 4)</b>", this);
    speakerColumn->addWidget(speakerTitle);

    m_speakerRowsLayout = new QVBoxLayout();
    m_speakerRowsLayout->setSpacing(4);
    speakerColumn->addLayout(m_speakerRowsLayout);

    m_addSpeakerButton = new QPushButton("+ Add Speaker", this);
    m_addSpeakerButton->setMaximumWidth(150);
    speakerColumn->addWidget(m_addSpeakerButton, 0, Qt::AlignLeft);
    
    speakerColumn->addStretch();  // 下に余白
    
    deviceColumnsLayout->addLayout(speakerColumn);
    deviceColumnsLayout->setStretch(0, 1);
    deviceColumnsLayout->setStretch(2, 1);
    
    multiDeviceLayout->addLayout(deviceColumnsLayout);

    // スクロールエリアでラップ（デバイス追加時に画面が広がっても対応）
    QScrollArea* scrollArea = new QScrollArea(this);
    scrollArea->setWidget(multiDeviceBox);
    scrollArea->setWidgetResizable(true);  // コンテンツに合わせてリサイズ
    scrollArea->setMinimumHeight(70);      // 初期はコンパクト
    scrollArea->setMaximumHeight(500);     // 最大高さ（これ以上は自動スクロール）
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    scrollArea->setMinimumWidth(1120);
    m_multiDeviceScrollArea = scrollArea;
    
    mainLayout->addWidget(scrollArea);
    
    PTRACE(0, "QtVideo\t🔵 setupMultiDeviceAudioUI() EXIT - UI created successfully");
}

void QtVideoMainWindow::setupMultiDeviceCameraUI(QVBoxLayout* mainLayout)
{
    QGroupBox* cameraBox = new QGroupBox("Multi-Camera Configuration", this);
    QVBoxLayout* cameraLayout = new QVBoxLayout(cameraBox);

    QLabel* cameraTitle = new QLabel("<b>Cameras (Up to 4)</b>", this);
    cameraLayout->addWidget(cameraTitle);

    m_cameraRowsLayout = new QVBoxLayout();
    m_cameraRowsLayout->setSpacing(4);
    cameraLayout->addLayout(m_cameraRowsLayout);

    m_addCameraButton = new QPushButton("+ Add Camera", this);
    m_addCameraButton->setMaximumWidth(150);
    cameraLayout->addWidget(m_addCameraButton, 0, Qt::AlignLeft);

    cameraLayout->addStretch();
    mainLayout->addWidget(cameraBox);
}

void QtVideoMainWindow::onAddMicClicked()
{
    PTRACE(0, "QtVideo\t🔴🔴🔴 onAddMicClicked() ENTER - m_micRows.size()=" << m_micRows.size());
    
    // 最大4デバイスまで
    if (m_micRows.size() >= 4) {
        PTRACE(0, "QtVideo\t⚠️ Maximum 4 microphones reached");
        QMessageBox::warning(this, "Limit Reached", "Maximum 4 microphones allowed.");
        return;
    }

    QStringList devices = getAvailableDevices(QT_DEVICE_TYPE_MIC);
    PTRACE(0, "QtVideo\tAvailable mic devices: " << devices.size());
    
    AudioDeviceRowWidget* row = new AudioDeviceRowWidget(QT_DEVICE_TYPE_MIC, devices, this);
    PTRACE(0, "QtVideo\tCreated AudioDeviceRowWidget: " << (void*)row);

    // シグナル接続
    connect(row, &AudioDeviceRowWidget::removeRequested,
            this, &QtVideoMainWindow::onDeviceRowRemoveRequested);
    connect(row, &AudioDeviceRowWidget::deviceChanged,
            this, &QtVideoMainWindow::onDeviceRowChanged);
    connect(row, &AudioDeviceRowWidget::gainChanged,
            this, &QtVideoMainWindow::onGainChanged);
    PTRACE(0, "QtVideo\tSignals connected");

    // 削除ボタンの表示（1行目は削除不可、2行目以降は削除可能）
    row->setRemoveButtonVisible(m_micRows.size() > 0);

    m_micRowsLayout->addWidget(row);
    m_micRows.append(row);

    PTRACE(0, "QtVideo\t✅✅✅ Mic row added - total now: " << m_micRows.size());
    // 追加行の初期ゲインは旧式の現在値に合わせる
    AudioDeviceEntry entry = row->getDeviceEntry();
    entry.gain = kLinTable[m_baseMicGainIndex];
    row->setDeviceEntry(entry);
    updateDeviceRowControls(m_micRows);
    updateMultiDevicePanelHeight();
    onDeviceRowChanged();  // 追加直後に設定を反映
}

void QtVideoMainWindow::onAddSpeakerClicked()
{
    // 最大4デバイスまで
    if (m_speakerRows.size() >= 4) {
        QMessageBox::warning(this, "Limit Reached", "Maximum 4 speakers allowed.");
        return;
    }

    QStringList devices = getAvailableDevices(QT_DEVICE_TYPE_SPEAKER);
    AudioDeviceRowWidget* row = new AudioDeviceRowWidget(QT_DEVICE_TYPE_SPEAKER, devices, this);

    // シグナル接続
    connect(row, &AudioDeviceRowWidget::removeRequested,
            this, &QtVideoMainWindow::onDeviceRowRemoveRequested);
    connect(row, &AudioDeviceRowWidget::deviceChanged,
            this, &QtVideoMainWindow::onDeviceRowChanged);
    connect(row, &AudioDeviceRowWidget::gainChanged,
            this, &QtVideoMainWindow::onGainChanged);

    // 削除ボタンの表示（1行目は削除不可、2行目以降は削除可能）
    row->setRemoveButtonVisible(m_speakerRows.size() > 0);

    m_speakerRowsLayout->addWidget(row);
    m_speakerRows.append(row);

    QT_TRACE(2, "Speaker row added (total=" << m_speakerRows.size() << ")");
    // 追加行の初期ゲインは旧式の現在値に合わせる
    AudioDeviceEntry entry = row->getDeviceEntry();
    entry.gain = kLinTable[m_baseSpeakerGainIndex];
    row->setDeviceEntry(entry);
    updateDeviceRowControls(m_speakerRows);
    updateMultiDevicePanelHeight();
    onDeviceRowChanged();  // 追加直後に設定を反映
}

void QtVideoMainWindow::onAddCameraClicked()
{
    if (m_cameraRows.size() >= 4) {
        QMessageBox::warning(this, "Limit Reached", "Maximum 4 cameras allowed.");
        return;
    }

    QStringList devices = getAvailableDevices(QT_DEVICE_TYPE_CAMERA);
    VideoDeviceRowWidget* row = new VideoDeviceRowWidget(devices, this);

    connect(row, &VideoDeviceRowWidget::removeRequested,
            this, &QtVideoMainWindow::onCameraRowRemoveRequested);
    connect(row, &VideoDeviceRowWidget::deviceChanged,
            this, &QtVideoMainWindow::onDeviceRowChanged);

    row->setRemoveButtonVisible(m_cameraRows.size() > 0);
    m_cameraRowsLayout->addWidget(row);
    m_cameraRows.append(row);

    updateCameraRowControls();
    onDeviceRowChanged();
}

void QtVideoMainWindow::onCameraRowRemoveRequested(VideoDeviceRowWidget* widget)
{
    removeCameraRow(widget);
    updateCameraRowControls();
    onDeviceRowChanged();
}
void QtVideoMainWindow::onDeviceRowRemoveRequested(AudioDeviceRowWidget* widget)
{
    // マイクかスピーカーか判定
    int micIndex = m_micRows.indexOf(widget);
    int speakerIndex = m_speakerRows.indexOf(widget);

    if (micIndex >= 0) {
        removeDeviceRow(widget, m_micRows);
        updateDeviceRowControls(m_micRows);
        updateMultiDevicePanelHeight();
        QT_TRACE(2, "Mic row removed (remaining=" << m_micRows.size() << ")");
    } else if (speakerIndex >= 0) {
        removeDeviceRow(widget, m_speakerRows);
        updateDeviceRowControls(m_speakerRows);
        updateMultiDevicePanelHeight();
        QT_TRACE(2, "Speaker row removed (remaining=" << m_speakerRows.size() << ")");
    }
    
    // デバイス削除後も自動適用
    onDeviceRowChanged();
}

void QtVideoMainWindow::onDeviceRowChanged()
{
    // デバイス設定変更時に自動適用（接続前のみ）
    PTRACE(0, "QtVideo\t🟢🟢🟢 onDeviceRowChanged() ENTER");
    
    AudioDeviceSelection selection = getAudioDeviceSelection();
    PTRACE(0, "QtVideo\tDevice change: " << selection.inputDevices.size() << " mics, "
             << selection.outputDevices.size() << " speakers, "
             << selection.cameraDevices.size() << " cameras");
    
    // デバイス内容をログ出力
    for (size_t i = 0; i < selection.inputDevices.size(); i++) {
        PTRACE(0, "QtVideo\t  Input[" << i << "]: " << selection.inputDevices[i].name.toStdString());
    }
    for (size_t i = 0; i < selection.cameraDevices.size(); i++) {
        PTRACE(0, "QtVideo\t  Camera[" << i << "]: " << selection.cameraDevices[i].name.toStdString()
               << (selection.cameraDevices[i].muted ? " (muted)" : ""));
    }
    
    // QtVideoManager経由でバックエンドに適用
    PTRACE(0, "QtVideo\tCalling applyAudioDeviceSelection...");
    QtVideoManager::instance().applyAudioDeviceSelection(selection);
    PTRACE(0, "QtVideo\t🟢🟢🟢 onDeviceRowChanged() EXIT");
}

void QtVideoMainWindow::onGainChanged()
{
    // ゲイン設定変更時に自動適用（接続中も可能）
    QT_TRACE(2, "Audio gain changed - applying gain settings");
    
    AudioDeviceSelection selection = getAudioDeviceSelection();
    QT_TRACE(3, "Gain update: " << selection.inputDevices.size() << " mics, "
             << selection.outputDevices.size() << " speakers");
    
    // QtVideoManager経由でゲイン設定のみを適用
    QtVideoManager::instance().applyAudioGainSettings(selection);
}

/**
 * @brief オーディオビジュアライザーを更新（タイマーから呼ばれる）
 */
void QtVideoMainWindow::updateAudioVisualizers()
{
    // 通話がアクティブでない場合は即座に終了（dangling pointer 防止）
    if (!m_isCallActive.load(std::memory_order_acquire)) {
        return;
    }

    // グローバルH323EndPointへアクセス
    MyH323EndPoint* endpoint = GetGlobalH323Endpoint();
    if (!endpoint) {
        return;
    }

    MyH323Connection* conn = endpoint->GetCurrentConnection();
    
    // 接続がない、または確立されていない場合はビジュアライザーをリセット
    if (!conn || !conn->IsEstablished()) {
        // 接続なし or 未確立 → ビジュアライザーをリセット
        for (int i = 0; i < m_micRows.size(); i++) {
            if (m_micRows[i]) {
                m_micRows[i]->resetSpectrum(160, 16000);
            }
        }
        for (int i = 0; i < m_speakerRows.size(); i++) {
            if (m_speakerRows[i]) {
                m_speakerRows[i]->resetSpectrum(160, 16000);
            }
        }
        return;
    }

    // マイクのレベルを更新（安全性チェック強化）
    MicMixerChannel* micMixer = conn->GetMicMixer();
    QT_TRACE(4, "updateAudioVisualizers: micMixer=" << (void*)micMixer 
        << " m_micRows.size()=" << m_micRows.size());
    
    if (micMixer) {
        size_t micCount = micMixer->GetDeviceCount();
        QT_TRACE(4, "updateAudioVisualizers: micCount=" << micCount);
        const unsigned micRate = micMixer->GetSampleRate();
        const size_t fallbackSamples = micMixer->GetFrameSamples();
        const bool muted = conn->IsLocalMicMuted();
        const size_t micOffset = (m_micCombo && !m_micCombo->currentText().isEmpty()) ? 1 : 0;
        
        // UI行数を実際のデバイス数でクリップ
        size_t available = (micCount > micOffset) ? (micCount - micOffset) : 0;
        size_t maxMicIndex = (available < (size_t)m_micRows.size()) ? available : (size_t)m_micRows.size();
        QT_TRACE(4, "updateAudioVisualizers: maxMicIndex=" << maxMicIndex);
        
        for (size_t i = 0; i < maxMicIndex; i++) {
            QT_TRACE(4, "updateAudioVisualizers: Processing mic " << i << " m_micRows[i]=" << (void*)m_micRows[i]);
            size_t devIndex = i + micOffset;
            if (m_micRows[i] && devIndex < micCount) {  // 二重チェック
                if (muted) {
                    m_micRows[i]->resetSpectrum(fallbackSamples, micRate);
                    continue;
                }
                std::vector<int16_t> samples;
                if (micMixer->GetDeviceSamples(devIndex, samples) && !samples.empty()) {
                    m_micRows[i]->updateSpectrum(samples.data(), samples.size(), 1, micRate);
                } else {
                    m_micRows[i]->resetSpectrum(fallbackSamples, micRate);
                }
            }
        }
        QT_TRACE(4, "updateAudioVisualizers: Resetting excess mic rows from " << maxMicIndex);
        // 実際のデバイス数より多い行はリセット
        for (size_t i = maxMicIndex; i < (size_t)m_micRows.size(); i++) {
            QT_TRACE(4, "updateAudioVisualizers: Resetting mic row " << i << " m_micRows[i]=" << (void*)m_micRows[i]);
            if (m_micRows[i]) {
                m_micRows[i]->resetSpectrum(fallbackSamples, micRate);
                QT_TRACE(4, "updateAudioVisualizers: Reset mic row " << i);
            }
        }
        QT_TRACE(4, "updateAudioVisualizers: Mic processing completed");
    } else {
        // MicMixerがない場合は全てリセット
        QT_TRACE(4, "updateAudioVisualizers: No micMixer, resetting all mic rows");
        for (int i = 0; i < m_micRows.size(); i++) {
            if (m_micRows[i]) {
                m_micRows[i]->resetSpectrum(160, 16000);
            }
        }
    }

    // スピーカーのレベルを更新（安全性チェック強化）
    SpeakerFanoutChannel* speakerFanout = conn->GetSpeakerFanout();
    QT_TRACE(4, "updateAudioVisualizers: speakerFanout=" << (void*)speakerFanout 
        << " m_speakerRows.size()=" << m_speakerRows.size());
    
    if (speakerFanout) {
        size_t speakerCount = speakerFanout->GetDeviceCount();
        QT_TRACE(4, "updateAudioVisualizers: speakerCount=" << speakerCount);
        const unsigned spkRate = speakerFanout->GetSampleRate();
        const size_t spkOffset = (m_speakerCombo && !m_speakerCombo->currentText().isEmpty()) ? 1 : 0;
        
        // UI行数を実際のデバイス数でクリップ
        size_t available = (speakerCount > spkOffset) ? (speakerCount - spkOffset) : 0;
        size_t maxSpeakerIndex = (available < (size_t)m_speakerRows.size()) ? available : (size_t)m_speakerRows.size();
        for (size_t i = 0; i < maxSpeakerIndex; i++) {
            size_t devIndex = i + spkOffset;
            if (m_speakerRows[i] && devIndex < speakerCount) {  // 二重チェック
                std::vector<int16_t> samples;
                if (speakerFanout->GetDeviceSamples(devIndex, samples) && !samples.empty()) {
                    m_speakerRows[i]->updateSpectrum(samples.data(), samples.size(), 1, spkRate);
                } else {
                    m_speakerRows[i]->resetSpectrum(spkRate / 100, spkRate);
                }
            }
        }
        // 実際のデバイス数より多い行はリセット
        for (size_t i = maxSpeakerIndex; i < (size_t)m_speakerRows.size(); i++) {
            if (m_speakerRows[i]) {
                m_speakerRows[i]->resetSpectrum(spkRate / 100, spkRate);
            }
        }
    } else {
        // SpeakerFanoutがない場合は全てリセット
        for (int i = 0; i < m_speakerRows.size(); i++) {
            if (m_speakerRows[i]) {
                m_speakerRows[i]->resetSpectrum(160, 16000);
            }
        }
    }
    
    QT_TRACE(4, "updateAudioVisualizers: completed successfully");
}

/**
 * @brief オーディオビジュアライザータイマーを停止（接続クローズ時に呼ばれる）
 */
void QtVideoMainWindow::stopAudioVisualizerTimer()
{
    QT_TRACE(2, "stopAudioVisualizerTimer called - clearing call flag and stopping timer");
    
    // 通話アクティブフラグを最初にクリア（dangling pointer 防止）
    m_isCallActive.store(false, std::memory_order_release);
    m_connectEstablished = false;
    stopConnectBlink();
    
    // タイマーを停止
    if (m_audioVisualizerTimer && m_audioVisualizerTimer->isActive()) {
        m_audioVisualizerTimer->stop();
        QT_TRACE(2, "Audio visualizer timer stopped");
    }
    
    // ビジュアライザーをリセット
    for (auto* row : m_micRows) {
        if (row) {
            row->resetSpectrum(160, 16000);
        }
    }
    for (auto* row : m_speakerRows) {
        if (row) {
            row->resetSpectrum(160, 16000);
        }
    }
}

AudioDeviceSelection QtVideoMainWindow::getAudioDeviceSelection() const
{
    AudioDeviceSelection selection;

    QSet<QString> seenInputs;
    QSet<QString> seenOutputs;
    QSet<QString> seenCameras;

    if (m_micCombo) {
        const QString name = m_micCombo->currentText();
        if (!name.isEmpty()) {
            selection.inputDevices.append(AudioDeviceEntry(name, kLinTable[m_baseMicGainIndex], false));
            seenInputs.insert(name);
        }
    }

    if (m_speakerCombo) {
        const QString name = m_speakerCombo->currentText();
        if (!name.isEmpty()) {
            selection.outputDevices.append(AudioDeviceEntry(name, kLinTable[m_baseSpeakerGainIndex], false));
            seenOutputs.insert(name);
        }
    }

    for (const AudioDeviceRowWidget* row : m_micRows) {
        AudioDeviceEntry entry = row->getDeviceEntry();
        if (!entry.name.isEmpty() && !seenInputs.contains(entry.name)) {
            selection.inputDevices.append(entry);
            seenInputs.insert(entry.name);
        }
    }

    for (const AudioDeviceRowWidget* row : m_speakerRows) {
        AudioDeviceEntry entry = row->getDeviceEntry();
        if (!entry.name.isEmpty() && !seenOutputs.contains(entry.name)) {
            selection.outputDevices.append(entry);
            seenOutputs.insert(entry.name);
        }
    }

    if (m_cameraRows.isEmpty()) {
        if (m_cameraCombo) {
            const QString name = m_cameraCombo->currentText();
            if (!name.isEmpty()) {
                selection.cameraDevices.append(VideoDeviceEntry(name, false));
                seenCameras.insert(name);
            }
        }
    } else {
        for (const VideoDeviceRowWidget* row : m_cameraRows) {
            VideoDeviceEntry entry = row->getDeviceEntry();
            if (!entry.name.isEmpty() && !seenCameras.contains(entry.name)) {
                selection.cameraDevices.append(entry);
                seenCameras.insert(entry.name);
            }
        }
    }

    return selection;
}

void QtVideoMainWindow::setAudioDeviceSelection(const AudioDeviceSelection& selection)
{
    // 既存の行をクリア
    while (m_micRows.size() > 0) {
        AudioDeviceRowWidget* row = m_micRows.takeLast();
        m_micRowsLayout->removeWidget(row);
        row->deleteLater();
    }

    while (m_speakerRows.size() > 0) {
        AudioDeviceRowWidget* row = m_speakerRows.takeLast();
        m_speakerRowsLayout->removeWidget(row);
        row->deleteLater();
    }

    while (m_cameraRows.size() > 0) {
        VideoDeviceRowWidget* row = m_cameraRows.takeLast();
        m_cameraRowsLayout->removeWidget(row);
        row->deleteLater();
    }

    // 新しい設定で行を追加
    for (const AudioDeviceEntry& entry : selection.inputDevices) {
        if (m_micCombo && entry.name == m_micCombo->currentText()) {
            continue;  // 旧式の基本デバイスは行にしない
        }
        onAddMicClicked();
        if (!m_micRows.isEmpty()) {
            m_micRows.last()->setDeviceEntry(entry);
        }
    }

    for (const AudioDeviceEntry& entry : selection.outputDevices) {
        if (m_speakerCombo && entry.name == m_speakerCombo->currentText()) {
            continue;  // 旧式の基本デバイスは行にしない
        }
        onAddSpeakerClicked();
        if (!m_speakerRows.isEmpty()) {
            m_speakerRows.last()->setDeviceEntry(entry);
        }
    }

    for (const VideoDeviceEntry& entry : selection.cameraDevices) {
        if (m_cameraCombo && entry.name == m_cameraCombo->currentText()) {
            continue;
        }
        onAddCameraClicked();
        if (!m_cameraRows.isEmpty()) {
            m_cameraRows.last()->setDeviceEntry(entry);
        }
    }

    updateDeviceRowControls(m_micRows);
    updateDeviceRowControls(m_speakerRows);
    updateCameraRowControls();
    updateMultiDevicePanelHeight();

    QT_TRACE(2, "Audio device selection applied: "
             << selection.inputDevices.size() << " mics, "
             << selection.outputDevices.size() << " speakers, "
             << selection.cameraDevices.size() << " cameras");
}

QStringList QtVideoMainWindow::getAvailableDevices(int deviceType)
{
    QStringList devices;
    QtVideoManager::instance().getDeviceList(deviceType, devices);
    return devices;
}

void QtVideoMainWindow::removeDeviceRow(AudioDeviceRowWidget* widget,
                                        QVector<AudioDeviceRowWidget*>& rows)
{
    int index = rows.indexOf(widget);
    if (index < 0) return;

    rows.removeAt(index);
    widget->deleteLater();

    // 残りが1行だけなら削除ボタンを非表示
    if (rows.size() == 1) {
        rows[0]->setRemoveButtonVisible(false);
    }
}

void QtVideoMainWindow::updateDeviceRowControls(QVector<AudioDeviceRowWidget*>& rows)
{
    const bool selectionEnabled = (m_h323Connection == nullptr);
    for (int i = 0; i < rows.size(); i++) {
        AudioDeviceRowWidget* row = rows[i];
        if (!row) {
            continue;
        }
        // ミュートは全体のみ（行ミュートは常に非表示）
        row->setMuteButtonVisible(false);
        // 先頭行は削除不可、追加行のみ削除可
        row->setRemoveButtonVisible(i > 0);
        row->setDeviceSelectionEnabled(selectionEnabled);
    }
}

void QtVideoMainWindow::updateMultiDevicePanelHeight()
{
    if (!m_multiDeviceScrollArea) {
        return;
    }
    const int rows = std::max(m_micRows.size(), m_speakerRows.size());
    if (rows <= 0) {
        m_multiDeviceScrollArea->setMinimumHeight(70);
        m_multiDeviceScrollArea->setMaximumHeight(90);
        return;
    }
    // 旧式ビジュアライザーの高さに合わせて余裕を持たせる
    const int baseHeight = 150;   // タイトル/ボタン/余白
    const int rowHeight = 120;    // 1行あたりの目安
    int height = baseHeight + rows * rowHeight;
    height = qBound(220, height, 520);
    m_multiDeviceScrollArea->setMinimumHeight(height);
    m_multiDeviceScrollArea->setMaximumHeight(520);
}

void QtVideoMainWindow::updateCameraRowControls()
{
    for (int i = 0; i < m_cameraRows.size(); ++i) {
        if (m_cameraRows[i]) {
            m_cameraRows[i]->setRemoveButtonVisible(m_cameraRows.size() > 1);
        }
    }
    if (m_addCameraButton) {
        m_addCameraButton->setEnabled(m_cameraRows.size() < 4);
    }
}

void QtVideoMainWindow::removeCameraRow(VideoDeviceRowWidget* widget)
{
    int index = m_cameraRows.indexOf(widget);
    if (index < 0) {
        return;
    }
    m_cameraRows.removeAt(index);
    widget->deleteLater();
}

///////////////////////////////////////////////////////////////////////////////
// End of Phase 1 Implementation
///////////////////////////////////////////////////////////////////////////////

#endif // USE_QT6
