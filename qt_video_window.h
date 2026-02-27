/**
 * @file qt_video_window.h
 * @brief Qt6ベースのビデオ表示ウィンドウ
 * 
 * SDL2Managerの置き換えとして、Qt6を使用したビデオ表示を提供します。
 * 既存のH.323/メディア処理バックエンドはそのまま維持し、
 * UI層のみをQtに置き換える設計です。
 */

#ifndef QT_VIDEO_WINDOW_H
#define QT_VIDEO_WINDOW_H

#ifdef USE_QT6

#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QImage>
#include <QByteArray>
#include <QMutex>
#include <QTimer>
#include <QDateTime>
#include <thread>
#include <atomic>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QPushButton>
#include <QDialog>
#include <QLineEdit>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QStatusBar>
#include <QPainter>
#include <QKeyEvent>
#include <QCloseEvent>
#include <QStringList>
#include <QSettings>
#include <QCompleter>
#include <QInputDialog>
#include <QScrollArea>
#include <QGuiApplication>
#include <QWindow>
#include <QMessageBox>
#include <QScreen>
#include <QPixmap>
#include <QProgressBar>  // Phase 1: Audio Visualizer
#include <atomic>
#include <QSlider>
#include <QPointer>

#ifdef Q_OS_MAC
#ifndef __MAC_OS_X_DISABLE_AVAILABILITY
#define __MAC_OS_X_DISABLE_AVAILABILITY 1
#endif
#include <ApplicationServices/ApplicationServices.h>
#else
typedef unsigned int CGWindowID;
#endif

// Forward declarations
class MyH323Connection;
class MyH323EndPoint;

// ============================================================================
// Phase 1: Multi-Device Audio Support - Data Structures
// ============================================================================

/**
 * @struct AudioDeviceEntry
 * @brief 単一音声デバイスのエントリ（UI側）
 */
struct AudioDeviceEntry {
    QString name;        // デバイス名
    double gain;         // ゲイン倍率（リニア、1.0 = 0dB）
    bool muted;          // デバイス個別のミュート状態
    
    AudioDeviceEntry() : gain(1.0), muted(false) {}
    AudioDeviceEntry(const QString& n, double g = 1.0, bool m = false)
        : name(n), gain(g), muted(m) {}
};

/**
 * @struct VideoDeviceEntry
 * @brief 単一カメラデバイスのエントリ（UI側）
 */
struct VideoDeviceEntry {
    QString name;        // デバイス名
    bool muted;          // デバイス個別のミュート状態
    
    VideoDeviceEntry() : muted(false) {}
    VideoDeviceEntry(const QString& n, bool m = false)
        : name(n), muted(m) {}
};

/**
 * @struct AudioDeviceSelection
 * @brief UI全体の音声デバイス選択状態
 */
struct AudioDeviceSelection {
    QVector<AudioDeviceEntry> inputDevices;   // マイク（最大4台）
    QVector<AudioDeviceEntry> outputDevices;  // スピーカー（最大4台）
    QVector<VideoDeviceEntry> cameraDevices;  // カメラ（最大4台）
    
    AudioDeviceSelection() {}
};

// ============================================================================
// Callback Function Type Definitions
// ============================================================================

// コールバック関数型定義（main.hへの依存を避けるため）
typedef bool (*MakeCallCallback)(const char* address, void* userData);
typedef void (*HangupCallCallback)(void* userData);
typedef void (*ToggleMuteCallback)(void* userData);
typedef void (*ToggleCameraCallback)(void* userData);
typedef bool (*IsMutedCallback)(void* userData);
typedef bool (*GetDeviceListCallback)(int deviceType, QStringList& outList, void* userData);

// 既存の単一デバイスコールバック（後方互換性のため維持）
typedef void (*ApplyDeviceSelectionCallback)(const QString& mic, const QString& speaker, const QString& camera, void* userData);

// Phase 1: マルチデバイス音声コールバック
typedef void (*ApplyAudioDeviceSelectionCallback)(const AudioDeviceSelection& selection, void* userData);
typedef void (*ApplyAudioProfileCallback)(int index, void* userData);
typedef void (*SetOverlayTextCallback)(const QString& text, bool enabled, void* userData);

// デバイスタイプ定数
#define QT_DEVICE_TYPE_MIC 0
#define QT_DEVICE_TYPE_SPEAKER 1
#define QT_DEVICE_TYPE_CAMERA 2

/**
 * @struct VideoFrame
 * @brief ビデオフレームデータ構造（main.hからの再定義を避けるため前方宣言）
 */
struct QtVideoFrame {
    unsigned char* data;
    unsigned width;
    unsigned height;
    size_t dataSize;
    
    QtVideoFrame() : data(nullptr), width(0), height(0), dataSize(0) {}
    QtVideoFrame(unsigned char* d, unsigned w, unsigned h, size_t size)
        : data(d), width(w), height(h), dataSize(size) {}
};

