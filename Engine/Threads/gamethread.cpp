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
#include <QElapsedTimer>
#include <cmath>
#include <algorithm>
#include <csignal>

GameThread::GameThread(WaveformFifo* waveformFifo_, SystemState* state_, QObject* parent) :
    QThread(parent),
    waveformFifo(waveformFifo_),
    state(state_),
    pongGame(new PongGame()),
    thresholdMultiplier(5.0f),
    minThreshold(-20.0f), // 默认阈值改为负数
    refractoryPeriod(1000),  // 默认1000个样本的不应期
    lastStatsUpdate(0)
{
    keepGoing = false;
    running = false;
    stopThread = false;
    lastStatsUpdate = 0;

    // 运动区域通道在此处不再硬编码
}

GameThread::~GameThread()
{
    cleanupChannelProcessors();
    delete pongGame;
}

void GameThread::run()
{
    const int NumSamplesPer10ms = 200; // 20000Hz / 100Hz = 200个样本

    // macOS退出信号处理
#ifdef Q_OS_MAC
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGKILL);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
#endif

    // 初始化通道处理器
    initializeChannelProcessors();

    // 初始化统计数据
    int totalRallyCount = 0;
    int totalRallyLengthSum = 0;
    float averageRallyLength = 0.0f;

    while (!stopThread) {
        if (keepGoing) {
            running = true;

            while (keepGoing && !stopThread) {
                // 1. 等待并处理一个10ms的数据块，macOS上使用非阻塞读取避免退出时堵塞
#ifdef Q_OS_MAC
                if (waveformFifo->requestReadNewData(WaveformFifo::ReaderDisk, NumSamplesPer10ms, false)) {
#else
                if (waveformFifo->requestReadNewData(WaveformFifo::ReaderDisk, NumSamplesPer10ms, true)) {
#endif
                    processSampleBlock(NumSamplesPer10ms);

                    // 2. 游戏更新：严格在处理完数据后执行
                    // 统计运动区域的尖峰
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

                    // 更新游戏并获取事件
                    GameEvent event = pongGame->update(spikesUp, spikesDown);

                    // 处理游戏事件以触发反馈
                    switch(event) {
                        case GameEvent::BallHitPlayerPaddle:
                            // Hit: 所有8个刺激电极同时进行100Hz持续100ms的双相脉冲刺激
                            triggerHitStimulus();
                            emit sendHitStim();
                            break;
                        case GameEvent::PlayerMissed:
                            if (pongGame->getCondition() == ExperimentCondition::Stimulus) {
                                // Miss in Stimulus: 所有电极进行5Hz持续4秒的150mV刺激
                                triggerMissStimulus();
                                emit sendMissStim();
                            } else if (pongGame->getCondition() == ExperimentCondition::Silent) {
                                // Miss in Silent: 停止所有刺激
                                triggerSilentStimulus();
                                emit stopAllStim();
                            }
                            // NoFeedback模式下不提供额外反馈，但游戏逻辑已在PongGame中处理
                            break;
                        default:
                            break;
                    }

                    // 发送位置刺激
                    emit sendSensoryStim(pongGame->getSensoryStimZone());

                    // 更新rally统计 (仅在miss事件发生时)
                    if (event == GameEvent::PlayerMissed) {
                        int rallyLength = pongGame->getBounces();
                        totalRallyLengthSum += rallyLength;
                        totalRallyCount++;
                        if (totalRallyCount > 0) {
                            averageRallyLength = static_cast<float>(totalRallyLengthSum) / totalRallyCount;
                        }
                    }

                    // 发送游戏状态到UI
                    GameState currentState;
                    currentState.paddle1Y = pongGame->getPaddle1Y();
                    currentState.ballX = pongGame->getBallX();
                    currentState.ballY = pongGame->getBallY();
                    currentState.paddle2Y = 0; // AI paddle not used
                    currentState.paddleHeight = pongGame->getPaddleHeight(); // 球拍高度
                    currentState.bounces = pongGame->getBounces(); // 当前回合的反弹次数
                    currentState.rallyCount = totalRallyCount;     // 总回合数
                    currentState.avgRallyLength = averageRallyLength; // 平均回合长度
                    emit gameDataUpdated(currentState);

                    // 重置尖峰计数器
                    for (auto& counter : spikeCounters) {
                        counter.second = 0;
                    }

                    waveformFifo->freeOldData(WaveformFifo::ReaderDisk);
                } else {
                    // 如果FIFO中没有足够的数据，短暂休眠以避免CPU空转
                    usleep(100);
                    // macOS: 更频繁检查退出条件，避免堵塞
#ifdef Q_OS_MAC
                    if (stopThread || !keepGoing) continue;
#endif
                }

                // macOS: 在每轮循环后检查退出信号
#ifdef Q_OS_MAC
                if (stopThread || !keepGoing) break;
#endif
            }
            running = false;
        } else {
            // 非活跃时更频繁检查退出条件
            usleep(1000);
        }

        // macOS退出信号检查
#ifdef Q_OS_MAC
        if (stopThread) break;
#endif
    }

    cleanupChannelProcessors();
}

