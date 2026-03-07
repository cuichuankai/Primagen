# Primagen - Complete Nanobot C Replication

## 项目对标分析

本项目是对 nanobot（HKUDS 开源的轻量级个人AI助手框架）的完整 C 语言复刻。

### 核心架构实现对标

#### ✓ Agent Loop（代理循环）
- **nanobot**: `nanobot/agent/loop.py` - ReAct 范式实现
- **Primagen**: `src/agent/agent_loop.c` - C 语言对等实现
- 功能：
  - 迭代推理循环
  - 工具调用执行
  - 消息历史管理
  - 最大迭代次数限制
  - LLM 集成接口
  - 多轮 ReAct 循环支持

#### ✓ Message Bus（消息总线）
- **nanobot**: `nanobot/bus/queue.py` - 异步消息队列
- **Primagen**: `src/bus/message_bus.c`
- 功能：
  - 入站消息队列 (InboundMessage)
  - 出站消息队列 (OutboundMessage)
  - 线程安全的消息发送/接收
  - 事件驱动模式

#### ✓ Context Builder（上下文构建器）
- **nanobot**: `nanobot/agent/context.py` - 提示词组装
- **Primagen**: `src/context/context_builder.c`
- 功能：
  - 身份信息注入
  - 启动文件加载（IDENTITY.md, SOUL.md, USER.md, TOOLS.md, AGENTS.md）
  - 长期记忆集成
  - 技能（Skills）加载和展示
  - 消息历史摘要
  - 运行时元数据注入

#### ✓ Tool Registry（工具注册表）
- **nanobot**: `nanobot/agent/tools/registry.py` - 动态工具管理
- **Primagen**: `src/tools/tool.c`
- 功能：
  - 工具注册/注销
  - 工具定义获取（JSON Schema）
  - 工具执行
  - 工具结果处理

#### ✓ Session Manager（会话管理器）
- **nanobot**: `nanobot/session/manager.py` - 会话持久化
- **Primagen**: `src/session/session.c`
- 功能：
  - 会话创建/加载
  - 消息历史存储
  - 会话保存到文件 (JSONL 格式)
  - 会话索引管理
  - 智能过滤（不保存冗余的工具调用详情）

#### ✓ Memory System（记忆系统）
- **nanobot**: `nanobot/agent/memory.py` - 两层记忆架构
- **Primagen**: `src/memory/memory.c`
- 功能：
  - MEMORY.md（长期事实记忆，位于 `.primagen/memory/`）
  - HISTORY.md（可grep搜索的日志历史，位于 `.primagen/memory/`）
  - 记忆加载/保存
  - 记忆巩固（consolidation）工具支持 (`memory` tool)
- **注**：此处 Memory 指代 Agent 的长期与短期记忆，而非计算机内存 (RAM)。

#### ✓ Subagent Manager（子代理管理器）
- **nanobot**: `nanobot/agent/subagent.py` - 后台任务执行
- **Primagen**: `src/subagent/subagent.c`
- 功能：
  - 后台任务生成
  - 子代理工具集
  - 任务ID跟踪
  - 结果宣告
  - 按会话取消

#### ✓ Cron Service（定时服务）
- **nanobot**: `nanobot/cron/service.py` - 定时任务管理
- **Primagen**: `src/cron/cron.c`
- 功能：
  - 任务调度
  - 任务存储 (`.primagen/cron_store.json`)
  - 周期性执行
  - 任务状态查询

#### ✓ Heartbeat Service（心跳服务）
- **nanobot**: `nanobot/heartbeat/service.py` - 周期性操作
- **Primagen**: `src/heartbeat/heartbeat.c`
- 功能：
  - 定时任务检查
  - 定时消息送达
  - 可配置的间隔
  - 启用/禁用开关

#### ✓ Skills Loader（技能加载器）
- **nanobot**: `nanobot/agent/skills.py` - 可扩展能力
- **Primagen**: `src/skills/skills.c`
- 功能：
  - 内置技能列表
  - 用户自定义技能
  - 技能加载和上下文
  - SKILL.md 文件格式
  - 依赖检查
  - 元数据解析
  - `skill` 工具支持 (load/list)

#### ✓ LLM Provider（LLM提供者）
- **nanobot**: `nanobot/providers/` - 多个LLM提供者
- **Primagen**: `src/providers/llm_provider.c`
- 功能：
  - LLM 调用接口 (OpenAI Compatible)
  - 工具定义传递
  - 响应解析
  - 模型参数配置
  - 请求/响应日志记录
  - 历史消息滑动窗口

#### ✓ Configuration System（配置系统）
- **nanobot**: `nanobot/config/schema.py` - Pydantic 配置
- **Primagen**: `src/config/config.c`
- 功能：
  - Agent 配置（模型、温度、max_tokens 等）
  - Tool 配置（执行、Web 搜索、约束等）
  - Channel 配置（Telegram、WhatsApp 等）
  - Heartbeat 配置
  - **差异**: 目前仅支持硬编码路径加载 (`.primagen/config.json`)。

### 待实现/部分实现模块

#### ○ Channels (通信通道)
- **nanobot**: 支持 Telegram, WhatsApp, Discord 等
- **Primagen**:
  - 配置结构已就绪 (`src/config/config.c`)
  - 核心通信层 **CLI (命令行交互)** 已完整实现
  - 多个通道 (Telegram, Email, Discord, Slack, DingTalk, Feishu) 已有基础实现代码，等待真实 API 配置和测试。

