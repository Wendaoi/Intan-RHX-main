
#ifndef ABSTRACTGAMECONTROLLER_H
#define ABSTRACTGAMECONTROLLER_H

#include "ponggame.h"
#include <vector>

// Enum to define the discrete actions our agent can take
enum class PaddleAction {
    Stay,
    MoveUp,
    MoveDown
};

// A structure to hold the current state of the game
struct GameStateInfo {
    float paddleY;
    float ballY;
    float ballX;
    float ballVY;
    float ballVX;
};

class AbstractGameController {
public:
    virtual ~AbstractGameController() = default;

    // Main function to get the next action based on the game state
    virtual PaddleAction getAction(const GameStateInfo& state) = 0;

    // Function to provide feedback (reward) to the controller for learning
    virtual void update(const GameStateInfo& oldState, PaddleAction action, float reward, const GameStateInfo& newState) = 0;
};

#endif // ABSTRACTGAMECONTROLLER_H