/**
 * @class QtVideoWidget
 * @brief 単一のビデオ表示用ウィジェット
 * 
 * YUV420PまたはRGB形式のフレームを受け取り、QImageとして描画します。
 */
class QtVideoWidget : public QWidget
{
    Q_OBJECT

public:
    explicit QtVideoWidget(const QString& title, QWidget* parent = nullptr);
    virtual ~QtVideoWidget();

    /**
     * @brief フレームを更新（YUV420P形式）
     * @param yuvData YUV420Pデータ
     * @param width フレーム幅
     * @param height フレーム高さ
     */
    void updateFrameYUV420P(const unsigned char* yuvData, unsigned width, unsigned height);

    /**
     * @brief フレームを更新（RGB24形式）
     * @param rgbData RGB24データ
     * @param width フレーム幅
     * @param height フレーム高さ
     */
    void updateFrameRGB24(const unsigned char* rgbData, unsigned width, unsigned height);

    /**
     * @brief フレームサイズを取得
     */
    QSize frameSize() const { return QSize(m_frameWidth, m_frameHeight); }

    /**
     * @brief ミュート状態を設定
     * @param muted true=ミュート中, false=アンミュート
     */
    void setMuted(bool muted);

    /**
     * @brief ミュート状態を取得
     */
    bool isMuted() const { return m_muted; }

    /**
     * @brief ミュートアイコン表示の有効化/無効化
     */
    void setShowMuteIcon(bool show) { m_showMuteIcon = show; update(); }

    /**
     * @brief カメラミュート状態を設定（黒画面表示）
     * @param muted true=カメラOFF（黒画面）, false=通常表示
     */
    void setCameraMuted(bool muted);

    /**
     * @brief カメラミュート状態を取得
     */
    bool isCameraMuted() const { return m_cameraMuted; }

signals:
    /**
     * @brief フレーム更新シグナル（スレッドセーフな更新用）
     */
    void frameUpdated();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

private:
    void convertYUV420PtoRGB(const unsigned char* yuvData, unsigned width, unsigned height);
    void drawMicrophoneIcon(QPainter& painter, int x, int y, int size, bool muted);
    void updateTargetRect();  // 描画領域の再計算

    QImage m_currentFrame;
    QMutex m_frameMutex;
    QString m_title;
    unsigned m_frameWidth;
    unsigned m_frameHeight;
    bool m_muted;           // マイクミュート状態
    bool m_cameraMuted;     // カメラミュート状態（黒画面表示）
    bool m_showMuteIcon;    // アイコン表示有効
    
    // RGB変換バッファ
    QByteArray m_rgbBuffer;
    
    // フレームレート制限
    qint64 m_lastFrameTime;
    int m_frameUpdateCount;  // フレーム更新カウント（デバッグ用）
    int m_paintCallCount;    // paint呼び出しカウント（デバッグ用）
    int m_timerCallCount;    // タイマー呼び出しカウント（デバッグ用）
    
    // 描画領域キャッシュ（カクカク防止）
    QRect m_targetRect;
    QSize m_cachedFrameSize;
    QSize m_cachedWidgetSize;
    
    // タイマーベース描画更新
    QTimer* m_repaintTimer;
    std::atomic<bool> m_needsRepaint;
};

/**
 * @class QtAudioSpectrumWidget
 * @brief オーディオスペクトラムアナライザ表示ウィジェット
 * 
 * グラフィックイコライザ風の縦棒グラフで音声レベルを可視化します。
 * FFT（高速フーリエ変換）を使用して周波数帯域ごとのレベルを表示。
 */
class QtAudioSpectrumWidget : public QWidget
{
    Q_OBJECT

public:
    explicit QtAudioSpectrumWidget(QWidget* parent = nullptr);
    virtual ~QtAudioSpectrumWidget();

    /**
     * @brief PCM音声データを受け取りスペクトラムを更新
     * @param pcmData PCMデータ（16bit signed, mono または stereo）
     * @param sampleCount サンプル数
     * @param channels チャンネル数（1=mono, 2=stereo）
     * @param sampleRate サンプルレート（Hz）
     */
    void updateSpectrum(const int16_t* pcmData, size_t sampleCount, int channels, int sampleRate);

    /**
     * @brief バンド数を設定（デフォルト: 16）
     */
    void setBandCount(int count);

    /**
     * @brief 色を設定
     */
    void setBarColor(const QColor& color) { m_barColor = color; }
    void setPeakColor(const QColor& color) { m_peakColor = color; }
    void setBackgroundColor(const QColor& color) { m_bgColor = color; }

