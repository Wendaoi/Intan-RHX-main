#ifndef PONGGAME_H
#define PONGGAME_H

#include <vector>
#include <QString>
#include <mutex>
#include <shared_mutex>

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
    GameEvent update(int paddle1_movement);

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
    float paddleSpeed;

    // 游戏状态（使用无体积点和线段简化碰撞）
    int paddleY; // 玩家 (线段起点Y坐标，线段宽度为paddleHeight)
    float ballX, ballY; // 球作为一个点
    float ballSpeedX, ballSpeedY; // 浮点速度以提高精确性

    // 实验控制
    ExperimentCondition currentCondition;

    // 统计
    int bounces_in_rally;
    
    // 线程安全保护
    mutable std::shared_mutex gameMutex; // 保护游戏状态的读写锁

public:
    // 配置
    void setCondition(ExperimentCondition condition);
    ExperimentCondition getCondition() const;
    void resetBounces();

    // 获取游戏状态 (GUI中使用这些值绘制球和球拍的运动)
    int getPaddle1Y() const { 
        std::shared_lock<std::shared_mutex> lock(gameMutex);
        return static_cast<int>(paddleY); 
    }
    int getBallX() const { 
        std::shared_lock<std::shared_mutex> lock(gameMutex);
        return static_cast<int>(ballX); 
    }
    int getBallY() const { 
        std::shared_lock<std::shared_mutex> lock(gameMutex);
        return static_cast<int>(ballY); 
    }
    float getBallVX() const { 
        std::shared_lock<std::shared_mutex> lock(gameMutex);
        return ballSpeedX; 
    }
    float getBallVY() const { 
        std::shared_lock<std::shared_mutex> lock(gameMutex);
        return ballSpeedY; 
    }
    int getBounces() const { 
        std::shared_lock<std::shared_mutex> lock(gameMutex);
        return bounces_in_rally; 
    }
    int getPaddleHeight() const { 
        std::shared_lock<std::shared_mutex> lock(gameMutex);
        return paddleHeight; 
    }
    int getGameHeight() const {
        std::shared_lock<std::shared_mutex> lock(gameMutex);
        return gameHeight;
    }

    // 获取感觉输入信息 (返回0-7代表8个刺激区域, -1代表无刺激，基于球点相对于球拍线段的位置)
    int getSensoryStimZone() const;
};

#endif // PONGGAME_H
