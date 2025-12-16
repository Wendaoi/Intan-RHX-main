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

#ifndef CONTROLLERINTERFACE_H
#define CONTROLLERINTERFACE_H

#include <QObject>
#include <QString>
#include <QElapsedTimer>
#include <QTimer>
#include <QThread>
#include "rhxcontroller.h"
#include "datafilereader.h"
#include "rhxglobals.h"
#include "datastreamfifo.h"
#include "usbdatathread.h"
#include "waveformprocessorthread.h"
#include "rhxregisters.h"
#include "rhxdatareader.h"
#include "multicolumndisplay.h"
#include "savetodiskthread.h"
#include "audiothread.h"
#include "tcpdataoutputthread.h"
#include "systemstate.h"
#include "signalsources.h"
#include "xpucontroller.h"
#include "isidialog.h"
#include "psthdialog.h"
#include "spectrogramdialog.h"
#include "spikesortingdialog.h"
#include "gamethread.h" // 包含GameThread
#include "stimworker.h"

// Forward declaration for GameState
struct GameState;

const int BufferSizeInBlocks = 32;

class AbstractPanel;

class ControllerInterface : public QObject
{
    Q_OBJECT
public:
    ControllerInterface(SystemState* state_, AbstractRHXController* rhxController_, const QString& boardSerialNumber, bool useOpenCL,
                        DataFileReader* dataFileReader_ = nullptr, QObject* parent = nullptr, bool is7310_ = false);
    ~ControllerInterface();

    void rescanPorts(bool updateDisplay = false);

    void updateChipCommandLists(bool updateStimParams = false);
    void getCableDelay(std::vector<int> &delays) const { rhxController->getCableDelay(delays); }
    void setCableDelay(BoardPort port, int delay) { rhxController->setCableDelay(port, delay); }
    void enableExternalDigOut(BoardPort port, bool enable) { rhxController->enableExternalDigOut(port, enable); }
    void setExternalDigOutChannel(BoardPort port, int channel) { rhxController->setExternalDigOutChannel(port, channel); }
    void setManualCableDelays();

    void toggleAudioThread(bool enabled);
    void runTCPDataOutputThread();

    void runController();
    void runControllerSilently(double nSeconds, QProgressDialog* progress = nullptr);
    float measureRmsLevel(std::string waveName, double timeSec) const;
    void setAllSpikeDetectionThresholds();
    void sweepDisplay(double speed);
    bool rewindPossible() const { return waveformFifo->numWordsInMemory(WaveformFifo::ReaderDisplay) > 0; }
    bool fastForwardPossible() const { return currentSweepPosition < 0; }

    // 运行时退出保护方法
    void stopController();
    bool isRunning() const;

    void setDisplay(MultiColumnDisplay* display_) { display = display_; }
    void setControlPanel(AbstractPanel* controlPanel_) { controlPanel = controlPanel_; }
    void setISIDialog(ISIDialog* isiDialog_) { isiDialog = isiDialog_; }
    void setPSTHDialog(PSTHDialog* psthDialog_) { psthDialog = psthDialog_; }
    void setSpectrogramDialog(SpectrogramDialog* spectrogramDialog_) { spectrogramDialog = spectrogramDialog_; }
    void setSpikeSortingDialog(SpikeSortingDialog* spikeSortingDialog_) { spikeSortingDialog = spikeSortingDialog_; }

    QString getCurrentAudioChannel() const { return currentAudioChannel; }

    void setStimSequenceParameters(Channel* ampChannel);
    void setAnalogOutSequenceParameters(Channel* anOutChannel);
    void setDigitalOutSequenceParameters(Channel* digOutChannel);

    void setManualStimTrigger(int trigger, bool triggerOn);
    void setManualStimTrigger(QString keyName, bool triggerOn);

    void setChargeRecoveryParameters(bool mode, RHXRegisters::ChargeRecoveryCurrentLimit currentLimit,
                                     double targetVoltage);

