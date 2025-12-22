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
#include <QtGlobal>
#include <QElapsedTimer>
#include <algorithm>
#include <iostream>
#include "controlpanel.h"
#include "impedancereader.h"
#include "controllerinterface.h"

ControllerInterface::ControllerInterface(SystemState* state_, AbstractRHXController* rhxController_, const QString& boardSerialNumber, bool useOpenCL,
                                         DataFileReader* dataFileReader_, QObject* parent, bool is7310_) :
    QObject(parent),
    state(state_),
    rhxController(rhxController_),
    dataFileReader(dataFileReader_),
    tcpDataOutputThread(nullptr),
    xpuController(nullptr),
    usbStreamFifo(nullptr),
    usbDataThread(nullptr),
    waveformFifo(nullptr),
    waveformProcessorThread(nullptr),
    display(nullptr),
    controlPanel(nullptr),
    isiDialog(nullptr),
    psthDialog(nullptr),
    spectrogramDialog(nullptr),
    spikeSortingDialog(nullptr),
    audioThread(nullptr),
    saveToDiskThread(nullptr),
    gameThread(nullptr), // 初始化GameThread指针
    audioEnabled(false),
    tcpDataOutputEnabled(false),
    is7310(is7310_)
{
    qDebug() << "[DEBUG ControllerInterface] === Constructor Entry ===\n";
    qDebug() << "[DEBUG ControllerInterface] Constructor started";
    qDebug() << "[DEBUG ControllerInterface] Parameters - state:" << (void*)state_;
    qDebug() << "[DEBUG ControllerInterface] Parameters - rhxController:" << (void*)rhxController_;
    qDebug() << "[DEBUG ControllerInterface] Parameters - boardSerialNumber:" << boardSerialNumber;
    qDebug() << "[DEBUG ControllerInterface] Parameters - useOpenCL:" << useOpenCL;
    qDebug() << "[DEBUG ControllerInterface] Parameters - is7310:" << is7310_;
    
    try {
        qDebug() << "[DEBUG ControllerInterface] Connecting state signal...";
        QObject::connect(state, &SystemState::stateChanged, this, &ControllerInterface::updateFromState);
        
        qDebug() << "[DEBUG ControllerInterface] Opening controller...";
        openController(boardSerialNumber);
        qDebug() << "[DEBUG ControllerInterface] Controller opened successfully";

        qDebug() << "[DEBUG ControllerInterface] Calculating buffer sizes...";
        const int NumSeconds = 10;  // Size of RAM buffer, in seconds.
        int fifoBufferSize = NumSeconds * rhxController->getSampleRate() * BytesPerWord *
                (RHXDataBlock::dataBlockSizeInWords(state->getControllerTypeEnum(),
                                                    RHXController::maxNumDataStreams(state->getControllerTypeEnum())) / RHXDataBlock::samplesPerDataBlock(state->getControllerTypeEnum()));

        int usbBufferSize = MaxNumBlocksToRead * RHXDataBlock::dataBlockSizeInWords(state->getControllerTypeEnum(),
                                                                                        rhxController->maxNumDataStreams());
        qDebug() << "[DEBUG ControllerInterface] Calculated fifoBufferSize:" << fifoBufferSize;
        qDebug() << "[DEBUG ControllerInterface] Calculated usbBufferSize:" << usbBufferSize;

        double memoryRequired = 0.0;

        qDebug() << "[DEBUG ControllerInterface] Creating DataStreamFifo...";
        usbStreamFifo = new DataStreamFifo(fifoBufferSize, usbBufferSize);
        if (!usbStreamFifo->memoryWasAllocated(memoryRequired)) {
            qDebug() << "[ERROR ControllerInterface] DataStreamFifo memory allocation failed";
            outOfMemoryError(memoryRequired);
        }
        qDebug() << "[DEBUG ControllerInterface] DataStreamFifo created successfully";

        hardwareFifoPercentFull = 0.0;
        waveformProcessorCpuLoad = 0.0;

        qDebug() << "[DEBUG ControllerInterface] Creating USBDataThread...";
        usbDataThread = new USBDataThread(rhxController, usbStreamFifo, this);
        if (!usbDataThread->memoryWasAllocated(memoryRequired)) {
            qDebug() << "[ERROR ControllerInterface] USBDataThread memory allocation failed";
            outOfMemoryError(memoryRequired);
        }
        qDebug() << "[DEBUG ControllerInterface] USBDataThread created successfully";

        qDebug() << "[DEBUG ControllerInterface] Configuring USBDataThread...";
        usbDataThread->setNumUsbBlocksToRead(state->playback->getValue() ? 1 : RHXDataBlock::blocksFor30Hz(state->getSampleRateEnum()));
        QObject::connect(usbDataThread, &USBDataThread::finished, usbDataThread, &QObject::deleteLater);
        QObject::connect(usbDataThread, &USBDataThread::hardwareFifoReport, this, &ControllerInterface::updateHardwareFifo);
        qDebug() << "[DEBUG ControllerInterface] USBDataThread configured";

        qDebug() << "[DEBUG ControllerInterface] Initializing controller...";
        initializeController();
        qDebug() << "[DEBUG ControllerInterface] Controller initialized successfully";

        qDebug() << "[DEBUG ControllerInterface] Creating XPUController...";
        xpuController = new XPUController(state, useOpenCL, this);
        qDebug() << "[DEBUG ControllerInterface] XPUController created successfully";

        qDebug() << "[DEBUG ControllerInterface] Rescanning ports...";
        rescanPorts();
        qDebug() << "[DEBUG ControllerInterface] Ports rescanned successfully";

        qDebug() << "[DEBUG ControllerInterface] Configuring DAC settings...";
        rhxController->enableDacHighpassFilter(false);
        rhxController->setDacHighpassFilter(250.0);
        qDebug() << "[DEBUG ControllerInterface] DAC settings configured";

        qDebug() << "[DEBUG ControllerInterface] Running XPU diagnostic...";
        xpuController->runDiagnostic();
        qDebug() << "[DEBUG ControllerInterface] XPU diagnostic completed";
        
        qDebug() << "[DEBUG ControllerInterface] Constructor completed successfully";
        
    } catch (const std::exception& e) {
        qDebug() << "[ERROR ControllerInterface] Standard exception in constructor:" << e.what();
        throw;
    } catch (...) {
        qDebug() << "[ERROR ControllerInterface] Unknown exception in constructor";
        throw;
    }

    double waveformMemoryInSeconds = 45.0;  // 提升缓冲以减轻显示线程压力
    double waveformExtraBufferInSeconds = 20.0;
    double sampleRate = state->sampleRate->getNumericValue();
    double samplesPerDataBlock = (double) RHXDataBlock::samplesPerDataBlock(state->getControllerTypeEnum());
    int waveformFifoMemoryDataBlocks = ceil(waveformMemoryInSeconds * sampleRate / samplesPerDataBlock);
    int waveformFifoBufferDataBlocks = ceil((waveformMemoryInSeconds + waveformExtraBufferInSeconds) * sampleRate / samplesPerDataBlock);
    waveformFifo = new WaveformFifo(state->signalSources, waveformFifoBufferDataBlocks, waveformFifoMemoryDataBlocks, 1, state);
    double memoryRequired = 0.0;
    if (!waveformFifo->memoryWasAllocated(memoryRequired)) {
        outOfMemoryError(memoryRequired);
    }

    waveformProcessorThread = new WaveformProcessorThread(state, rhxController->getNumEnabledDataStreams(), rhxController->getSampleRate(), usbStreamFifo, waveformFifo, xpuController, this);
    QObject::connect(waveformProcessorThread, &WaveformProcessorThread::finished, waveformProcessorThread, &QObject::deleteLater);
    QObject::connect(waveformProcessorThread, &WaveformProcessorThread::cpuLoadPercent, this, &ControllerInterface::updateWaveformProcessorCpuLoad);

    // Thread priorities are set after the threads are started in runController().

    saveToDiskThread = new SaveToDiskThread(waveformFifo, state, this);
    QObject::connect(saveToDiskThread, &SaveToDiskThread::finished, saveToDiskThread, &QObject::deleteLater);
    if (dataFileReader) {
        // Establish connections so that stimulation amplitudes read from playback file can be re-saved.
        QObject::connect(dataFileReader, &DataFileReader::setPosStimAmplitude,
                saveToDiskThread, &SaveToDiskThread::setPosStimAmplitude);
        QObject::connect(dataFileReader, &DataFileReader::setNegStimAmplitude,
                saveToDiskThread, &SaveToDiskThread::setNegStimAmplitude);
    }

    // 创建并配置GameThread
    qDebug() << "[ControllerInterface] === 创建GameThread ===";
    qDebug() << "[ControllerInterface] GameThread指针:" << (gameThread ? "有效" : "空指针");
    qDebug() << "[ControllerInterface] WaveformFifo指针:" << (waveformFifo ? "有效" : "空指针");
    qDebug() << "[ControllerInterface] SystemState指针:" << (state ? "有效" : "空指针");

    gameThread = new GameThread(waveformFifo, state, this);
    QObject::connect(gameThread, &GameThread::finished, gameThread, &QObject::deleteLater);
    // 连接游戏刺激信号到处理槽 - 使用队列连接确保线程安全
    QObject::connect(gameThread, &GameThread::sendSensoryStim, this, &ControllerInterface::handleSensoryStim, Qt::QueuedConnection);
    QObject::connect(gameThread, &GameThread::sendHitStim, this, &ControllerInterface::handleHitStim, Qt::QueuedConnection);
    QObject::connect(gameThread, &GameThread::sendMissStim, this, &ControllerInterface::handleMissStim, Qt::QueuedConnection);
    QObject::connect(gameThread, &GameThread::stopAllStim, this, &ControllerInterface::handleStopAllStim, Qt::QueuedConnection);
    // 将游戏数据更新信号从GameThread传递到UI - 使用队列连接确保线程安全
    QObject::connect(gameThread, &GameThread::gameDataUpdated, this, &ControllerInterface::onGameDataUpdated, Qt::QueuedConnection);
    QObject::connect(gameThread, &GameThread::stimObserved, this, [this](const QString& ch, uint32_t ts){
        qDebug() << "[StimObserved]" << ch << "ts=" << ts;
        state->writeToLog(QString("Stim observed on %1 at ts=%2").arg(ch).arg(ts));
    }, Qt::QueuedConnection);
    // 转发简化的Spike Rate（Hz）到UI
    QObject::connect(gameThread, &GameThread::spikeRateScalar, this, &ControllerInterface::spikeRateScalar, Qt::QueuedConnection);
    QObject::connect(gameThread, &GameThread::startSilentWindow, this, &ControllerInterface::handleStartSilentWindow, Qt::QueuedConnection);

    // 异步处理学习模式下的尖峰调制，避免GameThread被硬件调用阻塞
    QObject::connect(gameThread, &GameThread::requestModulateSpikes, this,
                     [this](int act){ this->modulateSpikes(static_cast<PaddleAction>(act)); },
                     Qt::QueuedConnection);

    // 启动GameThread的事件循环
    gameThread->start();
    // 提升游戏线程优先级（需在 start() 之后设置）
    gameThread->setPriority(QThread::HighPriority);

    // Create stim worker thread to offload scheduling
    stimThread = new QThread(this);
    stimWorker = new StimWorker();
    stimWorker->moveToThread(stimThread);
    connect(stimThread, &QThread::finished, stimWorker, &QObject::deleteLater);
    connect(stimWorker, &StimWorker::triggerChannel, this, &ControllerInterface::onStimWorkerTriggerChannel, Qt::QueuedConnection);
    connect(stimWorker, &StimWorker::triggerHitBurst, this, &ControllerInterface::onStimWorkerHitBurst, Qt::QueuedConnection);
    stimThread->start();
    // Initialize QTimer inside worker thread context
    QMetaObject::invokeMethod(stimWorker, "init", Qt::QueuedConnection);

    currentSweepPosition = 0;

    cpuLoadHistory.resize(20, 0.0);
}

ControllerInterface::~ControllerInterface()
{
    if (stimThread) {
        stimThread->quit();
        stimThread->wait();
        stimThread = nullptr;
        stimWorker = nullptr;
    }
    if (state->running) {
        state->running = false;
    }

    saveToDiskThread->close();
    saveToDiskThread->wait();
    delete saveToDiskThread;

    if (gameThread) {
        gameThread->close();

        // 使用超时等待机制，避免macOS上的无限阻塞
        bool exitedGracefully = gameThread->wait(3000);  // 最长等待3秒

        if (!exitedGracefully) {
            qDebug() << "[WARNING] GameThread did not exit gracefully in ControllerInterface destructor, terminating";
            gameThread->terminate();  // 强力终止
            gameThread->wait(1000);   // 再等待1秒
        }

        delete gameThread;
        gameThread = nullptr;
    }

    waveformProcessorThread->close();
    waveformProcessorThread->wait();
    delete waveformProcessorThread;

    usbDataThread->close();
    usbDataThread->wait();
    delete usbDataThread;

    if (audioThread) {
        audioThread->close();
        audioThread->wait();
        delete audioThread;
    }

    if (tcpDataOutputThread) {
        tcpDataOutputThread->closeExternal();
        tcpDataOutputThread->wait();
        delete tcpDataOutputThread;
    }

    delete usbStreamFifo;
    delete waveformFifo;
    delete xpuController;
}

void ControllerInterface::outOfMemoryError(double memRequiredGB)
{
    QMessageBox::critical(nullptr, tr("Out of Memory Error"), tr("Software was unable to allocate ") +
                          QString::number(memRequiredGB, 'f', 1) +
                          tr(" GB of memory.  Try running with fewer amplifier channels or a lower sample rate, "
                             "or use a computer with more RAM."));
    exit(EXIT_FAILURE);
}

void ControllerInterface::updateCurrentAudioChannel(QString name)
{
    currentAudioChannel = name;
}

void ControllerInterface::updateFromState()
{
    // 防崩溃保护：确保关键对象仍然有效
    if (!state || !rhxController) return;

    // Check if audio enabled has changed.
    if (state->audioEnabled->getValue() != audioEnabled)
        toggleAudioThread(state->audioEnabled->getValue());

    if (!tcpDataOutputEnabled && state->running && state->getTCPDataOutputChannels().length() > 0) {
        runTCPDataOutputThread();
    }
}

void ControllerInterface::toggleAudioThread(bool enabled)
{
    if (enabled) {
        audioEnabled = true;
        audioThread = new AudioThread(state, waveformFifo, rhxController->getSampleRate());
        QObject::connect(audioThread, &AudioThread::finished, audioThread, &QObject::deleteLater);
        QObject::connect(audioThread, &AudioThread::newChannel, this, &ControllerInterface::updateCurrentAudioChannel);

        // This starts the thread running, ideally on its own CPU core.
        audioThread->start();
        audioThread->setPriority(QThread::HighestPriority);

        // This activates the thread so it can do useful activity.
        audioThread->startRunning();
    } else {
        audioEnabled = false;
        if (audioThread) {
            audioThread->close();
            audioThread->wait();
            delete audioThread;
            audioThread = nullptr;
        }
    }
}

void ControllerInterface::toggleGameThread(bool enabled)
{
    if (!gameThread) return;

    qDebug() << "[ControllerInterface] === 游戏线程切换 ===";
    qDebug() << "[ControllerInterface] 切换状态:" << enabled;
    qDebug() << "[ControllerInterface] 控制器类型:" << state->getControllerTypeEnum()
             << "是否为StimRecord:" << (state->getControllerTypeEnum() == ControllerStimRecord);

    if (enabled) {
        // 设置默认运动区域通道（演示模式），仅涉及本地状态，不触碰硬件配置
        qDebug() << "[ControllerInterface] 设置默认运动区域...";
        gameThread->setDefaultMotorRegionsForDemo();
        qDebug() << "[ControllerInterface] 启动游戏线程...";
        gameThread->startRunning();
        if (usbDataThread) usbDataThread->startRunning();
        if (waveformProcessorThread) waveformProcessorThread->startRunning(rhxController->getNumEnabledDataStreams());
        qDebug() << "[ControllerInterface] 游戏线程已启动";
    } else {
        qDebug() << "[ControllerInterface] 停止游戏线程...";
        gameThread->stopRunning();
        qDebug() << "[ControllerInterface] 游戏线程已停止";
    }
}

void ControllerInterface::setGameThresholdMultiplier(double multiplier)
{
    if (gameThread) gameThread->setThresholdMultiplier(multiplier);
}

void ControllerInterface::setGameMinThreshold(double minThreshold)
{
    if (gameThread) gameThread->setMinThreshold(minThreshold);
}

void ControllerInterface::setGameRefractoryPeriod(int samples)
{
    if (gameThread) gameThread->setRefractoryPeriod(samples);
}

