//------------------------------------------------------------------------------
//
//  Intan Technologies RHX Data Acquisition Software
//  Spike Detection and Game Interface Implementation
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
//------------------------------------------------------------------------------

#include "gamethread.h"
#include "controllerinterface.h"
#include "Engine/API/Hardware/rhxdatablock.h" // Added for block size
#include <QElapsedTimer>
#include <QDateTime>
#include <cmath>
#include <algorithm>
#include <csignal>

// New includes for the learning architecture
#include "qlearningagent.h"
#include "Engine/API/Synthetic/syntheticrhxcontroller.h"


void GameThread::initializeThreadSafety()
{
    // 确保所有互斥锁处于解锁状态
    // 这在构造函数中调用，确保线程安全的初始化
    
    // 重置所有统计数据
    {
        std::lock_guard<std::mutex> statsLock(statsMutex);
        spikeCounters.clear();
        spikesPerSecond.clear();
        lastStatsUpdate = 0;

        // 重置游戏统计 - 修复avg计数问题
        totalRallyCount = 0;
        totalRallyLengthSum = 0;
        averageRallyLength = 0.0f;
    }
    
    // 重置尖峰检测参数
    {
        std::lock_guard<std::mutex> spikeLock(spikeDetectionMutex);
        // 参数已在初始化列表中设置
    }
    
    // 清空运动区域通道
    {
        std::unique_lock<std::shared_mutex> channelsLock(motorChannelsMutex);
        motorRegion1Channels.clear();
        motorRegion2Channels.clear();
    }
    
    // 清空刺激参数锁
    {
        std::lock_guard<std::mutex> stimLock(stimParamsMutex);
        // 无特定清理需要
    }
}

GameThread::GameThread(WaveformFifo* waveformFifo_, SystemState* state_, ControllerInterface* controllerInterface_, QObject* parent) :
    QThread(parent),
    controllerInterface(controllerInterface_),
    waveformFifo(waveformFifo_),
    state(state_),
    pongGame(new PongGame()),
    gameController(nullptr),
    thresholdMultiplier(5.0f),
    minThreshold(-20.0f), // 默认阈值改为负数
    refractoryPeriod(1000),  // 默认1000个样本的不应期
    lastStatsUpdate(0),
    totalRallyCount(0),
    totalRallyLengthSum(0),
    averageRallyLength(0.0f),
    hitStimAmplitude(100.0),
    hitStimFrequency(100.0),
    hitStimDuration(100.0),
    missStimAmplitude(150.0),
    missStimFrequency(5.0),
    missStimDuration(4000.0)
{
    keepGoing = false;
    running = false;
    stopThread = false;
    lastStatsUpdate = 0;

    // 初始化线程安全机制
    initializeThreadSafety();
    setDefaultMotorRegionsForDemo();

    // 添加硬件诊断调试
    qDebug() << "[GameThread] === 游戏线程初始化完成 ===";
    qDebug() << "[GameThread] 控制器类型:" << (state ? state->getControllerTypeEnum() : -1);
    qDebug() << "[GameThread] 运动区域通道 - 上移:" << motorRegion1Channels.size() << "个";
    qDebug() << "[GameThread] 运动区域通道 - 下移:" << motorRegion2Channels.size() << "个";
}

GameThread::~GameThread()
{
    cleanupChannelProcessors();
    delete pongGame;
}

void GameThread::initializeGameController() {
    if (state->getAcquisitionMode() == LearningMode) {
        gameController = std::make_unique<QLearningAgent>();
        qDebug() << "[GameThread] Initialized QLearningAgent for LearningMode.";
    } else {
        gameController = std::make_unique<SpikeBasedController>([this](){ return safeGetMotorRegionSpikeCounts(); });
        qDebug() << "[GameThread] Initialized SpikeBasedController for non-learning mode.";
    }
}

GameStateInfo GameThread::getCurrentGameStateInfo() {
    return {
        static_cast<float>(pongGame->getPaddle1Y()),
        static_cast<float>(pongGame->getBallY()),
        static_cast<float>(pongGame->getBallX()),
        pongGame->getBallVY(),
        pongGame->getBallVX()
    };
}