### 工具实现

#### 支持的工具集

| nanobot Tools | Primagen Status | 描述 |
|---|---|---|
| ReadFileTool | ✓ 实现 | 读取文件 |
| WriteFileTool | ✓ 实现 | 写入文件 |
| EditFileTool | ✓ 实现 | 编辑文件 |
| ListDirTool | ✓ 实现 | 列出目录 |
| ExecTool | ✓ 实现 | 执行shell命令 |
| WebSearchTool | ✓ 实现 | Web 搜索（Brave API） |
| WebFetchTool | ✓ 实现 | 获取网页内容 |
| MessageTool | ✓ 实现 | 发送消息到特定通道 |
| SpawnTool | ✓ 实现 | 执行子代理任务 |
| CronTool | ✓ 实现 | 定时任务管理 |
| SkillTool | ✓ 实现 | 动态加载和管理技能 |
| MemoryTool | ✓ 实现 | 管理长期记忆和历史 |
| MCPTools | ○ 存根 | Model Context Protocol 工具 |

### 数据结构

#### 核心数据结构

| nanobot | Primagen | 说明 |
|---|---|---|
| InboundMessage | `InboundMessage` | 入站消息 |
| OutboundMessage | `OutboundMessage` | 出站消息 |
| Message | `Message` | 消息（角色、内容、时间戳） |
| ToolCall | `ToolCall` | 工具调用 |
| Session | `Session` | 会话状态 |

### 集成点

#### 完整的消息流

```
[User Input]
    ↓
[InboundMessage] → MessageBus → AgentLoop
    ↓
[Context Building]
    ├─ Identity (身份)
    ├─ Bootstrap Files (启动文件)
    ├─ Memory (记忆)
    ├─ Skills (技能)
    └─ History (历史)
    ↓
[LLM Call] → Provider Interface
    ↓
[Tool Execution] ← ToolRegistry
    ├─ File Operations
    ├─ Shell Execution
    ├─ Web Operations
    ├─ Message Sending
    ├─ Subagent Spawning
    ├─ Cron Task Management
    ├─ Skill Management
    ├─ Memory Management
    └─ Others
    ↓
[Response Generation]
    ↓
[OutboundMessage] → MessageBus → Channel → User
    ↓
[Session Persistence]
    ├─ Session Save (JSONL)
    ├─ Memory Consolidation (MEMORY.md/HISTORY.md)
    └─ History Logging
```

## 项目统计

### 代码规模

| 指标 | 数值 |
|---|---|
| C 源文件 | 20+ |
| 头文件 | 15+ |
| C 模块数 | 15+ |
| 总代码行数 | ~6000+ |
| 编译结果 | 可执行文件 |

### 编译系统

- **Makefile**: 自动化编译，模块化构建
- **编译标志**: `-Wall -Wextra -std=c99 -pthread`
- **编译对象**: 所有源文件独立编译，最后联接

## 主要成就

### ✓ 完整复刻的核心模块

1. **Agent Loop** - 完整的 ReAct 推理-行动循环
2. **Message Bus** - 异步消息队列系统
3. **Context Builder** - 综合的上下文和提示词组装
4. **Tool Registry** - 动态工具注册和执行
5. **Session Manager** - 会话和历史管理
6. **Memory System** - 两层记忆架构
7. **Subagent Manager** - 后台任务执行
8. **Cron Service** - 定时任务调度
9. **Heartbeat Service** - 周期性操作
10. **Skills Loader** - 可扩展技能系统
11. **Config System** - 灵活的配置管理

### ✓ 能够运行

- 完整编译无错误
- 可执行文件成功运行
- 所有模块正确集成

### ✓ 设计特点

- **模块化**: 按功能分离，易于维护和扩展
- **线程安全**: 使用 pthread 和互斥锁
- **内存安全**: 适当的资源分配和释放
- **类型安全**: 结构化数据类型
- **可扩展**: 工具、技能、提供者的插件架构

## 下一步开发方向

### 可选的增强功能

1. **Channel Implementation** - 完善 Telegram、WhatsApp、Discord 等通道的真实 API 对接
2. **CLI Arguments** - 实现与 nanobot 一致的命令行参数解析 (如 `--config`, `--workspace` 等)
3. **HTTP Client** - 增强 Web 工具的鲁棒性
4. **JSON Parser** - 增强配置文件解析
5. **Database** - 改进的会话和记忆持久化
6. **Error Handling** - 更详细的错误处理和恢复
7. **Performance** - 内存池、对象缓存等优化
8. **Testing** - 单元测试和集成测试框架

## 对标总结

该项目成功地将 nanobot 的关键架构和功能从 Python 复刻到 C 语言，保留了所有核心的 AI 代理功能：

- ✓ ReAct 推理-行动范式
- ✓ 动态工具执行
- ✓ 多层次记忆系统
- ✓ 后台任务管理
- ✓ 定时任务调度
- ✓ 可扩展技能系统
- ✓ 配置化管理
- ✓ 异步消息处理
- ✓ 会话持久化

该实现可作为：
- 学习 C 语言系统编程的参考
- AI 代理框架的轻量级实现
- 性能关键场景下的替代方案
- 快速原型开发的基础
