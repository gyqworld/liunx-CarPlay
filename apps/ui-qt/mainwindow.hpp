#pragma once

#include "carplay/platform/linux_hotspot.hpp"
#include "carplay/platform/system_info.hpp"
#include "carplay/session/controller.hpp"

#include <QMainWindow>
#include <QString>
#include <memory>

class QComboBox;
class QLineEdit;
class QSpinBox;
class QPlainTextEdit;
class QPushButton;
class QLabel;
class VideoSurface;

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit MainWindow(QWidget* parent = nullptr);

 public slots:
  void appendStatus(const QString& text);

 private slots:
  void onStart();
  void onStop();
  void onDoctor();
  void onSave();
  void onModeChanged(int index);
  void onResolutionPreset(int index);
  void onCustomSizeEdited();
  void onReadNetwork();
  void onReadBluetooth();
  void onAutoFill();
  void onImportAuth();
  void onShowVideo();
  void onHostUiRequested();
  void onReadScreen();
  void onClarityPriority();
  void onStartHotspot();
  void onStopHotspot();

 private:
  carplay::RuntimeConfig readForm() const;
  void writeForm(const carplay::RuntimeConfig& config);
  void setRunning(bool running);
  void applyNetwork(const carplay::CurrentNetworkInfo& info);
  void applyBluetooth(const carplay::BluetoothInventory& inventory);
  void ensureVideoWindow();
  void syncResolutionPreset();
  void updateSizeEditorsEnabled();
  bool applyPrimaryScreen(bool announce);
  void applyClarityPhysical(bool announce);
  void fitStreamToPanel();
  bool ensureHotspot(QString* detail);
  void adoptCarplayAp(const QString& detail);
  bool takeStreamStat(const QString& text);
  void resetStreamPanel();

  std::unique_ptr<carplay::Controller> controller_;
  VideoSurface* video_ = nullptr;
  QComboBox* mode_ = nullptr;
  QComboBox* iface_ = nullptr;
  QLineEdit* ssid_ = nullptr;
  QLineEdit* password_ = nullptr;
  QSpinBox* channel_ = nullptr;
  QComboBox* target_bt_ = nullptr;
  QLineEdit* adapter_bt_ = nullptr;
  QLineEdit* ap_mac_ = nullptr;
  QComboBox* resolution_ = nullptr;
  QSpinBox* width_ = nullptr;
  QSpinBox* height_ = nullptr;
  QSpinBox* phys_w_ = nullptr;
  QSpinBox* phys_h_ = nullptr;
  QSpinBox* max_bitrate_ = nullptr;
  QComboBox* fps_ = nullptr;
  QPushButton* start_ = nullptr;
  QPushButton* stop_ = nullptr;
  QPushButton* show_video_ = nullptr;
  QPushButton* doctor_ = nullptr;
  QPushButton* save_ = nullptr;
  QPushButton* read_net_ = nullptr;
  QPushButton* read_bt_ = nullptr;
  QPushButton* read_screen_ = nullptr;
  QPushButton* clarity_ = nullptr;
  QPushButton* hotspot_on_ = nullptr;
  QPushButton* hotspot_off_ = nullptr;
  QPushButton* import_auth_ = nullptr;
  QLabel* state_ = nullptr;
  QPlainTextEdit* log_ = nullptr;
  QLabel* stat_rate_ = nullptr;
  QLabel* stat_fps_ = nullptr;
  QLabel* stat_frames_ = nullptr;
  QLabel* stat_idr_ = nullptr;
  QLabel* stat_idr_bytes_ = nullptr;
  QLabel* stat_size_ = nullptr;
  QLabel* stat_codec_ = nullptr;
  QLabel* stat_link_ = nullptr;
};
