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

signals:
    void setGameEnabled(bool enabled);

public slots:
    void updateGameData(const GameState& gameState);

private slots:
    void toggleGame(bool enabled);
    void setThresholdMultiplier(double value);
    void setMinThreshold(double value);
    void setRefractoryPeriod(int value);
    void setExperimentCondition(int index);

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

    // 游戏图形显示widget
    PongGameWidget *pongGameWidget;
};

#endif // CONTROLPANELGAMETAB_H
