#include "mainwindow.hpp"

#include "carplay/platform/system_info.hpp"
#include "carplay/protocol/mfi_signer.hpp"
#include "carplay/protocol/version.hpp"
#include "carplay/session/settings_io.hpp"
#include "video_surface.hpp"

#include <QApplication>
#include <QByteArray>
#include <QComboBox>
#include <QGuiApplication>
#include <QScreen>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSize>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QVariant>

#include <cmath>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  controller_ = carplay::make_default_controller();
  carplay::RuntimeConfig config = controller_->config();
  carplay::load_runtime_config(controller_->storage(), config);
  controller_->set_config(config);

  auto* central = new QWidget(this);
  auto* row = new QHBoxLayout(central);
  auto* left = new QWidget(central);
  auto* root = new QVBoxLayout(left);
  row->addWidget(left, 1);
  auto* form = new QFormLayout();

  mode_ = new QComboBox(central);
  mode_->addItem(QStringLiteral("本机移动热点"), QStringLiteral("hotspot"));
  mode_->addItem(QStringLiteral("现有局域网"), QStringLiteral("lan"));
  connect(mode_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onModeChanged);

  iface_ = new QComboBox(central);
  iface_->setEditable(true);
  ssid_ = new QLineEdit(central);
  password_ = new QLineEdit(central);
  password_->setEchoMode(QLineEdit::Password);
  channel_ = new QSpinBox(central);
  channel_->setRange(0, 196);
  target_bt_ = new QComboBox(central);
  target_bt_->setEditable(true);
  target_bt_->lineEdit()->setPlaceholderText(QStringLiteral("AA:BB:CC:DD:EE:FF"));
  adapter_bt_ = new QLineEdit(central);
  adapter_bt_->setPlaceholderText(QStringLiteral("本机蓝牙地址"));
  ap_mac_ = new QLineEdit(central);
  ap_mac_->setPlaceholderText(QStringLiteral("局域网 BSSID，可留空"));
  resolution_ = new QComboBox(central);
  const struct {
    const char* label;
    int w;
    int h;
  } presets[] = {{"800 × 480（车机常见）", 800, 480},
                 {"1024 × 600", 1024, 600},
                 {"1280 × 720", 1280, 720},
                 {"1920 × 1080", 1920, 1080},
                 {"2560 × 1440", 2560, 1440},
                 {"2880 × 1620", 2880, 1620},
                 {"3200 × 1800", 3200, 1800},
                 {"3840 × 2160（4K）", 3840, 2160},
                 {"5120 × 2880（5K）", 5120, 2880},
                 {"自定义", 0, 0}};
  for (const auto& p : presets) {
    resolution_->addItem(QString::fromUtf8(p.label), QSize(p.w, p.h));
  }
  width_ = new QSpinBox(central);
  width_->setRange(320, 7680);
  width_->setSingleStep(2);
  height_ = new QSpinBox(central);
  height_->setRange(200, 4320);
  height_->setSingleStep(2);
  phys_w_ = new QSpinBox(central);
  phys_w_->setRange(1, 2000);
  phys_w_->setSuffix(QStringLiteral(" mm"));
  phys_h_ = new QSpinBox(central);
  phys_h_->setRange(1, 2000);
  phys_h_->setSuffix(QStringLiteral(" mm"));
  phys_w_->setToolTip(QStringLiteral("上报给 iPhone 的物理宽度（毫米）。数值越小，图标越按高倍绘制"));
  phys_h_->setToolTip(QStringLiteral("上报给 iPhone 的物理高度（毫米）"));
  max_bitrate_ = new QSpinBox(central);
  max_bitrate_->setRange(0, 120);
  max_bitrate_->setSpecialValueText(QStringLiteral("自动"));
  max_bitrate_->setValue(0);
  max_bitrate_->setSuffix(QStringLiteral(" Mbps"));
  max_bitrate_->setToolTip(QStringLiteral("0=不限制，由热点带宽决定。填了才会写入 maxBitRate"));
  fps_ = new QComboBox(central);
  for (int value : {30, 60, 90, 120}) fps_->addItem(QString::number(value), value);

  form->addRow(QStringLiteral("无线模式"), mode_);
  form->addRow(QStringLiteral("网络接口"), iface_);
  form->addRow(QStringLiteral("Wi-Fi 名称"), ssid_);
  form->addRow(QStringLiteral("Wi-Fi 密码"), password_);
  form->addRow(QStringLiteral("信道"), channel_);
  form->addRow(QStringLiteral("iPhone 蓝牙"), target_bt_);
  form->addRow(QStringLiteral("本机蓝牙"), adapter_bt_);
  form->addRow(QStringLiteral("接入点 BSSID"), ap_mac_);
  form->addRow(QStringLiteral("分辨率预设"), resolution_);
  form->addRow(QStringLiteral("像素宽度"), width_);
  form->addRow(QStringLiteral("像素高度"), height_);
  form->addRow(QStringLiteral("物理宽度"), phys_w_);
  form->addRow(QStringLiteral("物理高度"), phys_h_);
  form->addRow(QStringLiteral("码率上限"), max_bitrate_);
  form->addRow(QStringLiteral("帧率"), fps_);
  root->addLayout(form);

  auto* fetch = new QHBoxLayout();
  read_net_ = new QPushButton(QStringLiteral("读取当前网络"), central);
  read_bt_ = new QPushButton(QStringLiteral("刷新蓝牙"), central);
  read_screen_ = new QPushButton(QStringLiteral("读取屏幕"), central);
  clarity_ = new QPushButton(QStringLiteral("抗锯齿"), central);
  clarity_->setToolTip(QStringLiteral("分辨率不变，上报约 240 PPI。真实大屏毫米会让手机用粗像素画图标，边缘易出锯齿"));
  import_auth_ = new QPushButton(QStringLiteral("导入认证"), central);
  fetch->addWidget(read_net_);
  fetch->addWidget(read_bt_);
  fetch->addWidget(read_screen_);
  hotspot_on_ = new QPushButton(QStringLiteral("开启热点"), central);
  hotspot_on_->setToolTip(QStringLiteral("用本机 Wi-Fi 开 2.4GHz SoftAP，并带上 Apple CarPlay 信标，手机才会把码率从约 1 Mbps 提上来"));
  hotspot_off_ = new QPushButton(QStringLiteral("关闭热点"), central);
  fetch->addWidget(clarity_);
  fetch->addWidget(hotspot_on_);
  fetch->addWidget(hotspot_off_);
  fetch->addWidget(import_auth_);
  root->addLayout(fetch);

  auto* buttons = new QHBoxLayout();
  start_ = new QPushButton(QStringLiteral("启动接收"), central);
  stop_ = new QPushButton(QStringLiteral("停止"), central);
  show_video_ = new QPushButton(QStringLiteral("全屏画面"), central);
  doctor_ = new QPushButton(QStringLiteral("自检"), central);
  save_ = new QPushButton(QStringLiteral("保存设置"), central);
  buttons->addWidget(start_);
  buttons->addWidget(stop_);
  buttons->addWidget(show_video_);
  buttons->addWidget(doctor_);
  buttons->addWidget(save_);
  root->addLayout(buttons);

  state_ = new QLabel(central);
  log_ = new QPlainTextEdit(central);
  log_->setReadOnly(true);
  log_->setMaximumBlockCount(500);
  root->addWidget(state_);
  root->addWidget(log_, 1);

  auto* stats = new QGroupBox(QStringLiteral("投屏数据"), central);
  stats->setMinimumWidth(230);
  auto* stats_form = new QFormLayout(stats);
  auto make_stat = [stats](const QString& dash) {
    auto* label = new QLabel(dash, stats);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
  };
  stat_rate_ = make_stat(QStringLiteral("—"));
  stat_rate_->setStyleSheet(QStringLiteral("font-size: 22px; font-weight: 600;"));
  stat_fps_ = make_stat(QStringLiteral("—"));
  stat_frames_ = make_stat(QStringLiteral("—"));
  stat_idr_ = make_stat(QStringLiteral("—"));
  stat_idr_bytes_ = make_stat(QStringLiteral("—"));
  stat_size_ = make_stat(QStringLiteral("—"));
  stat_codec_ = make_stat(QStringLiteral("—"));
  stat_link_ = make_stat(QStringLiteral("未连接"));
  stat_link_->setWordWrap(true);
  stats_form->addRow(QStringLiteral("码率"), stat_rate_);
  stats_form->addRow(QStringLiteral("帧率"), stat_fps_);
  stats_form->addRow(QStringLiteral("总帧"), stat_frames_);
  stats_form->addRow(QStringLiteral("关键帧"), stat_idr_);
  stats_form->addRow(QStringLiteral("关键帧大小"), stat_idr_bytes_);
  stats_form->addRow(QStringLiteral("分辨率"), stat_size_);
  stats_form->addRow(QStringLiteral("编码"), stat_codec_);
  stats_form->addRow(QStringLiteral("链路"), stat_link_);
  row->addWidget(stats, 0);

  connect(start_, &QPushButton::clicked, this, &MainWindow::onStart);
  connect(stop_, &QPushButton::clicked, this, &MainWindow::onStop);
  connect(show_video_, &QPushButton::clicked, this, &MainWindow::onShowVideo);
  connect(doctor_, &QPushButton::clicked, this, &MainWindow::onDoctor);
  connect(save_, &QPushButton::clicked, this, &MainWindow::onSave);
  connect(read_net_, &QPushButton::clicked, this, &MainWindow::onReadNetwork);
  connect(read_bt_, &QPushButton::clicked, this, &MainWindow::onReadBluetooth);
  connect(read_screen_, &QPushButton::clicked, this, &MainWindow::onReadScreen);
  connect(clarity_, &QPushButton::clicked, this, &MainWindow::onClarityPriority);
  connect(hotspot_on_, &QPushButton::clicked, this, &MainWindow::onStartHotspot);
  connect(hotspot_off_, &QPushButton::clicked, this, &MainWindow::onStopHotspot);
  connect(import_auth_, &QPushButton::clicked, this, &MainWindow::onImportAuth);
  connect(resolution_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onResolutionPreset);
  connect(width_, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onCustomSizeEdited);
  connect(height_, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onCustomSizeEdited);

  controller_->set_status_handler([this](const std::string& status) {
    QMetaObject::invokeMethod(this, "appendStatus", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromUtf8(status.c_str())));
  });
  controller_->set_video_handler([this](const carplay::ByteBuffer& annexb) {
    QPointer<VideoSurface> surface = video_;
    if (!surface) return;
    const QByteArray data(reinterpret_cast<const char*>(annexb.data()), static_cast<int>(annexb.size()));
    QMetaObject::invokeMethod(surface, "onAnnexB", Qt::QueuedConnection, Q_ARG(QByteArray, data));
  });
  controller_->set_host_ui_handler([this] {
    QMetaObject::invokeMethod(this, "onHostUiRequested", Qt::QueuedConnection);
  });

  writeForm(config);
  onModeChanged(mode_->currentIndex());
  setRunning(false);
  setCentralWidget(central);
  setWindowTitle(QStringLiteral("专业拧瓶盖员"));
  setWindowIcon(QApplication::windowIcon());
  resize(860, 720);
  appendStatus(QStringLiteral("就绪。配置目录：%1")
                   .arg(QString::fromStdString(controller_->storage().data_dir())));
  QTimer::singleShot(0, this, &MainWindow::onAutoFill);
}