float GameThread::calculateReward(GameEvent event, const PongGame& game) {
    switch(event) {
        case GameEvent::BallHitPlayerPaddle:
            return 10.0f; // Strong positive reward for hitting the ball
        case GameEvent::PlayerMissed:
            return -10.0f; // Strong negative reward for missing
        default:
            // Continuous negative reward for being far from the ball
            return -std::abs(game.getPaddle1Y() - game.getBallY());
    }
}


void GameThread::run()
{
    const int numSamples = RHXDataBlock::samplesPerDataBlock(state->getControllerTypeEnum());
    int consecutiveFifoErrors = 0;
    const int MaxConsecutiveFifoErrors = 10;

#ifdef Q_OS_MAC
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGKILL);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
#endif

    initializeChannelProcessors();
    initializeGameController(); // Factory call to create the correct controller

    // For learning mode
    GameStateInfo oldState = {};
    PaddleAction action = PaddleAction::Stay;


    while (!stopThread) {
        if (keepGoing) {
            running = true;

            // Clear any excess semaphore resources that may have accumulated during startup.
            if (waveformFifo->dataForGameThread.available() > 0) {
                waveformFifo->dataForGameThread.acquire(waveformFifo->dataForGameThread.available());
            }

            while (keepGoing && !stopThread) {
                // Acquire the semaphore, blocking until a data block is made available by the producer thread.
                waveformFifo->dataForGameThread.acquire();

                // Now that we've been woken up, we know data is available.
                bool fifoSuccess = waveformFifo->requestReadNewData(WaveformFifo::ReaderDisk, numSamples, true);
                
                if (fifoSuccess) {
                    // Process spikes regardless of mode, as it might be needed for visualization
                    processSampleBlock(numSamples);

                    // --- LEARNING/CONTROL LOGIC ---
                    int paddle_movement = 0; // -1 for up, 1 for down, 0 for stay

                    if (state->getAcquisitionMode() == LearningMode) {
                        // --- LEARNING MODE ---
                        oldState = getCurrentGameStateInfo();
                        action = gameController->getAction(oldState);

                        // In Learning Mode, the agent's action modulates the spike rates of the synthetic controller.
                        if (controllerInterface) {
                            controllerInterface->modulateSpikes(action);
                        }
                    }

                    // --- UNIFIED CONTROL (for both modes) ---
                    // The paddle is always controlled by comparing spike counts from the two motor regions.
                    // In LearningMode, these counts are a result of the agent's modulation.
                    // In normal SpikeBasedMode, these counts come from the hardware or default synthetic signals.
                    auto counts = safeGetMotorRegionSpikeCounts();
                    if (counts.first > counts.second) {
                        paddle_movement = -1; // Up
                    } else if (counts.second > counts.first) {
                        paddle_movement = 1; // Down
                    }
                    
                    // --- GAME UPDATE ---
                    GameEvent event = pongGame->update(paddle_movement);
                    
                    // --- LEARNING AGENT UPDATE ---
                    if (state->getAcquisitionMode() == LearningMode) {
                        float reward = calculateReward(event, *pongGame);
                        GameStateInfo newState = getCurrentGameStateInfo();
                        gameController->update(oldState, action, reward, newState);
                    }

                    // --- EVENT HANDLING & UI UPDATES ---
                    switch(event) {
                        case GameEvent::BallHitPlayerPaddle:
                            triggerHitStimulus();
                            emit sendHitStim();
                            break;
                        case GameEvent::PlayerMissed:
                            if (pongGame->getCondition() == ExperimentCondition::Stimulus) {
                                triggerMissStimulus();
                                emit sendMissStim();
                            } else if (pongGame->getCondition() == ExperimentCondition::Silent) {
                                triggerSilentStimulus();
                                emit stopAllStim();
                            }
                            break;
                        default:
                            break;
                    }

                    emit sendSensoryStim(pongGame->getSensoryStimZone());

                    if (event == GameEvent::PlayerMissed) {
                        int rallyLength = pongGame->getBounces();
                        totalRallyLengthSum += rallyLength;
                        totalRallyCount++;
                        if (totalRallyCount > 0) {
                            averageRallyLength = static_cast<float>(totalRallyLengthSum) / totalRallyCount;
                        }
                        pongGame->resetBounces(); // 在使用其值后重置计数器
                    }

                    GameState currentState;
                    currentState.paddle1Y = pongGame->getPaddle1Y();
                    currentState.ballX = pongGame->getBallX();
                    currentState.ballY = pongGame->getBallY();
                    currentState.paddle2Y = 0;
                    currentState.paddleHeight = pongGame->getPaddleHeight();
                    currentState.bounces = pongGame->getBounces();
                    currentState.rallyCount = totalRallyCount;
                    currentState.avgRallyLength = averageRallyLength;
                    emit gameDataUpdated(currentState);

                    safeUpdateSpikeCounters("", 0); // Reset spike counters for next block

                    waveformFifo->freeOldData(WaveformFifo::ReaderDisk);
                } else {
                    // This should not happen with the semaphore logic, but we keep it for safety.
                    qDebug() << "[GameThread] ERROR: Acquired semaphore but failed to read from FIFO!";
                    usleep(100);
                }
            }
            running = false;
            usleep(1000); // Add a small sleep even when not running to yield CPU
        } else {
            usleep(1000);
        }
    }

    cleanupChannelProcessors();
}
void GameThread::startRunning()
{
    keepGoing = true;
    qDebug() << "[GameThread] === 游戏开始运行 ===";
    qDebug() << "[GameThread] 运行状态设置为:true";
}

