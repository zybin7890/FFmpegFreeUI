// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "jobqueue.h"
#include <QMainWindow>
#include <QMap>
#include <QSettings>
#include <QTimer>
class QListWidget;
class QLineEdit;
class QPlainTextEdit;
class QComboBox;
class QSpinBox;
class QTableWidget;
class QStackedWidget;
class QLabel;
class QCloseEvent;
class QDragEnterEvent;
class QDropEvent;

namespace fui {
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(bool isolated = false);
    void addInputs(const QStringList &paths);
    void selectPage(int page);
    void capture(const QString &path, int page, const QString &report);
    JobQueue queue;
protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
private:
    QSettings settings_;
    QString dataDir_, cachePath_;
    bool isolated_;
    QJsonObject basePreset_;
    QMap<QString, QWidget *> fields_;
    QMap<QString, QString> types_;
    QListWidget *inputs_ = nullptr, *navigation_ = nullptr;
    QStackedWidget *pages_ = nullptr;
    QLineEdit *outputDir_ = nullptr, *ffmpegPath_ = nullptr, *ffprobePath_ = nullptr, *ffplayPath_ = nullptr;
    QComboBox *builtins_ = nullptr;
    QSpinBox *concurrency_ = nullptr;
    QTableWidget *table_ = nullptr;
    QPlainTextEdit *log_ = nullptr, *preview_ = nullptr, *probeOutput_ = nullptr;
    QLabel *summary_ = nullptr, *title_ = nullptr, *platform_ = nullptr;
    QTimer refresh_, previewTimer_, persist_;
    QWidget *queuePage();
    QWidget *presetPage();
    QWidget *mediaPage();
    QWidget *toolsPage();
    QWidget *settingsPage();
    QWidget *aboutPage();
    QJsonObject currentPreset() const;
    void setPreset(const QJsonObject &preset);
    void updatePreview();
    void refreshQueue();
    void enqueue();
    QString selectedId() const;
    QString selectedInput() const;
    void showError(const QString &message);
    void probe();
    void play();
    void mux();
    void concatenate();
    void assess();
    void addToolJob(const QString &name, const QStringList &inputs, const QString &output, const QStringList &args);
};
}
