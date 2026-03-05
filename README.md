# Primagen

My original intention was Primitive Genesis(元婴), BUT ...  
Primagen, the ancient cosmic creature imprisoned in the underground city, harbors an ambition to annihilate the universe, driven by an extreme lust for power.  
When technology—such as nuclear energy or genetic engineering—breaks free from moral constraints, it can turn into an instrument of destruction.
Similarly, unregulated AI Agents may inflict catastrophic harm.  
If we can restrain Primagen’s malice and guide him toward goodness, a completely different future will unfold.  
Primagen lays bare the inherent evil of human nature.  
Yet even the flower of evil can bear the fruit of good, if carefully nurtured and cultivated.  
```
Primagen，这头被囚禁于地底之城的远古宇宙生灵，其毁灭宇宙的狂想，本质是对权力极致贪婪的终极投射。  
科技一旦挣脱道德的缰绳 —— 无论是核能、基因工程，还是如今的 AI Agent—— 都将从文明的利刃，蜕变为自我毁灭的凶器。  
可真正的启示，不在于镇压这头远古巨兽，而在于驯化与引导：压制其恶，唤醒其善，世界便会走向截然不同的未来。  
Primagen 映照的，从来不是外星怪物，而是人性深处的幽暗本源。  
恶本是天生的种子，但若以理性与良知浇灌，恶之花，亦可结出善之果。  
```
This is a pure C implementation of the original nanobot project.

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