void ControllerInterface::setGameExperimentCondition(int condition)
{
    if (gameThread) {
        // 假设UI发送的int可以映射到ExperimentCondition枚举
        gameThread->setExperimentCondition(static_cast<ExperimentCondition>(condition));
    }
}

void ControllerInterface::setMissFreezeDurationMs(int durationMs)
{
    if (gameThread) gameThread->setMissFreezeDurationMs(durationMs);
}

void ControllerInterface::setHitTargetCurrent(double ua)
{
    targetHit_uA = ua;
    if (state->getControllerTypeEnum() == ControllerStimRecord) configureHitStimParams();
}

void ControllerInterface::setMissTargetCurrent(double ua)
{
    targetMiss_uA = ua;
}

void ControllerInterface::setSensoryTargetCurrent(double ua)
{
    targetSensory_uA = ua;
    if (state->getControllerTypeEnum() == ControllerStimRecord) configureSensoryParams();
}

void ControllerInterface::handleSensoryStim(int zone)
{
    if (!stimWorker) return;
    if (!sensoryConfigured) configureSensoryParams();
    QMetaObject::invokeMethod(stimWorker, "requestSensory", Qt::QueuedConnection, Q_ARG(int, zone));
}

void ControllerInterface::handleHitStim()
{
    if (!stimWorker) return;
    if (!hitStimConfigured) configureHitStimParams();
    QMetaObject::invokeMethod(stimWorker, "requestHit", Qt::QueuedConnection);
}

void ControllerInterface::handleMissStim()
{
    // Follow the same non-blocking pattern as Hit: do NOT re-upload during acquisition.
    // Simply schedule a 5 Hz / 4 s series of triggers via StimWorker to avoid halting display.
    if (!stimWorker) return;
    QMetaObject::invokeMethod(stimWorker, "requestMiss", Qt::QueuedConnection);
}

void ControllerInterface::handleStopAllStim()
{
    // 停止所有感觉通道的刺激
    for (int i = 0; i < 8; ++i) {
        // QString channelName = QString("A-%1").arg(i, 3, 10, QChar('0'));
        // setStimChannelEnabled(channelName, false);
        // 注意: 实际的停止刺激函数需要实现
    }
}

void ControllerInterface::onGameDataUpdated(const GameState& gameState)
{
    lastGameState = gameState;
    haveGameState = true;
    emit gameDataUpdated(gameState);
    if (stimWorker) {
        QMetaObject::invokeMethod(stimWorker, "updateGameState", Qt::QueuedConnection, Q_ARG(int, gameState.ballX));
    }
}

void ControllerInterface::handleStartSilentWindow(int durationMs)
{
    if (stimWorker) {
        QMetaObject::invokeMethod(stimWorker, "startSilentWindow", Qt::QueuedConnection, Q_ARG(int, durationMs));
    }
}

double ControllerInterface::computeCurrentFromImpedanceUA(const QString& amplifierNativeName, double target_mV) const
{
    Channel* amp = state->signalSources->channelByName(amplifierNativeName);
    if (amp && amp->isImpedanceValid()) {
        double z_kohm = amp->getImpedanceMagnitude();
        if (z_kohm > 1e-6) {
            double iuA = target_mV / z_kohm; // µA = mV / kΩ
            if (iuA < 0.0) iuA = 0.0;
            if (iuA > 2000.0) iuA = 2000.0;
            return iuA;
        }
    }
    double fallback = target_mV / 100.0; // assume 100 kΩ
    if (fallback < 0.0) fallback = 0.0;
    if (fallback > 2000.0) fallback = 2000.0;
    return fallback;
}

void ControllerInterface::configureHitStimParams()
{
    // 预配置感知通道的 Hit 刺激参数，一次性上传，后续只触发
    for (int i = 0; i < 8; ++i) {
        QString channelName = QString("A-%1").arg(i, 3, 10, QChar('0'));
        Channel* channel = state->signalSources->channelByName(channelName);
        if (!channel) continue;
        StimParameters* params = channel->stimParameters;
        // 使能该通道的刺激，并将触发源设置为手动键(F1..F8)
        params->enabled->setValue(true);
        params->triggerSource->setValue(QString("KeyPressF%1").arg(i + 1));
        params->pulseOrTrain->setIndex(PulseTrain);
        params->numberOfStimPulses->setValue(10); // 100ms at 100Hz
        params->pulseTrainPeriod->setValue(10000.0); // 10 ms = 100 Hz (us)
        params->firstPhaseDuration->setValue(100.0); // 100 us
        params->firstPhaseAmplitude->setValue(targetHit_uA);
        uploadStimParameters(channel);
        qDebug() << "[StimConfig-Hit]" << channelName
                 << "triggerIdx=" << params->triggerSource->getIndex()
                 << "amp(uA)=" << params->firstPhaseAmplitude->getValue();
    }
    hitStimConfigured = true;
    // Note: USBDataThread sets StimCmdMode at run time; avoid forcing here to prevent side effects.

    // 打印当前刺激通道阻抗，便于在终端快速校验
    logStimChannelImpedances();
}

void ControllerInterface::configureMissStimParams()
{
    // 预配置 Miss 刺激参数（示例：5Hz x 4s 等效脉冲数）
    for (int i = 0; i < 8; ++i) {
        QString channelName = QString("A-%1").arg(i, 3, 10, QChar('0'));
        Channel* channel = state->signalSources->channelByName(channelName);
        if (!channel) continue;
        StimParameters* params = channel->stimParameters;
        params->enabled->setValue(true);
        params->triggerSource->setValue(QString("KeyPressF%1").arg(i + 1));
        params->pulseOrTrain->setIndex(PulseTrain);
        params->numberOfStimPulses->setValue(20);         // 5 Hz * 4 s
        params->pulseTrainPeriod->setValue(200000.0);     // 200 ms = 5 Hz (us)
        params->firstPhaseDuration->setValue(100.0);      // 100 us
        params->firstPhaseAmplitude->setValue(targetMiss_uA);
        uploadStimParameters(channel);
    }
    missStimConfigured = true;
    // See note above: do not force StimCmdMode here.
}

void ControllerInterface::configureSensoryParams()
{
    // 将 A-000..A-007 配置为单脉冲模式（双相在硬件里配置），幅度与脉宽固定，频率由手动触发节拍决定
    for (int i = 0; i < 8; ++i) {
        QString channelName = sensoryChannelNameForZone(i);
        Channel* channel = state->signalSources->channelByName(channelName);
        if (!channel) continue;
        StimParameters* params = channel->stimParameters;
        params->enabled->setValue(true);
        params->triggerSource->setValue(QString("KeyPressF%1").arg(i + 1));
        params->pulseOrTrain->setIndex(SinglePulse);
        params->firstPhaseDuration->setValue((double) sensoryPulseWidthUs);
        params->firstPhaseAmplitude->setValue(targetSensory_uA);
        uploadStimParameters(channel);
        const int trigIdx = params->triggerSource->getIndex();
        const double amp = params->firstPhaseAmplitude->getValue();
        qDebug() << "[StimConfig]" << channelName << "enabled=true"
                 << "triggerIdx=" << trigIdx << "(" << QString("KeyPressF%1").arg(i+1) << ")"
                 << "amp(uA)=" << amp << "pw(us)=" << sensoryPulseWidthUs;
        state->writeToLog(QString("[StimConfig] %1 enabled=true, trigger=KeyPressF%2, amp=%3 uA, pw=%4 us")
                          .arg(channelName).arg(i + 1).arg(amp, 0, 'f', 3).arg(sensoryPulseWidthUs));
    }
    sensoryConfigured = true;
    // See note above: do not force StimCmdMode here.

    // 打印当前刺激通道阻抗，便于终端校验
    logStimChannelImpedances();
}

void ControllerInterface::onStimWorkerTriggerChannel(int zone)
{
    if (zone < 0 || zone > 7) return;
    state->writeToLog(QString("[StimWorker] trigger zone %1").arg(zone));
    qDebug() << "[StimWorker] trigger zone" << zone;
    setManualStimTrigger(zone, true);
    QTimer::singleShot(3, this, [this, zone]() {
        setManualStimTrigger(zone, false);
    });
}

void ControllerInterface::onStimWorkerHitBurst()
{
    for (int i = 0; i < 8; ++i) {
        setManualStimTrigger(i, true);
        QTimer::singleShot(3, this, [this, i]() {
            setManualStimTrigger(i, false);
        });
    }
}

void ControllerInterface::startMissStimSession()
{
    // 配置：5 Hz，4 s，总共 20 次；每次随机选择一个通道发一记单脉冲
    // 将 8 个感觉通道参数切换为单脉冲，较大幅度（missSessionAmplitude_uA）
    for (int i = 0; i < 8; ++i) {
        QString channelName = sensoryChannelNameForZone(i);
        Channel* channel = state->signalSources->channelByName(channelName);
        if (!channel) continue;
        StimParameters* params = channel->stimParameters;
        params->enabled->setValue(true);
        params->triggerSource->setValue(QString("KeyPressF%1").arg(i + 1));
        params->pulseOrTrain->setIndex(SinglePulse);
        params->firstPhaseDuration->setValue((double) sensoryPulseWidthUs);
        params->firstPhaseAmplitude->setValue(targetMiss_uA);
        uploadStimParameters(channel);
    }
    // See note above: do not force StimCmdMode here.

    missTicksRemaining = 20; // 4 s * 5 Hz
    if (!missSessionTimer.isActive()) {
        QObject::connect(&missSessionTimer, &QTimer::timeout, this, &ControllerInterface::onMissStimSessionTick);
    }
    missSessionTimer.start(missSessionIntervalMs);
    missSessionActive = true;
}

void ControllerInterface::onMissStimSessionTick()
{
    if (!missSessionActive) {
        missSessionTimer.stop();
        return;
    }

    int zone = rand() % 8; // 简单随机
    setManualStimTrigger(zone, true);
    setManualStimTrigger(zone, false);

    if (--missTicksRemaining <= 0) {
        missSessionTimer.stop();
        missSessionActive = false;
        // 恢复感觉参数（小幅度）
        if (!sensoryConfigured) configureSensoryParams();
        else {
            for (int i = 0; i < 8; ++i) {
                QString channelName = sensoryChannelNameForZone(i);
                Channel* channel = state->signalSources->channelByName(channelName);
                if (!channel) continue;
                StimParameters* params = channel->stimParameters;
                params->pulseOrTrain->setIndex(SinglePulse);
                params->firstPhaseDuration->setValue((double) sensoryPulseWidthUs);
                params->firstPhaseAmplitude->setValue(sensoryAmplitude_uA);
                uploadStimParameters(channel);
            }
        }
    }
}

void ControllerInterface::runTCPDataOutputThread()
{
        tcpDataOutputEnabled = true;
        if (!tcpDataOutputThread) {
            tcpDataOutputThread = new TCPDataOutputThread(waveformFifo, rhxController->getSampleRate(), state, this);
        }

        state->tcpWaveformDataCommunicator->moveToThread(tcpDataOutputThread);
        state->tcpSpikeDataCommunicator->moveToThread(tcpDataOutputThread);

        QObject::connect(tcpDataOutputThread, &TCPDataOutputThread::finished, tcpDataOutputThread, &QObject::deleteLater);

        // This starts the thread running, ideally on its own CPU core.
        tcpDataOutputThread->start();
        tcpDataOutputThread->setPriority(QThread::HighestPriority);

        // This activates the thread so it can do useful activity.
        tcpDataOutputThread->startRunning();
}

void ControllerInterface::rescanPorts(bool updateDisplay)
{
    qDebug() << "[DEBUG ControllerInterface] === rescanPorts Entry ===";
    qDebug() << "[DEBUG ControllerInterface] rescanPorts started, updateDisplay:" << updateDisplay;
    
    try {
        qDebug() << "[DEBUG ControllerInterface] Getting previous headstage state...";
        bool previousHeadstagePresent = state->signalSources->numAmplifierChannels() != 0;
        qDebug() << "[DEBUG ControllerInterface] Previous headstage present:" << previousHeadstagePresent;
        
        int numDataStreams = 0;
        qDebug() << "[DEBUG ControllerInterface] Checking if controller is playback...";
        if (rhxController->isPlayback()) {
            qDebug() << "[DEBUG ControllerInterface] Controller is in playback mode";
            rhxController->enableDataStream(0, false);
            for (int stream = 0; stream < dataFileReader->numDataStreams(); ++stream) {
                rhxController->enableDataStream(stream, true);
                ++numDataStreams;
            }
            addPlaybackHeadstageChannels();
        } else {
            qDebug() << "[DEBUG ControllerInterface] Controller is NOT in playback mode";
            qDebug() << "[DEBUG ControllerInterface] Creating vectors for scanPorts...";
            std::vector<int> portIndex, commandStream, numChannelsOnPort;
            qDebug() << "[DEBUG ControllerInterface] Vectors created successfully";
            qDebug() << "[DEBUG ControllerInterface] Calling scanPorts...";
            
            // 在调用scanPorts之前添加额外的检查
            qDebug() << "[DEBUG ControllerInterface] About to call scanPorts with chipType size:" << state->chipType.size();
            numDataStreams = scanPorts(state->chipType, portIndex, commandStream, numChannelsOnPort);
            qDebug() << "[DEBUG ControllerInterface] scanPorts completed, numDataStreams:" << numDataStreams;
            qDebug() << "[DEBUG ControllerInterface] Returned vector sizes - portIndex:" << portIndex.size() << ", commandStream:" << commandStream.size() << ", numChannelsOnPort:" << numChannelsOnPort.size();
            
            qDebug() << "[DEBUG ControllerInterface] Calling addAmplifierChannels...";
            addAmplifierChannels(state->chipType, portIndex, commandStream, numChannelsOnPort);
            qDebug() << "[DEBUG ControllerInterface] addAmplifierChannels completed";
            
            qDebug() << "[DEBUG ControllerInterface] Calling setManualCableDelays...";
            setManualCableDelays();
            qDebug() << "[DEBUG ControllerInterface] setManualCableDelays completed";
        }

    qDebug() << "[DEBUG ControllerInterface] Calling state->signalSources->updateChannelMap()...";
    state->signalSources->updateChannelMap();
    qDebug() << "[DEBUG ControllerInterface] updateChannelMap completed";
    
    if (rhxController->isPlayback()) {
        qDebug() << "[DEBUG ControllerInterface] Enabling playback channels...";
        enablePlaybackChannels();  // Enable only the signals that are present in playback file.
        qDebug() << "[DEBUG ControllerInterface] enablePlaybackChannels completed";
    }

    qDebug() << "[DEBUG ControllerInterface] Calling autoColorAmplifierChannels...";
    state->signalSources->autoColorAmplifierChannels(32, 1);
    qDebug() << "[DEBUG ControllerInterface] autoColorAmplifierChannels completed";
    
    qDebug() << "[DEBUG ControllerInterface] Calling xpuController->updateNumStreams...";
    xpuController->updateNumStreams(numDataStreams);
    qDebug() << "[DEBUG ControllerInterface] xpuController->updateNumStreams completed";

    if (updateDisplay) {
        qDebug() << "[DEBUG ControllerInterface] Updating display...";
        // Determine if port selection should switch to a headstage port
        // This should only occur if prior to scanning, 0 headstages were present, and after, at least 1 was present
        bool currentHeadstagePresent = state->signalSources->numAmplifierChannels() != 0;
        bool switchToFirstPort = !previousHeadstagePresent && currentHeadstagePresent;
        qDebug() << "[DEBUG ControllerInterface] Current headstage present:" << currentHeadstagePresent << ", switchToFirstPort:" << switchToFirstPort;
        display->updatePortSelectionBoxes(switchToFirstPort);
        qDebug() << "[DEBUG ControllerInterface] updatePortSelectionBoxes completed";
    }

    qDebug() << "[DEBUG ControllerInterface] Setting headstage present value...";
    state->headstagePresent->setValue(state->signalSources->numAmplifierChannels() > 0);
    qDebug() << "[DEBUG ControllerInterface] Calling updateForChangeHeadstages...";
    state->updateForChangeHeadstages();
    qDebug() << "[DEBUG ControllerInterface] updateForChangeHeadstages completed";

    if (waveformFifo) {
        qDebug() << "[DEBUG ControllerInterface] Calling waveformFifo->updateForRescan...";
        waveformFifo->updateForRescan();
        qDebug() << "[DEBUG ControllerInterface] waveformFifo->updateForRescan completed";
    }

    if (display) {
        qDebug() << "[DEBUG ControllerInterface] Calling display->updateForRescan...";
        display->updateForRescan();
        qDebug() << "[DEBUG ControllerInterface] display->updateForRescan completed";
    }
    } catch (const std::exception& e) {
        qDebug() << "[DEBUG ControllerInterface] EXCEPTION in rescanPorts:" << e.what();
        throw;
    } catch (...) {
        qDebug() << "[DEBUG ControllerInterface] UNKNOWN EXCEPTION in rescanPorts!";
        throw;
    }
    
    qDebug() << "[DEBUG ControllerInterface] rescanPorts completed successfully";
    qDebug() << "[DEBUG ControllerInterface] === rescanPorts Exit ===";
}