void GameThread::stopRunning()
{
    keepGoing = false;
}

void GameThread::close()
{
    keepGoing = false;
    stopThread = true;
}

bool GameThread::isActive() const
{
    return running;
}

void GameThread::setThresholdMultiplier(float multiplier)
{
    std::lock_guard<std::mutex> lock(spikeDetectionMutex);
    thresholdMultiplier = multiplier;
}

void GameThread::setMinThreshold(float minThresholdValue)
{
    std::lock_guard<std::mutex> lock(spikeDetectionMutex);
    minThreshold = minThresholdValue;
}

void GameThread::setRefractoryPeriod(int samples)
{
    std::lock_guard<std::mutex> lock(spikeDetectionMutex);
    refractoryPeriod = samples;
}

void GameThread::setExperimentCondition(ExperimentCondition condition)
{
    if (pongGame) {
        pongGame->setCondition(condition);
    }
}

void GameThread::setMotorRegions(const std::vector<QString>& upChannels, const std::vector<QString>& downChannels)
{
    std::unique_lock<std::shared_mutex> lock(motorChannelsMutex);
    motorRegion1Channels.clear();
    motorRegion2Channels.clear();
    for(const auto& ch : upChannels) {
        motorRegion1Channels.push_back(ch);
    }
    for(const auto& ch : downChannels) {
        motorRegion2Channels.push_back(ch);
    }
}

void GameThread::setDefaultMotorRegionsForDemo()
{
    // 新配置：A000-A015 vs A016-A032 对比控制
    std::vector<QString> defaultUpChannels;
    std::vector<QString> defaultDownChannels;

    // 前16个通道控制上移
    for (int i = 0; i <= 15; ++i) {
        QString channelName = QString("A-%1").arg(i, 3, 10, QChar('0'));
        defaultUpChannels.push_back(channelName);
    }

    // 后16个通道控制下移（A016-A031）
    for (int i = 16; i <= 31; ++i) {
        QString channelName = QString("A-%1").arg(i, 3, 10, QChar('0'));
        defaultDownChannels.push_back(channelName);
    }

    setMotorRegions(defaultUpChannels, defaultDownChannels);

    qDebug() << "[GameThread] 设置新的运动区域对比配置:";
    qDebug() << "  上移控制 (A000-A015):" << defaultUpChannels.size() << "个通道";
    qDebug() << "  下移控制 (A016-A031):" << defaultDownChannels.size() << "个通道";
    qDebug() << "[GameThread] 配置逻辑: 比较两组尖峰总数，多者控制方向";
}

void GameThread::setStimChannelEnabled(const QString& channelName, bool enabled)
{
    std::lock_guard<std::mutex> lock(signalSourcesMutex);
    Channel* channel = state->signalSources->channelByName(channelName);
    if (!channel) return;
    
    // 启用或禁用通道的刺激功能
    channel->stimParameters->enabled->setValue(enabled);
    
    // 应用刺激参数
    if (enabled) {
        applyStimParameters(channelName);
    }
}

