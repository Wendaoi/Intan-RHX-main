#ifndef PONGGAMEWIDGET_H
#define PONGGAMEWIDGET_H

#include <QtWidgets>
#include "Engine/Threads/gamethread.h"

class PongGameWidget : public QWidget
{
    Q_OBJECT

public:
    explicit PongGameWidget(QWidget *parent = nullptr);
    ~PongGameWidget();

    // 设置游戏尺寸
    void setGameSize(int width, int height);

public slots:
    // 更新游戏状态并重绘
    void updateGameState(const GameState& gameState);

protected:
    // 重写绘制事件
    void paintEvent(QPaintEvent *event) override;

private:
    // 游戏状态
    GameState currentGameState;
    QMutex gameStateMutex;

    // 游戏尺寸
    int gameWidth;
    int gameHeight;

    // 游戏元素尺寸（相对于游戏坐标）
    int paddleWidth;
    int paddleHeight;
    int ballSize;
};

#endif // PONGGAMEWIDGET_H