void GameThread::startRunning()
{
    keepGoing = true;
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
    thresholdMultiplier = multiplier;
}

void GameThread::setMinThreshold(float minThresholdValue)
{
    minThreshold = minThresholdValue;
}

void GameThread::setRefractoryPeriod(int samples)
{
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
    motorRegion1Channels.clear();
    motorRegion2Channels.clear();
    for(const auto& ch : upChannels) {
        motorRegion1Channels.push_back(ch);
    }
    for(const auto& ch : downChannels) {
        motorRegion2Channels.push_back(ch);
    }
}

void GameThread::setStimChannelEnabled(const QString& channelName, bool enabled)
{
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
    // 获取所有放大器通道
    std::vector<std::string> channelNames = state->signalSources->amplifierChannelsNameList();
    
    channelProcessors.clear();
    channelIndexMap.clear();
    spikeCounters.clear();
    
    double sampleRate = state->sampleRate->getNumericValue();
    
    for (size_t i = 0; i < channelNames.size(); ++i) {
        ChannelProcessor processor;
        processor.name = QString::fromStdString(channelNames[i]);
        processor.highpassFilter = new SecondOrderHighpassFilter(100.0, 0.707, sampleRate);  // 100Hz Bessel高通滤波器
        processor.lowpassFilter = new FirstOrderLowpassFilter(1.0, sampleRate);               // 1Hz Bessel低通滤波器
        processor.smoothedAbsValue = 0.0f;
        processor.threshold = minThreshold;
        processor.prevValue = 0.0f;
        processor.samplesSinceLastSpike = 0;
        
        channelProcessors.push_back(processor);
        channelIndexMap[processor.name] = static_cast<int>(i);
        spikeCounters[processor.name] = 0;
        spikesPerSecond[processor.name] = 0.0f;
    }
    
    emit statusUpdated("Initialized " + QString::number(channelProcessors.size()) + " channel processors");
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
        // 获取通道波形数据指针
        float* waveform = waveformFifo->getAnalogWaveformPointer(processor.name.toStdString());
        if (!waveform) continue;
        
        // 处理每个样本
        for (int t = 0; t < numSamples; ++t) {
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
                spikeCounters[processor.name]++;
            }
            
            processor.prevValue = filteredValue;
            processor.samplesSinceLastSpike++;
        }
    }
}

bool GameThread::detectSpike(ChannelProcessor& processor, float value, int64_t /*timestamp*/)
{
    // 检查是否在不应期内
    if (processor.samplesSinceLastSpike < refractoryPeriod) {
        return false;
    }
    
    // 检测是否超过负阈值且为负向过零
    if (processor.prevValue > processor.threshold && value <= processor.threshold) {
        processor.samplesSinceLastSpike = 0;
        return true;
    }
    
    return false;
}

void GameThread::applyStimParameters(const QString& channelName)
{
    // 注意：实际应用刺激参数需要ControllerInterface的参与
    // 这里只是一个接口示例，实际实现需要与ControllerInterface协作
    emit statusUpdated("Stim parameters updated for channel: " + channelName);
}

void GameThread::updateSpikesPerSecond()
{
    // 此函数现在可以用于更新UI的SPS显示，但游戏逻辑不再依赖它
    // The spike counters are now reset every 10ms for game logic.
    // This function might need rethinking if a separate 1-second counter is needed for display.
    emit spikesPerSecondUpdated(spikesPerSecond);
}

// 刺激触发函数实现
void GameThread::triggerHitStimulus()
{
    // Hit: 所有8个刺激电极同时进行100Hz持续100ms的双相脉冲刺激
    // 此处需要设置每个刺激电极的参数

    // 暂时通过信号发出，实际的刺激控制需要通过ControllerInterface
    emit statusUpdated("Hit stimulus triggered: 100Hz, 100ms for all 8 electrodes");

    // TODO: 实际实现时需要：
    // 1. 获取所有8个刺激电极通道
    // 2. 设置每个通道为100Hz频率，100ms持续时间
    // 3. 同时触发所有8个电极
}

void GameThread::triggerMissStimulus()
{
    // Miss in Stimulus mode: 所有电极进行5Hz持续4秒的150mV刺激
    emit statusUpdated("Miss stimulus triggered: 5Hz, 4s, 150mV for all electrodes");

    // TODO: 实际实现时需要：
    // 1. 获取所有刺激电极通道
    // 2. 设置阶段一刺激幅度为150mV，相位为-150mV (双相)
    // 3. 设置频率为5Hz，持续时间为4秒
    // 4. 触发所有8个电极同时进行刺激
}

void GameThread::triggerSilentStimulus()
{
    // Silent mode: 停止所有刺激一段时间，然后让游戏以随机方向重新开始
    emit statusUpdated("Silent mode: Stopping all stimulation temporarily");

    // TODO: 实际实现时需要：
    // 1. 停止所有8个刺激电极的刺激
    // 2. 等待一段时间后（例如2秒）
    // 3. 使球以随机向量重新开始运动
}