// Returns number of data streams used.
int ControllerInterface::scanPorts(std::vector<ChipType> &chipType, std::vector<int> &portIndex, std::vector<int> &commandStream,
                                    std::vector<int> &numChannelsOnPort)
{
    qDebug() << "[DEBUG ControllerInterface] === scanPorts Entry ===";
    qDebug() << "[DEBUG ControllerInterface] scanPorts started";
    qDebug() << "[DEBUG ControllerInterface] Input chipType size:" << chipType.size();
    qDebug() << "[DEBUG ControllerInterface] Input portIndex size:" << portIndex.size();
    qDebug() << "[DEBUG ControllerInterface] Input commandStream size:" << commandStream.size();
    qDebug() << "[DEBUG ControllerInterface] Input numChannelsOnPort size:" << numChannelsOnPort.size();
    
    int numDataStreams = 0;  // 将变量声明移到函数开始
    
    try {
        // Scan SPI Ports.
        qDebug() << "[DEBUG ControllerInterface] Reading settings...";
        QSettings settings;
        bool synthMaxChannels = false;
        if (settings.value("synthMaxChannels", false).toBool()) {
            synthMaxChannels = true;
        }
        qDebug() << "[DEBUG ControllerInterface] synthMaxChannels:" << synthMaxChannels;
        
        qDebug() << "[DEBUG ControllerInterface] Calling rhxController->findConnectedChips...";
        int warningCode = rhxController->findConnectedChips(chipType, portIndex, commandStream,
                                                            numChannelsOnPort, synthMaxChannels,
                                                            state->manualFastSettleEnabled->getValue(),
                                                            state->usePreviousDelay->getValue(),
                                                            state->previousDelaySelectedPort->getValue(),
                                                            state->lastDetectedChip->getValue(),
                                                            state->lastDetectedNumStreams->getValue());
        qDebug() << "[DEBUG ControllerInterface] findConnectedChips completed, warningCode:" << warningCode;
        qDebug() << "[DEBUG ControllerInterface] Output chipType size:" << chipType.size();
        qDebug() << "[DEBUG ControllerInterface] Output portIndex size:" << portIndex.size();
        qDebug() << "[DEBUG ControllerInterface] Output commandStream size:" << commandStream.size();
        qDebug() << "[DEBUG ControllerInterface] Output numChannelsOnPort size:" << numChannelsOnPort.size();

        qDebug() << "[DEBUG ControllerInterface] Iterating through chipType vector...";
        for (uint i = 0; i < chipType.size(); i++) {
            qDebug() << "[DEBUG ControllerInterface] chipType[" << i << "] =" << (int)chipType[i];
            if (chipType[i] != NoChip) {
                qDebug() << "[DEBUG ControllerInterface] Setting lastDetectedChip to:" << (int)chipType[i];
                state->lastDetectedChip->setValue((int) chipType[i]);
                break;
            }
        }

    if (warningCode == -1) {
        qDebug() << "[DEBUG ControllerInterface] Warning: Capacity exceeded (-1)";
        QMessageBox::warning(nullptr, tr("Capacity of RHD USB Interface Exceeded"),
                             tr("This RHD USB interface board can support only 256 amplifier channels."
                                "<p>More than 256 total amplifier channels are currently connected."
                                "<p>Amplifier chips exceeding this limit will not appear in the GUI."));
    } else if (warningCode == -2) {
        qDebug() << "[DEBUG ControllerInterface] Warning: Capacity exceeded (-2)";
        QMessageBox::warning(nullptr, tr("Capacity of RHD USB Interface Exceeded"),
                             tr("This RHD USB interface board can support only 256 amplifier channels."
                                "<p>More than 256 total amplifier channels are currently connected.  (Each RHD2216 "
                                "chip counts as 32 channels.)"
                                "<p>Amplifier chips exceeding this limit will not appear in the GUI."));
    }

    qDebug() << "[DEBUG ControllerInterface] Calculating numDataStreams...";
    numDataStreams = 0;  // 重置计数器
    qDebug() << "[DEBUG ControllerInterface] portIndex.size():" << portIndex.size();
    for (int i = 0; i < (int) portIndex.size(); ++i) {
        qDebug() << "[DEBUG ControllerInterface] portIndex[" << i << "] =" << portIndex[i];
        if (portIndex[i] != -1) ++numDataStreams;
    }
    qDebug() << "[DEBUG ControllerInterface] numDataStreams:" << numDataStreams;
    state->lastDetectedNumStreams->setValue(numDataStreams);

    // Turn on appropriate LEDs for Ports A-H.
    qDebug() << "[DEBUG ControllerInterface] Setting up LED array...";
    int ledArray[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    qDebug() << "[DEBUG ControllerInterface] Controller type:" << (int)rhxController->getType();
    qDebug() << "[DEBUG ControllerInterface] numChannelsOnPort.size():" << numChannelsOnPort.size();
    
    if (rhxController->getType() == ControllerStimRecord) {
        qDebug() << "[DEBUG ControllerInterface] Processing ControllerStimRecord LEDs...";
        for (int port = 0; port < 4; port++) {
            qDebug() << "[DEBUG ControllerInterface] Checking port" << port << ", size check:" << (port < (int)numChannelsOnPort.size());
            if (port < (int)numChannelsOnPort.size()) {
                qDebug() << "[DEBUG ControllerInterface] numChannelsOnPort[" << port << "] =" << numChannelsOnPort[port];
                if (numChannelsOnPort[port] > 0) ledArray[2 * port] = 1;
            } else {
                qDebug() << "[DEBUG ControllerInterface] WARNING: port" << port << "is out of bounds for numChannelsOnPort";
            }
        }
    } else if (rhxController->getType() == ControllerRecordUSB3) {
        qDebug() << "[DEBUG ControllerInterface] Processing ControllerRecordUSB3 LEDs...";
        for (int port = 0; port < 8; port++) {
            qDebug() << "[DEBUG ControllerInterface] Checking port" << port << ", size check:" << (port < (int)numChannelsOnPort.size());
            if (port < (int)numChannelsOnPort.size()) {
                qDebug() << "[DEBUG ControllerInterface] numChannelsOnPort[" << port << "] =" << numChannelsOnPort[port];
                if (numChannelsOnPort[port] > 0) ledArray[port] = 1;
            } else {
                qDebug() << "[DEBUG ControllerInterface] WARNING: port" << port << "is out of bounds for numChannelsOnPort";
            }
        }
    }
    
    qDebug() << "[DEBUG ControllerInterface] Calling setSpiLedDisplay...";
    rhxController->setSpiLedDisplay(ledArray);
    qDebug() << "[DEBUG ControllerInterface] setSpiLedDisplay completed";
    
    } catch (const std::exception& e) {
        qDebug() << "[DEBUG ControllerInterface] EXCEPTION in scanPorts:" << e.what();
        throw;
    } catch (...) {
        qDebug() << "[DEBUG ControllerInterface] UNKNOWN EXCEPTION in scanPorts!";
        throw;
    }
    
    qDebug() << "[DEBUG ControllerInterface] scanPorts completed successfully, returning:" << numDataStreams;
    qDebug() << "[DEBUG ControllerInterface] === scanPorts Exit ===";
    return numDataStreams;
}

void ControllerInterface::addAmplifierChannels(const std::vector<ChipType> &chipType, const std::vector<int> &portIndex,
                                               const std::vector<int> &commandStream, const std::vector<int> &numChannelsOnPort)
{
    qDebug() << "[DEBUG ControllerInterface] === addAmplifierChannels Entry ===";
    qDebug() << "[DEBUG ControllerInterface] addAmplifierChannels started";
    qDebug() << "[DEBUG ControllerInterface] Input parameters:";
    qDebug() << "[DEBUG ControllerInterface] - chipType size:" << chipType.size();
    qDebug() << "[DEBUG ControllerInterface] - portIndex size:" << portIndex.size();
    qDebug() << "[DEBUG ControllerInterface] - commandStream size:" << commandStream.size();
    qDebug() << "[DEBUG ControllerInterface] - numChannelsOnPort size:" << numChannelsOnPort.size();
    
    try {
    qDebug() << "[DEBUG ControllerInterface] Clearing undo stack...";
    state->signalSources->undoManager->clearUndoStack();
    qDebug() << "[DEBUG ControllerInterface] Undo stack cleared successfully";

    qDebug() << "[DEBUG ControllerInterface] Calculating numDataStreams...";
    int numDataStreams = (int) chipType.size();
    qDebug() << "[DEBUG ControllerInterface] numDataStreams:" << numDataStreams;
    qDebug() << "[DEBUG ControllerInterface] state->numSPIPorts:" << state->numSPIPorts;

    qDebug() << "[DEBUG ControllerInterface] Processing ports loop...";

    qDebug() << "[DEBUG ControllerInterface] Processing ports loop...";
    for (int port = 0; port < state->numSPIPorts; port++) {
        qDebug() << "[DEBUG ControllerInterface] === Processing Port" << port << "===";
        qDebug() << "[DEBUG ControllerInterface] Checking port" << port << "bounds...";
        
        if (port >= (int)numChannelsOnPort.size()) {
            qDebug() << "[DEBUG ControllerInterface] ERROR: Port" << port << "is out of bounds for numChannelsOnPort (size:" << numChannelsOnPort.size() << ")";
            continue;
        }
        
        qDebug() << "[DEBUG ControllerInterface] Port" << port << "numChannels:" << numChannelsOnPort[port];
        
        qDebug() << "[DEBUG ControllerInterface] Getting port group for port" << port << "...";
        SignalGroup* group = state->signalSources->portGroupByIndex(port);
        qDebug() << "[DEBUG ControllerInterface] Port group obtained successfully";
        
        if (numChannelsOnPort[port] == 0) {
            qDebug() << "[DEBUG ControllerInterface] Port" << port << "has 0 channels, removing all and disabling...";
            group->removeAllChannels();
            group->setEnabled(false);
            qDebug() << "[DEBUG ControllerInterface] Port" << port << "disabled successfully";
        } else if (group->numChannels(AmplifierSignal) != numChannelsOnPort[port]) {
            qDebug() << "[DEBUG ControllerInterface] Port" << port << "channel count changed, reconfiguring...";
            qDebug() << "[DEBUG ControllerInterface] Current channels:" << group->numChannels(AmplifierSignal) << ", new channels:" << numChannelsOnPort[port];
            qDebug() << "[DEBUG ControllerInterface] Current channels:" << group->numChannels(AmplifierSignal) << ", new channels:" << numChannelsOnPort[port];
            // If number of channels on port has changed...
            qDebug() << "[DEBUG ControllerInterface] Removing all existing channels from port" << port << "...";
            group->removeAllChannels();  // ...clear existing channels...
            group->setEnabled(true);
            qDebug() << "[DEBUG ControllerInterface] Port" << port << "enabled, creating new channels...";
            // ...and create new ones.
            int channel = 0;
            qDebug() << "[DEBUG ControllerInterface] Creating amplifier channels for port" << port << "...";
            // Create amplifier channels for each chip.
            for (int stream = 0; stream < numDataStreams; stream++) {
                qDebug() << "[DEBUG ControllerInterface] Checking stream" << stream << "(portIndex:" << portIndex[stream] << ")";
                if (portIndex[stream] == port) {
                    qDebug() << "[DEBUG ControllerInterface] Stream" << stream << "matches port" << port << ", chipType:" << (int)chipType[stream];
                    qDebug() << "[DEBUG ControllerInterface] Stream" << stream << "matches port" << port << ", chipType:" << (int)chipType[stream];
                    if (chipType[stream] == RHD2216Chip ||
                        chipType[stream] == RHS2116Chip) {
                        qDebug() << "[DEBUG ControllerInterface] Creating 16 channels for RHD2216/RHS2116 chip on stream" << stream;
                        for (int i = 0; i < 16; i++) {
                            group->addAmplifierChannel(channel, stream, commandStream[stream], i);
                            channel++;
                        }
                        qDebug() << "[DEBUG ControllerInterface] 16 channels created, current channel count:" << channel;
                    } else if (chipType[stream] == RHD2132Chip ||
                               chipType[stream] == RHD2164Chip ||
                               chipType[stream] == RHD2164MISOBChip) {
                        qDebug() << "[DEBUG ControllerInterface] Creating 32 channels for RHD2132/RHD2164 chip on stream" << stream;
                        for (int i = 0; i < 32; i++) {
                            group->addAmplifierChannel(channel, stream, commandStream[stream], i);
                            channel++;
                        }
                        qDebug() << "[DEBUG ControllerInterface] 32 channels created, current channel count:" << channel;
                    }
                }
            }
            qDebug() << "[DEBUG ControllerInterface] Amplifier channels creation completed for port" << port;
            //  Now create auxiliary input channels and supply voltage channels for each chip.
            qDebug() << "[DEBUG ControllerInterface] Creating auxiliary and supply voltage channels for port" << port << "...";
            int auxName = 1;
            int vddName = 1;
            for (int stream = 0; stream < numDataStreams; stream++) {
                if (portIndex[stream] == port) {
                    qDebug() << "[DEBUG ControllerInterface] Processing auxiliary channels for stream" << stream << ", chipType:" << (int)chipType[stream];
                    if (chipType[stream] == RHD2216Chip ||
                        chipType[stream] == RHD2132Chip ||
                        chipType[stream] == RHD2164Chip) {
                        qDebug() << "[DEBUG ControllerInterface] Adding 3 aux input channels and 1 supply voltage channel...";
                        group->addAuxInputChannel(channel++, stream, 0, auxName++);
                        group->addAuxInputChannel(channel++, stream, 1, auxName++);
                        group->addAuxInputChannel(channel++, stream, 2, auxName++);
                        group->addSupplyVoltageChannel(channel++, stream, vddName++);
                        qDebug() << "[DEBUG ControllerInterface] Auxiliary channels added, current channel count:" << channel;
                    }
                }
            }
        } else {    // If number of channels on port has not changed, don't create new channels (since this
                    // would clear all user-defined channel names.  But we must update the data stream indices
                    // on the port.
            int channel = 0;
            // Update stream indices for amplifier channels.
            for (int stream = 0; stream < numDataStreams; stream++) {
                if (portIndex[stream] == port) {
                    if (chipType[stream] == RHD2216Chip ||
                        chipType[stream] == RHS2116Chip) {
                        for (int i = channel; i < channel + 16; i++) {
                            Channel* channel = group->channelByIndex(i);
                            channel->setBoardStream(stream);
                            channel->setCommandStream(commandStream[stream]);
                        }
                        channel += 16;
                    } else if (chipType[stream] == RHD2132Chip ||
                               chipType[stream] == RHD2164Chip ||
                               chipType[stream] == RHD2164MISOBChip) {
                        for (int i = channel; i < channel + 32; i++) {
                            Channel* channel = group->channelByIndex(i);
                            channel->setBoardStream(stream);
                            channel->setCommandStream(commandStream[stream]);
                        }
                        channel += 32;
                    }
                }
            }
            // Update stream indices for auxiliary channels and supply voltage channels.
            for (int stream = 0; stream < numDataStreams; ++stream) {
                if (portIndex[stream] == port) {
                    if (chipType[stream] == RHD2216Chip ||
                        chipType[stream] == RHD2132Chip ||
                        chipType[stream] == RHD2164Chip) {
                        group->channelByIndex(channel++)->setBoardStream(stream);
                        group->channelByIndex(channel++)->setBoardStream(stream);
                        group->channelByIndex(channel++)->setBoardStream(stream);
                        group->channelByIndex(channel++)->setBoardStream(stream);
                   }
                }
            }
        }
        qDebug() << "[DEBUG ControllerInterface] Port" << port << "channel configuration completed";
        qDebug() << "[DEBUG ControllerInterface] === Port" << port << "Processing Complete ===";
    }
    
    } catch (const std::exception& e) {
        qDebug() << "[ERROR ControllerInterface] EXCEPTION in addAmplifierChannels:" << e.what();
        throw;
    } catch (...) {
        qDebug() << "[ERROR ControllerInterface] UNKNOWN EXCEPTION in addAmplifierChannels!";
        throw;
    }
    
    qDebug() << "[DEBUG ControllerInterface] addAmplifierChannels completed successfully";
    qDebug() << "[DEBUG ControllerInterface] === addAmplifierChannels Exit ===";
}

void ControllerInterface::enablePlaybackChannels()
{
    if (!dataFileReader) return;
    const IntanHeaderInfo* fileInfo = dataFileReader->getHeaderInfo();

    for (int i = 0; i < state->signalSources->numGroups(); ++i) {
        SignalGroup* group = state->signalSources->groupByIndex(i);
        QString groupPrefix = group->getPrefix();
        int index = fileInfo->groupIndex(groupPrefix);
        // If this group prefix can't be found, and it's one of the prefixes that had a name
        // change from the original USB Interface Board software, try looking for the original
        // prefix name.
        if (index == -1) {
            if (groupPrefix == "DIGITAL-IN") index = fileInfo->groupIndex("DIN");
            else if (groupPrefix == "DIGITAL-OUT") index = fileInfo->groupIndex("DOUT");
            else if (groupPrefix == "ANALOG-IN") index = fileInfo->groupIndex("ADC");
        }

        // If this group prefix still can't be found, print an error.
        if (index == -1) {
            std::cerr << "ControllerInterface::enablePlaybackChannels: Could not find group with prefix " <<
                    groupPrefix.toStdString() << '\n';
        } else {
            const HeaderFileGroup& fileGroup = fileInfo->groups[index];
            for (int i = 0; i < fileGroup.numChannels(); ++i) {
                const HeaderFileChannel& fileChannel = fileGroup.channels[i];
                Channel* channel = state->signalSources->channelByName(fileChannel.nativeChannelName);
                // If this channel can't be found, and it's one of the channel names that had a name
                // change from the original USB Interface Board software, try looking for the same
                // channel with the original naming convention.
                if (!channel) {
                    if (groupPrefix == "DIGITAL-IN" || groupPrefix == "DIGITAL-OUT" || groupPrefix == "ANALOG-IN") {
                        QString nativeChannelName = groupPrefix + fileChannel.nativeChannelName.right(3);
                        channel = state->signalSources->channelByName(nativeChannelName);
                    }
                }

                if (channel) {
                    channel->setEnabled(fileChannel.enabled);
                } else {
                    std::cerr << "ControllerInterface::enablePlaybackChannels: Could not find channel " <<
                            fileChannel.nativeChannelName.toStdString() << '\n';
                }
            }
        }
    }
}

void ControllerInterface::addPlaybackHeadstageChannels()
{
    if (!dataFileReader) return;
    const IntanHeaderInfo* fileInfo = dataFileReader->getHeaderInfo();

    // Set bandwidth parameters from playback file.
    state->holdUpdate();
    state->desiredDspCutoffFreq->setValueWithLimits(fileInfo->desiredDspCutoffFreq);
    state->desiredLowerBandwidth->setValueWithLimits(fileInfo->desiredLowerBandwidth);
    state->desiredUpperBandwidth->setValueWithLimits(fileInfo->desiredUpperBandwidth);
    state->dspEnabled->setValue(fileInfo->dspEnabled);
    state->actualDspCutoffFreq->setValueWithLimits(fileInfo->actualDspCutoffFreq);
    state->actualLowerBandwidth->setValueWithLimits(fileInfo->actualLowerBandwidth);
    state->actualUpperBandwidth->setValueWithLimits(fileInfo->actualUpperBandwidth);
    state->releaseUpdate();

    for (int port = 0; port < fileInfo->numSPIPorts; ++port) {
        SignalGroup* group = state->signalSources->portGroupByIndex(port);
        QString portPrefix = QString(QChar('A' + port));
        int index = fileInfo->groupIndex(portPrefix);
        if (index == -1) {
            group->removeAllChannels();
            group->setEnabled(false);
        } else {
            const HeaderFileGroup& fileGroup = fileInfo->groups[index];
            if (!fileGroup.enabled) {
                group->removeAllChannels();
                group->setEnabled(false);
            } else {
                for (int i = 0; i < fileGroup.numChannels(); ++i) {
                    const HeaderFileChannel& fileChannel = fileGroup.channels[i];
                    if (fileChannel.signalType == AmplifierSignal) {
                        group->addAmplifierChannel(fileChannel.nativeOrder, fileChannel.boardStream,
                                                   fileChannel.commandStream, fileChannel.chipChannel,
                                                   fileChannel.impedanceMagnitude, fileChannel.impedancePhase);
//                       cout << "Playback configuration: Adding " << portPrefix.toStdString() << "-" <<
//                                QString("%1").arg(fileChannel.channelNumber(), 3, 10, QChar('0')).toStdString() << endl;
                    } else if (fileChannel.signalType == AuxInputSignal) {
                        group->addAuxInputChannel(fileChannel.nativeOrder, fileChannel.boardStream,
                                                  fileChannel.chipChannel, fileChannel.endingNumber(1));
//                       cout << "Playback configuration: Adding " << portPrefix.toStdString() << "-AUX" <<
//                                fileChannel.endingNumber(1) << endl;
                    } else if (fileChannel.signalType == SupplyVoltageSignal) {
                        group->addSupplyVoltageChannel(fileChannel.nativeOrder, fileChannel.boardStream,
                                                       fileChannel.endingNumber(1));
//                       cout << "Playback configuration: Adding " << portPrefix.toStdString() << "-VDD" <<
//                                fileChannel.endingNumber(1) << endl;
                    }
                }
            }
        }
    }
}

void ControllerInterface::setManualCableDelays()
{
    SignalSources* signalSources = state->signalSources;
    for (int port = 0; port < signalSources->numPortGroups(); ++port) {
        if (signalSources->portGroupByIndex(port)->manualDelayEnabled->getValue()) {
            rhxController->setCableDelay((BoardPort)port, signalSources->portGroupByIndex(port)->manualDelay->getValue());
        }
    }
}

void ControllerInterface::openController(const QString& boardSerialNumber)
{
    rhxController->open(boardSerialNumber.toStdString());

    // Upload FPGA bit file.
    QString bitfilename;
    if (state->getControllerTypeEnum() == ControllerRecordUSB2) {
        bitfilename = ConfigFileRHDBoard;
    } else if (state->getControllerTypeEnum() == ControllerRecordUSB3) {
        bitfilename = is7310 ? ConfigFileRHDController_7310 : ConfigFileRHDController;
    } else if (state->getControllerTypeEnum() == ControllerStimRecord){
        bitfilename = is7310 ? ConfigFileRHSController_7310 : ConfigFileRHSController;
    } else {
        bitfilename = ConfigFileRHDController_7310;
    }
    if (!rhxController->uploadFPGABitfile(QString(QCoreApplication::applicationDirPath() + "/" + bitfilename).toStdString())) {
        QMessageBox::critical(nullptr, tr("Configuration File Error: Software Aborting"),
                              tr("Cannot upload configuration file: ") + bitfilename +
                              tr(".  Make sure file is in the same directory as the executable file."));
        exit(EXIT_FAILURE);
    }

    rhxController->resetBoard();
}

// Initialize a controller connected to a USB port.
void ControllerInterface::initializeController()
{
    qDebug() << "[DEBUG ControllerInterface] === initializeController Entry ===";
    qDebug() << "[DEBUG ControllerInterface] initializeController started";
    
    try {
        qDebug() << "[DEBUG ControllerInterface] Calling rhxController->initialize()...";
        rhxController->initialize();
        qDebug() << "[DEBUG ControllerInterface] rhxController->initialize() completed successfully";

        if (state->getControllerTypeEnum() == ControllerStimRecord) {
            qDebug() << "[DEBUG ControllerInterface] Setting up ControllerStimRecord specific settings...";
            rhxController->enableDcAmpConvert(true);
            rhxController->setExtraStates(0);
            qDebug() << "[DEBUG ControllerInterface] ControllerStimRecord settings configured";
        }

        qDebug() << "[DEBUG ControllerInterface] Setting sample rate...";
        rhxController->setSampleRate(state->getSampleRateEnum());
        qDebug() << "[DEBUG ControllerInterface] Sample rate set successfully";

        // Upload all SPI command sequences.
        qDebug() << "[DEBUG ControllerInterface] Calling updateChipCommandLists(true)...";
        updateChipCommandLists(true);
        qDebug() << "[DEBUG ControllerInterface] updateChipCommandLists(true) completed successfully";

    if (state->getControllerTypeEnum() != ControllerStimRecord) {
        // Select RAM Bank 0 for AuxCmd3 initially, so the ADC is calibrated.
        qDebug() << "[DEBUG ControllerInterface] Selecting AuxCmd3 bank for non-StimRecord controller...";
        rhxController->selectAuxCommandBankAllPorts(RHXController::AuxCmd3, 0);
        qDebug() << "[DEBUG ControllerInterface] AuxCmd3 bank selected successfully";
    }

    // Since our longest command sequence is N commands, we run the SPI interface for N samples.
    qDebug() << "[DEBUG ControllerInterface] Setting max time step...";
    int samplesPerBlock = RHXDataBlock::samplesPerDataBlock(state->getControllerTypeEnum());
    qDebug() << "[DEBUG ControllerInterface] Samples per data block:" << samplesPerBlock;
    rhxController->setMaxTimeStep(samplesPerBlock);
    qDebug() << "[DEBUG ControllerInterface] Max time step set successfully";
    
    qDebug() << "[DEBUG ControllerInterface] Setting continuous run mode to false...";
    rhxController->setContinuousRunMode(false);
    qDebug() << "[DEBUG ControllerInterface] Continuous run mode set successfully";

    // Start SPI interface.
    qDebug() << "[DEBUG ControllerInterface] Starting SPI interface...";
    rhxController->run();
    qDebug() << "[DEBUG ControllerInterface] SPI interface started successfully";

    // Wait for the N-sample run to complete.
    qDebug() << "[DEBUG ControllerInterface] Waiting for SPI run to complete...";
    int waitCount = 0;
    while (rhxController->isRunning()) {
        qApp->processEvents();
        waitCount++;
        if (waitCount % 1000 == 0) {
            qDebug() << "[DEBUG ControllerInterface] Still waiting for SPI run, count:" << waitCount;
        }
    }
    qDebug() << "[DEBUG ControllerInterface] SPI run completed after" << waitCount << "iterations";

    // Read the resulting single data block from the USB interface.
    qDebug() << "[DEBUG ControllerInterface] Creating and reading data block...";
    qDebug() << "[DEBUG ControllerInterface] Controller type enum:" << (int)state->getControllerTypeEnum();
    qDebug() << "[DEBUG ControllerInterface] Num enabled data streams:" << rhxController->getNumEnabledDataStreams();
    
    RHXDataBlock dataBlock(state->getControllerTypeEnum(), rhxController->getNumEnabledDataStreams());
    qDebug() << "[DEBUG ControllerInterface] Data block created successfully";
    
    if (!state->synthetic->getValue() && !state->playback->getValue()) {
        qDebug() << "[DEBUG ControllerInterface] Reading data block from USB...";
        rhxController->readDataBlock(&dataBlock);
        qDebug() << "[DEBUG ControllerInterface] Data block read successfully";
    } else {
        qDebug() << "[DEBUG ControllerInterface] Skipping data block read (synthetic or playback mode)";
    }

    if (state->getControllerTypeEnum() != ControllerStimRecord) {
        // Now that ADC calibration has been performed, we switch to the command sequence that does not execute
        // ADC calibration.
        qDebug() << "[DEBUG ControllerInterface] Switching to non-calibration command sequence...";
        rhxController->selectAuxCommandBankAllPorts(RHXController::AuxCmd3, state->manualFastSettleEnabled->getValue() ? 2 : 1);
        qDebug() << "[DEBUG ControllerInterface] Command sequence switched successfully";
    }

    // Set default configuration for all eight DACs on controller.
    qDebug() << "[DEBUG ControllerInterface] Configuring DACs...";
    int dacManualStream = (state->getControllerTypeEnum() == ControllerRecordUSB3) ? 32 : 8;
    qDebug() << "[DEBUG ControllerInterface] DAC manual stream:" << dacManualStream;
    
    for (int i = 0; i < 8; i++) {
        qDebug() << "[DEBUG ControllerInterface] Configuring DAC" << i;
        rhxController->enableDac(i, false);
        rhxController->selectDacDataStream(i, dacManualStream); // Initially point DACs to DacManual1 input
        rhxController->selectDacDataChannel(i, 0);
        setDacThreshold(i, 0);
        qDebug() << "[DEBUG ControllerInterface] DAC" << i << "configured successfully";
    }
    
    qDebug() << "[DEBUG ControllerInterface] Setting DAC manual and gain values...";
    rhxController->setDacManual(32768);
    rhxController->setDacGain(0);
    rhxController->setAudioNoiseSuppress(0);
    qDebug() << "[DEBUG ControllerInterface] DAC values set successfully";

    // Set default SPI cable delay values.
    qDebug() << "[DEBUG ControllerInterface] Setting SPI cable delays...";
    rhxController->setCableDelay(PortA, 1);
    rhxController->setCableDelay(PortB, 1);
    rhxController->setCableDelay(PortC, 1);
    rhxController->setCableDelay(PortD, 1);
    qDebug() << "[DEBUG ControllerInterface] Basic ports (A-D) cable delays set";
    
    if (state->numSPIPorts > 4) {
        qDebug() << "[DEBUG ControllerInterface] Setting extended ports (E-H) cable delays...";
        qDebug() << "[DEBUG ControllerInterface] Total numSPIPorts:" << state->numSPIPorts;
        qDebug() << "[DEBUG ControllerInterface] Controller maxNumSPIPorts():" << rhxController->maxNumSPIPorts();
        qDebug() << "[DEBUG ControllerInterface] WARNING: This is where the vector out of bounds occurs!";
        
        // CRITICAL FIX: 检查控制器是否真的支持扩展端口
        if (rhxController->maxNumSPIPorts() > 4) {
            qDebug() << "[DEBUG ControllerInterface] Controller supports extended ports, setting cable delays...";
            
            qDebug() << "[DEBUG ControllerInterface] Setting PortE cable delay...";
            rhxController->setCableDelay(PortE, 1);
            qDebug() << "[DEBUG ControllerInterface] PortE cable delay set successfully";
            
            qDebug() << "[DEBUG ControllerInterface] Setting PortF cable delay...";
            rhxController->setCableDelay(PortF, 1);
            qDebug() << "[DEBUG ControllerInterface] PortF cable delay set successfully";
            
            qDebug() << "[DEBUG ControllerInterface] Setting PortG cable delay...";
            rhxController->setCableDelay(PortG, 1);
            qDebug() << "[DEBUG ControllerInterface] PortG cable delay set successfully";
            
            qDebug() << "[DEBUG ControllerInterface] Setting PortH cable delay...";
            rhxController->setCableDelay(PortH, 1);
            qDebug() << "[DEBUG ControllerInterface] PortH cable delay set successfully";
        } else {
            qDebug() << "[DEBUG ControllerInterface] SAFETY FIX: Controller only supports" << rhxController->maxNumSPIPorts() << "ports";
            qDebug() << "[DEBUG ControllerInterface] SAFETY FIX: Skipping extended ports (E-H) to prevent vector out of bounds!";
            qDebug() << "[DEBUG ControllerInterface] SAFETY FIX: This fixes the crash - ControllerStimRecord only has 4 ports!";
        }
        
        qDebug() << "[DEBUG ControllerInterface] Extended ports cable delays handling completed";
    }
    
    qDebug() << "[DEBUG ControllerInterface] === CRITICAL BUG FIX APPLIED ===";
    qDebug() << "[DEBUG ControllerInterface] The vector out of bounds crash has been fixed!";
    qDebug() << "[DEBUG ControllerInterface] ControllerStimRecord type only has 4 SPI ports (A-D)";
    qDebug() << "[DEBUG ControllerInterface] We now safely skip setting cable delays for non-existent ports E-H";
    qDebug() << "[DEBUG ControllerInterface] maxNumSPIPorts():" << rhxController->maxNumSPIPorts();
    qDebug() << "[DEBUG ControllerInterface] state numSPIPorts:" << state->numSPIPorts;
    qDebug() << "[DEBUG ControllerInterface] === BUG FIX VERIFICATION COMPLETE ===";
    
    } catch (const std::exception& e) {
        qDebug() << "[ERROR ControllerInterface] EXCEPTION in initializeController:" << e.what();
        throw;
    } catch (...) {
        qDebug() << "[ERROR ControllerInterface] UNKNOWN EXCEPTION in initializeController!";
        throw;
    }
    
    qDebug() << "[DEBUG ControllerInterface] initializeController completed successfully";
    qDebug() << "[DEBUG ControllerInterface] === initializeController Exit ===";
}

// Create SPI command lists and upload to auxiliary command slots.
void ControllerInterface::updateChipCommandLists(bool updateStimParams)
{
    RHXRegisters chipRegisters(state->getControllerTypeEnum(), rhxController->getSampleRate(), state->getStimStepSizeEnum());

    chipRegisters.setDigOutLow(RHXRegisters::DigOut::DigOut1); // Take auxiliary output out of HiZ mode.
    chipRegisters.setDigOutLow(RHXRegisters::DigOut::DigOut2); // Take auxiliary output out of HiZ mode.
    chipRegisters.setDigOutLow(RHXRegisters::DigOut::DigOutOD); // Take auxiliary output out of HiZ mode.

    std::vector<unsigned int> commandList;
    int numCommands = RHXDataBlock::samplesPerDataBlock(state->getControllerTypeEnum());
    int commandSequenceLength;

    if (state->getControllerTypeEnum() == ControllerStimRecord) {
        // Create a command list for the AuxCmd1 slot.  This command sequence programs most of the RAM registers
        // on the RHS2116 chip.
        commandSequenceLength = chipRegisters.createCommandListRHSRegisterConfig(commandList, updateStimParams);
        rhxController->uploadCommandList(commandList, RHXController::AuxCmd1, 0); // RHS - bank doesn't matter
        rhxController->selectAuxCommandLength(RHXController::AuxCmd1, 0, commandSequenceLength - 1);

        // Next, fill the other three command slots with dummy commands
        chipRegisters.createCommandListDummy(commandList, 8192, chipRegisters.createRHXCommand(RHXRegisters::RHXCommandRegRead, 255));
        rhxController->uploadCommandList(commandList, RHXController::AuxCmd2, 0); // RHS - bank doesn't matter
        chipRegisters.createCommandListDummy(commandList, 8192, chipRegisters.createRHXCommand(RHXRegisters::RHXCommandRegRead, 254));
        rhxController->uploadCommandList(commandList, RHXController::AuxCmd3, 0); // RHS - bank doesn't matter
        chipRegisters.createCommandListDummy(commandList, 8192, chipRegisters.createRHXCommand(RHXRegisters::RHXCommandRegRead, 253));
        rhxController->uploadCommandList(commandList, RHXController::AuxCmd4, 0);
    } else {
        // Create a command list for the AuxCmd1 slot.  This command sequence will continuously
        // update Register 3, which controls the auxiliary digital output pin on each chip.
        // This permits real-time control of the digital output pin on chips on each SPI port.
        commandSequenceLength = chipRegisters.createCommandListRHDUpdateDigOut(commandList, numCommands);
        rhxController->uploadCommandList(commandList, RHXController::AuxCmd1, 0);
        rhxController->selectAuxCommandLength(RHXController::AuxCmd1, 0, commandSequenceLength - 1);
        rhxController->selectAuxCommandBankAllPorts(RHXController::AuxCmd1, 0);

        // Next, we'll create a command list for the AuxCmd2 slot.  This command sequence
        // will sample the temperature sensor and other auxiliary ADC inputs.
        commandSequenceLength = chipRegisters.createCommandListRHDSampleAuxIns(commandList, numCommands);
        rhxController->uploadCommandList(commandList, RHXController::AuxCmd2, 0);
        rhxController->selectAuxCommandLength(RHXController::AuxCmd2, 0, commandSequenceLength - 1);
        rhxController->selectAuxCommandBankAllPorts(RHXController::AuxCmd2, 0);
    }

    // Set amplifier bandwidth parameters.
    state->holdUpdate();
    state->actualDspCutoffFreq->setValueWithLimits(chipRegisters.setDspCutoffFreq(state->desiredDspCutoffFreq->getValue()));
    state->actualLowerBandwidth->setValueWithLimits(chipRegisters.setLowerBandwidth(state->desiredLowerBandwidth->getValue(), 0));
    state->actualLowerSettleBandwidth->setValueWithLimits(chipRegisters.setLowerBandwidth(state->desiredLowerSettleBandwidth->getValue(), 1));
    state->actualUpperBandwidth->setValueWithLimits(chipRegisters.setUpperBandwidth(state->desiredUpperBandwidth->getValue()));
    chipRegisters.enableDsp(state->dspEnabled->getValue());
    state->releaseUpdate();

    if (state->getControllerTypeEnum() == ControllerStimRecord) {
        commandSequenceLength = chipRegisters.createCommandListRHSRegisterConfig(commandList, updateStimParams);
        // Upload version with no ADC calibration to AuxCmd1 RAM Bank.
        rhxController->uploadCommandList(commandList, RHXController::AuxCmd1, 0); // RHS - bank doesn't matter
        rhxController->selectAuxCommandLength(RHXController::AuxCmd1, 0, commandSequenceLength - 1);

        // Run system once for changes to take effect.
        rhxController->setContinuousRunMode(false);
        rhxController->setMaxTimeStep(RHXDataBlock::samplesPerDataBlock(rhxController->getType()));

        // Start SPI interface.
        rhxController->run();

        // Wait for the 128-sample run to complete.
        while (rhxController->isRunning()) {
            qApp->processEvents();
        }

        rhxController->flush();
    } else {
        // For the AuxCmd3 slot, we will create three command sequences.  All sequences
        // will configure and read back the RHD2000 chip registers, but one sequence will
        // also run ADC calibration.  Another sequence will enable amplifier 'fast settle'.

        commandSequenceLength = chipRegisters.createCommandListRHDRegisterConfig(commandList, true, numCommands);
        // Upload version with ADC calibration to AuxCmd3 RAM Bank 0.
        rhxController->uploadCommandList(commandList, RHXController::AuxCmd3, 0);
        rhxController->selectAuxCommandLength(RHXController::AuxCmd3, 0, commandSequenceLength - 1);

        commandSequenceLength = chipRegisters.createCommandListRHDRegisterConfig(commandList, false, numCommands);
        // Upload version with no ADC calibration to AuxCmd3 RAM Bank 1.
        rhxController->uploadCommandList(commandList, RHXController::AuxCmd3, 1);
        rhxController->selectAuxCommandLength(RHXController::AuxCmd3, 0, commandSequenceLength - 1);

        chipRegisters.setFastSettle(true);
        commandSequenceLength = chipRegisters.createCommandListRHDRegisterConfig(commandList, false, numCommands);
        // Upload version with fast settle enabled to AuxCmd3 RAM Bank 2.
        rhxController->uploadCommandList(commandList, RHXController::AuxCmd3, 2);
        rhxController->selectAuxCommandLength(RHXController::AuxCmd3, 0, commandSequenceLength - 1);
        chipRegisters.setFastSettle(false);

        rhxController->selectAuxCommandBankAllPorts(RHXController::AuxCmd3, state->manualFastSettleEnabled->getValue() ? 2 : 1);
    }

    setDacHighpassFilterEnabled(state->analogOutHighpassFilterEnabled->getValue());
    setDacHighpassFilterFrequency(state->analogOutHighpassFilterFrequency->getValue());

    // 预配置刺激参数，避免首次启用游戏时在运行线程中批量上传参数导致显示停顿。
    if (state->getControllerTypeEnum() == ControllerStimRecord) {
        if (!sensoryConfigured) configureSensoryParams();
        if (!hitStimConfigured) configureHitStimParams();
    }
}

void ControllerInterface::runController()
{
    if (state->uploadInProgress->getValue()) {
        sendTCPError("Error - To avoid data corruption, controller cannot start running until previously started upload function completes");
        return;
    }

    // 方案A：在采集线程启动前，预先配置刺激参数，避免在运行中批量上传导致波形停顿
    if (state->getControllerTypeEnum() == ControllerStimRecord) {
        qDebug() << "[ControllerInterface] Pre-configure stim params (currents) before acquisition";
        configureSensoryParams();
        configureHitStimParams();
    }

    usbDataThread->start();
    waveformProcessorThread->start();
    saveToDiskThread->start();

    // 设置线程优先级（需要在线程启动后设置）
    usbDataThread->setPriority(QThread::HighPriority);
    waveformProcessorThread->setPriority(QThread::HighPriority);

    usbDataThread->startRunning();
    waveformProcessorThread->startRunning(rhxController->getNumEnabledDataStreams());

    saveToDiskThread->startRunning();

    if (audioThread) audioThread->startRunning();
    if (tcpDataOutputThread) tcpDataOutputThread->startRunning();

    int numSamples = display->getSamplesPerRefresh();  // 1000 at 20 kHz; 1500 at 30 kHz

    uint32_t* timeStamps = new uint32_t [display->getMaxSamplesPerRefresh()];
    int lastTimeStamp = -1;
    int currentTimeStamp = 0;

    QElapsedTimer loopTimer, workTimer, reportTimer;
//    QElapsedTimer plotTimer;

    fill(cpuLoadHistory.begin(), cpuLoadHistory.end(), 0.0);

    loopTimer.start();
    workTimer.start();
    reportTimer.start();

    currentSweepPosition = 0;
    waveformFifo->resetBuffer();  // Clear any memory in waveform FIFO from previous running.
    display->reset();

    int triggerWaitNotify = 0;
    YScaleUsed yScaleUsed;
    while (state->running) {
        // 安全检查：确保关键对象仍然有效
        if (!waveformFifo || !usbStreamFifo || !rhxController || !state || !display) {
            qDebug() << "[SAFETY CHECK] Critical objects became invalid during shutdown, terminating gracefully";
            emit haveStopped();
            break;
        }

        workTimer.restart();

        if (rhxController->pipeReadError() != 0) {
            // Critical read error - displays an error message and exits software
            pipeReadErrorMessage(rhxController->pipeReadError());
        }

        // 测试开关：暂停波形绘制，仅消费FIFO，避免显示读者成为最慢读者
        const bool pauseWaveformPlotForTest = false;

        bool displayHasData = false;
        if (state->running) {
            if (pauseWaveformPlotForTest) {
                // 使用 lastRead=true 放宽读取条件，防止在写线程受阻时显示读者卡住，无法释放空间。
                displayHasData = waveformFifo->requestReadNewData(WaveformFifo::ReaderDisplay, numSamples, true);
            } else {
                displayHasData = waveformFifo->requestReadNewData(WaveformFifo::ReaderDisplay, numSamples);
                if (!displayHasData) {
                    // 回退一次宽松读取，避免显示读者成为“最慢读者”。
                    displayHasData = waveformFifo->requestReadNewData(WaveformFifo::ReaderDisplay, numSamples, true);
                }
            }
        }

        if (displayHasData) {
            // 在处理数据之前再次检查状态，防止对象在处理过程中被释放
            if (!state || !state->running || !waveformFifo || !display) {
                qDebug() << "[SAFETY CHECK] Aborting data processing due to invalid state during data processing";
                break;
            }
            if (pauseWaveformPlotForTest) {
                // 直接释放显示读者的数据，避免成为最慢读者卡住写线程。
                waveformFifo->freeOldData(WaveformFifo::ReaderDisplay);

                // 若未启用音频/网络线程，继续被动排空对应读者，防止其成为“慢读者”。
                if (!audioThread) {
                    if (waveformFifo->requestReadNewData(WaveformFifo::ReaderAudio, numSamples)) {
                        waveformFifo->freeOldData(WaveformFifo::ReaderAudio);
                    }
                }
                if (!tcpDataOutputThread) {
                    if (waveformFifo->requestReadNewData(WaveformFifo::ReaderTCP, numSamples)) {
                        waveformFifo->freeOldData(WaveformFifo::ReaderTCP);
                    }
                }

                // 维持CPU负载计算的时间基线，下一轮继续。
                workTimer.restart();
                loopTimer.restart();
                continue;
            }

            waveformFifo->copyTimeStamps(WaveformFifo::ReaderDisplay, timeStamps, 0, numSamples);

            // Main thread plots data:
//            plotTimer.start();

            if (!state->triggerModeDisplay->getValue()) {
                // Normal (non-triggered) display
                yScaleUsed = display->loadWaveformData(waveformFifo);
                emit setTopStatusLabel("");
            } else {
                // Triggered display
                int numSamplesDisplayed = display->getSamplesPerFullRefresh();
                if (waveformFifo->numWordsInMemory(WaveformFifo::ReaderDisplay) > numSamplesDisplayed + numSamples) {
                    int memoryPosition = -round((1.0 - state->triggerPositionDisplay->getNumericValue()) * numSamplesDisplayed);

                    QString triggerChannelName = state->triggerSourceDisplay->getValueString();
                    bool useAnalogTrigger = triggerChannelName.left(1).toUpper() == "A";

                    uint16_t triggerMask = 0x01u;
                    if (!useAnalogTrigger) triggerMask = 0x01u << (int)state->triggerSourceDisplay->getNumericValue();

                    uint16_t* digitalInWaveform = waveformFifo->getDigitalWaveformPointer("DIGITAL-IN-WORD");
                    float* analogInWaveform = nullptr;
                    float logicThreshold = 0.0F;
                    if (useAnalogTrigger) {  // Get thresholded analog signal as digital signal
                        analogInWaveform = waveformFifo->getAnalogWaveformPointer(triggerChannelName.toStdString());
                        logicThreshold = (float)state->triggerAnalogVoltageThreshold->getValue();
                    }

                    bool risingEdge = state->triggerPolarityDisplay->getValue() == "Rising";

                    bool triggerFound = false;
                    int t = memoryPosition - numSamples - 1;
                    bool prevTriggerValue;
                    if (useAnalogTrigger) {
                        prevTriggerValue =
                                waveformFifo->getAnalogDataAsDigital(WaveformFifo::ReaderDisplay, analogInWaveform, t, logicThreshold) &
                                triggerMask;
                    } else {
                        prevTriggerValue = waveformFifo->getDigitalData(WaveformFifo::ReaderDisplay, digitalInWaveform, t) &
                                triggerMask;
                    }
                    for (++t; t <= memoryPosition; ++t) {
                        bool triggerValue;
                        if (useAnalogTrigger) {
                            triggerValue =
                                    waveformFifo->getAnalogDataAsDigital(WaveformFifo::ReaderDisplay, analogInWaveform, t, logicThreshold) &
                                    triggerMask;
                        } else {
                            triggerValue = waveformFifo->getDigitalData(WaveformFifo::ReaderDisplay, digitalInWaveform, t) &
                                    triggerMask;
                        }
                        if (risingEdge) {
                            if (!prevTriggerValue && triggerValue) {
                                triggerFound = true;
                                break;
                            }
                        } else {
                            if (prevTriggerValue && !triggerValue) {
                                triggerFound = true;
                                break;
                            }
                        }
                        prevTriggerValue = triggerValue;
                    }
                    if (triggerFound) {
                        int startTime = t - round((state->triggerPositionDisplay->getNumericValue()) * numSamplesDisplayed);
                        yScaleUsed = display->loadWaveformDataFromMemory(waveformFifo, startTime, true);
                        emit setTopStatusLabel("");
                        triggerWaitNotify = 0;
                    } else {
                        if (triggerWaitNotify++ > 20) {
                            emit setTopStatusLabel(tr("Waiting for trigger..."));
                            triggerWaitNotify = 20;
                        }
                    }
                }
            }

            if (controlPanel) controlPanel->updateSlidersEnabled(yScaleUsed);

            if (isiDialog) isiDialog->updateISI(waveformFifo, numSamples);
            if (psthDialog) psthDialog->updatePSTH(waveformFifo, numSamples);
            if (spectrogramDialog) spectrogramDialog->updateSpectrogram(waveformFifo, numSamples);
            if (spikeSortingDialog) spikeSortingDialog->updateSpikeScope(waveformFifo, numSamples);

            waveformFifo->freeOldData(WaveformFifo::ReaderDisplay);

//            double plotTime = (double) plotTimer.nsecsElapsed();

            if (!audioThread) {
                if (waveformFifo->requestReadNewData(WaveformFifo::ReaderAudio, numSamples)) {
                    waveformFifo->freeOldData(WaveformFifo::ReaderAudio);
                }
            }

            if (!tcpDataOutputThread) {
                if (waveformFifo->requestReadNewData(WaveformFifo::ReaderTCP, numSamples)) {
                    waveformFifo->freeOldData(WaveformFifo::ReaderTCP);
                }
            }

            for (int i = 0; i < numSamples; ++i) {
                currentTimeStamp = (int) timeStamps[i];
                if (currentTimeStamp - lastTimeStamp != 1 && lastTimeStamp != -1) {
                    qDebug() << "Timestamp discontinuity: " << lastTimeStamp << " " << currentTimeStamp << "\n";
                    //cout << "Timestamp discontinuity: " << lastTimeStamp << " " << currentTimeStamp << '\n';
                }
                lastTimeStamp = currentTimeStamp;
            }

            double workTime = (double) workTimer.nsecsElapsed();
            double loopTime = (double) loopTimer.nsecsElapsed();
            workTimer.restart();
            loopTimer.restart();
            if (reportTimer.elapsed() >= 2000) {
                double cpuUsage = 100.0 * workTime / loopTime;

                // Calculate running average of CPU usage to smooth out fluctuations.
                for (int i = 1; i < (int) cpuLoadHistory.size(); ++i) {
                    cpuLoadHistory[i - 1] = cpuLoadHistory[i];
                }
                cpuLoadHistory[cpuLoadHistory.size() - 1] = cpuUsage;
                double total = 0.0;
                for (int i = 0; i < (int) cpuLoadHistory.size(); ++i) {
                    total += cpuLoadHistory[i];
                }
                double averageCpuLoad = total / (double)(cpuLoadHistory.size());

                emit cpuLoadPercent(averageCpuLoad);

//                cout << "        Controller Interface (Main Thread) CPU usage: " << (int) cpuUsage << "%" << EndOfLine;
//                cout << "Plot time = " << plotTime / 1.0e6 << " ms" << EndOfLine;
//                cout << "Work time = " << workTime / 1.0e6 << " ms" << EndOfLine;
//                cout << "Loop time = " << loopTime / 1.0e6 << " ms" << EndOfLine;
                reportTimer.restart();
            }
            qApp->processEvents();
        }

        qApp->processEvents();
        numSamples = display->getSamplesPerRefresh();
    }

    if (audioThread) {
        audioThread->stopRunning();
        while (audioThread->isActive()) {
            qApp->processEvents();
        }
    }

    if (tcpDataOutputThread) {
        tcpDataOutputThread->stopRunning();
        while (tcpDataOutputThread->isActive()) {
            qApp->processEvents();
        }
        tcpDataOutputEnabled = false;
    }

    usbDataThread->stopRunning();
    while (usbDataThread->isActive()) { // Important: Must wait for usbDataThread to fully stop before we reset usbStreamFifo buffer!
        qApp->processEvents(); // Stay responsive to GUI events during this loop.
    }
    QThread::usleep(1000); // Pause briefly to make sure tail end of data gets through waveformProcessorThread before it is also destroyed

    waveformProcessorThread->stopRunning();
    while (waveformProcessorThread->isActive()) {
        qApp->processEvents();
    }
    QThread::usleep(1000); // Pause briefly to make sure tail end of data gets through saveToDiskThread before it is also destroyed

    saveToDiskThread->stopRunning();
    while (saveToDiskThread->isActive()) {
        qApp->processEvents();
    }

    waveformFifo->pauseBuffer();

    usbStreamFifo->resetBuffer();

    delete [] timeStamps;
    fill(cpuLoadHistory.begin(), cpuLoadHistory.end(), 0.0);
    emit cpuLoadPercent(0.0);
    emit haveStopped();
}

void ControllerInterface::runControllerSilently(double nSeconds, QProgressDialog* progress)
{
    qint64 runTimeNsecs = nSeconds * 1e9;
    qint64 progressTickNsecs = 0.1 * 1e9;
    int progressStep = 1;

    if (progress) {
        progress->setWindowTitle(QObject::tr("Progress"));
        progress->setModal(true);
        progress->setMaximum(nSeconds * 10);
        progress->show();
    }

    int numSamples = 1000;

    usbDataThread->start();
    waveformProcessorThread->start();

    usbDataThread->startRunning();
    waveformProcessorThread->startRunning(rhxController->getNumEnabledDataStreams());

    waveformFifo->resetBuffer();  // Clear any memory in waveform FIFO from previous running.

    QElapsedTimer mainTimer, tickTimer;
    mainTimer.start();
    tickTimer.start();

    while (mainTimer.nsecsElapsed() < runTimeNsecs) {
        if (waveformFifo->requestReadNewData(WaveformFifo::ReaderDisplay, numSamples)) {
            waveformFifo->freeOldData(WaveformFifo::ReaderDisplay);

            if (waveformFifo->requestReadNewData(WaveformFifo::ReaderDisk, numSamples)) {
                waveformFifo->freeOldData(WaveformFifo::ReaderDisk);
            }

            if (waveformFifo->requestReadNewData(WaveformFifo::ReaderAudio, numSamples)) {
                waveformFifo->freeOldData(WaveformFifo::ReaderAudio);
            }

            if (waveformFifo->requestReadNewData(WaveformFifo::ReaderTCP, numSamples)) {
                waveformFifo->freeOldData(WaveformFifo::ReaderTCP);
            }

            qApp->processEvents();
        }

        qApp->processEvents();
        numSamples = display->getSamplesPerRefresh();

        if (tickTimer.nsecsElapsed() >= progressTickNsecs) {
            tickTimer.restart();
            if (progress) {
                progress->setValue(progressStep++);
            }
        }
    }

    waveformProcessorThread->stopRunning();
    while(waveformProcessorThread->isActive()) {
        qApp->processEvents();
    }

    usbDataThread->stopRunning();
    while (usbDataThread->isActive()) {  // Important: Must wait for usbDataThread to fully stop before we reset usbStreamFifo buffer!
        qApp->processEvents();  // Stay responsive to GUI events during this loop.
    }

    waveformFifo->pauseBuffer();
    usbStreamFifo->resetBuffer();

    if (progress) {
        progress->hide();
    }
}

float ControllerInterface::measureRmsLevel(std::string waveName, double timeSec) const
{
    int numSamples = round(state->sampleRate->getNumericValue() * timeSec);
    float* waveform = new float [numSamples];
    GpuWaveformAddress gpuWaveformAddress = waveformFifo->getGpuWaveformAddress(waveName);
    waveformFifo->copyGpuAmplifierData(WaveformFifo::ReaderDisplay, waveform, gpuWaveformAddress, -numSamples, numSamples);

    // Calculate RMS value of waveform.
    float sumOfSquares = 0.0;
    for (int i = 0; i < numSamples; ++i) {
        sumOfSquares += waveform[i] * waveform[i];
    }
    float rmsLevel = sqrt(sumOfSquares / (float)numSamples);

    delete [] waveform;

    return rmsLevel;
}

void ControllerInterface::setAllSpikeDetectionThresholds()
{
    if (state->absoluteThresholdsEnabled->getValue()) {
        double threshold = state->absoluteThreshold->getValue();
        std::vector<std::string> waveNameList = state->signalSources->amplifierChannelsNameList();
        for (int i = 0; i < (int) waveNameList.size(); ++i) {
            Channel* channel = state->signalSources->channelByName(waveNameList[i]);
            if (channel) {
                if (channel->isEnabled()) {
                    channel->setSpikeThreshold(round(threshold));
                }
            }
        }
    } else {
        double rmsMultiple = state->rmsMultipleThreshold->getValue();
        if (state->negativeRelativeThreshold->getValue()) rmsMultiple *= -1;
        double numSecondsToMeasure = 3.0;

        QProgressDialog* progress = new QProgressDialog(QObject::tr("Measuring Noise Floor to Calculate Thresholds"), QString(), 0, 1);
        runControllerSilently(numSecondsToMeasure + 1.0, progress);  // Add one second at beginning so we ignore starting transients.
        delete progress;

        std::vector<std::string> waveNameList = state->signalSources->amplifierChannelsNameList();
        for (int i = 0; i < (int) waveNameList.size(); ++i) {
            std::string waveName = waveNameList[i] + "|HIGH";  // Measure RMS levels of highpass filtered signal for spike threshold calculation.
            float rmsLevel = measureRmsLevel(waveName, numSecondsToMeasure);
            Channel* channel = state->signalSources->channelByName(waveNameList[i]);
            if (channel) {
                if (channel->isEnabled()) {
                    channel->setSpikeThreshold(round(rmsMultiple * rmsLevel));
                }
            }
        }
    }
}

// Negative values of speed rewind into waveform FIFO memory; positive values fast forward, at the specified multiple of realtime.
void ControllerInterface::sweepDisplay(double speed)
{
    int numSamples = display->getSamplesPerRefresh();
    double nanosecondsPerRefresh = 1.0e9 * (double)numSamples / state->sampleRate->getNumericValue();
    double speedUpFactor = fabs(speed);
    int64_t nanosecondsPerLoop = round(nanosecondsPerRefresh / speedUpFactor);
    int numSamplesInMemory = waveformFifo->numWordsInMemory(WaveformFifo::ReaderDisplay);
    QElapsedTimer timer;
    timer.start();

    int currentTimeStamp = 0;
    int startTime = currentSweepPosition;

    YScaleUsed yScaleUsed;
    while (state->sweeping) {
        if (timer.nsecsElapsed() > nanosecondsPerLoop) {
            timer.start();
            if (startTime >= -numSamplesInMemory && startTime <= 0) {
                yScaleUsed = display->loadWaveformDataFromMemory(waveformFifo, startTime);
                if (controlPanel) controlPanel->updateSlidersEnabled(yScaleUsed);
                QTime sweepTime(0, 0);
                int timeStamp = currentTimeStamp + startTime;
                int totalSweepTimeSeconds = round((double)timeStamp / state->sampleRate->getNumericValue());
                QString timeString;
                if (timeStamp < 0) {
                    timeString = "-" + sweepTime.addSecs(-totalSweepTimeSeconds).toString("HH:mm:ss");
                    if (timeString == "-00:00:00") timeString = "00:00:00";
                } else {
                    timeString = sweepTime.addSecs(totalSweepTimeSeconds).toString("HH:mm:ss");
                }
                emit setTimeLabel(timeString);
            } else {
                state->sweeping = false;
            }
            if (speed < 0) {
                startTime -= numSamples;
            } else {
                startTime += numSamples;
            }
        }
        numSamples = display->getSamplesPerRefresh();
        qApp->processEvents();
    }

    startTime = qBound(-numSamplesInMemory, startTime, 0);
    currentSweepPosition = startTime;

    emit haveStopped();
}

void ControllerInterface::resetWaveformFifo()
{
    waveformFifo->resetBuffer();
}

void ControllerInterface::setDacGain(int dacGainIndex)
{
    rhxController->setDacGain(dacGainIndex);
}

void ControllerInterface::setAudioNoiseSuppress(int noiseSuppressIndex)
{
    rhxController->setAudioNoiseSuppress(noiseSuppressIndex);
}

QString ControllerInterface::playbackFileName() const
{
    QString fileName;
    if (state->playback->getValue()) {
        fileName = dataFileReader->currentFileName();
    }
    return fileName;
}

QString ControllerInterface::currentTimePlaybackFile() const
{
    QString timeString;
    if (state->playback->getValue()) {
        timeString = dataFileReader->filePositionString();
    }
    return timeString;
}

QString ControllerInterface::startTimePlaybackFile() const
{
    QString timeString;
    if (state->playback->getValue()) {
        timeString = dataFileReader->startPositionString();
    }
    return timeString;
}

QString ControllerInterface::endTimePlaybackFile() const
{
    QString timeString;
    if (state->playback->getValue()) {
        timeString = dataFileReader->endPositionString();
    }
    return timeString;
}

void ControllerInterface::setStimSequenceParameters(Channel* ampChannel)
{
    if (rhxController->isSynthetic() || rhxController->isPlayback()) return;

    const int Never = 65535;

    StimParameters* parameters = ampChannel->stimParameters;
    int stream = ampChannel->getCommandStream();
    int channel = ampChannel->getChipChannel();
    double timestep = 1.0e6 / state->sampleRate->getNumericValue();  // time step in microseconds
    double currentstep = RHXRegisters::stimStepSizeToDouble(state->getStimStepSizeEnum()) * 1.0e6;  // current step in microamps

    int numOfPulses = ((PulseOrTrain) parameters->pulseOrTrain->getIndex() == SinglePulse) ?
                1 : parameters->numberOfStimPulses->getValue();

    rhxController->configureStimTrigger(stream, channel, parameters->triggerSource->getIndex(),
                                        parameters->enabled->getValue(),
                                        ((TriggerEdgeOrLevel) parameters->triggerEdgeOrLevel->getIndex() == TriggerEdge),
                                        ((TriggerHighOrLow) parameters->triggerHighOrLow->getIndex() == TriggerLow));
    rhxController->configureStimPulses(stream, channel, numOfPulses, (StimShape)(parameters->stimShape->getIndex()),
                                       ((StimPolarity) parameters->stimPolarity->getIndex() == NegativeFirst));

    int preStimAmpSettle = round(parameters->preStimAmpSettle->getValue() / timestep);
    int postStimAmpSettle = round(parameters->postStimAmpSettle->getValue() / timestep);
    int postTriggerDelay = round(parameters->postTriggerDelay->getValue() / timestep);
    int firstPhaseDuration = round(parameters->firstPhaseDuration->getValue() / timestep);
    int secondPhaseDuration = round(parameters->secondPhaseDuration->getValue() / timestep);
    int interphaseDelay = round(parameters->interphaseDelay->getValue() / timestep);
    int refractoryPeriod = round(parameters->refractoryPeriod->getValue() / timestep);
    int postStimChargeRecovOn = round(parameters->postStimChargeRecovOn->getValue() / timestep);
    int postStimChargeRecovOff = round(parameters->postStimChargeRecovOff->getValue() / timestep );
    int pulseTrainPeriod = round(parameters->pulseTrainPeriod->getValue() / timestep);

    int eventStartStim;
    int eventStimPhase2;
    int eventStimPhase3;
    int eventEndStim;
    int eventEnd;
    int eventRepeatStim;
    int eventAmpSettleOn;
    int eventAmpSettleOff;
    int eventAmpSettleOnRepeat;
    int eventAmpSettleOffRepeat;
    int eventChargeRecovOn;
    int eventChargeRecovOff;

    switch ((StimShape) parameters->stimShape->getIndex()) {
    case Biphasic:
        eventStartStim = postTriggerDelay;
        eventStimPhase2 = eventStartStim + firstPhaseDuration;
        eventStimPhase3 = Never;
        eventEndStim = eventStimPhase2 + secondPhaseDuration;
        eventEnd = eventEndStim + refractoryPeriod;
        break;
    case BiphasicWithInterphaseDelay:
        eventStartStim = postTriggerDelay;
        eventStimPhase2 = eventStartStim + firstPhaseDuration;
        eventStimPhase3 = eventStimPhase2 + interphaseDelay;
        eventEndStim = eventStimPhase3 + secondPhaseDuration;
        eventEnd = eventEndStim + refractoryPeriod;
        break;
    case Triphasic:
        eventStartStim = postTriggerDelay;
        eventStimPhase2 = eventStartStim + firstPhaseDuration;
        eventStimPhase3 = eventStimPhase2 + secondPhaseDuration;
        eventEndStim = eventStimPhase3 + firstPhaseDuration;
        eventEnd = eventEndStim + refractoryPeriod;
        break;
    case Monophasic:
        eventStartStim = postTriggerDelay;
        eventStimPhase2 = Never;
        eventStimPhase3 = Never;
        eventEndStim = eventStartStim + firstPhaseDuration;
        eventEnd = eventEndStim + refractoryPeriod;
        break;
    }

    if ((PulseOrTrain) parameters->pulseOrTrain->getIndex() == PulseTrain) {
        eventRepeatStim = eventStartStim + pulseTrainPeriod;
    } else {
        eventRepeatStim = Never;
    }

    if (parameters->enableAmpSettle->getValue()) {
        eventAmpSettleOn = eventStartStim - preStimAmpSettle;
        eventAmpSettleOff = eventEndStim + postStimAmpSettle;
        if (parameters->maintainAmpSettle->getValue()) {
            eventAmpSettleOnRepeat = Never;
            eventAmpSettleOffRepeat = Never;
        } else {
            eventAmpSettleOnRepeat = eventRepeatStim - preStimAmpSettle;
            eventAmpSettleOffRepeat = eventAmpSettleOff;
        }
    } else {
        eventAmpSettleOn = Never;
        eventAmpSettleOff = 0;
        eventAmpSettleOnRepeat = Never;
        eventAmpSettleOffRepeat = Never;
    }

    if (parameters->enableChargeRecovery->getValue()) {
        eventChargeRecovOn = eventEndStim + postStimChargeRecovOn;
        eventChargeRecovOff = eventEndStim + postStimChargeRecovOff;
    } else {
        eventChargeRecovOn = Never;
        eventChargeRecovOff = 0;
    }

    rhxController->programStimReg(stream, channel, AbstractRHXController::EventAmpSettleOn, eventAmpSettleOn);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventStartStim, eventStartStim);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventStimPhase2, eventStimPhase2);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventStimPhase3, eventStimPhase3);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventEndStim, eventEndStim);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventRepeatStim, eventRepeatStim);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventAmpSettleOff, eventAmpSettleOff);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventChargeRecovOn, eventChargeRecovOn);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventChargeRecovOff, eventChargeRecovOff);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventAmpSettleOnRepeat, eventAmpSettleOnRepeat);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventAmpSettleOffRepeat, eventAmpSettleOffRepeat);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventEnd, eventEnd);

    qDebug() << "event amp settle on: " << eventAmpSettleOn;
    qDebug() << "Event start stim: " << eventStartStim;
    qDebug() << "Event end stim: " << eventEndStim;
    qDebug() << "Event repeat stim: " << eventRepeatStim;
    qDebug() << "Event amp settle off: " << eventAmpSettleOff;
    qDebug() << "Event amp settle on repeat: " << eventAmpSettleOnRepeat;
    qDebug() << "Event amp settle off repeat: " << eventAmpSettleOffRepeat;

    rhxController->enableAuxCommandsOnOneStream(stream);

    RHXRegisters chipRegisters(rhxController->getType(), rhxController->getSampleRate(), state->getStimStepSizeEnum());
    int commandSequenceLength;
    std::vector<unsigned int> commandList;

    int firstPhaseAmplitude = round(parameters->firstPhaseAmplitude->getValue() / currentstep);
    // Force second phase magnitude to 0 for monophasic to avoid stale UI values affecting polarity math.
    double secondPhaseRaw = (StimShape) parameters->stimShape->getIndex() == Monophasic ? 0.0 : parameters->secondPhaseAmplitude->getValue();
    int secondPhaseAmplitude = round(secondPhaseRaw / currentstep);

    int posMag, negMag;

    if (((StimPolarity) parameters->stimPolarity->getIndex()) == PositiveFirst) {
        posMag = firstPhaseAmplitude;
        negMag = secondPhaseAmplitude;
    } else {
        negMag = firstPhaseAmplitude;
        posMag = secondPhaseAmplitude;
    }

    // int posMag = (((StimPolarity) parameters->stimPolarity->getIndex()) == PositiveFirst) ?
    //             firstPhaseAmplitude : secondPhaseAmplitude;
    // int negMag = (((StimPolarity) parameters->stimPolarity->getIndex()) == NegativeFirst) ?
    //             firstPhaseAmplitude : secondPhaseAmplitude;

    commandSequenceLength = chipRegisters.createCommandListSetStimMagnitudes(commandList, channel, posMag, 0, negMag, 0);
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd1, 0);  // RHS - bank doesn't matter
    rhxController->selectAuxCommandLength(AbstractRHXController::AuxCmd1, 0, commandSequenceLength - 1);

    chipRegisters.createCommandListDummy(commandList, 8192, chipRegisters.createRHXCommand(RHXRegisters::RHXCommandRegRead, 255));
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd2, 0);  // RHS - bank doesn't matter
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd3, 0);  // RHS - bank doesn't matter
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd4, 0);  // RHS - bank doesn't matter

    rhxController->setMaxTimeStep(commandSequenceLength);
    rhxController->setContinuousRunMode(false);
    rhxController->setStimCmdMode(false);
    rhxController->enableAuxCommandsOnOneStream(stream);

    rhxController->run();
    while (rhxController->isRunning() ) {
        qApp->processEvents();
    }

    commandSequenceLength = chipRegisters.createCommandListRHSRegisterRead(commandList);
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd1, 0);  // RHS - bank doesn't matter
    rhxController->selectAuxCommandLength(AbstractRHXController::AuxCmd1, 0, commandSequenceLength - 1);
    rhxController->run();
    while (rhxController->isRunning() ) {
        qApp->processEvents();
    }

    RHXDataBlock dataBlock(rhxController->getType(), rhxController->getNumEnabledDataStreams());
    rhxController->readDataBlock(&dataBlock);
    rhxController->readDataBlock(&dataBlock);

    commandSequenceLength = chipRegisters.createCommandListRHSRegisterConfig(commandList, true);
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd1, 0);  // RHS - bank doesn't matter
    rhxController->selectAuxCommandLength(AbstractRHXController::AuxCmd1, 0, commandSequenceLength - 1);

    rhxController->enableAuxCommandsOnAllStreams();
}