void GameThread::setStimChannelParameters(const QString& channelName, double frequency, double duration)
{
    std::lock_guard<std::mutex> lock(signalSourcesMutex);
    Channel* channel = state->signalSources->channelByName(channelName);
    if (!channel) return;
    
    // 设置刺激参数
    StimParameters* params = channel->stimParameters;
    params->pulseOrTrain->setIndex(PulseTrain);
    params->pulseTrainPeriod->setValue(1.0 / frequency * 1e6);  // 转换为微秒
    params->firstPhaseDuration->setValue(duration * 1e6);       // 转换为微秒
    params->numberOfStimPulses->setValue(1);
    
    applyStimParameters(channelName);
}

void GameThread::triggerStimChannel(const QString& channelName, double amplitude)
{
    std::lock_guard<std::mutex> lock(signalSourcesMutex);
    Channel* channel = state->signalSources->channelByName(channelName);
    if (!channel) return;
    
    // 设置刺激幅度并触发一次刺激
    StimParameters* params = channel->stimParameters;
    params->firstPhaseAmplitude->setValue(amplitude);
    params->enabled->setValue(true);
    params->pulseOrTrain->setIndex(SinglePulse);
    
    applyStimParameters(channelName);
}

void GameThread::updateChannelList()
{
    // 重新初始化通道处理器以反映通道列表的变化
    cleanupChannelProcessors();
    initializeChannelProcessors();
}

void GameThread::initializeChannelProcessors()
{
    // 线程安全地获取所有放大器通道
    std::vector<QString> channelNames;
    {
        std::lock_guard<std::mutex> lock(signalSourcesMutex);
        for (int i = 0; i < state->signalSources->numGroups(); ++i) {
            SignalGroup* group = state->signalSources->groupByIndex(i);
            for (int j = 0; j < group->numChannels(); ++j) {
                Channel* channel = group->channelByIndex(j);
                if (channel && channel->getSignalType() == SignalType::AmplifierSignal) {
                    channelNames.push_back(channel->getNativeName());
                }
            }
        }
    }
    
    // 线程安全地清理和重新初始化
    {
        std::lock_guard<std::mutex> statsLock(statsMutex);
        std::lock_guard<std::mutex> spikeLock(spikeDetectionMutex);
        
        cleanupChannelProcessors(); // 先清理现有处理器
        
        double sampleRate = state->sampleRate->getNumericValue();
        
        for (size_t i = 0; i < channelNames.size(); ++i) {
            ChannelProcessor processor;
            processor.name = channelNames[i];
            processor.sampleRate = sampleRate; // Add this line
            processor.highpassFilter = new SecondOrderHighpassFilter(10.0, 0.707, sampleRate);  // DIAGNOSTIC: Lowered to 10Hz from 100Hz
            processor.lowpassFilter = new FirstOrderLowpassFilter(1.0, sampleRate);               // 1Hz Bessel低通滤波器
            processor.smoothedAbsValue = 0.0f;
            processor.threshold = minThreshold;
            processor.prevValue = 0.0f;
            processor.samplesSinceLastSpike = 0;

            channelProcessors.push_back(processor);
            channelIndexMap[processor.name] = static_cast<int>(i);
            spikeCounters[processor.name] = 0;
            spikesPerSecond[processor.name] = 0.0f;

            // 添加单个通道创建调试（使用state的采样率）
            if (i < 3) { // 只显示前3个通道避免输出过多
                qDebug() << "[GameThread] 创建通道处理器[" << i << "]:"
                         << "名称:" << processor.name
                         << "采样率:" << sampleRate << "Hz"
                         << "初始阈值:" << processor.threshold;
            }
        }
    }

    emit statusUpdated("Initialized " + QString::number(channelProcessors.size()) + " channel processors");

    // 添加通道处理器初始化调试
    qDebug() << "[GameThread] === 通道处理器初始化完成 ===";
    qDebug() << "[GameThread] 处理器数量:" << channelProcessors.size();
    qDebug() << "[GameThread] 通道索引映射数量:" << channelIndexMap.size();
    qDebug() << "[GameThread] Spike计数器数量:" << spikeCounters.size();

    // 显示前几个通道的信息
    int count = 0;
    for (const auto& processor : channelProcessors) {
        if (count >= 5) { // 只显示前5个通道避免输出过多
            qDebug() << "[GameThread]   ... 还有" << (channelProcessors.size() - 5) << "个通道";
            break;
        }
        qDebug() << "[GameThread]   通道" << count << ":" << processor.name
                 << "采样率:" << processor.sampleRate << "Hz"
                 << "阈值:" << processor.threshold;
        count++;
    }

    // 检查运动区域通道是否在可用通道中
    qDebug() << "[GameThread] 运动区域通道验证:";
    for (const auto& chName : motorRegion1Channels) {
        bool found = channelIndexMap.count(chName) > 0;
        qDebug() << "[GameThread]   上移通道" << chName << ":" << (found ? "找到" : "未找到");
    }
    for (const auto& chName : motorRegion2Channels) {
        bool found = channelIndexMap.count(chName) > 0;
        qDebug() << "[GameThread]   下移通道" << chName << ":" << (found ? "找到" : "未找到");
    }
}