    /**
     * @brief ピークホールド機能の有効/無効
     */
    void setPeakHoldEnabled(bool enabled) { m_peakHoldEnabled = enabled; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void timerEvent(QTimerEvent* event) override;

private:
    void performFFT(const int16_t* pcmData, size_t sampleCount, int channels);
    void updatePeaks();
    void decayBars();

    // スペクトラムデータ
    QVector<float> m_bandLevels;      // 各バンドの現在レベル (0.0-1.0)
    QVector<float> m_peakLevels;      // 各バンドのピークレベル
    QVector<int> m_peakHoldCounters;  // ピークホールド用カウンタ
    
    // FFT用バッファ
    QVector<float> m_fftBuffer;
    QVector<float> m_fftReal;         // FFT実数部
    QVector<float> m_fftImag;         // FFT虚数部
    QVector<float> m_fftMagnitude;    // FFT振幅スペクトル
    QVector<float> m_window;          // ハミング窓
    QVector<int16_t> m_sampleBuffer;  // サンプル蓄積バッファ
    int m_fftSize;                    // FFTサイズ（2の累乗）
    int m_sampleBufferPos;            // バッファ書き込み位置
    
    // 設定
    int m_bandCount;
    QColor m_barColor;
    QColor m_peakColor;
    QColor m_bgColor;
    bool m_peakHoldEnabled;
    
    // アニメーション
    int m_timerId;
    float m_decayRate;
    int m_peakHoldTime;
    
    // ミューテックス
    QMutex m_dataMutex;
};

// ==================== Phase 1: Multi-Device Audio UI ====================

/**
 * @class AudioDeviceRowWidget
 * @brief 単一のオーディオデバイス行を表すウィジェット
 * 
 * デバイス選択コンボボックス、ゲインスライダー（-12dB～+18dB）、
 * ミュートボタン、削除ボタンを含む1行のUIコンポーネント。
 */
class AudioDeviceRowWidget : public QWidget
{
    Q_OBJECT

public:
    /**
     * @brief コンストラクタ
     * @param deviceType デバイスタイプ（QT_DEVICE_TYPE_MIC または QT_DEVICE_TYPE_SPEAKER）
     * @param availableDevices 利用可能なデバイス名のリスト
     * @param parent 親ウィジェット
     */
    explicit AudioDeviceRowWidget(int deviceType, 
                                   const QStringList& availableDevices,
                                   QWidget* parent = nullptr);
    virtual ~AudioDeviceRowWidget();

    /**
     * @brief デバイス設定を取得
     * @return AudioDeviceEntry 構造体
     */
    AudioDeviceEntry getDeviceEntry() const;

    /**
     * @brief デバイス設定を適用
     * @param entry AudioDeviceEntry 構造体
     */
    void setDeviceEntry(const AudioDeviceEntry& entry);

    /**
     * @brief デバイスリストを更新
     * @param devices 新しいデバイスリスト
     */
    void updateDeviceList(const QStringList& devices);

    /**
     * @brief 削除ボタンの表示/非表示
     * @param show true=表示, false=非表示
     */
    void setRemoveButtonVisible(bool show);

    /**
     * @brief ミュートボタンの表示/非表示
     * @param show true=表示, false=非表示
     */
    void setMuteButtonVisible(bool show);

    /**
     * @brief デバイス選択の有効/無効を切り替え
     * @param enabled true=選択可能, false=選択不可
     */
    void setDeviceSelectionEnabled(bool enabled);

    /**
     * @brief 音声レベルメーターを更新
     * @param pcmData PCMデータ
     * @param sampleCount サンプル数
     * @param channels チャンネル数
     * @param sampleRate サンプルレート
     */
    void updateSpectrum(const int16_t* pcmData, size_t sampleCount, int channels, int sampleRate);

    /**
     * @brief ビジュアライザーをリセット
     */
    void resetSpectrum(size_t sampleCount, int sampleRate);

signals:
    /**
     * @brief デバイス設定が変更された（デバイス追加/削除/変更）
     * @param row このウィジェットの行番号（外部で管理）
     */
    void deviceChanged();
    
    /**
     * @brief ゲイン設定のみが変更された（ゲイン/ミュート）
     */
    void gainChanged();

    /**
     * @brief 削除ボタンがクリックされた
     * @param widget このウィジェットへのポインタ
     */
    void removeRequested(AudioDeviceRowWidget* widget);

private slots:
    void onDeviceComboChanged(int index);
    void onGainSliderChanged(int value);
    void onMuteToggled(bool checked);
    void onRemoveClicked();

private:
    void setupUI(const QStringList& availableDevices);
    QString formatGainLabel(double gainDb) const;
    