    void setAmpSettleMode(bool useFastSettle) { rhxController->setAmpSettleMode(useFastSettle); }
    void setGlobalSettlePolicy(bool settleWholeHeadstageA, bool settleWholeHeadstageB, bool settleWholeHeadstageC,
                               bool settleWholeHeadstageD, bool settleAllHeadstages)
        { rhxController->setGlobalSettlePolicy( settleWholeHeadstageA, settleWholeHeadstageB, settleWholeHeadstageC,
                                                settleWholeHeadstageD, settleAllHeadstages); }

    void setDacGain(int dacGainIndex);
    void setAudioNoiseSuppress(int noiseSuppressIndex);
    void setDacHighpassFilterEnabled(bool enabled);
    void setDacHighpassFilterFrequency(double frequency);
    void setDacChannel(int dac, const QString& channelName);
    void setDacRefChannel(const QString& channelName);
    void setDacThreshold(int dac, int threshold);
    void setDacEnabled(int dac, bool enabled);
    void setTtlOutMode(bool mode1, bool mode2, bool mode3, bool mode4, bool mode5, bool mode6, bool mode7, bool mode8);

    // 游戏线程控制槽
    void toggleGameThread(bool enabled);
    void setGameThresholdMultiplier(double multiplier);
    void setGameMinThreshold(double minThreshold);
    void setGameRefractoryPeriod(int samples);
    void setGameExperimentCondition(int condition);
    void setMissFreezeDurationMs(int durationMs);

    void setHitStimAmplitude(double amplitude) { if (gameThread) gameThread->setHitStimAmplitude(amplitude); }
    void setHitStimFrequency(double frequency) { if (gameThread) gameThread->setHitStimFrequency(frequency); }
    void setHitStimDuration(double duration) { if (gameThread) gameThread->setHitStimDuration(duration); }
    void setMissStimAmplitude(double amplitude) { if (gameThread) gameThread->setMissStimAmplitude(amplitude); }
    void setMissStimFrequency(double frequency) { if (gameThread) gameThread->setMissStimFrequency(frequency); }
    void setMissStimDuration(double duration) { if (gameThread) gameThread->setMissStimDuration(duration); }
    // Voltage-target setters (mV). Currents will be auto-computed from electrode impedance.
    void setHitTargetCurrent(double ua);
    void setMissTargetCurrent(double ua);
    void setSensoryTargetCurrent(double ua);

    void enableFastSettle(bool enabled);
    void enableExternalFastSettle(bool enabled);
    void setExternalFastSettleChannel(int channel);

    SaveToDiskThread* saveThread() const { return saveToDiskThread; }

    QString playbackFileName() const;
    QString currentTimePlaybackFile() const;
    QString startTimePlaybackFile() const;
    QString endTimePlaybackFile() const;

    void resetWaveformFifo();

    bool measureImpedances();
    bool saveImpedances();

    double swBufferPercentFull() const;
    double latestWaveformProcessorCpuLoad() const { return waveformProcessorCpuLoad; }

    void uploadAmpSettleSettings();
    void uploadChargeRecoverySettings();
    void uploadBandwidthSettings();
    void uploadAutoStimParameters(int stream);
    void clearStimParameters(int stream);
    void uploadStimParameters(Channel* channel);
    void uploadStimParameters();
    void modulateSpikes(PaddleAction action);

    void logStimChannelImpedances();

signals:
    void setTimeLabel(QString text);
    void setTopStatusLabel(QString text);
    void haveStopped();
    void setHardwareFifoStatus(double percentFull);
    void cpuLoadPercent(double percent);
    void TCPErrorMessage(QString errorMessage);
    void gameDataUpdated(const GameState& gameState);
    void spikeRateScalar(float rateHz);

public slots:
    void updateFromState();
    void updateCurrentAudioChannel(QString name);
    void manualStimTriggerOn(QString keyName);
    void manualStimTriggerOff(QString keyName);
    void manualStimTriggerPulse(QString keyName);

private slots:
    void updateHardwareFifo(double percentFull) { emit setHardwareFifoStatus(percentFull); }
    void updateWaveformProcessorCpuLoad(double percentLoad) { waveformProcessorCpuLoad = percentLoad; }

    // 游戏数据更新槽
    void onGameDataUpdated(const GameState& gameState);