void GameThread::cleanupChannelProcessors()
{
    for (auto& processor : channelProcessors) {
        delete processor.highpassFilter;
        delete processor.lowpassFilter;
    }
    channelProcessors.clear();
    channelIndexMap.clear();
    spikeCounters.clear();
    spikesPerSecond.clear();
}

void GameThread::processSampleBlock(int numSamples)
{
    // 对每个通道进行处理
    for (auto& processor : channelProcessors) {
        // 获取通道波形数据指针 - 注意：此指针仅在当前数据块有效
        float* waveform = waveformFifo->getAnalogWaveformPointer((processor.name + "|DC").toStdString());
        if (!waveform) {
            emit error("Failed to get waveform pointer for channel: " + processor.name);
            continue;
        }
        
        // 处理每个样本
        for (int t = 0; t < numSamples; ++t) {
            try {
                // 获取原始数据 - WaveformFifo内部有锁保护
                float rawValue = waveformFifo->getAnalogData(WaveformFifo::ReaderDisk, waveform, t);
                
                // 应用高通滤波器
                float filteredValue = processor.highpassFilter->filterOne(rawValue);
                
                // 计算绝对值
                float absValue = fabsf(filteredValue);
                
                // 应用低通滤波器平滑绝对值
                processor.smoothedAbsValue = processor.lowpassFilter->filterOne(absValue);
                
                // 计算动态阈值 (注意：规则是<-5mV，这里我们使用一个更通用的动态阈值)
                // 阈值现在是负数
                processor.threshold = (std::min)(minThreshold, -processor.smoothedAbsValue * thresholdMultiplier);
                
                // 检测尖峰
                int64_t timestamp = waveformFifo->getTimeStamp(WaveformFifo::ReaderDisk, t);
                if (detectSpike(processor, filteredValue, timestamp)) {
                    // 发送尖峰检测信号
                    SpikeEvent spike;
                    spike.channelName = processor.name;
                    spike.timestamp = timestamp;
                    spike.amplitude = fabsf(filteredValue);
                    spike.threshold = processor.threshold;
                    emit spikeDetected(spike);

                    // 更新计数器
                    safeUpdateSpikeCounters(processor.name, 1);
                }
                
                processor.prevValue = filteredValue;
                processor.samplesSinceLastSpike++;
                
            } catch (const std::exception& e) {
                emit error(QString("Error processing sample for channel %1: %2").arg(processor.name).arg(e.what()));
                continue;
            }
        }
    }
}

bool GameThread::detectSpike(ChannelProcessor& processor, float value, int64_t /*timestamp*/)
{
    // 检查是否在不应期内
    if (processor.samplesSinceLastSpike < refractoryPeriod) {
        return false;
    }
    
    // 线程安全地获取当前阈值
    float currentThreshold;
    {
        std::lock_guard<std::mutex> lock(spikeDetectionMutex);
        currentThreshold = processor.threshold;
    }
    
    // 检测是否超过负阈值且为负向过零
    if (processor.prevValue > currentThreshold && value <= currentThreshold) {
        processor.samplesSinceLastSpike = 0;
        return true;
    }
    
    return false;
}