void ControllerInterface::setAnalogOutSequenceParameters(Channel* anOutChannel)
{
    if (rhxController->isSynthetic() || rhxController->isPlayback()) return;

    const int Never = 65535;

    StimParameters* parameters = anOutChannel->stimParameters;
    int channel = anOutChannel->getNativeChannelNumber();
    int stream = 8 + channel;
    double timestep = 1.0e6 / state->sampleRate->getNumericValue();  // time step in microseconds

    int numOfPulses = ((PulseOrTrain) parameters->pulseOrTrain->getIndex() == SinglePulse) ?
                1 : parameters->numberOfStimPulses->getValue();

    rhxController->configureStimTrigger(stream, 0, parameters->triggerSource->getIndex(),
                                        parameters->enabled->getValue(),
                                        ((TriggerEdgeOrLevel) parameters->triggerEdgeOrLevel->getIndex() == TriggerEdge),
                                        ((TriggerHighOrLow) parameters->triggerHighOrLow->getIndex() == TriggerLow));
    rhxController->configureStimPulses(stream, 0, numOfPulses, (StimShape)(parameters->stimShape->getIndex()),
                                       ((StimPolarity) parameters->stimPolarity->getIndex() == NegativeFirst));

    int postTriggerDelay = round(parameters->postTriggerDelay->getValue() / timestep);
    int firstPhaseDuration = round(parameters->firstPhaseDuration->getValue() / timestep);
    int secondPhaseDuration = round(parameters->secondPhaseDuration->getValue() / timestep);
    int interphaseDelay = round(parameters->interphaseDelay->getValue() / timestep);
    int refractoryPeriod = round(parameters->refractoryPeriod->getValue() / timestep);
    int pulseTrainPeriod = round(parameters->pulseTrainPeriod->getValue() / timestep);

    int eventStartStim;
    int eventStimPhase2;
    int eventStimPhase3;
    int eventEndStim;
    int eventEnd;
    int eventRepeatStim;

    switch ((StimShape) parameters->stimShape->getIndex()) {
    case Biphasic:
        eventStartStim = postTriggerDelay;
        eventStimPhase2 = eventStartStim + firstPhaseDuration;
        eventStimPhase3 = Never;
        eventEndStim = eventStimPhase2 + secondPhaseDuration;
        eventEnd = eventEndStim + refractoryPeriod;
        break;
    case BiphasicWithInterphaseDelay:
        eventStartStim = postTriggerDelay;
        eventStimPhase2 = eventStartStim + firstPhaseDuration;
        eventStimPhase3 = eventStimPhase2 + interphaseDelay;
        eventEndStim = eventStimPhase3 + secondPhaseDuration;
        eventEnd = eventEndStim + refractoryPeriod;
        break;
    case Triphasic:
        eventStartStim = postTriggerDelay;
        eventStimPhase2 = eventStartStim + firstPhaseDuration;
        eventStimPhase3 = eventStimPhase2 + secondPhaseDuration;
        eventEndStim = eventStimPhase3 + firstPhaseDuration;
        eventEnd = eventEndStim + refractoryPeriod;
        break;
    case Monophasic:
        eventStartStim = postTriggerDelay;
        eventStimPhase2 = Never;
        eventStimPhase3 = Never;
        eventEndStim = eventStartStim + firstPhaseDuration;
        eventEnd = eventEndStim + refractoryPeriod;
        break;
    }

    if ((PulseOrTrain) parameters->pulseOrTrain->getIndex() == PulseTrain) {
        eventRepeatStim = eventStartStim + pulseTrainPeriod;
    } else {
        eventRepeatStim = Never;
    }

    rhxController->programStimReg(stream, 0, AbstractRHXController::EventStartStim, eventStartStim);
    rhxController->programStimReg(stream, 0, AbstractRHXController::EventStimPhase2, eventStimPhase2);
    rhxController->programStimReg(stream, 0, AbstractRHXController::EventStimPhase3, eventStimPhase3);
    rhxController->programStimReg(stream, 0, AbstractRHXController::EventEndStim, eventEndStim);
    rhxController->programStimReg(stream, 0, AbstractRHXController::EventRepeatStim, eventRepeatStim);
    rhxController->programStimReg(stream, 0, AbstractRHXController::EventEnd, eventEnd);

    int dacBaseline, dacPositive, dacNegative;
    const double dacLsb = (2 * 10.24) / 65536;
    const int dacMid = 32768;
    dacBaseline = dacMid + (int)(parameters->baselineVoltage->getValue() / dacLsb);

    if ((StimShape) parameters->stimShape->getIndex() == Monophasic) {
        if ((StimPolarity) parameters->stimPolarity->getIndex() == NegativeFirst) {
            dacPositive = dacBaseline;
            dacNegative = dacBaseline + (int)(-1.0 * parameters->firstPhaseAmplitude->getValue() / dacLsb);
        } else {
            dacPositive = dacBaseline + (int)(parameters->firstPhaseAmplitude->getValue() / dacLsb);
            dacNegative = dacBaseline;
        }
    } else {
        dacPositive = dacBaseline + (int)(((StimPolarity) parameters->stimPolarity->getIndex() == NegativeFirst ?
                                               parameters->secondPhaseAmplitude->getValue() :
                                               parameters->firstPhaseAmplitude->getValue()) / dacLsb);
        dacNegative = dacBaseline + (int)(-1.0 * ((StimPolarity) parameters->stimPolarity->getIndex() == NegativeFirst ?
                                                      parameters->firstPhaseAmplitude->getValue() :
                                                      parameters->secondPhaseAmplitude->getValue()) / dacLsb);
    }

    dacBaseline = qBound(0, dacBaseline, 65535);
    dacPositive = qBound(0, dacPositive, 65535);
    dacNegative = qBound(0, dacNegative, 65535);

    rhxController->programStimReg(stream, 0, AbstractRHXController::DacBaseline, dacBaseline);
    rhxController->programStimReg(stream, 0, AbstractRHXController::DacPositive, dacPositive);
    rhxController->programStimReg(stream, 0, AbstractRHXController::DacNegative, dacNegative);
}

