# Intan-RHX - 中文版

# TODO List - 待完善功能

## 🔄 实验条件实现
- [ ] **实现4种不同实验条件下的游戏控制**（Stimulus、Silent、No-feedback、Rest条件）
  - Stimulus条件：完整闭环反馈系统
  - Silent条件：错过后暂停所有刺激
  - No-feedback条件：移除重新开始机制
  - Rest条件：只有运动控制无感觉信息

## 🧬 信号处理完善
- [ ] **正确处理神经信号尖峰输入对球拍的控制**
  - 每10毫秒（200个样本）对运动区域尖峰计数
  - 区分Motor Region 1和2的活动强度
  - 根据尖峰数量控制球拍移动方向
  - 阈值设置排除噪声（<-5mV不予考虑）

## 🧪 刺激编码优化
- [ ] **将游戏状态正确编码为电极刺激信号**
  - 球相对球拍的位置编码为8个感觉电极之一
  - 拓扑一致的位置编码系统
  - 双相方波脉冲刺激机制
  - 不同情境下的反馈编码（hit/miss）

## 🛠️ 线程安全与并发
- [ ] **处理线程间可能存在的冲突问题**
  - GameThread与USBDataThread之间的数据同步
  - WaveProcessorThread与游戏线程的并发访问
  - 信号槽连接的安全性验证
  - 资源竞争（FIFO、配置参数）的锁机制

## 🎮 GUI完善
- [ ] **完善GUI对游戏参数的修改方式**
  - 神经元活动阈值动态调节
  - 刺激强度参数实时调整
  - 实验条件切换界面优化
  - 实时性能监控仪表板
  - 参数验证与错误提示

---

## 项目介绍
Intan RHX 是一款免费且强大的数据采集软件，用于显示和记录来自任何 Intan RHD 或 RHS 系统的电生理信号。该软件使用 RHD USB 接口板、RHD 记录控制器或 RHS 刺激/记录控制器。

最新的二进制文件可从 Intan 官网获取：https://intantech.com，或者从 GitHub 的 Releases 部分下载：

* IntanRHXInstaller.exe -> Windows 64位安装程序（Wix Burn 引导程序，引导用户完成安装）

* IntanRHX.dmg -> MacOS 64位磁盘映像

* IntanRHX.tar.gz -> Linux 64位归档文件

这些二进制文件使用 Qt 6.8.2 构建。

虽然开发者可以自由下载源代码或 fork 他们自己的仓库来修改自己的版本，但我们通常不会将这些更改集成到公共发布版本中。如果您希望在官方 Intan 发布版本中看到某些功能或发现错误，请向我们提供反馈！谢谢！

# 运行软件步骤

## 所有平台：

运行时，各种文件需要与二进制可执行文件位于同一目录中。这些文件包括：
* kernel.cl
* ConfigRHDController.bit
* ConfigRHDInterfaceBoard.bit
* ConfigRHSController.bit
* ConfigXEM6010Tester.bit
* USBEvaluationBoard.bit

### Windows：

RHX 软件依赖于 Opal Kelly USB 驱动程序和 Microsoft 可再发行组件。从 Intan 官网运行分发的 Windows 安装程序时，这些组件会自动安装，但在从源代码构建 RHX 时，这些组件应该已经安装在系统上。应安装 Opal Kelly USB 驱动程序，以便 Intan 硬件可以通过 USB 通信。这些驱动程序可从以下位置获取：https://intantech.com/files/Intan_controller_USB_drivers.zip。这些驱动程序依赖于 Microsoft Visual C++ 可再发行组件（x64）的 2010、2013 和 2015-2019 版本，这些版本可从 Microsoft 获取，也应在运行 IntanRHX 之前安装。最后，okFrontPanel.dll（位于 libraries 目录中）应在运行时位于与二进制可执行文件相同的目录中。

### Mac：

libokFrontPanel.dylib 应位于构建的 IntanRHX.app 中 MacOS 目录旁边的 "Frameworks" 目录中。对此应用程序运行 macdeployqt 也将使用所需的 Qt 库填充此目录。