void GameThread::applyStimParameters(const QString& channelName)
{
    // 注意：此函数可能被多个线程调用，需要保护signalSources访问
    std::lock_guard<std::mutex> sourcesLock(signalSourcesMutex);
    std::lock_guard<std::mutex> lock(stimParamsMutex);
    
    // 获取通道
    Channel* channel = state->signalSources->channelByName(channelName);
    if (!channel) {
        emit error("Channel not found: " + channelName);
        return;
    }
    
    // 验证刺激参数
    StimParameters* params = channel->stimParameters;
    if (!params) {
        emit error("No stimulation parameters for channel: " + channelName);
        return;
    }
    
    // 参数验证和约束
    if (params->firstPhaseAmplitude->getValue() > 2000.0) {  // 最大2mA
        params->firstPhaseAmplitude->setValue(2000.0);
        emit statusUpdated("Amplitude clamped to 2mA for safety");
    }
    
    if (params->firstPhaseDuration->getValue() < 10.0) {  // 最小10μs
        params->firstPhaseDuration->setValue(10.0);
        emit statusUpdated("Pulse duration set to minimum 10μs");
    }
    
    // 通过ControllerInterface应用参数到硬件
    if (controllerInterface) {
        try {
            // 上传刺激参数到控制器
            controllerInterface->uploadStimParameters(channel);
            
            // 如果通道已启用，立即应用新的参数
            if (params->enabled->getValue()) {
                controllerInterface->setManualStimTrigger(channel->getNativeName(), true);
            }
            
            emit statusUpdated(QString("Stim parameters applied to %1: %2mV, %3μs, %4Hz")
                             .arg(channelName)
                             .arg(params->firstPhaseAmplitude->getValue())
                             .arg(params->firstPhaseDuration->getValue())
                             .arg(1000000.0 / params->pulseTrainPeriod->getValue()));
                             
        } catch (const std::exception& e) {
            emit error(QString("Failed to apply stim parameters: %1").arg(e.what()));
        }
    } else {
        emit statusUpdated("Stim parameters staged for channel: " + channelName + 
                          " (controller not connected)");
    }
}

// 线程安全的尖峰计数器更新
void GameThread::safeUpdateSpikeCounters(const QString& channelName, int count)
{
    std::lock_guard<std::mutex> lock(statsMutex);
    if (channelName.isEmpty()) {
        // 重置所有计数器
        for (auto& counter : spikeCounters) {
            counter.second = 0;
        }
    } else {
        spikeCounters[channelName] += count;
    }
}

// 线程安全的运动区域尖峰统计
std::pair<int, int> GameThread::safeGetMotorRegionSpikeCounts()
{
    std::unique_lock<std::shared_mutex> channelsLock(motorChannelsMutex);
    std::lock_guard<std::mutex> statsLock(statsMutex);
    
    int spikesUp = 0;
    int spikesDown = 0;
    
    for(const auto& chName : motorRegion1Channels) {
        if (spikeCounters.count(chName)) {
            spikesUp += spikeCounters[chName];
        }
    }
    for(const auto& chName : motorRegion2Channels) {
        if (spikeCounters.count(chName)) {
            spikesDown += spikeCounters[chName];
        }
    }
    
    return std::make_pair(spikesUp, spikesDown);
}

// 线程安全的游戏状态获取
GameState GameThread::safeGetCurrentGameState() const
{
    GameState state;
    
    // 从游戏对象获取当前状态（假设PongGame是线程安全的）
    if (pongGame) {
        state.paddle1Y = pongGame->getPaddle1Y();
        state.ballX = pongGame->getBallX();
        state.ballY = pongGame->getBallY();
        state.paddleHeight = pongGame->getPaddleHeight();
        state.bounces = pongGame->getBounces();
    }
    
    // 从统计数据获取其他信息
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        // 注意：rallyCount和avgRallyLength需要单独维护
        state.paddle2Y = 0; // AI paddle not used
    }
    
    return state;
}