    int m_deviceType;                    // QT_DEVICE_TYPE_MIC or QT_DEVICE_TYPE_SPEAKER
    QComboBox* m_deviceCombo;            // デバイス選択
    QSlider* m_gainSlider;               // ゲイン (-12dB ~ +18dB)
    QLabel* m_gainLabel;                 // ゲイン表示ラベル
    QCheckBox* m_muteCheckbox;           // ミュート
    QPushButton* m_removeButton;         // 削除ボタン
    QtAudioSpectrumWidget* m_spectrum;   // 旧式ビジュアライザー
};

/**
 * @class VideoDeviceRowWidget
 * @brief 単一のカメラデバイス行を表すウィジェット
 */
class VideoDeviceRowWidget : public QWidget
{
    Q_OBJECT

public:
    explicit VideoDeviceRowWidget(const QStringList& availableDevices,
                                  QWidget* parent = nullptr);
    virtual ~VideoDeviceRowWidget();

    VideoDeviceEntry getDeviceEntry() const;
    void setDeviceEntry(const VideoDeviceEntry& entry);
    void updateDeviceList(const QStringList& devices);
    void setRemoveButtonVisible(bool show);
    void setMuteButtonVisible(bool show);

signals:
    void deviceChanged();
    void removeRequested(VideoDeviceRowWidget* widget);

private slots:
    void onDeviceComboChanged(int index);
    void onMuteToggled(bool checked);
    void onRemoveClicked();

private:
    void setupUI(const QStringList& availableDevices);

    QComboBox* m_deviceCombo;
    QCheckBox* m_muteCheckbox;
    QPushButton* m_removeButton;
};

// ==================== End of Phase 1 Audio UI ====================

/**
 * @class QtVideoMainWindow
 * @brief メインウィンドウ（ローカル/リモートビデオ + コントロールパネル）
 * 
 * SDL2Managerの機能を置き換えるメインウィンドウです。
 * - ローカルプレビュー表示
 * - リモートビデオ表示
 * - 接続/切断ボタン
 * - ミュート/カメラOFFボタン
 * - デバイス選択
 */
class QtVideoMainWindow : public QMainWindow
{
    Q_OBJECT
    
    friend class QtVideoManager;  // QtVideoManagerがメンバーにアクセスできるように

public:
    explicit QtVideoMainWindow(QWidget* parent = nullptr);
    virtual ~QtVideoMainWindow();

    /**
     * @brief H323Connectionを設定
     */
    void setH323Connection(MyH323Connection* connection);
    
    /**
     * @brief H323Connectionを取得
     */
    MyH323Connection* getH323Connection() const { return m_h323Connection; }

    /**
     * @brief ローカルフレームを更新（H.323スレッドから呼び出し可能）
     */
    void enqueueLocalFrame(const unsigned char* yuvData, unsigned width, unsigned height);

    /**
     * @brief リモートフレームを更新（H.323スレッドから呼び出し可能）
     */
    void enqueueRemoteFrame(const unsigned char* yuvData, unsigned width, unsigned height);

    /**
     * @brief ウィンドウが実行中かどうか
     */
    bool isRunning() const { return m_running; }

    /**
     * @brief 接続状態を更新
     */
    void setConnectionStatus(const QString& status);
    
    /**
     * @brief コンテンツボタンの有効/無効を設定
     */
    void setContentButtonEnabled(bool enabled);

    /**
     * @brief ローカルビデオのミュート状態を設定
     */
    void setLocalMuted(bool muted);

    /**
     * @brief リモートビデオのミュート状態を設定
     */
    void setRemoteMuted(bool muted);

    /**
     * @brief ローカルカメラのミュート状態を設定（黒画面表示）
     */
    void setLocalCameraMuted(bool muted);

    /**
     * @brief デバイスリストを再取得してUIを更新
     */
    void refreshDeviceLists();

signals:
    /**
     * @brief ローカルフレーム到着シグナル（スレッド間通信用）
     */
    void localFrameReady(const QByteArray& yuvData, unsigned width, unsigned height);

    /**
     * @brief リモートフレーム到着シグナル（スレッド間通信用）
     */
    void remoteFrameReady(const QByteArray& yuvData, unsigned width, unsigned height);

    /**
     * @brief 接続要求シグナル
     */
    void connectRequested(const QString& address);

    /**
     * @brief 切断要求シグナル
     */
    void disconnectRequested();

    /**
     * @brief プログラム終了要求シグナル
     */
    void exitRequested();

    /**
     * @brief ミュートトグル要求シグナル
     */
    void muteToggleRequested();

    /**
     * @brief カメラトグル要求シグナル
     */
    void cameraToggleRequested();

    /**
     * @brief 音声品質プロファイル変更要求シグナル
     */
    void audioProfileChanged(int index);

    /**
     * @brief ローカル映像の背景テキスト変更要求シグナル
     */
    void overlayTextChanged(const QString& text, bool enabled);

public slots:
    /**
     * @brief ローカルフレームを処理（Qtメインスレッド）
     */
    void onLocalFrameReady(const QByteArray& yuvData, unsigned width, unsigned height);

    /**
     * @brief リモートフレームを処理（Qtメインスレッド）
     */
    void onRemoteFrameReady(const QByteArray& yuvData, unsigned width, unsigned height);

