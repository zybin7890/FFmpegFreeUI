// SPDX-License-Identifier: AGPL-3.0-only
#include "mainwindow.h"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QIcon>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableWidget>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>
#include <QUuid>
#include <algorithm>
#include <functional>

namespace fui {
namespace {
QPushButton *button(const QString &text, QBoxLayout *layout, const std::function<void()> &action, bool primary = false) {
    auto *b = new QPushButton(text); if (primary) b->setObjectName("primary"); layout->addWidget(b);
    QObject::connect(b, &QPushButton::clicked, b, action); return b;
}
QString sizeText(qint64 size) { return QString::number(double(size) / 1048576, 'f', 1) + " MB"; }
QString durationText(double value) { if (value <= 0) return QStringLiteral("—"); int s = int(value); return QString("%1:%2:%3").arg(s / 3600, 2, 10, QChar('0')).arg((s / 60) % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0')); }
QJsonObject initialPreset() {
    auto p = defaults(); p[QStringLiteral("输出容器")] = ".mp4"; p[QStringLiteral("视频参数_编码器_类型")] = 1;
    p[QStringLiteral("视频参数_编码器_具体编码")] = "libx264"; p[QStringLiteral("视频参数_编码器_编码预设")] = "medium";
    p[QStringLiteral("视频参数_比特率_控制方式")] = 1; p[QStringLiteral("视频参数_质量控制_值")] = "23";
    p[QStringLiteral("音频参数_编码器_代号")] = "aac.native"; p[QStringLiteral("音频参数_比特率")] = "192k"; return p;
}
}
MainWindow::MainWindow(bool isolated) : settings_(QSettings::IniFormat, QSettings::UserScope, "FFmpegFreeUI-Native", "FFmpegFreeUI"), isolated_(isolated) {
    setWindowTitle(QStringLiteral("FFmpegFreeUI Native · C++")); setWindowIcon(QIcon(":/fui/icon.png"));
    resize(1280, 820); setMinimumSize(960, 640); setAcceptDrops(true);
    auto *root = new QWidget; setCentralWidget(root); auto *layout = new QHBoxLayout(root); layout->setSpacing(0); layout->setContentsMargins(0, 0, 0, 0);
    auto *sidebar = new QWidget; sidebar->setObjectName("sidebar"); sidebar->setFixedWidth(224); auto *side = new QVBoxLayout(sidebar); side->setContentsMargins(16, 24, 16, 16);
    auto *brand = new QLabel("FFmpegFreeUI"); brand->setObjectName("brand"); side->addWidget(brand);
    auto *subtitle = new QLabel(QStringLiteral("NATIVE  /  原生 C++")); subtitle->setObjectName("muted"); side->addWidget(subtitle); side->addSpacing(25);
    navigation_ = new QListWidget; navigation_->setObjectName("navigation");
    navigation_->addItems({QStringLiteral("编码队列"), QStringLiteral("参数与预设"), QStringLiteral("媒体信息 / 播放"), QStringLiteral("混流 / 合并 / 评测"), QStringLiteral("软件设置"), QStringLiteral("关于与许可")}); side->addWidget(navigation_);
    platform_ = new QLabel(QStringLiteral("显示后端：") + QGuiApplication::platformName()); platform_->setWordWrap(true); platform_->setObjectName("muted"); side->addWidget(platform_);
    layout->addWidget(sidebar);
    auto *content = new QWidget; auto *main = new QVBoxLayout(content); main->setContentsMargins(24, 22, 24, 16); main->setSpacing(18);
    auto *head = new QHBoxLayout; title_ = new QLabel; title_->setObjectName("pageTitle"); head->addWidget(title_); head->addStretch(); summary_ = new QLabel; summary_->setObjectName("muted"); head->addWidget(summary_); main->addLayout(head);
    pages_ = new QStackedWidget; pages_->addWidget(queuePage()); pages_->addWidget(presetPage()); pages_->addWidget(mediaPage()); pages_->addWidget(toolsPage()); pages_->addWidget(settingsPage()); pages_->addWidget(aboutPage()); main->addWidget(pages_); layout->addWidget(content, 1);
    connect(navigation_, &QListWidget::currentRowChanged, this, [this](int i) { pages_->setCurrentIndex(i); title_->setText(navigation_->item(i)->text()); }); navigation_->setCurrentRow(0);
    connect(&queue, &JobQueue::error, this, &MainWindow::showError);
    connect(&queue, &JobQueue::changed, this, [this] { if (!persist_.isActive() && !isolated_) persist_.start(500); });
    connect(&persist_, &QTimer::timeout, this, [this] { try { queue.save(cachePath_); } catch (const std::exception &e) { statusBar()->showMessage(QString::fromUtf8(e.what())); } }); persist_.setSingleShot(true);
    refresh_.setInterval(250); connect(&refresh_, &QTimer::timeout, this, &MainWindow::refreshQueue); refresh_.start();
    previewTimer_.setSingleShot(true); connect(&previewTimer_, &QTimer::timeout, this, &MainWindow::updatePreview);
    dataDir_ = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation); cachePath_ = QDir(dataDir_).filePath("queue.json");
    if (!isolated_) {
        QDir().mkpath(dataDir_); ffmpegPath_->setText(settings_.value("ffmpeg").toString()); ffprobePath_->setText(settings_.value("ffprobe").toString()); ffplayPath_->setText(settings_.value("ffplay").toString());
        outputDir_->setText(settings_.value("output").toString()); concurrency_->setValue(settings_.value("concurrency", 1).toInt());
        try { queue.restore(cachePath_); } catch (const std::exception &e) { statusBar()->showMessage(QString::fromUtf8(e.what())); }
    }
    setPreset(initialPreset());
    if (!isolated_ && QFileInfo::exists(QDir(dataDir_).filePath("current.3fui"))) try { setPreset(loadPreset(QDir(dataDir_).filePath("current.3fui"))); } catch (const std::exception &e) { statusBar()->showMessage(QString::fromUtf8(e.what())); }
    refreshQueue(); statusBar()->showMessage(QStringLiteral("拖入媒体文件，设置参数，然后加入队列"));
}
QWidget *MainWindow::queuePage() {
    auto *page = new QWidget; auto *layout = new QVBoxLayout(page); layout->setContentsMargins(0, 0, 0, 0);
    auto *toolbar = new QHBoxLayout;
    button(QStringLiteral("开始队列"), toolbar, [this] { try { queue.ffmpeg = ffmpegPath_->text(); queue.ffprobe = ffprobePath_->text(); queue.concurrency = concurrency_->value(); queue.start(); } catch (const std::exception &e) { showError(QString::fromUtf8(e.what())); } }, true);
    button(QStringLiteral("暂停"), toolbar, [this] { queue.pause(selectedId()); }); button(QStringLiteral("恢复"), toolbar, [this] { queue.resume(selectedId()); });
    button(QStringLiteral("停止"), toolbar, [this] { queue.cancel(selectedId()); }); button(QStringLiteral("重置"), toolbar, [this] { queue.reset(selectedId()); });
    button(QStringLiteral("移除"), toolbar, [this] { queue.remove(selectedId()); }); button(QStringLiteral("↑"), toolbar, [this] { queue.move(selectedId(), -1); })->setAccessibleName(QStringLiteral("任务上移"));
    button(QStringLiteral("↓"), toolbar, [this] { queue.move(selectedId(), 1); })->setAccessibleName(QStringLiteral("任务下移")); toolbar->addStretch(); layout->addLayout(toolbar);
    auto *split = new QSplitter(Qt::Vertical); table_ = new QTableWidget(0, 7); table_->setHorizontalHeaderLabels({QStringLiteral("任务名称"), QStringLiteral("状态"), QStringLiteral("进度"), QStringLiteral("速度"), QStringLiteral("输出大小"), QStringLiteral("剩余时间"), QStringLiteral("输出文件")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows); table_->setSelectionMode(QAbstractItemView::SingleSelection); table_->setEditTriggers(QAbstractItemView::NoEditTriggers); table_->setAlternatingRowColors(true); table_->verticalHeader()->hide();
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents); table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch); table_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch); split->addWidget(table_);
    log_ = new QPlainTextEdit; log_->setReadOnly(true); log_->setPlaceholderText(QStringLiteral("选中任务查看 FFmpeg 命令、实时输出和错误日志")); log_->setMaximumBlockCount(1600); split->addWidget(log_); split->setStretchFactor(0, 3); split->setStretchFactor(1, 1); layout->addWidget(split, 1);
    auto *prepare = new QGroupBox(QStringLiteral("准备文件")); auto *prep = new QVBoxLayout(prepare); auto *actions = new QHBoxLayout;
    button(QStringLiteral("添加文件"), actions, [this] { addInputs(QFileDialog::getOpenFileNames(this, QStringLiteral("选择媒体文件"))); });
    button(QStringLiteral("移除选中"), actions, [this] { for (auto *item : inputs_->selectedItems()) delete inputs_->takeItem(inputs_->row(item)); updatePreview(); });
    button(QStringLiteral("清空输入"), actions, [this] { inputs_->clear(); updatePreview(); }); actions->addStretch();
    button(QStringLiteral("加入队列"), actions, [this] { enqueue(); }, true); prep->addLayout(actions);
    inputs_ = new QListWidget; inputs_->setSelectionMode(QAbstractItemView::ExtendedSelection); inputs_->setMaximumHeight(95); inputs_->setMinimumHeight(65); prep->addWidget(inputs_);
    auto *output = new QHBoxLayout; output->addWidget(new QLabel(QStringLiteral("输出目录"))); outputDir_ = new QLineEdit; outputDir_->setPlaceholderText(QStringLiteral("留空则使用预设目录或源文件目录；自动避免重名")); output->addWidget(outputDir_);
    button(QStringLiteral("浏览…"), output, [this] { auto p = QFileDialog::getExistingDirectory(this, QStringLiteral("选择输出目录")); if (!p.isEmpty()) outputDir_->setText(p); }); prep->addLayout(output); layout->addWidget(prepare);
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) { if (row < queue.jobs.size()) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(queue.jobs[row]->output).absolutePath())); });
    return page;
}
QWidget *MainWindow::presetPage() {
    auto *page = new QWidget; auto *layout = new QVBoxLayout(page); layout->setContentsMargins(0, 0, 0, 0); auto *bar = new QHBoxLayout;
    builtins_ = new QComboBox; builtins_->addItem(QStringLiteral("通用 H.264 / AAC"), initialPreset());
    for (const auto &v : builtins()) { auto item = v.toObject(); builtins_->addItem(item.value("name").toString(), item.value("preset").toObject()); }
    bar->addWidget(builtins_, 1); button(QStringLiteral("应用预设"), bar, [this] { setPreset(builtins_->currentData().toJsonObject()); });
    button(QStringLiteral("导入 .3fui"), bar, [this] { auto path = QFileDialog::getOpenFileName(this, QStringLiteral("导入预设"), {}, "3FUI (*.3fui *.json)"); if (path.isEmpty()) return; try { setPreset(loadPreset(path)); } catch (const std::exception &e) { showError(QString::fromUtf8(e.what())); } });
    button(QStringLiteral("保存预设"), bar, [this] { auto path = QFileDialog::getSaveFileName(this, QStringLiteral("保存预设"), {}, "3FUI (*.3fui)"); if (path.isEmpty()) return; if (!path.endsWith(".3fui")) path += ".3fui"; try { saveJson(path, currentPreset()); } catch (const std::exception &e) { showError(QString::fromUtf8(e.what())); } }); layout->addLayout(bar);
    auto *split = new QSplitter; auto *categories = new QListWidget; categories->setMinimumWidth(150); categories->setMaximumWidth(195); auto *stack = new QStackedWidget; split->addWidget(categories); split->addWidget(stack);
    QMap<QString, QFormLayout *> forms; QMap<QString, QWidget *> groupWidgets;
    for (const auto &v : schema()) {
        const auto def = v.toObject(); auto group = def.value("group").toString(), key = def.value("key").toString(), type = def.value("type").toString();
        if (!forms.contains(group)) {
            categories->addItem(group); auto *content = new QWidget; auto *form = new QFormLayout(content); form->setRowWrapPolicy(QFormLayout::WrapLongRows); form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); form->setSpacing(13);
            auto *area = new QScrollArea; area->setWidgetResizable(true); area->setWidget(content); stack->addWidget(area); forms[group] = form; groupWidgets[group] = content;
        }
        QWidget *editor; auto label = key; label.remove("视频参数_"); label.remove("音频参数_");
        if (type == "bool") { auto *e = new QCheckBox; editor = e; connect(e, &QCheckBox::toggled, this, [this] { previewTimer_.start(250); }); }
        else if (type == "enum") { auto *e = new QComboBox; e->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); e->setMinimumContentsLength(12);
            for (const auto &choice : def.value("choices").toArray()) { auto c = choice.toObject(); e->addItem(c.value("label").toString(), c.value("value").toInt()); }
            editor = e; connect(e, &QComboBox::currentIndexChanged, this, [this] { previewTimer_.start(250); }); }
        else if (type == "json") { auto *e = new QPlainTextEdit; e->setMaximumHeight(100); e->setPlaceholderText(QStringLiteral("JSON 数组或对象；保留原版字段名")); editor = e; connect(e, &QPlainTextEdit::textChanged, this, [this] { previewTimer_.start(250); }); }
        else { auto *e = new QLineEdit; editor = e; connect(e, &QLineEdit::textChanged, this, [this] { previewTimer_.start(250); }); }
        editor->setToolTip(key); editor->setAccessibleName(label); fields_[key] = editor; types_[key] = type; auto *name = new QLabel(label.replace('_', " / ")); name->setWordWrap(true); name->setMinimumWidth(180); name->setMaximumWidth(260); forms[group]->addRow(name, editor);
    }
    connect(categories, &QListWidget::currentRowChanged, stack, &QStackedWidget::setCurrentIndex); categories->setCurrentRow(0); layout->addWidget(split, 1);
    auto *caption = new QLabel(QStringLiteral("命令预览 · 实际媒体时长在启动任务时读取；修改参数仅影响新加入的任务")); caption->setObjectName("muted"); layout->addWidget(caption);
    preview_ = new QPlainTextEdit; preview_->setReadOnly(true); preview_->setMaximumHeight(150); layout->addWidget(preview_); return page;
}
QWidget *MainWindow::mediaPage() {
    auto *page = new QWidget; auto *layout = new QVBoxLayout(page); layout->setContentsMargins(0, 0, 0, 0); auto *bar = new QHBoxLayout;
    button(QStringLiteral("选择文件并探测"), bar, [this] { probe(); }, true); button(QStringLiteral("使用 ffplay 播放"), bar, [this] { play(); }); bar->addStretch(); layout->addLayout(bar);
    probeOutput_ = new QPlainTextEdit; probeOutput_->setReadOnly(true); probeOutput_->setPlaceholderText(QStringLiteral("ffprobe 将显示容器、视频、音频、字幕和章节的完整 JSON 信息")); layout->addWidget(probeOutput_, 1); return page;
}
QWidget *MainWindow::toolsPage() {
    auto *page = new QWidget; auto *layout = new QVBoxLayout(page); layout->setContentsMargins(0, 0, 0, 0);
    for (const auto &v : QList<QPair<QString, QString>>{{QStringLiteral("简易混流"), QStringLiteral("将多个媒体、音频或字幕文件映射到同一个容器，使用流复制。")},
        {QStringLiteral("无损合并"), QStringLiteral("使用 concat demuxer 按选择顺序连接文件。源文件须具有兼容的编码参数。")},
        {QStringLiteral("质量评测"), QStringLiteral("对比参考视频和压制视频，计算 SSIM 与 PSNR。结果保存在任务日志。")}}) {
        auto *group = new QGroupBox(v.first); auto *box = new QVBoxLayout(group); auto *label = new QLabel(v.second); label->setWordWrap(true); box->addWidget(label); auto *row = new QHBoxLayout;
        button(QStringLiteral("选择文件…"), row, [this, name = v.first] { if (name == QStringLiteral("简易混流")) mux(); else if (name == QStringLiteral("无损合并")) concatenate(); else assess(); }); row->addStretch(); box->addLayout(row); layout->addWidget(group);
    } layout->addStretch(); return page;
}
QWidget *MainWindow::settingsPage() {
    auto *page = new QWidget; auto *layout = new QVBoxLayout(page); layout->setContentsMargins(0, 0, 0, 0); auto *form = new QFormLayout;
    auto path = [&](const QString &label, QLineEdit **target) { auto *row = new QWidget; auto *box = new QHBoxLayout(row); box->setContentsMargins(0, 0, 0, 0); auto *edit = new QLineEdit; edit->setPlaceholderText(QStringLiteral("留空使用 PATH 或程序目录")); *target = edit; box->addWidget(edit); button(QStringLiteral("浏览…"), box, [this, edit] { auto p = QFileDialog::getOpenFileName(this, QStringLiteral("选择可执行程序")); if (!p.isEmpty()) edit->setText(p); }); form->addRow(label, row); };
    path("FFmpeg", &ffmpegPath_); path("FFprobe", &ffprobePath_); path("FFplay", &ffplayPath_);
    concurrency_ = new QSpinBox; concurrency_->setRange(1, 8); form->addRow(QStringLiteral("并行任务数"), concurrency_); layout->addLayout(form);
    auto *note = new QLabel(QStringLiteral("Linux 默认优先原生 Wayland，X11 会话使用 XCB。可通过 FUI_DISPLAY_BACKEND=wayland 或 x11 强制选择，也可使用 Qt 的 -platform 参数。\n\n预设、队列和设置遵循系统用户数据目录，不需要管理员权限。每个任务使用独立临时目录。")); note->setWordWrap(true); note->setObjectName("muted"); layout->addWidget(note); layout->addStretch(); return page;
}
QWidget *MainWindow::aboutPage() {
    auto *view = new QTextBrowser; view->setOpenExternalLinks(true); view->setHtml(QStringLiteral(
        "<h2>FFmpegFreeUI Native 0.1.0</h2><p>原生 C++23 / Qt 6 改写版，支持 Windows 与 Linux。</p>"
        "<h3>原出处</h3><p>原项目：<a href='https://github.com/Lake1059/FFmpegFreeUI'>Lake1059/FFmpegFreeUI</a><br>"
        "原作者：Lake1059 / 1059 Studio<br>基于版本 6.2.36，提交 65aec1ff4dcd62a54c4361fe1719520378750c36。</p>"
        "<h3>许可</h3><p>新 C++ 代码采用 <a href='https://www.gnu.org/licenses/agpl-3.0.html'>AGPL-3.0-only</a>。"
        "原项目的预设模型、内置预设及图标保留 MIT 版权和许可声明。Qt 动态链接，遵循 LGPL-3.0；FFmpeg 为系统外部程序。</p>"
        "<h3>迁移状态</h3><p>已实现参数编辑、v6 预设读写、常用滤镜、批量队列、暂停/恢复、日志、媒体探测、播放、混流、合并和质量评测。"
        "原版 .NET 插件、Agent、专用 NV_FRUC/VapourSynth 管线、复杂按流保留与封面映射尚未移植。"
        "此版不宣称与原版全部功能等价；以上未移植的编码管线和流处理设置会明确报错。</p>")); return view;
}
QJsonObject MainWindow::currentPreset() const {
    auto p = basePreset_;
    for (auto it = fields_.begin(); it != fields_.end(); ++it) {
        auto type = types_[it.key()]; auto *w = it.value();
        if (type == "bool") p[it.key()] = qobject_cast<QCheckBox *>(w)->isChecked();
        else if (type == "enum") p[it.key()] = qobject_cast<QComboBox *>(w)->currentData().toInt();
        else if (type == "json") { auto text = qobject_cast<QPlainTextEdit *>(w)->toPlainText(); QJsonParseError e; auto d = QJsonDocument::fromJson(text.toUtf8(), &e);
            if (e.error || d.isNull()) throw Error(it.key() + QStringLiteral("：JSON 格式错误"));
            p[it.key()] = d.isArray() ? QJsonValue(d.array()) : QJsonValue(d.object()); }
        else if (type == "number") { bool ok; auto number = qobject_cast<QLineEdit *>(w)->text().toDouble(&ok); if (!ok) throw Error(it.key() + QStringLiteral("：请输入数值")); p[it.key()] = number; }
        else p[it.key()] = qobject_cast<QLineEdit *>(w)->text();
    } return p;
}
void MainWindow::setPreset(const QJsonObject &preset) {
    basePreset_ = defaults(); for (auto it = preset.begin(); it != preset.end(); ++it) basePreset_[it.key()] = it.value();
    for (auto it = fields_.begin(); it != fields_.end(); ++it) {
        auto type = types_[it.key()]; const auto v = basePreset_.value(it.key()); auto *w = it.value(); QSignalBlocker blocker(w);
        if (type == "bool") qobject_cast<QCheckBox *>(w)->setChecked(v.toBool());
        else if (type == "enum") { auto *combo = qobject_cast<QComboBox *>(w); int i = combo->findData(v.toInt()); if (i < 0) { combo->addItem(QStringLiteral("未知值 %1").arg(v.toInt()), v.toInt()); i = combo->count() - 1; } combo->setCurrentIndex(i); }
        else if (type == "json") qobject_cast<QPlainTextEdit *>(w)->setPlainText(QString::fromUtf8(v.isArray() ? QJsonDocument(v.toArray()).toJson(QJsonDocument::Indented) : QJsonDocument(v.toObject()).toJson(QJsonDocument::Indented)));
        else qobject_cast<QLineEdit *>(w)->setText(v.isDouble() ? QString::number(v.toDouble()) : v.toString());
    } updatePreview();
}
void MainWindow::updatePreview() {
    if (!preview_) return;
    try { const auto p = currentPreset(); QString in = inputs_->count() ? inputs_->item(0)->text() : "<InputFile>";
        auto plan = compilePreset(p, in, "<OutputFile>", ffmpegPath_->text().isEmpty() ? "ffmpeg" : ffmpegPath_->text(), 120, "<PassLog>");
        QStringList lines; for (const auto &c : plan.steps) lines << displayCommand(c); preview_->setPlainText(lines.join("\n\n"));
    } catch (const std::exception &e) { preview_->setPlainText(QString::fromUtf8(e.what())); }
}
void MainWindow::addInputs(const QStringList &paths) {
    for (const auto &path : paths) if (QFileInfo(path).isFile()) { const auto absolute = QFileInfo(path).absoluteFilePath(); if (inputs_->findItems(absolute, Qt::MatchExactly).isEmpty()) inputs_->addItem(absolute); }
    updatePreview();
}
void MainWindow::enqueue() {
    try { auto preset = currentPreset(); if (!inputs_->count()) throw Error(QStringLiteral("请先添加媒体文件"));
        QStringList reserved; for (const auto &job : queue.jobs) reserved << job->output;
        // Compile all inputs before mutating the queue so an invalid preset adds no partial batch.
        QList<QPair<QString, QString>> batch;
        for (int i = 0; i < inputs_->count(); ++i) { auto in = inputs_->item(i)->text(); auto out = outputPath(preset, in, outputDir_->text(), reserved); compilePreset(preset, in, out, "ffmpeg", 120, "preview-pass"); batch << qMakePair(in, out); reserved << out; }
        for (const auto &v : batch) queue.add(v.first, v.second, preset);
        statusBar()->showMessage(QStringLiteral("已加入 %1 个任务").arg(batch.size())); selectPage(0); refreshQueue();
    } catch (const std::exception &e) { showError(QString::fromUtf8(e.what())); }
}
QString MainWindow::selectedId() const { auto *item = table_->item(table_->currentRow(), 0); return item ? item->data(Qt::UserRole).toString() : QString(); }
QString MainWindow::selectedInput() const { if (inputs_->currentItem()) return inputs_->currentItem()->text(); if (inputs_->count()) return inputs_->item(0)->text(); return {}; }
void MainWindow::refreshQueue() {
    const auto id = selectedId(); int active = 0, done = 0, errors = 0; table_->setRowCount(queue.jobs.size());
    for (int i = 0; i < queue.jobs.size(); ++i) {
        const auto &job = queue.jobs[i]; active += job->active(); done += job->state == State::Done; errors += job->state == State::Failed;
        auto eta = job->speed > 0 && job->plan.duration > 0 ? (job->plan.duration - job->mediaSeconds) / job->speed : 0;
        const QStringList values{job->name, stateName(job->state), QString::number(job->percent, 'f', 1) + '%', job->speed > 0 ? QString::number(job->speed, 'f', 2) + 'x' : QStringLiteral("—"), sizeText(job->size), durationText(eta), QFileInfo(job->output).fileName()};
        for (int col = 0; col < values.size(); ++col) { auto *item = table_->item(i, col); if (!item) { item = new QTableWidgetItem; table_->setItem(i, col, item); } item->setText(values[col]); item->setToolTip(col == 6 ? job->output : values[col]); item->setData(Qt::UserRole, job->id); }
        if (job->id == id) table_->selectRow(i);
    }
    summary_->setText(QStringLiteral("总数 %1   运行 %2   完成 %3   失败 %4").arg(queue.jobs.size()).arg(active).arg(done).arg(errors));
    auto job = queue.find(selectedId()); const auto text = job ? job->log : QString();
    if (log_->toPlainText() != text) { log_->setPlainText(text); log_->verticalScrollBar()->setValue(log_->verticalScrollBar()->maximum()); }
}
void MainWindow::showError(const QString &message) { QMessageBox::warning(this, QStringLiteral("FFmpegFreeUI"), message); }
void MainWindow::selectPage(int page) { navigation_->setCurrentRow(page); }
void MainWindow::probe() {
    auto input = QFileDialog::getOpenFileName(this, QStringLiteral("选择探测的媒体"), selectedInput()); if (input.isEmpty()) return;
    try { auto *process = new QProcess(this); probeOutput_->setPlainText(QStringLiteral("读取中…"));
        connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus) { probeOutput_->setPlainText(QString::fromUtf8(code ? process->readAllStandardError() : process->readAllStandardOutput())); process->deleteLater(); });
        connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError e) { if (e == QProcess::FailedToStart) { probeOutput_->setPlainText(process->errorString()); process->deleteLater(); } });
        process->start(resolveTool("ffprobe", ffprobePath_->text()), {"-v", "error", "-show_format", "-show_streams", "-show_chapters", "-of", "json", input});
        QTimer::singleShot(15000, process, [process] { if (process->state() != QProcess::NotRunning) process->kill(); });
    } catch (const std::exception &e) { showError(QString::fromUtf8(e.what())); }
}
void MainWindow::play() {
    auto input = QFileDialog::getOpenFileName(this, QStringLiteral("选择播放的媒体"), selectedInput()); if (input.isEmpty()) return;
    try { if (!QProcess::startDetached(resolveTool("ffplay", ffplayPath_->text()), {"-autoexit", input})) showError(QStringLiteral("FFplay 启动失败")); }
    catch (const std::exception &e) { showError(QString::fromUtf8(e.what())); }
}
void MainWindow::addToolJob(const QString &name, const QStringList &inputs, const QString &output, const QStringList &args) {
    try { if (inputs.isEmpty()) return; if (QFileInfo::exists(output)) throw Error(QStringLiteral("输出已经存在，未覆盖"));
        if (!QDir().mkpath(QFileInfo(output).absolutePath())) throw Error(QStringLiteral("无法创建输出目录"));
        for (const auto &in : inputs) if (QFileInfo(in).absoluteFilePath() == QFileInfo(output).absoluteFilePath()) throw Error(QStringLiteral("输出不能覆盖输入"));
        auto tool = resolveTool("ffmpeg", ffmpegPath_->text()); QStringList a{"-n", "-hide_banner", "-nostdin", "-progress", "pipe:1", "-nostats"}; a += args;
        queue.addPlan(name, inputs[0], output, Plan{{Command{tool, a, name}}, 0}); selectPage(0); refreshQueue();
    } catch (const std::exception &e) { showError(QString::fromUtf8(e.what())); }
}
void MainWindow::mux() {
    const auto files = QFileDialog::getOpenFileNames(this, QStringLiteral("选择混流输入（包括音频、字幕）")); if (files.isEmpty()) return;
    const auto output = QFileDialog::getSaveFileName(this, QStringLiteral("保存混流文件"), {}, "Matroska (*.mkv);;MP4 (*.mp4)"); if (output.isEmpty()) return;
    QStringList args; for (const auto &file : files) args << "-i" << file; for (int i = 0; i < files.size(); ++i) args << "-map" << QString::number(i); args << "-c" << "copy";
    if (output.endsWith(".mp4", Qt::CaseInsensitive)) args << "-c:s" << "mov_text";
    args << output; addToolJob(QStringLiteral("简易混流"), files, output, args);
}
void MainWindow::concatenate() {
    const auto files = QFileDialog::getOpenFileNames(this, QStringLiteral("选择合并输入（按选择顺序）")); if (files.size() < 2) return;
    auto output = QFileDialog::getSaveFileName(this, QStringLiteral("保存合并文件"), {}, "Matroska (*.mkv);;MP4 (*.mp4)"); if (output.isEmpty()) return;
    try { QDir().mkpath(dataDir_); auto list = QDir(dataDir_).filePath("concat-" + QUuid::createUuid().toString(QUuid::Id128) + ".txt");
        QFile file(list); if (!file.open(QIODevice::WriteOnly)) throw Error(file.errorString()); QByteArray data;
        for (auto path : files) { path = QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath()); path.replace('\'', "'\\''"); data += ("file '" + path + "'\n").toUtf8(); } if (file.write(data) != data.size()) throw Error(file.errorString()); file.close();
        addToolJob(QStringLiteral("无损合并"), files, output, {"-f", "concat", "-safe", "0", "-i", list, "-map", "0", "-c", "copy", output});
    } catch (const std::exception &e) { showError(QString::fromUtf8(e.what())); }
}
void MainWindow::assess() {
    auto reference = QFileDialog::getOpenFileName(this, QStringLiteral("选择参考视频")); if (reference.isEmpty()) return;
    auto encoded = QFileDialog::getOpenFileName(this, QStringLiteral("选择压制视频")); if (encoded.isEmpty()) return;
    auto out = QDir(dataDir_).filePath("quality-" + QUuid::createUuid().toString(QUuid::Id128) + ".log");
    QStringList args{"-i", reference, "-i", encoded, "-lavfi", "[0:v]split=2[r1][r2];[1:v]split=2[e1][e2];[r1][e1]ssim[s];[r2][e2]psnr[p]", "-map", "[s]", "-map", "[p]", "-an", "-f", "null"};
    args << "-";
    addToolJob(QStringLiteral("SSIM / PSNR"), {reference, encoded}, out, args);
}
void MainWindow::capture(const QString &path, int page, const QString &report) {
    selectPage(page); QTimer::singleShot(1200, this, [this, path, report] {
        const bool saved = grab().save(path); if (!report.isEmpty()) try { saveJson(report, {{"platform", QGuiApplication::platformName()}, {"visible", isVisible()}, {"imageSaved", saved}, {"width", width()}, {"height", height()}, {"fields", fields_.size()}}); } catch (...) {}
        QApplication::exit(saved ? 0 : 1);
    });
}
void MainWindow::closeEvent(QCloseEvent *event) {
    if (queue.active() && QMessageBox::question(this, QStringLiteral("退出"), QStringLiteral("还有任务正在运行。停止任务并退出？")) != QMessageBox::Yes) { event->ignore(); return; }
    queue.stop(); if (!isolated_) {
        settings_.setValue("ffmpeg", ffmpegPath_->text()); settings_.setValue("ffprobe", ffprobePath_->text()); settings_.setValue("ffplay", ffplayPath_->text()); settings_.setValue("output", outputDir_->text()); settings_.setValue("concurrency", concurrency_->value());
        try { saveJson(QDir(dataDir_).filePath("current.3fui"), currentPreset()); queue.save(cachePath_); } catch (const std::exception &e) { showError(QString::fromUtf8(e.what())); event->ignore(); return; }
    } event->accept();
}
void MainWindow::dragEnterEvent(QDragEnterEvent *event) { if (event->mimeData()->hasUrls()) event->acceptProposedAction(); }
void MainWindow::dropEvent(QDropEvent *event) { QStringList paths; for (const auto &url : event->mimeData()->urls()) if (url.isLocalFile()) paths << url.toLocalFile(); addInputs(paths); selectPage(0); event->acceptProposedAction(); }
}
