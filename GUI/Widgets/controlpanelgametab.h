#ifndef CONTROLPANELGAMETAB_H
#define CONTROLPANELGAMETAB_H

#include <QtWidgets>
#include "systemstate.h"
#include "gamethread.h"
#include "ponggamewidget.h"

class ControllerInterface;
class CommandParser;

class ControlPanelGameTab : public QWidget
{
    Q_OBJECT
public:
    explicit ControlPanelGameTab(ControllerInterface* controllerInterface_, SystemState* state_, CommandParser* parser_, QWidget *parent = nullptr);
    void updateFromState();

    PongGameWidget* getPongGameWidget() const;

signals:
    void setGameEnabled(bool enabled);

public slots:
    void updateGameData(const GameState& gameState);
    void updatePerformanceMetrics();
    void updateSpikeRate(const std::map<QString, float>& spikesPerSecond);

private slots:
    void toggleGame(bool enabled);
    void setThresholdMultiplier(double value);
    void setMinThreshold(double value);
    void setRefractoryPeriod(int value);
    void setExperimentCondition(int index);
    void setHitStimAmplitude(double value);
    void setHitStimFrequency(double value);
    void setHitStimDuration(double value);
    void setMissStimAmplitude(double value);
    void setMissStimFrequency(double value);
    void setMissStimDuration(double value);
    void validateParameters();

private:
    SystemState* state;
    ControllerInterface* controllerInterface;

    QCheckBox *enableGameCheckBox;
    QDoubleSpinBox *thresholdMultiplierSpinBox;
    QDoubleSpinBox *minThresholdSpinBox;
    QSpinBox *refractoryPeriodSpinBox;
    QComboBox *conditionComboBox;

    QLabel *ballPositionLabel;
    QLabel *paddlePositionLabel;
    QLabel *bouncesLabel;

    // 刺激强度控制
    QDoubleSpinBox *hitStimAmplitudeSpinBox;
    QDoubleSpinBox *hitStimFrequencySpinBox;
    QDoubleSpinBox *hitStimDurationSpinBox;
    QDoubleSpinBox *missStimAmplitudeSpinBox;
    QDoubleSpinBox *missStimFrequencySpinBox;
    QDoubleSpinBox *missStimDurationSpinBox;

    // 性能监控
    QLabel *cpuLoadLabel;
    QLabel *fifoStatusLabel;
    QLabel *spikeRateLabel;
    QProgressBar *cpuLoadProgressBar;
    QProgressBar *fifoProgressBar;

    // 参数验证状态
    QLabel *validationStatusLabel;

    // 游戏图形显示widget
    PongGameWidget *pongGameWidget;
};

#endif // CONTROLPANELGAMETAB_H
