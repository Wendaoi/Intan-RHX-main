//------------------------------------------------------------------------------
//
//  Intan Technologies RHX Data Acquisition Software
//  Version 3.4.0
//
//  Copyright (c) 2020-2025 Intan Technologies
//
//  This file is part of the Intan Technologies RHX Data Acquisition Software.
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published
//  by the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//  This software is provided 'as-is', without any express or implied warranty.
//  In no event will the authors be held liable for any damages arising from
//  the use of this software.
//
//  See <http://www.intantech.com> for documentation and product information.
//
//------------------------------------------------------------------------------

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QCoreApplication>
#include <csignal>
#ifdef __APPLE__
#include <QStyleFactory>
#endif
#include "boardselectdialog.h"

// 全局信号处理变量
static volatile bool g_isShuttingDown = false;

void signalHandler(int sig) {
    qDebug() << "[SIGNAL] Received signal:" << sig << "- initiating graceful shutdown";
    g_isShuttingDown = true;

    // 强制退出当前应用程序
    QCoreApplication* app = QCoreApplication::instance();
    if (app) {
        qDebug() << "[SIGNAL] Calling application quit";
        app->quit();
    } else {
        qDebug() << "[SIGNAL] No application instance found, calling exit";
        exit(0);
    }
}

void setupSignalHandlers() {
    qDebug() << "[DEBUG MAIN] Setting up signal handlers for graceful shutdown";

    // 为各种终止信号注册处理函数
    std::signal(SIGTERM, signalHandler);  // 终止信号
    std::signal(SIGINT, signalHandler);   // 中断信号 (Ctrl+C)
    std::signal(SIGHUP, signalHandler);   // 挂起信号
    std::signal(SIGQUIT, signalHandler);  // 退出信号
    std::signal(SIGKILL, signalHandler);  // 杀死信号 (通常不可接受)

    qDebug() << "[DEBUG MAIN] Signal handlers set up successfully";
}

int main(int argc, char *argv[])
{
    qDebug() << "[DEBUG MAIN] === Program Entry Point ===\n";
    qDebug() << "[DEBUG MAIN] Creating QApplication with argc:" << argc;
    
    try {
        QApplication app(argc, argv);
        qDebug() << "[DEBUG MAIN] QApplication created successfully";
        
        qDebug() << "[DEBUG MAIN] Intan RHX application starting...";
        qDebug() << "[DEBUG MAIN] Qt version:" << qVersion();
        qDebug() << "[DEBUG MAIN] Arguments:" << app.arguments();
        qDebug() << "[DEBUG MAIN] Working directory:" << QDir::currentPath();
        qDebug() << "[DEBUG MAIN] Application directory:" << QCoreApplication::applicationDirPath();

#ifdef __APPLE__
        qDebug() << "[DEBUG MAIN] Setting Fusion style for macOS...";
        app.setStyle(QStyleFactory::create("Fusion"));
#endif

        qDebug() << "[DEBUG MAIN] Setting up signal handlers for graceful shutdown...";
        setupSignalHandlers();
        qDebug() << "[DEBUG MAIN] Signal handlers set up successfully";

        qDebug() << "[DEBUG MAIN] About to create BoardSelectDialog...";
        BoardSelectDialog boardSelectDialog;
        qDebug() << "[DEBUG MAIN] BoardSelectDialog created successfully";

        qDebug() << "[DEBUG MAIN] Entering main event loop...";
        int result = app.exec();
        qDebug() << "[DEBUG MAIN] Application exited with code:" << result;
        return result;
        
    } catch (const std::exception& e) {
        qDebug() << "[ERROR MAIN] Standard exception caught:" << e.what();
        return -1;
    } catch (...) {
        qDebug() << "[ERROR MAIN] Unknown exception caught in main()";
        return -2;
    }
}