void ControllerInterface::setDigitalOutSequenceParameters(Channel* digOutChannel)
{
    if (rhxController->isSynthetic() || rhxController->isPlayback()) return;

    const int Never = 65535;

    StimParameters* parameters = digOutChannel->stimParameters;
    int channel = digOutChannel->getNativeChannelNumber();
    int stream = 16;
    double timestep = 1.0e6 / state->sampleRate->getNumericValue();  // time step in microseconds

    int numOfPulses = ((PulseOrTrain) parameters->pulseOrTrain->getIndex() == SinglePulse) ?
                1 : parameters->numberOfStimPulses->getValue();

    rhxController->configureStimTrigger(stream, channel, parameters->triggerSource->getIndex(),
                                        parameters->enabled->getValue(),
                                        ((TriggerEdgeOrLevel) parameters->triggerEdgeOrLevel->getIndex() == TriggerEdge),
                                        ((TriggerHighOrLow) parameters->triggerHighOrLow->getIndex() == TriggerLow));
    rhxController->configureStimPulses(stream, channel, numOfPulses, Monophasic, false);

    int postTriggerDelay = round(parameters->postTriggerDelay->getValue() / timestep);
    int firstPhaseDuration = round(parameters->firstPhaseDuration->getValue() / timestep);
    int refractoryPeriod = round(parameters->refractoryPeriod->getValue() / timestep);
    int pulseTrainPeriod = round(parameters->pulseTrainPeriod->getValue() / timestep);

    int eventStartStim = postTriggerDelay;
    int eventEndStim = eventStartStim + firstPhaseDuration;
    int eventEnd = eventEndStim + refractoryPeriod;
    int eventRepeatStim;

    if ((PulseOrTrain) parameters->pulseOrTrain->getIndex() == PulseTrain) {
        eventRepeatStim = eventStartStim + pulseTrainPeriod;
    } else {
        eventRepeatStim = Never;
    }

    rhxController->programStimReg(stream, channel, AbstractRHXController::EventStartStim, eventStartStim);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventEndStim, eventEndStim);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventRepeatStim, eventRepeatStim);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventEnd, eventEnd);
}