void MainWindow::resetStreamPanel() {
  if (!stat_rate_) return;
  stat_rate_->setText(QStringLiteral("—"));
  stat_fps_->setText(QStringLiteral("—"));
  stat_frames_->setText(QStringLiteral("—"));
  stat_idr_->setText(QStringLiteral("—"));
  stat_idr_bytes_->setText(QStringLiteral("—"));
  stat_size_->setText(QStringLiteral("—"));
  stat_codec_->setText(QStringLiteral("—"));
  stat_link_->setText(QStringLiteral("未连接"));
}

bool MainWindow::takeStreamStat(const QString& text) {
  const QRegularExpression rate(
      QStringLiteral("视频码率\\s+([0-9.]+)\\s+Mbps\\s*/\\s*([0-9.]+)\\s+fps\\s*/\\s*帧(\\d+)\\s*/\\s*IDR(\\d+)"));
  if (const auto match = rate.match(text); match.hasMatch()) {
    stat_rate_->setText(QStringLiteral("%1 Mbps").arg(match.captured(1)));
    stat_fps_->setText(QStringLiteral("%1 fps").arg(match.captured(2)));
    stat_frames_->setText(match.captured(3));
    stat_idr_->setText(match.captured(4));
    if (video_ && video_->isVisible()) {
      const QString hud = QStringLiteral("%1 Mbps  %2 fps").arg(match.captured(1), match.captured(2));
      QMetaObject::invokeMethod(video_, "setHudText", Qt::QueuedConnection, Q_ARG(QString, hud));
    }
    return true;
  }
  const QRegularExpression idr(QStringLiteral("^关键帧\\s+(\\d+)B"));
  if (const auto match = idr.match(text); match.hasMatch()) {
    stat_idr_bytes_->setText(QStringLiteral("%1 B").arg(match.captured(1)));
    return true;
  }
  const QRegularExpression cfg(QStringLiteral("^视频配置\\s+(\\S+)\\s+(\\d+)B"));
  if (const auto match = cfg.match(text); match.hasMatch()) {
    stat_codec_->setText(QStringLiteral("%1  %2 B").arg(match.captured(1), match.captured(2)));
    return true;
  }
  if (text.startsWith(QStringLiteral("视频TCP通道已连接"))) {
    stat_link_->setText(QStringLiteral("视频通道已连接"));
    return true;
  }
  if (text.startsWith(QStringLiteral("视频通道结束"))) {
    stat_link_->setText(text);
    return true;
  }
  return false;
}