    // ==================== Phase 1: Multi-Device Audio UI Slots ====================
    
    /**
     * @brief マイク行を追加
     */
    void onAddMicClicked();

    /**
     * @brief スピーカー行を追加
     */
    void onAddSpeakerClicked();

    /**
     * @brief デバイス行が削除要求を発行
     * @param widget 削除するウィジェット
     */
    void onDeviceRowRemoveRequested(AudioDeviceRowWidget* widget);

    /**
     * @brief デバイス設定が変更された（デバイス追加/削除/変更）
     */
    void onDeviceRowChanged();
    
    /**
     * @brief ゲイン設定のみが変更された（接続中も適用可能）
     */
    void onGainChanged();

    /**
     * @brief オーディオビジュアライザーを更新（タイマーから呼ばれる）
     */
    void updateAudioVisualizers();
    
    /**
     * @brief オーディオビジュアライザータイマーを停止（接続クローズ時に呼ばれる）
     */
    void stopAudioVisualizerTimer();

    // ==================== End of Phase 1 Audio UI Slots ====================

private:
    void updateConnectButtonStyle();
    void updateDisconnectButtonStyle();
    void updateRecordButtonStyle();
    void startConnectBlink();
    void stopConnectBlink();
    void setRecordingUiState(bool recording);

    /**
     * @brief 通話がアクティブかどうかのフラグ（スレッドセーフ）
     * タイマーコールバックで dangling pointer アクセスを防ぐために使用
     */
    std::atomic<bool> m_isCallActive{false};

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private slots:
    void onConnectClicked();
    void onDisconnectClicked();
    void onExitClicked();
    void onRecordClicked();
    void onMuteToggled(bool checked);
    void onCameraToggled(bool checked);
    void onMicDeviceChanged(int index);
    void onSpeakerDeviceChanged(int index);
    void onCameraDeviceChanged(int index);
    void onOverlayTextClicked();
    void onOverlayTextSendClicked();
    void onOverlayTextClearClicked();
    void onAddCameraClicked();
    void onCameraRowRemoveRequested(VideoDeviceRowWidget* widget);
    void onSeparateWindowsClicked();
    void onMultiDeviceWindowClicked();
    void onContentWindowClicked();
    void onContentSendClicked();
    void onRemoteWindowClosed();
    void onAddressActivated(int index);
    void onAudioProfileSliderChanged(int value);

private:
    void setupUI();
    void setupConnections();
    void populateDeviceLists();
    void loadAddressHistory();
    void saveAddressHistory();
    void addToAddressHistory(const QString& address);
    void clearAddressHistory();
    void appendClearHistoryItem();
    bool isClearHistoryItem(int index) const;
    void setupMultiDeviceWindow();

    // ==================== Phase 1: Multi-Device Audio UI Private Methods ====================
    
    /**
     * @brief マルチデバイスUIをセットアップ（setupUI内で呼ばれる）
     */
    void setupMultiDeviceAudioUI(QVBoxLayout* mainLayout);
    void setupMultiDeviceCameraUI(QVBoxLayout* mainLayout);

    /**
     * @brief 現在のオーディオデバイス設定を取得
     * @return AudioDeviceSelection 構造体
     */
    AudioDeviceSelection getAudioDeviceSelection() const;

    /**
     * @brief オーディオデバイス設定を適用
     * @param selection AudioDeviceSelection 構造体
     */
    void setAudioDeviceSelection(const AudioDeviceSelection& selection);

    /**
     * @brief 利用可能なデバイスリストを取得（PortAudio経由）
     * @param deviceType QT_DEVICE_TYPE_MIC または QT_DEVICE_TYPE_SPEAKER
     * @return デバイス名のリスト
     */
    QStringList getAvailableDevices(int deviceType);

    /**
     * @brief デバイス行を削除
     * @param widget 削除するウィジェット
     * @param rows ウィジェット配列（m_micRows または m_speakerRows）
     */
    void removeDeviceRow(AudioDeviceRowWidget* widget, QVector<AudioDeviceRowWidget*>& rows);

    /**
     * @brief 行ごとのUI制御を更新（削除/ミュート表示など）
     * @param rows ウィジェット配列（m_micRows または m_speakerRows）
     */
    void updateDeviceRowControls(QVector<AudioDeviceRowWidget*>& rows);
    void updateCameraRowControls();

    /**
     * @brief マルチデバイスパネルの高さを更新
     */
    void updateMultiDevicePanelHeight();
    void removeCameraRow(VideoDeviceRowWidget* widget);

    // ==================== End of Phase 1 Audio UI Private Methods ====================

    // ビデオ表示
    QtVideoWidget* m_localVideo;
    QtVideoWidget* m_remoteVideo;

    // ウィンドウ分離用
    QSplitter* m_videoSplitter;      // ビデオスプリッター
    QPushButton* m_separateButton;   // 分離/結合ボタン
    QPushButton* m_multiDeviceButton; // マルチデバイス設定ウィンドウ表示ボタン
    bool m_windowsSeparated;         // 分離状態フラグ

