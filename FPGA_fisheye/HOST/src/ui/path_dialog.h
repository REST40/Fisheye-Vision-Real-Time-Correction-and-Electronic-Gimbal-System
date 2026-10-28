#pragma once
#include "core/types.h"
#include <QDir>
#include <QFileDialog>
#include <QMessageBox>
#include <QUrl>

namespace df {
inline QString selectProjectDirectory(QWidget *parent, const QString &title,
                                      const QString &initial = {}) {
    const auto root = projectRoot();
    QFileDialog dialog(parent, title, initial.isEmpty() ? root : initial);
    dialog.setFileMode(QFileDialog::Directory);
    dialog.setOption(QFileDialog::ShowDirsOnly);
    if (!qEnvironmentVariableIsEmpty("DF_HOST_PERSISTENT_ROOT"))
        dialog.setSidebarUrls({QUrl::fromLocalFile(root)});
    while (dialog.exec() == QDialog::Accepted) {
        const auto files = dialog.selectedFiles();
        if (files.isEmpty())
            return {};
        const auto path = resolveProjectPath(files.first(), root);
        if (isPersistentPath(path))
            return path;
        QMessageBox::warning(parent, "目录未映射到本机",
                             "当前启动方式仅开放项目目录，请选择该目录下的位置：\n" + root);
        dialog.setDirectory(root);
    }
    return {};
}
} // namespace df