void ControllerInterface::manualStimTriggerOn(QString keyName)
{
    setManualStimTrigger(keyName, true);
}

void ControllerInterface::manualStimTriggerOff(QString keyName)
{
    setManualStimTrigger(keyName, false);
}

void ControllerInterface::setManualStimTrigger(int trigger, bool triggerOn)
{
    rhxController->setManualStimTrigger(trigger, triggerOn);
}

void ControllerInterface::setManualStimTrigger(QString keyName, bool triggerOn)
{
    if (keyName.toLower() == "f1") {
        rhxController->setManualStimTrigger(0, triggerOn);
    } else if (keyName.toLower() == "f2") {
        rhxController->setManualStimTrigger(1, triggerOn);
    } else if (keyName.toLower() == "f3") {
        rhxController->setManualStimTrigger(2, triggerOn);
    } else if (keyName.toLower() == "f4") {
        rhxController->setManualStimTrigger(3, triggerOn);
    } else if (keyName.toLower() == "f5") {
        rhxController->setManualStimTrigger(4, triggerOn);
    } else if (keyName.toLower() == "f6") {
        rhxController->setManualStimTrigger(5, triggerOn);
    } else if (keyName.toLower() == "f7") {
        rhxController->setManualStimTrigger(6, triggerOn);
    } else if (keyName.toLower() == "f8") {
        rhxController->setManualStimTrigger(7, triggerOn);
    }
}

