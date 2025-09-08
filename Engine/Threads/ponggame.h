#ifndef PONGGAME_H
#define PONGGAME_H

#include <vector>
#include <QString>

// 游戏事件，用于触发不同的反馈
enum class GameEvent {
    None,
    BallHitPlayerPaddle, // 成功拦截
    PlayerMissed,        // 未成功拦截
};

// 实验条件
enum class ExperimentCondition {
    Stimulus,    // 完整反馈
    Silent,      // Miss后静默
    NoFeedback,  // Miss后无中断
    Rest         // 无感觉输入
};

class PongGame {
public:
    PongGame();

    // 更新一帧游戏逻辑
    GameEvent update(int spikesUp, int spikesDown);

private:
    // 辅助函数
    GameEvent updateBallPosition();
    bool checkPaddleCollision(float newBallX, float newBallY);
    void resetBall(bool randomVector);

    // 游戏参数
    int gameWidth;
    int gameHeight;
    int paddleWidth;
    int paddleHeight;
    int ballSize;

    // 游戏状态（使用无体积点和线段简化碰撞）
    int paddleY; // 玩家 (线段起点Y坐标，线段宽度为paddleHeight)
    float ballX, ballY; // 球作为一个点
    float ballSpeedX, ballSpeedY; // 浮点速度以提高精确性

    // 实验控制
    ExperimentCondition currentCondition;

    // 统计
    int bounces_in_rally;

public:
    // 配置
    void setCondition(ExperimentCondition condition);
    ExperimentCondition getCondition() const;

    // 获取游戏状态 (GUI中使用这些值绘制球和球拍的运动)
    int getPaddle1Y() const { return static_cast<int>(paddleY); }
    int getBallX() const { return static_cast<int>(ballX); }
    int getBallY() const { return static_cast<int>(ballY); }
    int getBounces() const { return bounces_in_rally; }
    int getPaddleHeight() const { return paddleHeight; }

    // 获取感觉输入信息 (返回0-7代表8个刺激区域, -1代表无刺激，基于球点相对于球拍线段的位置)
    int getSensoryStimZone() const;
};

#endif // PONGGAME_H