### Linux：

应添加 udev 规则文件，以便 Intan 硬件可以通过 USB 通信。60-opalkelly.rules 文件应复制到 /etc/udev/rules.d/，之后应重新启动系统或运行命令 'udevadm control --reload-rules'。libokFrontPanel.so 应在运行时位于与二进制可执行文件相同的目录中。

# 新增功能 - Pong 游戏模块

## 概述

最新的更新版本新增了一个强大的 Pong 游戏模块，该模块专为神经科学实验设计，用于研究神经元活动对游戏行为的控制。该模块完全集成到 Intan RHX 系统中，提供实时刺激-响应闭环交互。

## 核心功能

### 🏓 Pong 游戏机制

1. **游戏规则**：
   - 玩家（神经元控制的球拍）需要拦截移动的球
   - 球会从游戏区域边缘和球拍上反弹
   - 当球击中球拍后侧边缘时，回合结束
   - 得分基于成功拦截次数

2. **神经调控系统**：
   - 通过高密度多电极阵列（HD-MEA）记录神经元活动
   - 分为两个运动区域：Motor Region 1 和 Motor Region 2
   - 神经活动强度控制球拍的上下移动

### 🧬 刺激系统

1. **感觉输入**：
   - 8个感觉电极提供位置编码刺激
   - 拓扑一致的位置编码系统
   - 双相方波脉冲刺激

2. **反馈机制**：
   - **成功拦截**：可预测的100Hz 100ms短暂刺激
   - **未成功拦截**：不可预测的5Hz 4秒持续刺激
   - 支持多种实验条件（Stimulus、Silent、No-feedback、Rest）

### 🎮 用户界面

1. **游戏控制面板**：
   - 专用游戏标签页（Game Tab）
   - 实时游戏画面显示
   - 参数调节和监控

2. **增强用户体验**：
   - 流畅的游戏可视化
   - 实时性能指标显示
   - 实验条件切换

## 技术特性

### ⚡ 性能优化

- **影子构建**：启用Qt的影子构建（shadow build），将所有构建产物集中到build目录
- **多线程架构**：GameThread独立处理游戏逻辑，不影响主数据采集流程
- **实时处理**：10ms更新频率，20000Hz采样支持

### 🔧 系统增强

1. **优雅关闭机制**：
   - 增强的窗口关闭事件处理，确保安全关闭所有线程
   - 信号处理器支持（如SIGTERM、SIGINT）实现优雅关闭

2. **构建系统改进**：
   - .gitignore优化，排除构建产物
   - 项目文件配置影子构建目录

## 使用指南

### 设置游戏参数

1. 启动Intan RHX应用程序
2. 选择合适的实验板
3. 在控制面板中切换到"Game"标签页
4. 配置神经刺激电极映射
5. 选择实验条件（Stimulus/Silent/No-feedback/Rest）
6. 开始实验记录

### 监控游戏性能

- **主要指标**：平均回合长度（Average Rally Length）
- **辅助指标**：Ace次数、长回合次数
- 实时显示游戏状态和神经响应

## 文件结构

新增的核心文件：

```
Engine/Threads/
├── gamethread.cpp/h        # 主游戏线程管理
└── ponggame.cpp/h          # 游戏逻辑实现

GUI/Widgets/
├── controlpanelgametab.cpp/h    # 游戏控制面板
└── ponggamewidget.cpp/h         # 游戏界面组件

PongGame.md                 # 详细实验规则文档
```

## 兼容性

- 支持所有现有Intan硬件系统
- 向后兼容原有的数据采集功能
- 适用于RHD和RHS控制器系列

## 注意事项

该游戏模块专为神经科学实验设计，提供了强大的闭环刺激-响应机制。请确保：
1. 正确配置电极映射
2. 选择合适的神经元活动阈值
3. 根据实验需求调整刺激强度
4. 监控系统性能并及时调整参数

---

如有任何问题或建议，请联系liuzisheng24@mails.ucas.ac.cn
