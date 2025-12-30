//------------------------------------------------------------------------------
//
//  Intan Technologies RHX Data Acquisition Software
//  Version 3.4.0
//
//  Copyright (c) 2020-2025 Intan Technologies
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

static void signalHandler(int sig) {
    qDebug() << "[SIGNAL] Received signal:" << sig << "- initiating graceful shutdown";
    g_isShuttingDown = true;

    // 触发 Qt 事件循环退出
    if (QCoreApplication* app = QCoreApplication::instance()) {
        qDebug() << "[SIGNAL] Calling application quit";
        app->quit();
    } else {
        qDebug() << "[SIGNAL] No application instance found, calling exit";
        std::exit(0);
    }
}

static void setupSignalHandlers() {
    qDebug() << "[DEBUG MAIN] Setting up signal handlers for graceful shutdown";

    // 这些信号在 Windows/macOS/Linux 上都可用（或基本可用）
    std::signal(SIGTERM, signalHandler);  // 终止信号
    std::signal(SIGINT,  signalHandler);  // Ctrl+C / 中断

#ifdef _WIN32
    // Windows: 通常没有 SIGHUP/SIGQUIT；SIGKILL 也不存在且不可捕获
    #ifdef SIGBREAK
    std::signal(SIGBREAK, signalHandler); // Ctrl+Break（不是所有环境都有）
    #endif
#else
    // POSIX (macOS/Linux)
    std::signal(SIGHUP,  signalHandler);  // 终端挂起/断开
    std::signal(SIGQUIT, signalHandler);  // 退出（可能触发 core dump）
#endif

    // 注意：SIGKILL 无法被捕获/处理，任何平台都不应注册 handler
    // std::signal(SIGKILL, signalHandler); // <-- 不要这样做

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