    // コントロール
    QComboBox* m_addressCombo;  // 履歴付きアドレス入力
    QPushButton* m_connectButton;
    QPushButton* m_disconnectButton;
    QPushButton* m_exitButton;
    QPushButton* m_recordButton;
    QCheckBox* m_muteCheckbox;
    QCheckBox* m_cameraCheckbox;
    QSlider* m_audioProfileSlider;
    QLabel* m_audioProfileNameLabel;
    int m_audioProfileIndex;
    QPushButton* m_contentButton;  // コンテンツ再表示ボタン
    QPushButton* m_contentSendButton; // コンテンツ送信ボタン
    QWidget* m_gainRowWidget;          // 旧来の全体ゲインUI
    QSlider* m_gainSlider;           // マイク入力ゲイン
    QLabel* m_gainValueLabel;        // 現在のdB表示
    QSlider* m_spkGainSlider;        // スピーカー出力ゲイン
    QLabel* m_spkGainValueLabel;     // スピーカーのdB表示
    int m_baseMicGainIndex;          // 旧式マイクゲイン(0-9)
    int m_baseSpeakerGainIndex;      // 旧式スピーカーゲイン(0-9)
    
    // デバイス選択
    QComboBox* m_micCombo;
    QComboBox* m_speakerCombo;
    QComboBox* m_cameraCombo;
    QPushButton* m_overlayTextButton;

    // ステータス
    QLabel* m_statusLabel;

    // 背景テキスト入力ウィンドウ
    QDialog* m_overlayTextDialog;
    QLineEdit* m_overlayTextLineEdit;
    QString m_overlayText;
    bool m_overlayTextEnabled;

    // オーディオスペクトラム
    QtAudioSpectrumWidget* m_localSpectrum;   // ローカル（マイク）
    QtAudioSpectrumWidget* m_remoteSpectrum;  // リモート（相手の音声）

    // アドレス履歴
    static const int MAX_ADDRESS_HISTORY = 20;

    // 状態
    MyH323Connection* m_h323Connection;
    bool m_running;

    // ==================== Phase 1: Multi-Device Audio UI Members ====================
    
    // マルチデバイスオーディオ用UI
    QVector<AudioDeviceRowWidget*> m_micRows;      // マイク行ウィジェット配列
    QVector<AudioDeviceRowWidget*> m_speakerRows;  // スピーカー行ウィジェット配列
    QVBoxLayout* m_micRowsLayout;                  // マイク行レイアウト
    QVBoxLayout* m_speakerRowsLayout;              // スピーカー行レイアウト
    QPushButton* m_addMicButton;                   // マイク追加ボタン
    QPushButton* m_addSpeakerButton;               // スピーカー追加ボタン
    QTimer* m_audioVisualizerTimer;                // ビジュアライザー更新タイマー (75ms間隔)
    QTimer* m_connectBlinkTimer;                   // Connect点滅タイマー
    bool m_connectBlinkOn;                         // 点滅表示ON/OFF
    bool m_connectBlinkActive;                     // 点滅有効状態
    bool m_connectEstablished;                     // 接続完了状態（Connect反転表示）
    bool m_recordingActive;                        // 録画中状態
    QScrollArea* m_multiDeviceScrollArea;          // マルチデバイススクロール領域
    QDialog* m_multiDeviceWindow;                  // マルチデバイス設定ウィンドウ
    
    // マルチデバイスカメラ用UI
    QVector<VideoDeviceRowWidget*> m_cameraRows;   // カメラ行ウィジェット配列
    QVBoxLayout* m_cameraRowsLayout;               // カメラ行レイアウト
    QPushButton* m_addCameraButton;                // カメラ追加ボタン

    // ==================== End of Phase 1 Audio UI Members ====================

public:
    /**
     * @brief ローカル音声スペクトラムを更新
     */
    void updateLocalAudioSpectrum(const int16_t* pcmData, size_t sampleCount, int channels, int sampleRate);
    
    /**
     * @brief リモート音声スペクトラムを更新
     */
    void updateRemoteAudioSpectrum(const int16_t* pcmData, size_t sampleCount, int channels, int sampleRate);
};

/**
 * @class QtContentWindow
 * @brief H.239コンテンツ表示用の単純なウィンドウ
 */
class QtContentWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit QtContentWindow(QWidget* parent = nullptr);
    virtual ~QtContentWindow();

    void enqueueContentFrame(const unsigned char* yuvData, unsigned width, unsigned height, size_t dataSize);

signals:
    void contentFrameReady(const QByteArray& yuvData, unsigned width, unsigned height);

private slots:
    void onContentFrameReady(const QByteArray& yuvData, unsigned width, unsigned height);

private:
    QtVideoWidget* m_contentVideo;
};