void MainWindow::appendStatus(const QString& text) {
  if (takeStreamStat(text)) {
    state_->setText(QStringLiteral("状态：%1")
                        .arg(QString::fromUtf8(carplay::session_state_name(controller_->state()))));
    return;
  }
  log_->appendPlainText(text);
  state_->setText(QStringLiteral("状态：%1 / %2")
                      .arg(QString::fromUtf8(carplay::session_state_name(controller_->state())))
                      .arg(text));
}

carplay::RuntimeConfig MainWindow::readForm() const {
  carplay::RuntimeConfig cfg = controller_->config();
  cfg.wireless_mode =
      mode_->currentData().toString() == QLatin1String("lan") ? carplay::WirelessMode::Lan : carplay::WirelessMode::Hotspot;
  cfg.network_interface = iface_->currentText().trimmed().toStdString();
  cfg.ssid = ssid_->text().trimmed().toStdString();
  cfg.password = password_->text().toStdString();
  cfg.channel = channel_->value();
  if (target_bt_->currentData().isValid()) {
    cfg.target_bluetooth = target_bt_->currentData().toString().toUpper().toStdString();
  } else {
    cfg.target_bluetooth = target_bt_->currentText().trimmed().toUpper().toStdString();
    const QRegularExpression mac(QStringLiteral("([0-9A-F]{2}(?::[0-9A-F]{2}){5})"),
                                 QRegularExpression::CaseInsensitiveOption);
    const auto match = mac.match(QString::fromStdString(cfg.target_bluetooth));
    if (match.hasMatch()) cfg.target_bluetooth = match.captured(1).toUpper().toStdString();
  }
  cfg.adapter_bluetooth = adapter_bt_->text().trimmed().toUpper().toStdString();
  cfg.access_point_mac = ap_mac_->text().trimmed().toUpper().toStdString();
  cfg.width = width_->value();
  cfg.height = height_->value();
  cfg.physical_width_mm = phys_w_->value();
  cfg.physical_height_mm = phys_h_->value();
  cfg.max_bit_rate = max_bitrate_->value() <= 0 ? 0 : max_bitrate_->value() * 1000000;
  cfg.fps = fps_->currentData().toInt();
  const QString typed_iface = iface_->currentText().trimmed().section(QLatin1Char(' '), 0, 0);
  if (typed_iface == QLatin1String("carplay0")) cfg.network_interface = "carplay0";
  else if (iface_->currentData().isValid()) cfg.network_interface = iface_->currentData().toString().toStdString();
  return cfg;
}