void GameThread::updateSpikesPerSecond()
{
    // 计算每个通道的每秒尖峰率
    const double updateInterval = 1.0; // 1秒更新间隔
    
    // 获取当前时间戳
    int64_t currentTime = QDateTime::currentMSecsSinceEpoch();
    
    // 计算时间差（秒）
    double timeDiff;
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        timeDiff = (currentTime - lastStatsUpdate) / 1000.0;
    }
    
    // 如果距离上次更新超过1秒，重新计算SPS
    if (timeDiff >= updateInterval) {
        // 线程安全地获取尖峰计数快照
        std::map<QString, int> spikeCountsSnapshot;
        {
            std::lock_guard<std::mutex> lock(statsMutex);
            spikeCountsSnapshot = spikeCounters;
        }
        
        // 重置统计
        std::map<QString, float> newSpikesPerSecond;
        
        // 计算每个通道的SPS
        for (const auto& processor : channelProcessors) {
            const QString& channelName = processor.name;
            
            // 获取该通道的尖峰计数
            int spikeCount = spikeCountsSnapshot[channelName];
            
            // 计算每秒尖峰率
            float sps = static_cast<float>(spikeCount) / static_cast<float>(timeDiff);
            
            // 存储结果
            newSpikesPerSecond[channelName] = sps;
            
            // 可选：应用低通滤波器使显示更平滑
            static std::map<QString, float> filteredSPS;
            if (filteredSPS.find(channelName) == filteredSPS.end()) {
                filteredSPS[channelName] = sps;
            } else {
                // 简单的指数平滑滤波器
                filteredSPS[channelName] = 0.9f * filteredSPS[channelName] + 0.1f * sps;
            }
            newSpikesPerSecond[channelName] = filteredSPS[channelName];
        }
        
        // 线程安全地更新统计数据
        {
            std::lock_guard<std::mutex> lock(statsMutex);
            spikesPerSecond = newSpikesPerSecond;
            lastStatsUpdate = currentTime;
        }
        
        // 发送更新信号
        emit spikesPerSecondUpdated(newSpikesPerSecond);
        
        // 可选：发送总体统计信息
        float totalSPS = 0.0f;
        int activeChannels = 0;
        for (const auto& pair : newSpikesPerSecond) {
            if (pair.second > 0.1f) { // 只计算有活动的通道
                totalSPS += pair.second;
                activeChannels++;
            }
        }
        
        if (activeChannels > 0) {
            float avgSPS = totalSPS / activeChannels;
            emit statusUpdated(QString("SPS Update: %1 active channels, avg %.1f Hz")
                             .arg(activeChannels).arg(avgSPS));
        }
    }
    
    // 如果更新时间间隔太短，仍然发送当前值（但不重新计算）
    else {
        std::lock_guard<std::mutex> lock(statsMutex);
        emit spikesPerSecondUpdated(spikesPerSecond);
    }
}

// 刺激触发函数实现
void GameThread::triggerHitStimulus()
{
    // Hit: 所有8个刺激电极同时进行100Hz持续100ms的双相脉冲刺激
    // 使用成员变量
    double currentHitFrequency, currentHitDuration, currentHitAmplitude;
    {
        std::lock_guard<std::mutex> lock(stimParamsMutex);
        currentHitFrequency = hitStimFrequency;
        currentHitDuration = hitStimDuration;
        currentHitAmplitude = hitStimAmplitude;
    }
    // 线程安全地获取所有刺激通道
    std::vector<QString> stimChannels;
    {
        std::lock_guard<std::mutex> lock(signalSourcesMutex);
        for (int i = 0; i < state->signalSources->numGroups(); ++i) {
            SignalGroup* group = state->signalSources->groupByIndex(i);
            for (int j = 0; j < group->numChannels(); ++j) {
                Channel* channel = group->channelByIndex(j);
                if (channel && channel->getSignalType() == SignalType::StimSignal) {
                    stimChannels.push_back(channel->getNativeName());
                }
            }
        }
    }
    
    // 限制为最多8个通道
    int channelsToStimulate = std::min(8, static_cast<int>(stimChannels.size()));
    
    // 配置并触发刺激
    for (int i = 0; i < channelsToStimulate; ++i) {
        const QString& channelName = stimChannels[i];
        
        // 设置刺激参数
        setStimChannelParameters(channelName, currentHitFrequency, currentHitDuration); // 转换为秒

        // 设置刺激幅度并触发
        triggerStimChannel(channelName, currentHitAmplitude);
    }

    emit statusUpdated(QString("Hit stimulus triggered: %1 channels, %2Hz, %3ms, %4mV")
                      .arg(channelsToStimulate).arg(currentHitFrequency).arg(currentHitDuration).arg(currentHitAmplitude));
}

