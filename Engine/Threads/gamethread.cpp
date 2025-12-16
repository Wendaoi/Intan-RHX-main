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
#include <QDebug>
#include <QElapsedTimer>
#include <QDateTime>
#include <unordered_set>
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
        lastSpikeDiff = 0;
        spikeDiffLogCounter = 0;
        resetBucketState(state->sampleRate->getNumericValue());
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
        motorUpSet.clear();
        motorDownSet.clear();
    }
    
    // 清空刺激参数锁
    {
        std::lock_guard<std::mutex> stimLock(stimParamsMutex);
        // 无特定清理需要
    }
}

void GameThread::resetBucketState(double sampleRate)
{
    samplesPerBucket = std::max(1, static_cast<int>(std::round(sampleRate * 0.01f)));
    bucketSamplesRemaining = samplesPerBucket;
    completedBucketDiffs.clear();
    decisionWindow.clear();
    windowSumUp = 0;
    windowSumDown = 0;
    // 将500ms转换为桶数：bucketDuration = samplesPerBucket / sampleRate
    double bucketDurationSec = (sampleRate > 0.0) ? (static_cast<double>(samplesPerBucket) / sampleRate) : 0.01;
    bucketsPerDecisionWindow = std::max(1, static_cast<int>(std::round(0.5 / bucketDurationSec)));
    latestBucketSpikeCountUp = 0;
    latestBucketSpikeCountDown = 0;
    spikeCounters.clear();
    spikesPerSecond.clear();
    lastSpikeDiff = 0;
    spikeDiffLogCounter = 0;
    lastStatsUpdate = 0;
    for (auto& processor : channelProcessors) {
        processor.bucketSpikeCount = 0;
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
    refractoryPeriod(std::max(1, static_cast<int>(std::round(state->sampleRate->getNumericValue() * 0.01f)))),
    samplesPerBucket(std::max(1, static_cast<int>(std::round(state->sampleRate->getNumericValue() * 0.01f)))),
    bucketSamplesRemaining(samplesPerBucket),
    completedBucketDiffs(),
    latestBucketSpikeCountUp(0),
    latestBucketSpikeCountDown(0),
    spikeCounters(),
    spikesPerSecond(),
    lastStatsUpdate(0),
    totalRallyCount(0),
    totalRallyLengthSum(0),
    averageRallyLength(0.0f),
    lastSpikeDiff(0),
    spikeDiffLogCounter(0),
    lastGameStateEmit(std::chrono::steady_clock::time_point::min()),
    // Limit UI updates to ~60 Hz to reduce cross-thread signal overhead.
    gameStateUiInterval(std::chrono::milliseconds(16)),
    hitStimAmplitude(100.0),
    hitStimFrequency(100.0),
    hitStimDuration(100.0),
    missStimAmplitude(150.0),
    missStimFrequency(5.0),
    missStimDuration(4000.0),
    missFreezeDurationMs(2000),
    missFreezeUntil(std::chrono::steady_clock::time_point::min())
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
        gameController = std::make_unique<SpikeBasedController>([this]() {
            return std::make_pair(latestBucketSpikeCountUp, latestBucketSpikeCountDown);
        });
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
            if (!running.load()) {
                // Force next emission to fire immediately when the game restarts.
                lastGameStateEmit = std::chrono::steady_clock::time_point::min();
            }
            running = true;

            // Clear any excess semaphore resources that may have accumulated during startup.
            if (waveformFifo->dataForGameThread.available() > 0) {
                waveformFifo->dataForGameThread.acquire(waveformFifo->dataForGameThread.available());
            }

            while (keepGoing && !stopThread) {
                // 每轮限流最多处理若干块，避免长时间独占影响其他读者/绘制。
                constexpr int kMaxDrainPerCycle = 8;
                int drainedThisCycle = 0;
                bool processedAny = false;
                while (keepGoing && !stopThread && drainedThisCycle < kMaxDrainPerCycle) {
                    if (!waveformFifo->requestReadNewData(WaveformFifo::ReaderGame, numSamples, true)) {
                        break; // 本轮没有更多连续数据
                    }

                    processedAny = true;

                    // 处理一块数据
                    processSampleBlock(numSamples);

                    // --- LEARNING/CONTROL LOGIC ---
                    int paddle_movement = 0; // -1 for up, 1 for down, 0 for stay
                    if (state->getAcquisitionMode() == LearningMode) {
                        oldState = getCurrentGameStateInfo();
                        action = gameController->getAction(oldState);
                        emit requestModulateSpikes(static_cast<int>(action));
                    }

                    // --- UNIFIED CONTROL ---
                constexpr int LogIntervalBuckets = 10;
                bool bucketUpdated = false;
                while (auto bucketCounts = consumeCompletedBucketCounts()) {
                    latestBucketSpikeCountUp = bucketCounts->first;
                    latestBucketSpikeCountDown = bucketCounts->second;
                    // 更新500ms滑动窗口（以桶为单位）
                    decisionWindow.emplace_back(latestBucketSpikeCountUp, latestBucketSpikeCountDown);
                    windowSumUp += latestBucketSpikeCountUp;
                    windowSumDown += latestBucketSpikeCountDown;
                    while ((int)decisionWindow.size() > bucketsPerDecisionWindow) {
                        windowSumUp -= decisionWindow.front().first;
                        windowSumDown -= decisionWindow.front().second;
                        decisionWindow.pop_front();
                    }
                    int spikeDiff = latestBucketSpikeCountUp - latestBucketSpikeCountDown;
                    bool shouldLog = (spikeDiff != lastSpikeDiff);
                    if (!shouldLog) {
                        if (++spikeDiffLogCounter >= LogIntervalBuckets) {
                            shouldLog = true;
                                spikeDiffLogCounter = 0;
                            }
                        } else {
                            spikeDiffLogCounter = 0;
                        }
                        if (shouldLog) {
                            lastSpikeDiff = spikeDiff;
                        }
                        bucketUpdated = true;
                }
                if (bucketUpdated) {
                    updateSpikesPerSecond();
                }

                // 使用500ms滑动窗口的区域总尖峰数来判定挡板方向
                int upWin = windowSumUp;
                int downWin = windowSumDown;
                if (upWin > downWin) {
                    paddle_movement = -1; // Up
                } else if (downWin > upWin) {
                    paddle_movement = 1; // Down
                }

                // Miss 刺激后的冻结窗口：保持挡板不动
                auto nowFreeze = std::chrono::steady_clock::now();
                if (nowFreeze < missFreezeUntil) {
                    paddle_movement = 0;
                }

                    // --- GAME UPDATE ---
                    GameEvent event = pongGame->update(paddle_movement);

                    // --- LEARNING AGENT UPDATE ---
                    if (state->getAcquisitionMode() == LearningMode) {
                        float reward = calculateReward(event, *pongGame);
                        GameStateInfo newState = getCurrentGameStateInfo();
                        gameController->update(oldState, action, reward, newState);
                    }

                    // --- EVENT HANDLING (condition-dependent feedback window) ---
                    ExperimentCondition cond = pongGame->getCondition();
                    constexpr int kSilentWindowMs = 2000; // 2 s silent window
                    switch(event) {
                        case GameEvent::BallHitPlayerPaddle:
                            if (cond == ExperimentCondition::Stimulus) {
                                emit sendHitStim();
                            } else if (cond == ExperimentCondition::Silent) {
                                emit startSilentWindow(kSilentWindowMs);
                            } // NoFeedback: do nothing
                            break;
                        case GameEvent::PlayerMissed:
                            if (cond == ExperimentCondition::Stimulus) {
                                emit sendMissStim();
                                // 触发Miss后冻结挡板移动一段时间
                                missFreezeUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(missFreezeDurationMs.load());
                            } else if (cond == ExperimentCondition::Silent) {
                                emit startSilentWindow(kSilentWindowMs);
                            } // NoFeedback: do nothing
                            break;
                        default:
                            break;
                    }
                    // 感知刺激请求改为与UI节流同步，避免每数据块都发出请求造成拥塞。

                    if (event == GameEvent::PlayerMissed) {
                        int rallyLength = pongGame->getBounces();
                        totalRallyLengthSum += rallyLength;
                        totalRallyCount++;
                        if (totalRallyCount > 0) {
                            averageRallyLength = static_cast<float>(totalRallyLengthSum) / totalRallyCount;
                        }
                        pongGame->resetBounces();
                    }

                    waveformFifo->freeOldData(WaveformFifo::ReaderGame);
                    ++drainedThisCycle;
                }

                // 若本轮未处理任何数据，则短暂让出CPU。
                if (!processedAny) {
                    usleep(100);
                    continue;
                }

                // 节流UI更新，仅在一定时间间隔后发送最后一帧状态。
                GameState currentState;
                currentState.paddle1Y = pongGame->getPaddle1Y();
                currentState.ballX = pongGame->getBallX();
                currentState.ballY = pongGame->getBallY();
                currentState.paddle2Y = 0;
                currentState.paddleHeight = pongGame->getPaddleHeight();
                currentState.bounces = pongGame->getBounces();
                currentState.rallyCount = totalRallyCount;
                currentState.avgRallyLength = averageRallyLength;

                auto now = std::chrono::steady_clock::now();
                if (lastGameStateEmit == std::chrono::steady_clock::time_point::min() ||
                    now - lastGameStateEmit >= gameStateUiInterval) {
                    // 在UI刷新时机同时发送一次感知刺激请求（StimWorker内部将做速率编码与间隔门控）。
                    emit sendSensoryStim(pongGame->getSensoryStimZone());
                    emit gameDataUpdated(currentState);
                    lastGameStateEmit = now;
                }
            }
            running = false;
            usleep(1000); // Add a small sleep even when not running to yield CPU
        } else {
            // 当游戏未运行时，ReaderGame 仍然是一个有效的FIFO读者。
            // 如果不主动消耗，它会成为“最慢的读者”，限制FIFO释放，导致SW缓冲区上升。
            // 这里做一次轻量级的被动排空：读取少量数据块后立即释放，不做任何计算。
            int drainIterations = 0;
            while (waveformFifo->dataForGameThread.available() > 0 && drainIterations < 4) {
                waveformFifo->dataForGameThread.acquire();
                if (waveformFifo->requestReadNewData(WaveformFifo::ReaderGame, numSamples, true)) {
                    waveformFifo->freeOldData(WaveformFifo::ReaderGame);
                }
                ++drainIterations;
            }
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
    motorUpSet.clear();
    motorDownSet.clear();
    for(const auto& ch : upChannels) {
        motorRegion1Channels.push_back(ch);
        motorUpSet.insert(ch);
    }
    for(const auto& ch : downChannels) {
        motorRegion2Channels.push_back(ch);
        motorDownSet.insert(ch);
    }

    {
        std::lock_guard<std::mutex> statsLock(statsMutex);
        resetBucketState(state->sampleRate->getNumericValue());
        for (const auto& name : motorRegion1Channels) {
            spikeCounters[name] = 0;
        }
        for (const auto& name : motorRegion2Channels) {
            spikeCounters[name] = 0;
        }
    }
}

void GameThread::setSensoryRegionChannels(const std::vector<QString>& sensoryChannels)
{
    std::unique_lock<std::shared_mutex> lock(motorChannelsMutex);
    sensoryRegionChannels = sensoryChannels;
    qDebug() << "[GameThread] 感知区域通道数量:" << sensoryRegionChannels.size();
}

void GameThread::setDefaultMotorRegionsForDemo()
{
    std::vector<QString> sensoryChannels;
    std::vector<QString> defaultUpChannels;
    std::vector<QString> defaultDownChannels;

    // 感知区域 A-000 - A-007
    for (int i = 0; i <= 7; ++i) {
        QString channelName = QString("A-%1").arg(i, 3, 10, QChar('0'));
        sensoryChannels.push_back(channelName);
    }

    // 上移控制 A-008 - A-015
    for (int i = 8; i <= 15; ++i) {
        QString channelName = QString("A-%1").arg(i, 3, 10, QChar('0'));
        defaultUpChannels.push_back(channelName);
    }

    // 下移控制 A-016 - A-023
    for (int i = 16; i <= 23; ++i) {
        QString channelName = QString("A-%1").arg(i, 3, 10, QChar('0'));
        defaultDownChannels.push_back(channelName);
    }

    setSensoryRegionChannels(sensoryChannels);
    setMotorRegions(defaultUpChannels, defaultDownChannels);

    qDebug() << "[GameThread] 设置新的区域配置:";
    qDebug() << "  感知区域 (A000-A007):" << sensoryChannels.size() << "个通道";
    qDebug() << "  上移控制 (A008-A015):" << defaultUpChannels.size() << "个通道";
    qDebug() << "  下移控制 (A016-A023):" << defaultDownChannels.size() << "个通道";
    qDebug() << "[GameThread] 配置逻辑: 比较上下区域尖峰总数控制挡板, 感知区域用于刺激映射";
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
        resetBucketState(sampleRate);
        refractoryPeriod = std::max(1, samplesPerBucket);

        std::shared_lock<std::shared_mutex> motorLock(motorChannelsMutex);
        bool filterByInterest = !(motorUpSet.empty() && motorDownSet.empty());
        for (size_t i = 0; i < channelNames.size(); ++i) {
            ChannelProcessor processor;
            processor.name = channelNames[i];
            bool inUp = motorUpSet.count(processor.name) > 0;
            bool inDown = motorDownSet.count(processor.name) > 0;
            bool inSensory = std::find(sensoryRegionChannels.begin(), sensoryRegionChannels.end(), processor.name) != sensoryRegionChannels.end();
            if (filterByInterest && !inUp && !inDown && !inSensory) {
                continue;
            }
            processor.sampleRate = sampleRate;
            processor.source = ChannelProcessor::DataSource::Invalid;
            processor.analogWaveform = nullptr;
            processor.gpuAddress = { GpuWaveformWideband, -1 };
            processor.hasSpkDigital = false;
            processor.spkWaveform = nullptr;
            processor.stimFlagsWaveform = nullptr;
            processor.belongsToUpRegion = inUp;
            processor.belongsToDownRegion = inDown;
            processor.bucketSpikeCount = 0;
            processor.stimActive = false;
            processor.highpassFilter = nullptr;
            processor.lowpassFilter = nullptr;
            processor.smoothedAbsValue = 0.0f;
            processor.threshold = minThreshold;
            processor.prevValue = 0.0f;
            processor.samplesSinceLastSpike = 0;

            std::string spkWaveName = (processor.name + "|SPK").toStdString();
            processor.spkWaveform = waveformFifo->getDigitalWaveformPointer(spkWaveName);
            if (processor.spkWaveform) {
                processor.hasSpkDigital = true;
            }

            if (!processor.hasSpkDigital) {
                std::string highWaveName = (processor.name + "|HIGH").toStdString();
                GpuWaveformAddress highAddress = waveformFifo->getGpuWaveformAddress(highWaveName);
                if (highAddress.waveformIndex >= 0) {
                    processor.source = ChannelProcessor::DataSource::GpuHighpass;
                    processor.gpuAddress = highAddress;
                } else {
                    std::string wideWaveName = (processor.name + "|WIDE").toStdString();
                    GpuWaveformAddress wideAddress = waveformFifo->getGpuWaveformAddress(wideWaveName);
                    if (wideAddress.waveformIndex >= 0) {
                        processor.source = ChannelProcessor::DataSource::GpuWideband;
                        processor.gpuAddress = wideAddress;
                    } else {
                        std::string dcWaveName = (processor.name + "|DC").toStdString();
                        processor.analogWaveform = waveformFifo->getAnalogWaveformPointer(dcWaveName);
                        if (processor.analogWaveform) {
                            processor.source = ChannelProcessor::DataSource::AnalogDC;
                            processor.highpassFilter = new SecondOrderHighpassFilter(10.0, 0.707, sampleRate);
                            processor.lowpassFilter = new FirstOrderLowpassFilter(1.0, sampleRate);
                        }
                    }
                }
            }

            // 获取该通道的刺激标志波形（仅 StimRecord 控制器有效）
            {
                std::string stimWaveName = (processor.name + "|STIM").toStdString();
                processor.stimFlagsWaveform = waveformFifo->getDigitalWaveformPointer(stimWaveName);
                if (!processor.stimFlagsWaveform) {
                    if (inSensory) {
                        qWarning() << "[GameThread] STIM waveform missing for" << processor.name;
                    }
                }
            }

            if (processor.source == ChannelProcessor::DataSource::Invalid && !processor.hasSpkDigital) {
                qWarning() << "[GameThread] 无法获取通道数据源:" << processor.name;
                delete processor.highpassFilter;
                delete processor.lowpassFilter;
                continue;
            }

            channelProcessors.push_back(processor);
            channelIndexMap[processor.name] = static_cast<int>(channelProcessors.size() - 1);
            spikeCounters[processor.name] = 0;
            spikesPerSecond[processor.name] = 0.0f;

            if (channelProcessors.size() <= 3) {
                qDebug() << "[GameThread] 创建通道处理器[" << channelProcessors.size() - 1 << "]:"
                         << "名称:" << processor.name
                         << "采样率:" << sampleRate << "Hz"
                         << "数据源:" << (processor.hasSpkDigital ? "GPU-SPK" :
                                          (processor.source == ChannelProcessor::DataSource::AnalogDC ? "DC" :
                                           (processor.source == ChannelProcessor::DataSource::GpuHighpass ? "GPU-HIGH" : "GPU-WIDE")));
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
        QString sourceStr = "UNKNOWN";
        if (processor.source == ChannelProcessor::DataSource::AnalogDC) {
            sourceStr = "DC";
        } else if (processor.source == ChannelProcessor::DataSource::GpuHighpass) {
            sourceStr = "GPU-HIGH";
        } else if (processor.source == ChannelProcessor::DataSource::GpuWideband) {
            sourceStr = "GPU-WIDE";
        }
        qDebug() << "[GameThread]   通道" << count << ":" << processor.name
                 << "采样率:" << processor.sampleRate << "Hz"
                 << "阈值:" << processor.threshold
                 << "数据源:" << sourceStr;
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
    completedBucketDiffs.clear();
}

void GameThread::processSampleBlock(int numSamples)
{
    if (numSamples <= 0) {
        return;
    }

    std::vector<ChannelProcessor*> activeChannels;  // for spike counting
    std::vector<ChannelProcessor*> stimChannels;    // for STIM observation
    activeChannels.reserve(channelProcessors.size());
    stimChannels.reserve(channelProcessors.size());
    for (auto& processor : channelProcessors) {
        if (processor.hasSpkDigital) {
            activeChannels.push_back(&processor);
        }
        if (processor.stimFlagsWaveform) {
            stimChannels.push_back(&processor);
        }
    }

    if (activeChannels.empty()) {
        bucketSamplesRemaining = std::max(0, bucketSamplesRemaining - numSamples);
        return;
    }

    int bufferIndex = waveformFifo->getReadIndex(WaveformFifo::ReaderGame);
    const int bufferCapacity = waveformFifo->getBufferCapacity();

    auto bucketStart = std::chrono::high_resolution_clock::now();
    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex) {
        for (ChannelProcessor* processor : activeChannels) {
            if (processor->spkWaveform[bufferIndex] & SpikeIdValidSpikeMask) {
                processor->bucketSpikeCount++;
            }
        }
        // STIM detection independent of spike availability
        for (ChannelProcessor* processor : stimChannels) {
            bool stimNow = (processor->stimFlagsWaveform[bufferIndex] & 0x0001u) != 0;
            if (stimNow && !processor->stimActive) {
                uint32_t ts = waveformFifo->getTimeStamp(WaveformFifo::ReaderGame, sampleIndex);
                emit stimObserved(processor->name, ts);
            }
            processor->stimActive = stimNow;
        }

        if (--bucketSamplesRemaining == 0) {
            int bucketSpikeUp = 0;
            int bucketSpikeDown = 0;

            auto bucketEnd = std::chrono::high_resolution_clock::now();
            {
                std::lock_guard<std::mutex> statsLock(statsMutex);
                spikeCounters.clear();
                for (auto* processor : activeChannels) {
                    const int count = processor->bucketSpikeCount;
                    spikeCounters[processor->name] = count;
                    if (processor->belongsToUpRegion) {
                        bucketSpikeUp += count;
                    } else if (processor->belongsToDownRegion) {
                        bucketSpikeDown += count;
                    }
                    processor->bucketSpikeCount = 0;
                }
                for (const auto& name : motorUpSet) {
                    spikeCounters.try_emplace(name, 0);
                }
                for (const auto& name : motorDownSet) {
                    spikeCounters.try_emplace(name, 0);
                }
            }

            completedBucketDiffs.emplace_back(bucketSpikeUp, bucketSpikeDown);
            // Removed per-bucket debug timing to reduce high-frequency logging overhead.
            // auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(bucketEnd - bucketStart).count();
            bucketStart = std::chrono::high_resolution_clock::now();
            bucketSamplesRemaining = samplesPerBucket;
        }

        if (++bufferIndex >= bufferCapacity) {
            bufferIndex = 0;
        }
    }
}

std::optional<std::pair<int, int>> GameThread::consumeCompletedBucketCounts()
{
    if (completedBucketDiffs.empty()) {
        return std::nullopt;
    }
    auto result = completedBucketDiffs.front();
    completedBucketDiffs.pop_front();
    return result;
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
        
        double windowSeconds = 0.0;
        const double currentSampleRate = state->sampleRate->getNumericValue();
        if (currentSampleRate > 0.0) {
            windowSeconds = static_cast<double>(samplesPerBucket) / currentSampleRate;
        }
        
        // 重置统计
        std::map<QString, float> newSpikesPerSecond;
        
        // 计算每个通道的SPS
        for (const auto& processor : channelProcessors) {
            const QString& channelName = processor.name;
            
            // 获取该通道的尖峰计数
            int spikeCount = spikeCountsSnapshot[channelName];
            
            // 计算每秒尖峰率
            float sps = 0.0f;
            if (windowSeconds > 0.0) {
                sps = static_cast<float>(static_cast<double>(spikeCount) / windowSeconds);
            }
            
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
        
        // 发送更新信号（完整映射）
        emit spikesPerSecondUpdated(newSpikesPerSecond);

        // 计算一个简化数值（活动通道的平均Hz）并发送，便于UI显示
        float totalSPS = 0.0f;
        int activeChannels = 0;
        for (const auto& pair : newSpikesPerSecond) {
            if (pair.second > 0.1f) {
                totalSPS += pair.second;
                activeChannels++;
            }
        }
        if (activeChannels > 0) {
            float avgSPS = totalSPS / activeChannels;
            emit spikeRateScalar(avgSPS);
            emit statusUpdated(QString("SPS Update: %1 active channels, avg %2 Hz")
                                   .arg(activeChannels)
                                   .arg(avgSPS, 0, 'f', 1));
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

void GameThread::setMissFreezeDurationMs(int durationMs)
{
    if (durationMs < 0) durationMs = 0;
    missFreezeDurationMs.store(durationMs);
}
