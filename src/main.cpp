// SPDX-License-Identifier: AGPL-3.0-only
#include "mainwindow.h"
#include "platform.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QJsonDocument>
#include <QStyleFactory>
#include <QTextStream>
#include <QTimer>
#include <cstring>
#include <algorithm>
#include <memory>

int main(int argc, char **argv) {
    bool cli = false;
    for (int i = 1; i < argc; ++i) if (!std::strcmp(argv[i], "--transcode") || !std::strcmp(argv[i], "--probe") || !std::strcmp(argv[i], "--print-command") || !std::strcmp(argv[i], "--version") || !std::strcmp(argv[i], "--help")) cli = true;
    fui::selectDisplayBackend(argc, argv);
    std::unique_ptr<QCoreApplication> app;
    if (cli) app = std::make_unique<QCoreApplication>(argc, argv); else app = std::make_unique<QApplication>(argc, argv);
    QCoreApplication::setApplicationName("ffmpegfreeui-native"); QCoreApplication::setOrganizationName("FFmpegFreeUI-Native"); QCoreApplication::setApplicationVersion("0.1.0");
    if (!cli) QGuiApplication::setDesktopFileName("ffmpegfreeui-native");
    QCommandLineParser parser; parser.setApplicationDescription(QStringLiteral("FFmpegFreeUI 原生 C++ 版；原出处 Lake1059/FFmpegFreeUI；AGPL-3.0-only")); parser.addHelpOption(); parser.addVersionOption();
    parser.addOptions({{{"i", "input"}, QStringLiteral("输入媒体"), "file"}, {{"o", "output"}, QStringLiteral("输出文件"), "file"},
        {"preset", QStringLiteral("v6 .3fui 预设"), "file"}, {"ffmpeg", QStringLiteral("FFmpeg 程序"), "path"}, {"ffprobe", QStringLiteral("FFprobe 程序"), "path"},
        {"transcode", QStringLiteral("无界面执行转码")}, {"probe", QStringLiteral("无界面探测媒体")}, {"print-command", QStringLiteral("打印命令而不执行")},
        {"render", QStringLiteral("渲染截图后退出（不读写用户设置）"), "png"}, {"capture-page", QStringLiteral("截图页面：0 队列，1 参数，5 关于"), "index", "0"},
        {"report", QStringLiteral("保存显示后端验证报告"), "json"}});
    parser.addPositionalArgument("files", QStringLiteral("在图形界面中添加媒体文件"), "[files...]"); parser.process(*app);
    try {
        if (cli) {
            auto input = parser.value("input"), output = parser.value("output");
            if (input.isEmpty()) throw fui::Error(QStringLiteral("请使用 --input 指定输入媒体"));
            if (parser.isSet("probe")) {
                QProcess p; p.start(fui::resolveTool("ffprobe", parser.value("ffprobe")), {"-v", "error", "-show_format", "-show_streams", "-show_chapters", "-of", "json", input});
                if (!p.waitForFinished(15000)) { p.kill(); p.waitForFinished(); throw fui::Error(QStringLiteral("FFprobe 超时")); }
                QTextStream(stdout) << p.readAllStandardOutput(); QTextStream(stderr) << p.readAllStandardError(); return p.exitCode();
            }
            auto preset = parser.isSet("preset") ? fui::loadPreset(parser.value("preset")) : fui::defaults();
            if (!parser.isSet("preset")) { preset[QStringLiteral("视频参数_编码器_具体编码")] = "libx264"; preset[QStringLiteral("视频参数_比特率_控制方式")] = 1; preset[QStringLiteral("视频参数_质量控制_值")] = "23"; preset[QStringLiteral("音频参数_编码器_代号")] = "aac.native"; }
            if (output.isEmpty()) output = fui::outputPath(preset, input, {});
            if (parser.isSet("print-command")) {
                QTemporaryDir temp; const auto plan = fui::compilePreset(preset, input, output, fui::resolveTool("ffmpeg", parser.value("ffmpeg")), fui::probeDuration(input, fui::resolveTool("ffprobe", parser.value("ffprobe"))), temp.filePath("pass"));
                for (const auto &command : plan.steps) QTextStream(stdout) << fui::displayCommand(command) << '\n';
                return 0;
            }
            fui::JobQueue queue; queue.ffmpeg = parser.value("ffmpeg"); queue.ffprobe = parser.value("ffprobe"); const auto job = queue.add(input, output, preset);
            QObject::connect(&queue, &fui::JobQueue::idle, app.get(), [&] { QTextStream(job->state == fui::State::Done ? stdout : stderr) << job->log; app->exit(job->state == fui::State::Done ? 0 : 1); });
            QTimer::singleShot(0, &queue, &fui::JobQueue::start); return app->exec();
        }
        auto *gui = static_cast<QApplication *>(app.get()); gui->setStyle(QStyleFactory::create("Fusion"));
        QPalette palette; palette.setColor(QPalette::Window, QColor("#171c24")); palette.setColor(QPalette::WindowText, QColor("#e1e7ef")); palette.setColor(QPalette::Base, QColor("#11161e"));
        palette.setColor(QPalette::AlternateBase, QColor("#1a202b")); palette.setColor(QPalette::Text, QColor("#e1e7ef")); palette.setColor(QPalette::Button, QColor("#242d3a")); palette.setColor(QPalette::ButtonText, QColor("#e1e7ef"));
        palette.setColor(QPalette::Highlight, QColor("#327ac4")); palette.setColor(QPalette::HighlightedText, Qt::white); palette.setColor(QPalette::PlaceholderText, QColor("#91a2b8")); palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#778596")); gui->setPalette(palette);
        gui->setStyleSheet("QWidget {font-size: 14px;} QWidget#sidebar {background:#11161e;} QLabel#brand {font-size:22px; font-weight:600; color:#71b9ff;} QLabel#muted {color:#91a2b8;} QLabel#pageTitle {font-size:25px; font-weight:600;} QListWidget#navigation {border:0; background:transparent;} QListWidget#navigation::item {padding:13px 8px; border-radius:4px;} QListWidget#navigation::item:selected {background:#25354b; color:#83c3ff;} QPushButton {padding:7px 12px; border:1px solid #3a4659; border-radius:4px;} QPushButton:hover {border-color:#71b9ff;} QPushButton#primary {background:#2e77bd; border-color:#2e77bd; color:white;} QLineEdit,QComboBox,QSpinBox {padding:6px;} QGroupBox {font-weight:600; border:1px solid #354051; border-radius:4px; margin-top:12px; padding:16px 8px 8px;} QGroupBox::title {subcontrol-origin:margin; left:12px; padding:0 5px;} QHeaderView::section {background:#242d3a; padding:8px; border:0; border-bottom:1px solid #3a4659;} QPlainTextEdit {font-family:monospace; font-size:13px;} QScrollArea {border:0;}");
        fui::MainWindow window(parser.isSet("render")); window.addInputs(parser.positionalArguments()); window.show();
        if (parser.isSet("render")) window.capture(parser.value("render"), std::clamp(parser.value("capture-page").toInt(), 0, 5), parser.value("report"));
        return app->exec();
    } catch (const std::exception &e) { QTextStream(stderr) << QString::fromUtf8(e.what()) << '\n'; return 1; }
}
