# Primagen

This is a pure C implementation replicating the functionalities of the original primagen project.

## Project Structure
- `src/`: Source code
  - `agent/`: Agent loop
  - `bus/`: Message bus
  - `context/`: Context builder
  - `memory/`: Memory management
  - `providers/`: LLM providers
  - `session/`: Session management
  - `tools/`: Tool registry
  - `common.h/c`, `message.h/c`, `main.c`: Common files
- `build/`: Build directory for object files and executable
- `Makefile`: Build script
- `README.md`: This file

## Features Implemented
- Agent Loop with ReAct Paradigm
- MessageBus for decoupling channels and agent core
- ContextBuilder for assembling prompts
- Tool registration mechanism
- Session persistence in JSONL format
- Memory management (short and long term)
- Basic error handling

## Compilation
```bash
make
```

## Running
```bash
./build/primagen
```

The program simulates an inbound message and prints the response.

## Verification
Compare the logic with the original Python code. The agent loop processes messages, builds context, calls LLM (stub), executes tools, and responds.

## Notes
- LLM provider is a stub; integrate real API calls for full functionality.
- JSON parsing is simplified; use a proper library for production.
- HTTP client not implemented; add for web tools.