void GameThread::triggerMissStimulus()
{
    // Miss in Stimulus mode: 所有电极进行5Hz持续4秒的150mV刺激
    // 使用成员变量
    double currentMissFrequency, currentMissDuration, currentMissAmplitude;
    {
        std::lock_guard<std::mutex> lock(stimParamsMutex);
        currentMissFrequency = missStimFrequency;
        currentMissDuration = missStimDuration;
        currentMissAmplitude = missStimAmplitude;
    }
    // 线程安全地获取所有刺激通道
    std::vector<QString> stimChannels;
    {
        std::lock_guard<std::mutex> lock(signalSourcesMutex);
        for (int i = 0; i < state->signalSources->numGroups(); ++i) {
            SignalGroup* group = state->signalSources->groupByIndex(i);
            for (int j = 0; j < group->numChannels(); ++j) {
                Channel* channel = group->channelByIndex(j);
                if (channel && channel->getSignalType() == SignalType::StimSignal) {
                    stimChannels.push_back(channel->getNativeName());
                }
            }
        }
    }
    
    // 限制为最多8个通道
    int channelsToStimulate = std::min(8, static_cast<int>(stimChannels.size()));
    
    // 配置并触发刺激
    for (int i = 0; i < channelsToStimulate; ++i) {
        const QString& channelName = stimChannels[i];
        
        // 设置刺激参数 (双相脉冲)
        setStimChannelParameters(channelName, currentMissFrequency, currentMissDuration); // 转换为秒

        // 设置刺激幅度并触发 (双相: 正相+负相)
        triggerStimChannel(channelName, currentMissAmplitude);
    }

    emit statusUpdated(QString("Miss stimulus triggered: %1 channels, %2Hz, %3s, %4mV")
                      .arg(channelsToStimulate).arg(currentMissFrequency).arg(currentMissDuration / 1000.0).arg(currentMissAmplitude));
}

void GameThread::triggerSilentStimulus()
{
    // Silent mode: 停止所有刺激一段时间，然后让游戏以随机方向重新开始
    const int silentDuration = 2000; // 2秒静默期

    // 线程安全地获取所有刺激通道并停止刺激
    std::vector<QString> stimChannels;
    {
        std::lock_guard<std::mutex> lock(signalSourcesMutex);
        for (int i = 0; i < state->signalSources->numGroups(); ++i) {
            SignalGroup* group = state->signalSources->groupByIndex(i);
            for (int j = 0; j < group->numChannels(); ++j) {
                Channel* channel = group->channelByIndex(j);
                if (channel && channel->getSignalType() == SignalType::StimSignal) {
                    stimChannels.push_back(channel->getNativeName());
                    // 禁用刺激
                    setStimChannelEnabled(channel->getNativeName(), false);
                }
            }
        }
    }

    emit statusUpdated(QString("Silent mode: Stopped %1 stimulation channels for %2ms")
                      .arg(stimChannels.size()).arg(silentDuration));

    // 让游戏在静默期后重新开始（这会在PongGame中处理）
    // 注意：实际的球重新启动逻辑应该在PongGame中实现
    // 这里只是发出状态信号
}

void GameThread::setHitStimAmplitude(double amplitude)
{
    std::lock_guard<std::mutex> lock(stimParamsMutex);
    hitStimAmplitude = amplitude;
}

void GameThread::setHitStimFrequency(double frequency)
{
    std::lock_guard<std::mutex> lock(stimParamsMutex);
    hitStimFrequency = frequency;
}

void GameThread::setHitStimDuration(double duration)
{
    std::lock_guard<std::mutex> lock(stimParamsMutex);
    hitStimDuration = duration;
}

void GameThread::setMissStimAmplitude(double amplitude)
{
    std::lock_guard<std::mutex> lock(stimParamsMutex);
    missStimAmplitude = amplitude;
}

void GameThread::setMissStimFrequency(double frequency)
{
    std::lock_guard<std::mutex> lock(stimParamsMutex);
    missStimFrequency = frequency;
}

void GameThread::setMissStimDuration(double duration)
{
    std::lock_guard<std::mutex> lock(stimParamsMutex);
    missStimDuration = duration;
}