/**
 * @class QtVideoManager
 * @brief MainThreadSDL2Managerの置き換え
 * 
 * シングルトンパターンでQtビデオ管理を提供します。
 * 既存のコードからの移行を容易にするため、
 * MainThreadSDL2Managerと同様のインターフェースを提供します。
 */
class QtVideoManager
{
public:
    static QtVideoManager& getInstance();
    static QtVideoManager& instance() { return getInstance(); }

    /**
     * @brief Qt初期化（QApplicationは外部で作成済みであること）
     */
    bool initialize();

    /**
     * @brief Qt終了
     */
    void shutdown();

    /**
     * @brief ウィンドウ作成
     */
    bool createWindow(int width = 1280, int height = 720);
    bool createLocalWindow(int width, int height);
    bool createRemoteWindow(int width, int height);
    bool createContentWindow(int width, int height);
    
    /**
     * @brief ウィンドウを表示
     */
    void showWindow();

    /**
     * @brief フレームをキューに追加（H.323スレッドから呼び出し）
     */
    void enqueueLocalFrame(const unsigned char* yuvData, unsigned width, unsigned height, size_t dataSize);
    void enqueueRemoteFrame(const unsigned char* yuvData, unsigned width, unsigned height, size_t dataSize);
    void enqueueContentFrame(const unsigned char* yuvData, unsigned width, unsigned height, size_t dataSize);
    
    /**
     * @brief フレームをキューに追加（Qt6VideoOutputDevice互換）
     */
    void queueLocalFrame(const unsigned char* data, size_t dataSize, unsigned width, unsigned height) {
        enqueueLocalFrame(data, width, height, dataSize);
    }
    void queueRemoteFrame(const unsigned char* data, size_t dataSize, unsigned width, unsigned height) {
        enqueueRemoteFrame(data, width, height, dataSize);
    }
    void queueContentFrame(const unsigned char* data, size_t dataSize, unsigned width, unsigned height) {
        enqueueContentFrame(data, width, height, dataSize);
    }

    /**
     * @brief H323Connectionを設定
     */
    void setH323Connection(MyH323Connection* connection);
    MyH323Connection* getH323Connection() const;
    
    /**
     * @brief H323Endpointを設定（UIからの接続開始に使用）
     */
    void setH323Endpoint(class MyH323EndPoint* endpoint);
    class MyH323EndPoint* getH323Endpoint() const;
    
    /**
     * @brief コールバック関数を設定
     */
    void setMakeCallCallback(MakeCallCallback cb, void* userData);
    void setHangupCallCallback(HangupCallCallback cb, void* userData);
    void setToggleMuteCallback(ToggleMuteCallback cb, void* userData);
    void setToggleCameraCallback(ToggleCameraCallback cb, void* userData);
    void setIsMutedCallback(IsMutedCallback cb, void* userData);
    void setGetDeviceListCallback(GetDeviceListCallback cb, void* userData);
    void setApplyDeviceSelectionCallback(ApplyDeviceSelectionCallback cb, void* userData);
    bool getDeviceList(int deviceType, QStringList& outList);
    void applyDeviceSelection(const QString& mic, const QString& speaker, const QString& camera);
    
    // ==================== Phase 1: Multi-Device Audio API ====================
    
    /**
     * @brief マルチデバイスオーディオ設定コールバックを設定
     */
    void setApplyAudioDeviceSelectionCallback(ApplyAudioDeviceSelectionCallback cb, void* userData);
    
    /**
     * @brief マルチデバイスオーディオ設定を適用（デバイス変更）
     */
    void applyAudioDeviceSelection(const AudioDeviceSelection& selection);
    
    /**
     * @brief オーディオゲイン設定のみを適用（接続中も可能）
     */
    void applyAudioGainSettings(const AudioDeviceSelection& selection);

    /**
     * @brief 音声品質プロファイルを適用
     */
    void setApplyAudioProfileCallback(ApplyAudioProfileCallback cb, void* userData);
    void applyAudioProfile(int index);
    void setOverlayTextCallback(SetOverlayTextCallback cb, void* userData);
    void applyOverlayText(const QString& text, bool enabled);
    
    // ==================== End of Phase 1 Audio API ====================
    
    /**
     * @brief UIから接続を開始
     */
    void makeCall(const QString& address);
    
    /**
     * @brief UIから切断
     */
    void hangupCall();

    /**
     * @brief UIからプログラム終了
     */
    void requestProgramExit();
    
    /**
     * @brief マイクミュートをトグル
     */
    void toggleMute();
    
    /**
     * @brief カメラON/OFFをトグル
     */
    void toggleCamera();

    /**
     * @brief 初期化済みかどうか
     */
    bool isInitialized() const { return m_initialized; }