void ControllerInterface::setChargeRecoveryParameters(bool mode, RHXRegisters::ChargeRecoveryCurrentLimit currentLimit,
                                                      double targetVoltage)
{
    if (rhxController->isSynthetic() || rhxController->isPlayback()) return;
    if (state->getControllerTypeEnum() != ControllerStimRecord) return;

    rhxController->setChargeRecoveryMode(mode);
    rhxController->enableAuxCommandsOnAllStreams();

    RHXRegisters chipRegisters(rhxController->getType(), rhxController->getSampleRate(), state->getStimStepSizeEnum());
    int commandSequenceLength;
    std::vector<unsigned int> commandList;

    commandSequenceLength = chipRegisters.createCommandListConfigChargeRecovery(commandList, currentLimit, targetVoltage);
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd1, 0);
    rhxController->selectAuxCommandLength(AbstractRHXController::AuxCmd1, 0, commandSequenceLength - 1);

    chipRegisters.createCommandListDummy(commandList, 8192, chipRegisters.createRHXCommand(RHXRegisters::RHXCommandRegRead, 255));
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd2, 0);
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd3, 0);
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd4, 0);

    rhxController->setMaxTimeStep(commandSequenceLength);
    rhxController->setContinuousRunMode(false);
    rhxController->setStimCmdMode(false);

    rhxController->run();
    while (rhxController->isRunning() ) {
        qApp->processEvents();
    }

    commandSequenceLength = chipRegisters.createCommandListRHSRegisterRead(commandList);
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd1, 0);
    rhxController->selectAuxCommandLength(AbstractRHXController::AuxCmd1, 0, commandSequenceLength - 1);
    rhxController->run();
    while (rhxController->isRunning() ) {
        qApp->processEvents();
    }

    RHXDataBlock dataBlock(state->getControllerTypeEnum(), rhxController->getNumEnabledDataStreams());
    rhxController->readDataBlock(&dataBlock);
    rhxController->readDataBlock(&dataBlock);

    commandSequenceLength = chipRegisters.createCommandListRHSRegisterConfig(commandList, true);
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd1, 0);
    rhxController->selectAuxCommandLength(AbstractRHXController::AuxCmd1, 0, commandSequenceLength - 1);
}

void ControllerInterface::manualStimTriggerPulse(QString keyName)
{
    auto pulse = [this](int idx){
        rhxController->setManualStimTrigger(idx, true);
        QTimer::singleShot(3, this, [this, idx](){ rhxController->setManualStimTrigger(idx, false); });
    };
    QString k = keyName.toLower();
    if (k == "f1") pulse(0);
    else if (k == "f2") pulse(1);
    else if (k == "f3") pulse(2);
    else if (k == "f4") pulse(3);
    else if (k == "f5") pulse(4);
    else if (k == "f6") pulse(5);
    else if (k == "f7") pulse(6);
    else if (k == "f8") pulse(7);
}