void MainWindow::writeForm(const carplay::RuntimeConfig& config) {
  mode_->setCurrentIndex(config.wireless_mode == carplay::WirelessMode::Lan ? 1 : 0);
  if (!config.network_interface.empty()) {
    if (iface_->findText(QString::fromStdString(config.network_interface)) < 0) {
      iface_->addItem(QString::fromStdString(config.network_interface));
    }
    iface_->setCurrentText(QString::fromStdString(config.network_interface));
  }
  ssid_->setText(QString::fromStdString(config.ssid));
  password_->setText(QString::fromStdString(config.password));
  channel_->setValue(config.channel);
  if (!config.target_bluetooth.empty()) {
    if (target_bt_->findText(QString::fromStdString(config.target_bluetooth), Qt::MatchContains) < 0) {
      target_bt_->addItem(QString::fromStdString(config.target_bluetooth));
    }
    target_bt_->setCurrentText(QString::fromStdString(config.target_bluetooth));
  }
  adapter_bt_->setText(QString::fromStdString(config.adapter_bluetooth));
  ap_mac_->setText(QString::fromStdString(config.access_point_mac));
  {
    const QSignalBlocker block_w(width_);
    const QSignalBlocker block_h(height_);
    width_->setValue(config.width);
    height_->setValue(config.height);
    if (phys_w_ && config.physical_width_mm > 0) phys_w_->setValue(config.physical_width_mm);
    if (phys_h_ && config.physical_height_mm > 0) phys_h_->setValue(config.physical_height_mm);
    if (max_bitrate_) max_bitrate_->setValue(config.max_bit_rate > 0 ? config.max_bit_rate / 1000000 : 0);
  }
  syncResolutionPreset();
  updateSizeEditorsEnabled();
  const int fps_index = fps_->findData(config.fps);
  fps_->setCurrentIndex(fps_index >= 0 ? fps_index : 0);
}

void MainWindow::syncResolutionPreset() {
  const QSize current(width_->value(), height_->value());
  int match = -1;
  for (int i = 0; i < resolution_->count(); ++i) {
    const QSize size = resolution_->itemData(i).toSize();
    if (size.width() > 0 && size == current) {
      match = i;
      break;
    }
  }
  const QSignalBlocker block(resolution_);
  if (match >= 0) resolution_->setCurrentIndex(match);
  else resolution_->setCurrentIndex(resolution_->count() - 1);  // 自定义
}