    /**
     * @brief ウィンドウが存在するかどうか
     */
    bool hasWindow() const { return m_mainWindow != nullptr; }
    bool hasLocalWindow() const { return m_mainWindow != nullptr; }
    bool hasRemoteWindow() const { return m_mainWindow != nullptr; }
    bool hasContentWindow() const { return m_contentWindow != nullptr; }
    void closeContentWindow(bool disableAvailability = false);
    void openContentWindow();
    void setContentAvailable(bool available);
    void requestContentSend(const QString& label, CGWindowID windowId);
    QString getContentSendTarget() const { return m_contentSendTarget; }
    void startContentCapture(CGWindowID windowId, const QString& label);
    void stopContentCapture();
    void captureContentFrame();      // Timer callback - captures in main thread
    void captureContentFrameNow();   // Called by encoder - returns buffered frame
    bool getLatestContentFrame(QByteArray& outFrame, unsigned& width, unsigned& height);
    void clearContentWindowPointer() { m_contentWindow = nullptr; }
    
    /**
     * @brief コンテンツ送信状態を確認
     */
    bool isContentSending() const { return !m_contentSendTarget.isEmpty(); }
    
    /**
     * @brief ローカルコンテンツプレビューウィンドウを開く/更新
     */
    void updateLocalContentPreview(const QByteArray& frame, unsigned width, unsigned height);
    
    /**
     * @brief ローカルコンテンツプレビューウィンドウを閉じる
     */
    void closeLocalContentPreview();

    /**
     * @brief メインウィンドウを取得
     */
    QtVideoMainWindow* mainWindow() const { return m_mainWindow; }
    QtVideoMainWindow* getMainWindow() const { return m_mainWindow; }
    QtContentWindow* getContentWindow() const { return m_contentWindow; }

    /**
     * @brief ミュート状態を更新
     * @param localMuted ローカルマイクのミュート状態
     * @param remoteMuted リモートマイクのミュート状態
     */
    void updateMuteState(bool localMuted, bool remoteMuted);
    
    /**
     * @brief カメラ状態を更新
     * @param cameraMuted カメラがオフの場合true
     */
    void updateCameraState(bool cameraMuted);
    
    /**
     * @brief 接続ステータスを更新
     */
    void setConnectionStatus(const QString& status);
    
    /**
     * @brief デバイスリストを更新（UIスレッドで実行）
     */
    void refreshDeviceLists();

private:
    QtVideoManager();
    ~QtVideoManager();

    // コピー禁止
    QtVideoManager(const QtVideoManager&) = delete;
    QtVideoManager& operator=(const QtVideoManager&) = delete;
    
    void setupSignalConnections();

    QPointer<QtVideoMainWindow> m_mainWindow;
    QPointer<QtContentWindow> m_contentWindow;  // リモートコンテンツ受信用
    QPointer<QtContentWindow> m_localContentWindow;  // ローカルコンテンツ送信プレビュー用
    class MyH323EndPoint* m_endpoint;
    bool m_initialized;
    int m_lastContentWidth;
    int m_lastContentHeight;
    bool m_contentAvailable;
    CGWindowID m_contentWindowId;
    QString m_contentSendTarget;
    QTimer m_contentCaptureTimer;
    QByteArray m_lastCapturedFrame;
    int m_captureWidth = 1280;
    int m_captureHeight = 720;
    QMutex m_contentFrameMutex; // protects content capture buffer
    
    // 🔧 Thread-based content capture (QTimer doesn't work in non-Qt threads)
    std::atomic<bool> m_captureThreadRunning{false};
    std::thread m_captureThread;
    
    // コールバック関数（各コールバックごとに専用のuserDataを保持）
    MakeCallCallback m_makeCallCb;
    void* m_makeCallUserData;
    
    HangupCallCallback m_hangupCallCb;
    void* m_hangupCallUserData;
    
    ToggleMuteCallback m_toggleMuteCb;
    void* m_toggleMuteUserData;
    
    ToggleCameraCallback m_toggleCameraCb;
    void* m_toggleCameraUserData;
    
    IsMutedCallback m_isMutedCb;
    void* m_isMutedUserData;
    
    GetDeviceListCallback m_getDeviceListCb;
    void* m_getDeviceListUserData;
    
    ApplyDeviceSelectionCallback m_applyDeviceSelectionCb;
    void* m_applyDeviceSelectionUserData;
    
    // ==================== Phase 1: Multi-Device Audio Callback ====================
    ApplyAudioDeviceSelectionCallback m_applyAudioDeviceSelectionCb;
    void* m_applyAudioDeviceSelectionUserData;
    ApplyAudioProfileCallback m_applyAudioProfileCb;
    void* m_applyAudioProfileUserData;
    SetOverlayTextCallback m_setOverlayTextCb;
    void* m_setOverlayTextUserData;
    // ==================== End of Phase 1 Audio Callback ====================
};

#endif // USE_QT6

#endif // QT_VIDEO_WINDOW_H
