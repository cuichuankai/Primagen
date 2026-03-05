# Primagen C 项目结构

## 目录组织

### 根目录 (/)
```
/workspaces/Primagen/
├── Makefile              # 编译配置文件
├── README.md             # 项目说明
├── PROJECT_STRUCTURE.md  # 项目结构说明
├── src/                  # 源代码目录
└── build/                # 编译输出目录（中间产物和可执行文件）
```

### 源代码目录 (src/)
```
src/
├── include/              # 头文件目录（通用头文件）
│   ├── common.h          # 公共数据结构和工具函数声明
│   └── message.h         # 消息数据结构声明
├── common/               # 通用实现模块
│   ├── common.c          # 公共模块实现
│   └── message.c         # 消息模块实现
├── main.c                # 主程序入口
├── agent/                # 代理循环模块
│   ├── agent_loop.h
│   └── agent_loop.c
├── bus/                  # 消息总线模块
│   ├── message_bus.h
│   └── message_bus.c
├── context/              # 上下文构建器模块
│   ├── context_builder.h
│   └── context_builder.c
├── memory/               # 记忆管理模块
│   ├── memory.h
│   └── memory.c
├── providers/            # LLM 提供者模块
│   ├── llm_provider.h
│   └── llm_provider.c
├── session/              # 会话管理模块
│   ├── session.h
│   └── session.c
└── tools/                # 工具注册模块
    ├── tool.h
    └── tool.c
```

### 编译输出目录 (build/)
```
build/
├── primagen               # 最终可执行文件
├── common/
│   ├── common.o          # 对象文件
│   └── message.o
├── main.o
├── agent/
│   └── agent_loop.o
├── bus/
│   └── message_bus.o
├── context/
│   └── context_builder.o
├── memory/
│   └── memory.o
├── providers/
│   └── llm_provider.o
├── session/
│   └── session.o
└── tools/
    └── tool.o
```

## 编译方法

### 从项目根目录执行：
```bash
cd /workspaces/Primagen
make              # 编译项目
make clean        # 清除编译产物
```

## 运行方法

```bash
./build/primagen
```

### 预期输出：
```
Primagen - AI Agent Framework (C Refactoring)
=============================================

Agent is running. Type your message (or 'exit' to quit):
>
```

## 项目特点

1. **模块化设计**：按功能将代码分为不同的模块（agent, bus, context, memory 等）
2. **头文件集中**：所有通用头文件统一放在 include 目录
3. **实现分离**：基础实现（common.c, message.c）与头文件分离，放在 common 目录
4. **清晰的目录结构**：模拟原始 Python 项目的组织方式
5. **编译产物隔离**：所有中间文件和可执行文件都在 build/ 目录中
6. **源码保持纯净**：只有源代码文件在 src/ 目录中，没有编译产物污染
7. **灵活的 Makefile**：支持增量编译和清除功能

## 核心功能说明

| 模块 | 功能 |
|------|------|
| `include/` | 公共头文件目录 |
| `common/` | 基础实现（common.c, message.c） |
| `agent/` | 核心 ReAct 代理循环实现 |
| `bus/` | 异步消息队列，解耦通道和代理核心 |
| `context/` | 构建系统提示，包含身份、引导文件、记忆和技能 |
| `tools/` | 动态工具注册和执行机制 |
| `session/` | 会话管理，JSONL 格式持久化存储 |
| `memory/` | 长期记忆和历史记录管理 |
| `providers/` | LLM 提供者接口（真实 API 调用） |

## 文件包含关系

```
main.c
├── include/common.h
├── include/message.h
├── agent/agent_loop.h
├── bus/message_bus.h
├── context/context_builder.h
├── tools/tool.h
├── session/session.h
├── memory/memory.h
└── providers/llm_provider.h
    └── (各模块内部相对引用)
    
include/message.h
└── include/common.h

common/
├── common.c
│   └── include/common.h
└── message.c
    ├── include/common.h
    └── include/message.h
```

## 编译配置

- **编译器**：gcc
- **标准**：C99
- **编译标志**：-Wall -Wextra -pthread
- **包含路径**：`-I src/include -I src` (在编译时自动添加)

## 验证步骤

1. 执行 `make` 编译项目
2. 检查 `build/` 目录中是否生成对应的 `.o` 文件和 `primagen` 可执行文件
3. 运行 `./build/primagen` 验证功能
4. 对比输出与原始 Python 项目的行为一致