void MainWindow::updateSizeEditorsEnabled() {
  const bool running = !start_->isEnabled();
  const QSize size = resolution_->currentData().toSize();
  const bool custom = size.width() <= 0;
  resolution_->setEnabled(!running);
  width_->setEnabled(!running && custom);
  height_->setEnabled(!running && custom);
  phys_w_->setEnabled(!running);
  phys_h_->setEnabled(!running);
  if (max_bitrate_) max_bitrate_->setEnabled(!running);
}

void MainWindow::onResolutionPreset(int) {
  const QSize size = resolution_->currentData().toSize();
  if (size.width() > 0 && size.height() > 0) {
    const QSignalBlocker block_w(width_);
    const QSignalBlocker block_h(height_);
    width_->setValue(size.width());
    height_->setValue(size.height());
  }
  updateSizeEditorsEnabled();
}

void MainWindow::onCustomSizeEdited() {
  // Manual edits switch the combo to「自定义」unless they still match a preset.
  syncResolutionPreset();
  updateSizeEditorsEnabled();
}

void MainWindow::setRunning(bool running) {
  start_->setEnabled(!running);
  stop_->setEnabled(running);
  show_video_->setEnabled(running);
  mode_->setEnabled(!running);
  iface_->setEnabled(!running);
  ssid_->setEnabled(!running);
  password_->setEnabled(!running);
  channel_->setEnabled(!running);
  target_bt_->setEnabled(!running);
  adapter_bt_->setEnabled(!running);
  ap_mac_->setEnabled(!running && mode_->currentData().toString() == QLatin1String("lan"));
  fps_->setEnabled(!running);
  phys_w_->setEnabled(!running);
  phys_h_->setEnabled(!running);
  if (max_bitrate_) max_bitrate_->setEnabled(!running);
  if (clarity_) clarity_->setEnabled(!running);
  read_net_->setEnabled(!running);
  read_bt_->setEnabled(!running);
  read_screen_->setEnabled(!running);
  import_auth_->setEnabled(!running);
  updateSizeEditorsEnabled();
}

void MainWindow::onModeChanged(int) {
  const bool lan = mode_->currentData().toString() == QLatin1String("lan");
  channel_->setMinimum(lan ? 1 : 0);
  ap_mac_->setEnabled(lan && start_->isEnabled());
}

void MainWindow::applyNetwork(const carplay::CurrentNetworkInfo& info) {
  if (!info.ok) {
    appendStatus(QString::fromStdString(info.error.empty() ? "读取网络失败" : info.error));
    return;
  }
  const QString previous_ssid = ssid_->text();
  const QString previous_password = password_->text();
  ssid_->setText(QString::fromStdString(info.ssid));
  if (info.password_read) {
    password_->setText(QString::fromStdString(info.password));
  } else if (previous_ssid == QString::fromStdString(info.ssid) && !previous_password.isEmpty()) {
    // Keep a previously saved password for the same SSID when nmcli secrets are locked.
  } else if (previous_ssid != QString::fromStdString(info.ssid)) {
    // Different network and no readable secret — leave blank for the user.
    password_->clear();
  }
  if (info.channel >= 0) channel_->setValue(info.channel);
  if (!info.interface_name.empty()) {
    if (iface_->findText(QString::fromStdString(info.interface_name)) < 0) {
      iface_->addItem(QString::fromStdString(info.interface_name));
    }
    iface_->setCurrentText(QString::fromStdString(info.interface_name));
  }
  if (!info.bssid.empty()) ap_mac_->setText(QString::fromStdString(info.bssid));
  QString summary = QStringLiteral("已读取网络：%1 / 信道 %2 / %3")
                        .arg(QString::fromStdString(info.ssid))
                        .arg(info.channel)
                        .arg(QString::fromStdString(info.interface_name));
  if (!info.bssid.empty()) summary += QStringLiteral(" / %1").arg(QString::fromStdString(info.bssid));
  appendStatus(summary);
  if (!info.warning.empty()) appendStatus(QString::fromStdString(info.warning));
}

void MainWindow::applyBluetooth(const carplay::BluetoothInventory& inventory) {
  if (!inventory.ok) {
    appendStatus(QString::fromStdString(inventory.error.empty() ? "读取蓝牙失败" : inventory.error));
    return;
  }
  if (!inventory.adapter.empty()) adapter_bt_->setText(QString::fromStdString(inventory.adapter));
  const QString previous = target_bt_->currentText();
  target_bt_->clear();
  for (const auto& device : inventory.devices) {
    target_bt_->addItem(QStringLiteral("%1 (%2)")
                            .arg(QString::fromStdString(device.name))
                            .arg(QString::fromStdString(device.address)),
                        QString::fromStdString(device.address));
  }
  if (!previous.isEmpty()) {
    const int index = target_bt_->findText(previous, Qt::MatchContains);
    if (index >= 0) target_bt_->setCurrentIndex(index);
    else target_bt_->setCurrentText(previous);
  } else if (target_bt_->count() == 1) {
    target_bt_->setCurrentIndex(0);
  }
  appendStatus(QStringLiteral("已读取蓝牙：适配器 %1，已配对 %2 台")
                   .arg(QString::fromStdString(inventory.adapter))
                   .arg(inventory.devices.size()));
}

