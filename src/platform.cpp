// SPDX-License-Identifier: AGPL-3.0-only
#include "platform.h"
#include <QByteArray>
#include <QList>
#include <cstring>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#else
#include <cerrno>
#include <csignal>
#endif

namespace fui {
bool suspendProcess(qint64 pid, bool suspend, QString *error) {
    if (pid <= 0) { if (error) *error = QStringLiteral("任务进程尚未启动"); return false; }
#ifdef Q_OS_WIN
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
    QList<HANDLE> changed;
    bool ok = true;
    if (Thread32First(snapshot, &entry)) do {
        if (entry.th32OwnerProcessID != static_cast<DWORD>(pid)) continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
        if (!thread) { ok = false; break; }
        DWORD result = suspend ? SuspendThread(thread) : ResumeThread(thread);
        if (result == static_cast<DWORD>(-1)) { CloseHandle(thread); ok = false; break; }
        changed.push_back(thread);
    } while (Thread32Next(snapshot, &entry));
    CloseHandle(snapshot);
    if (!ok && suspend) for (HANDLE thread : changed) ResumeThread(thread);
    for (HANDLE thread : changed) CloseHandle(thread);
    ok = ok && !changed.isEmpty();
    if (!ok && error) *error = QStringLiteral("无法暂停或恢复任务线程");
    return ok;
#else
    const bool ok = ::kill(static_cast<pid_t>(pid), suspend ? SIGSTOP : SIGCONT) == 0;
    if (!ok && error) *error = QString::fromLocal8Bit(std::strerror(errno));
    return ok;
#endif
}
void selectDisplayBackend(int argc, char **argv) {
#ifdef Q_OS_LINUX
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-platform") || !std::strcmp(argv[i], "--platform") ||
            !std::strncmp(argv[i], "-platform=", 10)) return;
    }
    if (qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) return;
    const auto requested = qgetenv("FUI_DISPLAY_BACKEND");
    if (requested == "x11") qputenv("QT_QPA_PLATFORM", "xcb");
    else if (requested == "wayland") qputenv("QT_QPA_PLATFORM", "wayland");
    else if (!qgetenv("WAYLAND_DISPLAY").isEmpty() || qgetenv("XDG_SESSION_TYPE") == "wayland")
        qputenv("QT_QPA_PLATFORM", "wayland;xcb");
    else if (!qgetenv("DISPLAY").isEmpty()) qputenv("QT_QPA_PLATFORM", "xcb");
#else
    Q_UNUSED(argc); Q_UNUSED(argv);
#endif
}
}