    // 游戏刺激处理槽
    void handleSensoryStim(int zone);
    void handleHitStim();
    void handleMissStim();
    void handleStopAllStim();
    void handleStartSilentWindow(int durationMs);
    void onStimWorkerTriggerChannel(int zone);
    void onStimWorkerHitBurst();

private:
    // Stim dispatch helpers and pacing guards
    void configureHitStimParams();
    void configureMissStimParams(); // legacy (train-based), may be unused when session-based enabled
    void startMissStimSession();
    void onMissStimSessionTick();
    void configureSensoryParams();
    int sensoryChannelIndexForZone(int zone) const { return (zone >= 0 && zone < 8) ? zone : -1; }
    QString sensoryChannelNameForZone(int zone) const { return QString("A-%1").arg(zone, 3, 10, QChar('0')); }
    double computeCurrentFromImpedanceUA(const QString& amplifierNativeName, double target_mV) const;

    bool hitStimConfigured = false;
    bool missStimConfigured = false;
    bool sensoryConfigured = false;
    int minStimIntervalMs = 50; // basic pacing to avoid swamping USB/GUI
    QElapsedTimer hitStimTimer;
    QElapsedTimer missStimTimer;
    QTimer missSessionTimer;
    bool missSessionActive = false;
    int missTicksRemaining = 0;
    int missSessionIntervalMs = 200; // 5 Hz
    double missSessionAmplitude_uA = 1.5;
    QElapsedTimer sensoryClock;
    qint64 lastSensoryTriggerMs[8] = {0,0,0,0,0,0,0,0};

    // Sensory coding parameters
    double sensoryMinHz = 4.0;
    double sensoryMaxHz = 40.0;
    double sensoryAmplitude_uA = 0.75; // starting point per paper suggestion
    int sensoryPulseWidthUs = 100;      // biphasic first phase duration

    // Last game state for proximity (ballX) and scan flags
    GameState lastGameState;
    bool haveGameState = false;

    // Feedback silent window (for Silent condition)
    bool feedbackSilentActive = false;
    int feedbackSilentDurationMs = 2000;
    QElapsedTimer feedbackSilentTimer;

    // Voltage targets (mV) that will be converted to per-channel current using measured impedance.
    double targetHit_uA = 1.0;
    double targetMiss_uA = 2.0;
    double targetSensory_uA = 1.0;
    void openController(const QString& boardSerialNumber);
    void initializeController();
    int scanPorts(std::vector<ChipType> &chipType, std::vector<int> &portIndex, std::vector<int> &commandStream,
                  std::vector<int> &numChannelsOnPort);
    void addAmplifierChannels(const std::vector<ChipType> &chipType, const std::vector<int> &portIndex,
                              const std::vector<int> &commandStream, const std::vector<int> &numChannelsOnPort);
    void enablePlaybackChannels();
    void addPlaybackHeadstageChannels();

    void sendTCPError(QString errorMessage);
    void pipeReadErrorMessage(int errorID);

    SystemState* state;
    AbstractRHXController* rhxController;
    DataFileReader* dataFileReader;
    TCPDataOutputThread* tcpDataOutputThread;

    XPUController* xpuController;

    DataStreamFifo* usbStreamFifo;
    USBDataThread* usbDataThread;
    WaveformFifo* waveformFifo;
    WaveformProcessorThread* waveformProcessorThread;

    MultiColumnDisplay* display;
    AbstractPanel* controlPanel;
    ISIDialog* isiDialog;
    PSTHDialog* psthDialog;
    SpectrogramDialog* spectrogramDialog;
    SpikeSortingDialog* spikeSortingDialog;

    AudioThread* audioThread;
    SaveToDiskThread* saveToDiskThread;
    GameThread* gameThread; // GameThread实例

    int currentSweepPosition;

    bool audioEnabled;
    bool tcpDataOutputEnabled;

    QString currentAudioChannel;

    double hardwareFifoPercentFull;
    double waveformProcessorCpuLoad;
    std::vector<double> cpuLoadHistory;

    bool is7310;

    void outOfMemoryError(double memRequiredGB);

    // Stim worker thread
    QThread* stimThread = nullptr;
    StimWorker* stimWorker = nullptr;
};

#endif // CONTROLLERINTERFACE_H