void MainWindow::onAutoFill() {
  iface_->clear();
  for (const auto& item : carplay::list_network_interfaces()) {
    const QString label = item.ipv4.empty()
                              ? QString::fromStdString(item.name)
                              : QStringLiteral("%1 (%2)").arg(QString::fromStdString(item.name),
                                                              QString::fromStdString(item.ipv4));
    iface_->addItem(label, QString::fromStdString(item.name));
  }
  // Prefer bare interface name for saved settings.
  const auto cfg = controller_->config();
  if (!cfg.network_interface.empty()) {
    for (int i = 0; i < iface_->count(); ++i) {
      if (iface_->itemData(i).toString() == QString::fromStdString(cfg.network_interface)) {
        iface_->setCurrentIndex(i);
        break;
      }
    }
  }
  onReadNetwork();
  onReadBluetooth();
  if (cfg.physical_width_mm <= 0 || cfg.physical_height_mm <= 0 || cfg.width <= 0 || cfg.height <= 0) {
    applyPrimaryScreen(false);
    applyClarityPhysical(true);
  }
}

bool MainWindow::applyPrimaryScreen(bool announce) {
  QScreen* screen = QGuiApplication::primaryScreen();
  if (!screen) return false;
  const QSize px = screen->size();
  const QSizeF mm = screen->physicalSize();
  const int w = px.width() & ~1;
  const int h = px.height() & ~1;
  const int pw = std::max(1, static_cast<int>(std::lround(mm.width())));
  const int ph = std::max(1, static_cast<int>(std::lround(mm.height())));
  if (w < 320 || h < 200) return false;
  {
    const QSignalBlocker block_w(width_);
    const QSignalBlocker block_h(height_);
    width_->setValue(w);
    height_->setValue(h);
  }
  phys_w_->setValue(pw);
  phys_h_->setValue(ph);
  syncResolutionPreset();
  if (announce) {
    appendStatus(QStringLiteral("已读取屏幕：%1×%2 像素，%3×%4 mm")
                     .arg(w)
                     .arg(h)
                     .arg(pw)
                     .arg(ph));
  }
  return true;
}

void MainWindow::onReadScreen() {
  if (!applyPrimaryScreen(true)) {
    appendStatus(QStringLiteral("读取屏幕失败"));
    return;
  }
  onSave();
}

void MainWindow::applyClarityPhysical(bool announce) {
  const int w = width_->value();
  const int h = height_->value();
  if (w < 320 || h < 200) return;
  // ~240 PPI: enough density for anti-aliased @2x assets without oversize encoding.
  // Real ~15" 1080p (~143 PPI) makes icons large and stair-stepped after H.264.
  constexpr int kAaPpi = 240;
  const int pw = std::max(1, (w * 254 + kAaPpi * 5) / (kAaPpi * 10));
  const int ph = std::max(1, (h * 254 + kAaPpi * 5) / (kAaPpi * 10));
  phys_w_->setValue(pw);
  phys_h_->setValue(ph);
  if (announce) {
    appendStatus(QStringLiteral("抗锯齿：分辨率 %1×%2，上报 %3×%4 mm（约 %5 PPI），码率 %6 Mbps")
                     .arg(w)
                     .arg(h)
                     .arg(pw)
                     .arg(ph)
                     .arg(kAaPpi)
                     .arg(max_bitrate_ && max_bitrate_->value() > 0 ? max_bitrate_->value() : 0));
  }
}

void MainWindow::onClarityPriority() {
  applyClarityPhysical(true);
  onSave();
}

void MainWindow::onReadNetwork() {
  const auto mode =
      mode_->currentData().toString() == QLatin1String("lan") ? carplay::WirelessMode::Lan : carplay::WirelessMode::Hotspot;
  std::string preferred;
  if (iface_->currentData().isValid()) preferred = iface_->currentData().toString().toStdString();
  else preferred = iface_->currentText().section(' ', 0, 0).toStdString();
  applyNetwork(carplay::read_current_network(mode, preferred));
}

void MainWindow::onReadBluetooth() { applyBluetooth(carplay::list_bluetooth_inventory()); }

