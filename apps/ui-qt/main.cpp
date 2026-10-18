#include "mainwindow.hpp"

#include <csignal>

#include <QApplication>
#include <QCoreApplication>
#include <QFont>
#include <QIcon>

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("专业拧瓶盖员"));
  QApplication::setOrganizationName(QStringLiteral("carplay-cpp"));
  const QString icon_path = QCoreApplication::applicationDirPath() + QStringLiteral("/icon.png");
  const QIcon icon(icon_path);
  if (!icon.isNull()) {
    QApplication::setWindowIcon(icon);
    app.setWindowIcon(icon);
  }
  QFont font = app.font();
  if (font.pointSize() < 11) font.setPointSize(11);
  app.setFont(font);
  MainWindow window;
  window.show();
  return app.exec();
}