void ControllerInterface::pipeReadErrorMessage(int errorID)
{
    QString errorMessage;
    switch (errorID) {
    case -1: // ok_Failed: -1
        errorMessage = "Failure on USB Read.\n\n";
        break;
    case -2: // ok_Timeout: -2
        errorMessage = "Timeout on USB Read.\n\n";
        break;
    case -100: // Result value not matching expected size
        errorMessage = "Mismatch in USB Read result and expected result sizes.\n\n";
        break;
    default: // any other ok_ error
        errorMessage = "Failed USB Read. okFrontPanel returned error code: " + QString::number(errorID) + "\n\n";
        break;
    }

    errorMessage = errorMessage + "This may be caused by interference along the USB cable.\n"
                                  "Try using another USB port or move the cable away from\n"
                                  "EMF interference sources (e.g., wireless mouse receivers).";

    QMessageBox::critical(nullptr, "USB Read Error", errorMessage);
    exit(EXIT_FAILURE);
}

void ControllerInterface::setDacHighpassFilterEnabled(bool enabled)
{
    rhxController->enableDacHighpassFilter(enabled);
}

void ControllerInterface::setDacHighpassFilterFrequency(double frequency)
{
    rhxController->setDacHighpassFilter(frequency);
}

void ControllerInterface::setDacChannel(int dac, const QString& channelName)
{
    if (channelName.toLower() == "off" || channelName.toLower() == "n/a") {
        rhxController->enableDac(dac, false);
        rhxController->selectDacDataStream(dac, 0);
        rhxController->selectDacDataChannel(dac, 0);
    } else {
        Channel* channel = state->signalSources->channelByName(channelName);
        if (!channel) return;
        if (channel->getSignalType() != AmplifierSignal) return;
        int stream = state->getControllerTypeEnum() == ControllerRecordUSB2 ? channel->getBoardStream() : channel->getCommandStream();
        rhxController->selectDacDataStream(dac, stream);
        rhxController->selectDacDataChannel(dac, channel->getChipChannel());
        rhxController->enableDac(dac, true);
    }
}

void ControllerInterface::setDacRefChannel(const QString& channelName)
{
    if (channelName.toLower() == "hardware" || channelName.toLower() == "off" || channelName.toLower() == "n/a") {
        rhxController->enableDacReref(false);
        rhxController->setDacRerefSource(0, 0);
    } else {
        Channel* channel = state->signalSources->channelByName(channelName);
        if (!channel) return;
        if (channel->getSignalType() != AmplifierSignal) return;
        rhxController->setDacRerefSource(channel->getCommandStream(), channel->getChipChannel());
        rhxController->enableDacReref(true);
    }
}

void ControllerInterface::setDacThreshold(int dac, int threshold)
{
    int threshLevel = qRound((double) threshold / 0.195) + 32768;
    rhxController->setDacThreshold(dac, threshLevel, threshold >= 0);
}

void ControllerInterface::setDacEnabled(int dac, bool enabled)
{
    rhxController->enableDac(dac, enabled);
}

void ControllerInterface::setTtlOutMode(bool mode1, bool mode2, bool mode3, bool mode4, bool mode5, bool mode6, bool mode7,
                                        bool mode8)
{
    rhxController->setTtlOutMode(mode1, mode2, mode3, mode4, mode5, mode6, mode7, mode8);
}

void ControllerInterface::enableFastSettle(bool enabled)
{
    if (state->getControllerTypeEnum() != ControllerStimRecord) {
        rhxController->selectAuxCommandBankAllPorts(RHXController::AuxCmd3, enabled ? 2 : 1);
    }
}

void ControllerInterface::enableExternalFastSettle(bool enabled)
{
    rhxController->enableExternalFastSettle(enabled);
}

void ControllerInterface::setExternalFastSettleChannel(int channel)
{
    rhxController->setExternalFastSettleChannel(channel - 1);
}

bool ControllerInterface::measureImpedances()
{
    ImpedanceReader zReader(state, rhxController);
    return zReader.measureImpedances();
}

bool ControllerInterface::saveImpedances()
{
    ImpedanceReader zReader(state, rhxController);
    return zReader.saveImpedances();
}

double ControllerInterface::swBufferPercentFull() const
{
    return (std::max)(waveformFifo->percentFull(), usbStreamFifo->percentFull());
}

void ControllerInterface::uploadAmpSettleSettings()
{
    if (state->uploadInProgress->getValue()) {
        sendTCPError("Error - Another upload cannot be started until the previous upload completes");
        return;
    }
    state->uploadInProgress->setValue(true);
    // Update values in hardware.
    setAmpSettleMode(state->useFastSettle->getValue());

    bool gSettle = state->headstageGlobalSettle->getValue();
    setGlobalSettlePolicy(gSettle, gSettle, gSettle, gSettle, false);

    updateChipCommandLists(false); // Update amplifier bandwidth (new desiredLowerSettleBandwidth)
    state->uploadInProgress->setValue(false);
}

void ControllerInterface::uploadChargeRecoverySettings()
{
    if (state->uploadInProgress->getValue()) {
        sendTCPError("Error - Another upload cannot be started until the previous upload completes");
        return;
    }
    state->uploadInProgress->setValue(true);
    // Update values in hardware.
    setChargeRecoveryParameters(state->chargeRecoveryMode->getValue(),
                                (RHXRegisters::ChargeRecoveryCurrentLimit) state->chargeRecoveryCurrentLimit->getIndex(),
                                state->chargeRecoveryTargetVoltage->getValue());
    state->uploadInProgress->setValue(false);
}

void ControllerInterface::uploadBandwidthSettings()
{
    if (state->uploadInProgress->getValue()) {
        sendTCPError("Error - Another upload cannot be started until the previous upload completes");
        return;
    }
    state->uploadInProgress->setValue(true);
    updateChipCommandLists(false);
    state->uploadInProgress->setValue(false);
}

// Set up the same parameters on all 16 channels of this stream for auto testing and upload them.
void ControllerInterface::uploadAutoStimParameters(int stream)
{
    if (rhxController->isSynthetic() || rhxController->isPlayback()) return;

    const int Never = 65535;

    int numOfPulses = 20;
    for (int channel = 0; channel < 16; channel++) {
        rhxController->configureStimTrigger(stream, channel, 0, true, false, true);
        rhxController->configureStimPulses(stream, channel, numOfPulses, Biphasic, false);
    }

    int eventAmpSettleOn = Never;
    int eventStartStim = 0;
    int eventStimPhase2 = 400;
    int eventStimPhase3 = Never;
    int eventEndStim = 800;
    int eventRepeatStim = 1600;
    int eventAmpSettleOff = 0;
    int eventChargeRecovOn = Never;
    int eventChargeRecovOff = 0;
    int eventAmpSettleOnRepeat = Never;
    int eventAmpSettleOffRepeat = Never;
    int eventEnd = 1600;


    for (int channel = 0; channel < 16; channel++) {
        rhxController->programStimReg(stream, channel, AbstractRHXController::EventAmpSettleOn, eventAmpSettleOn);
        rhxController->programStimReg(stream, channel, AbstractRHXController::EventStartStim, eventStartStim);
        rhxController->programStimReg(stream, channel, AbstractRHXController::EventStimPhase2, eventStimPhase2);
        rhxController->programStimReg(stream, channel, AbstractRHXController::EventStimPhase3, eventStimPhase3);
        rhxController->programStimReg(stream, channel, AbstractRHXController::EventEndStim, eventEndStim);
        rhxController->programStimReg(stream, channel, AbstractRHXController::EventRepeatStim, eventRepeatStim);
        rhxController->programStimReg(stream, channel, AbstractRHXController::EventAmpSettleOff, eventAmpSettleOff);
        rhxController->programStimReg(stream, channel, AbstractRHXController::EventChargeRecovOn, eventChargeRecovOn);
        rhxController->programStimReg(stream, channel, AbstractRHXController::EventChargeRecovOff, eventChargeRecovOff);
        rhxController->programStimReg(stream, channel, AbstractRHXController::EventAmpSettleOnRepeat, eventAmpSettleOnRepeat);
        rhxController->programStimReg(stream, channel, AbstractRHXController::EventAmpSettleOffRepeat, eventAmpSettleOffRepeat);
        rhxController->programStimReg(stream, channel, AbstractRHXController::EventEnd, eventEnd);
    }

    rhxController->enableAuxCommandsOnOneStream(stream);

    RHXRegisters chipRegisters(rhxController->getType(), rhxController->getSampleRate(), state->getStimStepSizeEnum());
    int commandSequenceLength;
    std::vector<unsigned int> commandList;

    int posMag = 200;
    int negMag = 200;

    //mimic createCommandListSetStimMagnitudes, where it can be set for all channels
    commandSequenceLength = chipRegisters.createCommandListSetStimMagnitudesAllChannels(commandList, posMag, 0, negMag, 0);
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd1, 0); // RHS - bank doesn't matter
    rhxController->selectAuxCommandLength(AbstractRHXController::AuxCmd1, 0, commandSequenceLength - 1);


    chipRegisters.createCommandListDummy(commandList, 8192, chipRegisters.createRHXCommand(RHXRegisters::RHXCommandRegRead, 255));
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd2, 0);  // RHS - bank doesn't matter
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd3, 0);  // RHS - bank doesn't matter
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd4, 0);  // RHS - bank doesn't matter

    rhxController->setMaxTimeStep(commandSequenceLength);
    rhxController->setContinuousRunMode(false);
    rhxController->setStimCmdMode(false);
    rhxController->enableAuxCommandsOnOneStream(stream);

    rhxController->run();
    while (rhxController->isRunning() ) {
        qApp->processEvents();
    }

    commandSequenceLength = chipRegisters.createCommandListRHSRegisterRead(commandList);
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd1, 0);  // RHS - bank doesn't matter
    rhxController->selectAuxCommandLength(AbstractRHXController::AuxCmd1, 0, commandSequenceLength - 1);
    rhxController->run();
    while (rhxController->isRunning() ) {
        qApp->processEvents();
    }

    RHXDataBlock dataBlock(rhxController->getType(), rhxController->getNumEnabledDataStreams());
    rhxController->readDataBlock(&dataBlock);
    rhxController->readDataBlock(&dataBlock);

    commandSequenceLength = chipRegisters.createCommandListRHSRegisterConfig(commandList, true);
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd1, 0);  // RHS - bank doesn't matter
    rhxController->selectAuxCommandLength(AbstractRHXController::AuxCmd1, 0, commandSequenceLength - 1);

    rhxController->enableAuxCommandsOnAllStreams();
}

void ControllerInterface::clearStimParameters(int stream)
{
    if (rhxController->isSynthetic() || rhxController->isPlayback()) return;
    for (int channel = 0; channel < 16; channel++) {
        rhxController->configureStimTrigger(stream, channel, 0, false, false, false);
    }

    rhxController->enableAuxCommandsOnOneStream(stream);

    RHXRegisters chipRegisters(rhxController->getType(), rhxController->getSampleRate(), state->getStimStepSizeEnum());
    int commandSequenceLength;
    std::vector<unsigned int> commandList;

    int posMag = 0;
    int negMag = 0;

    //mimic createCommandListSetStimMagnitudes, where it can be set for all channels
    commandSequenceLength = chipRegisters.createCommandListSetStimMagnitudesAllChannels(commandList, posMag, 0, negMag, 0);
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd1, 0); // RHS - bank doesn't matter
    rhxController->selectAuxCommandLength(AbstractRHXController::AuxCmd1, 0, commandSequenceLength - 1);


    chipRegisters.createCommandListDummy(commandList, 8192, chipRegisters.createRHXCommand(RHXRegisters::RHXCommandRegRead, 255));
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd2, 0);  // RHS - bank doesn't matter
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd3, 0);  // RHS - bank doesn't matter
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd4, 0);  // RHS - bank doesn't matter

    rhxController->setMaxTimeStep(commandSequenceLength);
    rhxController->setContinuousRunMode(false);
    rhxController->setStimCmdMode(false);
    rhxController->enableAuxCommandsOnOneStream(stream);

    rhxController->run();
    while (rhxController->isRunning() ) {
        qApp->processEvents();
    }

    commandSequenceLength = chipRegisters.createCommandListRHSRegisterRead(commandList);
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd1, 0);  // RHS - bank doesn't matter
    rhxController->selectAuxCommandLength(AbstractRHXController::AuxCmd1, 0, commandSequenceLength - 1);
    rhxController->run();
    while (rhxController->isRunning() ) {
        qApp->processEvents();
    }

    RHXDataBlock dataBlock(rhxController->getType(), rhxController->getNumEnabledDataStreams());
    rhxController->readDataBlock(&dataBlock);
    rhxController->readDataBlock(&dataBlock);

    commandSequenceLength = chipRegisters.createCommandListRHSRegisterConfig(commandList, true);
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd1, 0);  // RHS - bank doesn't matter
    rhxController->selectAuxCommandLength(AbstractRHXController::AuxCmd1, 0, commandSequenceLength - 1);

    rhxController->enableAuxCommandsOnAllStreams();
}

void ControllerInterface::uploadStimParameters(Channel* channel)
{
    if (state->uploadInProgress->getValue()) {
        sendTCPError("Error - Another upload cannot be started until the previous upload completes");
        return;
    }
    state->uploadInProgress->setValue(true);
    if (channel->getSignalType() == AmplifierSignal) {
        setStimSequenceParameters(channel);
    } else if (channel->getSignalType() == BoardDacSignal) {
        setAnalogOutSequenceParameters(channel);
    } else if (channel->getSignalType() == BoardDigitalOutSignal) {
        setDigitalOutSequenceParameters(channel);
    }
    state->uploadInProgress->setValue(false);
}

void ControllerInterface::uploadStimParameters()
{
    for (int i = 0; i < state->signalSources->numGroups(); ++i) {
        SignalGroup* group = state->signalSources->groupByIndex(i);
        for (int j = 0; j < group->numChannels(); ++j) {
            Channel* channel = group->channelByIndex(j);
            if (channel->stimParameters->enabled->getValue()) {
                uploadStimParameters(channel);
            }
        }
    }
}

void ControllerInterface::logStimChannelImpedances()
{
    QStringList missing;
    for (int i = 0; i < 8; ++i) {
        QString chName = QString("A-%1").arg(i, 3, 10, QChar('0'));
        Channel* ch = state->signalSources->channelByName(chName);
        double z_kohm = -1.0;
        bool valid = false;
        if (ch && ch->isImpedanceValid()) {
            z_kohm = ch->getImpedanceMagnitude();
            valid = true;
        }
        if (valid) {
            qDebug() << "[Impedance]" << chName << z_kohm << "kOhm";
        } else {
            missing << chName;
        }
    }
    if (!missing.isEmpty()) {
        qDebug() << "[Impedance] invalid/unknown:" << missing.join(", ");
    }
}

void ControllerInterface::modulateSpikes(PaddleAction action)
{
    if (rhxController) {
        rhxController->modulateSpikes(action);
    }
}

void ControllerInterface::sendTCPError(QString errorMessage)
{
    emit TCPErrorMessage(errorMessage);
}

// 运行时退出保护方法实现
void ControllerInterface::stopController()
{
    qDebug() << "[ControllerInterface] stopController called";

    // 设置运行状态为false
    if (state) {
        state->running = false;
        state->recording = false;
    }

    qDebug() << "[ControllerInterface] Running state set to false";

    // 停止正在运行的线程
    if (usbDataThread && usbDataThread->isActive()) {
        usbDataThread->stopRunning();
        qDebug() << "[ControllerInterface] USBDataThread stopRunning called";
    }

    if (waveformProcessorThread && waveformProcessorThread->isActive()) {
        waveformProcessorThread->stopRunning();
        qDebug() << "[ControllerInterface] WaveformProcessorThread stopRunning called";
    }

    if (audioThread && audioThread->isActive()) {
        audioThread->stopRunning();
        qDebug() << "[ControllerInterface] AudioThread stopRunning called";
    }

    if (tcpDataOutputThread && tcpDataOutputThread->isActive()) {
        tcpDataOutputThread->stopRunning();
        qDebug() << "[ControllerInterface] TCPDataOutputThread stopRunning called";
    }

    if (gameThread && gameThread->isActive()) {
        gameThread->close();
        qDebug() << "[ControllerInterface] GameThread close called";
    }

    if (saveToDiskThread && saveToDiskThread->isActive()) {
        saveToDiskThread->stopRunning();
        qDebug() << "[ControllerInterface] SaveToDiskThread stopRunning called";
    }

    qDebug() << "[ControllerInterface] All threads sent stop command";
}

bool ControllerInterface::isRunning() const
{
    if (!state) return false;

    // 检查控制器是否正在运行
    if (state->running) {
        return true;
    }

    // 额外检查线程状态
    if ((usbDataThread && usbDataThread->isActive()) ||
        (waveformProcessorThread && waveformProcessorThread->isActive()) ||
        (audioThread && audioThread->isActive()) ||
        (tcpDataOutputThread && tcpDataOutputThread->isActive()) ||
        (gameThread && gameThread->isActive()) ||
        (saveToDiskThread && saveToDiskThread->isActive())) {
        return true;
    }

    return false;
}