void MainWindow::onImportAuth() {
  const QStringList files = QFileDialog::getOpenFileNames(
      this, QStringLiteral("选择 identity.pk8 与 certificate.p7b"), QString(),
      QStringLiteral("认证文件 (*.pk8 *.p7b);;全部 (*)"));
  QString pk8;
  QString p7b;
  for (const QString& path : files) {
    if (path.endsWith(QLatin1String(".pk8"), Qt::CaseInsensitive)) pk8 = path;
    if (path.endsWith(QLatin1String(".p7b"), Qt::CaseInsensitive)) p7b = path;
  }
  if (pk8.isEmpty() || p7b.isEmpty()) {
    QMessageBox::warning(this, QStringLiteral("导入失败"),
                         QStringLiteral("请同时选择一个 identity.pk8 和一个 certificate.p7b"));
    return;
  }
  const std::string dir = controller_->storage().data_dir() + "/authentication";
  std::string error;
  if (!carplay::FileMfiSigner::import_files(dir, pk8.toStdString(), p7b.toStdString(), error)) {
    QMessageBox::warning(this, QStringLiteral("导入失败"), QString::fromStdString(error));
    return;
  }
  auto cfg = readForm();
  cfg.mfi_dir = dir;
  controller_->set_config(cfg);
  carplay::save_runtime_config(controller_->storage(), cfg);
  appendStatus(QStringLiteral("认证文件已导入到 %1").arg(QString::fromStdString(dir)));
}

void MainWindow::onSave() {
  auto cfg = readForm();
  controller_->set_config(cfg);
  if (!carplay::save_runtime_config(controller_->storage(), cfg)) {
    QMessageBox::warning(this, QStringLiteral("保存失败"), QStringLiteral("无法写入设置文件"));
    return;
  }
  appendStatus(QStringLiteral("设置已保存"));
}

void MainWindow::onDoctor() {
  const auto report = controller_->doctor();
  appendStatus(QStringLiteral("自检：%1").arg(QString::fromStdString(report.detail)));
  if (!report.ok) QMessageBox::warning(this, QStringLiteral("自检失败"), QString::fromStdString(report.detail));
}

void MainWindow::ensureVideoWindow() {
  if (!video_) {
    video_ = new VideoSurface(nullptr);
    video_->setAttribute(Qt::WA_DeleteOnClose, false);
    video_->setTouchHandler([this](float x, float y, bool down) { controller_->send_touch(x, y, down); });
    connect(video_, &VideoSurface::hostUiRequested, this, &MainWindow::onHostUiRequested);
    connect(video_, &VideoSurface::firstFramePresented, this, [this] {
      appendStatus(QStringLiteral("已出画，进入全屏（车辆键 / Esc 返回本界面）"));
    });
    connect(video_, &VideoSurface::streamSizeChanged, this, [this](QSize size) {
      if (stat_size_) stat_size_->setText(QStringLiteral("%1×%2").arg(size.width()).arg(size.height()));
    });
  }
}

void MainWindow::onShowVideo() {
  if (!stop_->isEnabled()) return;
  ensureVideoWindow();
  if (video_) video_->enterFullscreen();
  appendStatus(QStringLiteral("已回到全屏画面"));
}

void MainWindow::onHostUiRequested() {
  if (video_) video_->leaveFullscreen();
  show();
  raise();
  activateWindow();
  appendStatus(QStringLiteral("已返回系统主界面（车辆键或 iPhone 退出 CarPlay）"));
}

void MainWindow::fitStreamToPanel() {
  QScreen* screen = QGuiApplication::primaryScreen();
  if (!screen) return;
  const qreal dpr = screen->devicePixelRatio() > 0 ? screen->devicePixelRatio() : 1.0;
  const int sw = (static_cast<int>(std::lround(screen->size().width() * dpr)) & ~1);
  const int sh = (static_cast<int>(std::lround(screen->size().height() * dpr)) & ~1);
  if (sw < 320 || sh < 200) return;
  const int old_w = width_->value();
  const int old_h = height_->value();
  {
    const QSignalBlocker block_w(width_);
    const QSignalBlocker block_h(height_);
    width_->setValue(sw);
    height_->setValue(sh);
  }
  // Keep stream 1:1 with the panel; report ~240 PPI so CarPlay anti-aliases icons.
  applyClarityPhysical(false);
  if (fps_->findData(60) >= 0) fps_->setCurrentIndex(fps_->findData(60));
  syncResolutionPreset();
  if (old_w != sw || old_h != sh) {
    appendStatus(QStringLiteral("已对齐屏幕像素 %1×%2（原请求 %3×%4），上报 %5×%6 mm 以减轻图标锯齿")
                     .arg(sw)
                     .arg(sh)
                     .arg(old_w)
                     .arg(old_h)
                     .arg(phys_w_->value())
                     .arg(phys_h_->value()));
  } else {
    appendStatus(QStringLiteral("抗锯齿上报 %1×%2 mm（约 240 PPI），像素仍为 %3×%4")
                     .arg(phys_w_->value())
                     .arg(phys_h_->value())
                     .arg(sw)
                     .arg(sh));
  }
}

