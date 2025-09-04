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

GameThread::GameThread(WaveformFifo* waveformFifo_, SystemState* state_, QObject* parent) :
    QThread(parent),
    waveformFifo(waveformFifo_),
    state(state_),
    thresholdMultiplier(5.0f),
    minThreshold(20.0f),
    refractoryPeriod(1000)  // 默认1000个样本的不应期
{
    keepGoing = false;
    running = false;
    stopThread = false;
    lastStatsUpdate = 0;
}

GameThread::~GameThread()
{
    cleanupChannelProcessors();
}

void GameThread::run()
{
    const int NumSamples = RHXDataBlock::samplesPerDataBlock(state->getControllerTypeEnum());
    
    // 初始化通道处理器
    initializeChannelProcessors();
    
    while (!stopThread) {
        if (keepGoing) {
            running = true;
            
            QElapsedTimer statsTimer;
            statsTimer.start();
            lastStatsUpdate = 0;
            
            while (keepGoing && !stopThread) {
                if (waveformFifo->requestReadNewData(WaveformFifo::ReaderDisk, NumSamples, false)) {
                    // 处理新数据块
                    processSampleBlock(NumSamples);
                    
                    // 更新统计信息（每秒一次）
                    if (statsTimer.elapsed() >= 1000) {
                        updateSpikesPerSecond();
                        statsTimer.restart();
                    }
                    
                    waveformFifo->freeOldData(WaveformFifo::ReaderDisk);
                } else {
                    usleep(1000);  // 等待新数据
                }
            }
            running = false;
        } else {
            usleep(10000);  // 线程未激活时等待
        }
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
    
    emit statusUpdated(QString("Initialized %1 channel processors").arg(channelProcessors.size()));
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
            
            // 计算动态阈值
            processor.threshold = (std::max)(minThreshold, processor.smoothedAbsValue * thresholdMultiplier);
            
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

bool GameThread::detectSpike(ChannelProcessor& processor, float value, int64_t timestamp)
{
    // 检查是否在不应期内
    if (processor.samplesSinceLastSpike < refractoryPeriod) {
        return false;
    }
    
    // 检测是否超过阈值且为正向过零
    if (processor.prevValue < processor.threshold && value >= processor.threshold) {
        processor.samplesSinceLastSpike = 0;
        return true;
    }
    
    return false;
}

void GameThread::applyStimParameters(const QString& channelName)
{
    // 注意：实际应用刺激参数需要ControllerInterface的参与
    // 这里只是一个接口示例，实际实现需要与ControllerInterface协作
    emit statusUpdated(QString("Stim parameters updated for channel: %1").arg(channelName));
}

void GameThread::updateSpikesPerSecond()
{
    emit spikesPerSecondUpdated(spikesPerSecond);
    
    // 重置计数器
    for (auto& counter : spikeCounters) {
        counter.second = 0;
    }
}