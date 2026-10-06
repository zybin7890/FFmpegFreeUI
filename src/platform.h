// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QString>
#include <QtGlobal>
namespace fui {
bool suspendProcess(qint64 pid, bool suspend, QString *error = nullptr);
void selectDisplayBackend(int argc, char **argv);
}