void MainWindow::adoptCarplayAp(const QString& detail) {
  const QRegularExpression iface_re(QStringLiteral("iface=(\\S+)"));
  const QRegularExpression channel_re(QStringLiteral("channel=(\\d+)"));
  const QRegularExpression mac_re(QStringLiteral("mac=([0-9A-Fa-f:]{17})"));
  if (const auto match = iface_re.match(detail); match.hasMatch()) iface_->setEditText(match.captured(1));
  else iface_->setEditText(QStringLiteral("carplay0"));
  if (const auto match = channel_re.match(detail); match.hasMatch()) channel_->setValue(match.captured(1).toInt());
  if (const auto match = mac_re.match(detail); match.hasMatch()) ap_mac_->setText(match.captured(1).toUpper());
}

bool MainWindow::ensureHotspot(QString* detail) {
  const bool lan = mode_->currentData().toString() == QLatin1String("lan");
  const bool hotspot = mode_->currentData().toString() == QLatin1String("hotspot");
  if (!lan && !hotspot) return true;
  if (const auto running = carplay::query_carplay_hotspot(); running.running) {
    const QString text = QString::fromStdString(running.detail);
    if (detail) *detail = text;
    if (lan) adoptCarplayAp(text);
    return true;
  }
  carplay::HotspotRequest req;
  req.keep_station = lan;
  if (iface_->currentData().isValid()) req.iface = iface_->currentData().toString().toStdString();
  else req.iface = iface_->currentText().section(' ', 0, 0).toStdString();
  if (req.iface.empty() || req.iface == "carplay0") req.iface = "wlo1";
  req.ssid = ssid_->text().trimmed().toStdString();
  req.password = password_->text().toStdString();
  if (lan) {
    req.channel = 0;
  } else {
    int channel = channel_->value();
    if (channel != 1 && channel != 6 && channel != 11) channel = 6;
    channel_->setValue(channel);
    req.channel = channel;
  }
  req.bt_mac = adapter_bt_->text().trimmed().toStdString();
  std::string error;
  if (!carplay::start_carplay_hotspot(req, error)) {
    if (detail) *detail = QString::fromUtf8(error.c_str());
    return false;
  }
  const QString text = QString::fromUtf8(error.c_str());
  if (detail) *detail = text;
  if (lan) adoptCarplayAp(text);
  return true;
}

void MainWindow::onStartHotspot() {
  QString detail;
  if (!ensureHotspot(&detail)) {
    appendStatus(QStringLiteral("热点启动失败：%1").arg(detail));
    QMessageBox::warning(this, QStringLiteral("热点"), detail);
    return;
  }
  const bool lan = mode_->currentData().toString() == QLatin1String("lan");
  appendStatus(lan ? QStringLiteral("已在当前 Wi-Fi 同一信道加上 CarPlay 热点（电脑仍连着路由器）。请让 iPhone 改连这个热点后再启动接收。\n%1")
                        .arg(detail)
                   : QStringLiteral("热点已开启（2.4GHz，带 Apple CarPlay 信标）。请让 iPhone 连接这个热点后再启动接收。\n%1")
                         .arg(detail));
  onSave();
}

void MainWindow::onStopHotspot() {
  std::string error;
  if (!carplay::stop_carplay_hotspot(error)) {
    appendStatus(QStringLiteral("关闭热点失败：%1").arg(QString::fromUtf8(error.c_str())));
    return;
  }
  appendStatus(QStringLiteral("热点已关闭，Wi-Fi 交回系统"));
}

void MainWindow::onStart() {
  fitStreamToPanel();
  resetStreamPanel();
  if (mode_->currentData().toString() == QLatin1String("hotspot")) {
    QString detail;
    if (!ensureHotspot(&detail)) {
      appendStatus(QStringLiteral("热点启动失败：%1").arg(detail));
      QMessageBox::warning(this, QStringLiteral("热点"), detail);
      return;
    }
    if (!detail.isEmpty()) appendStatus(detail);
  }
  onSave();
  appendStatus(QStringLiteral("正在启动接收…"));
  ensureVideoWindow();
  if (video_) video_->prepareForSession();
  show();
  raise();
  activateWindow();
  if (!controller_->start()) {
    appendStatus(QString::fromStdString(controller_->last_error()));
    QMessageBox::warning(this, QStringLiteral("启动失败"), QString::fromStdString(controller_->last_error()));
    if (video_) video_->leaveFullscreen();
    setRunning(false);
    return;
  }
  setRunning(true);
  appendStatus(QStringLiteral("接收已启动，请看本窗口日志；iPhone 出画后会自动全屏"));
}

void MainWindow::onStop() {
  controller_->stop();
  resetStreamPanel();
  if (video_) video_->leaveFullscreen();
  setRunning(false);
  show();
  raise();
  activateWindow();
  appendStatus(QStringLiteral("接收已停止"));
}
