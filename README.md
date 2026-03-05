# Primagen

My original intention was Primitive Genesis(元婴), BUT ...  
Primagen, the ancient cosmic creature imprisoned in the underground city, harbors an ambition to annihilate the universe, driven by an extreme lust for power.  
When technology—such as nuclear energy or genetic engineering—breaks free from moral constraints, it can turn into an instrument of destruction.
Similarly, unregulated AI Agents may inflict catastrophic harm.  
If we can restrain Primagen’s malice and guide him toward goodness, a completely different future will unfold.  
Primagen lays bare the inherent evil of human nature.  
Yet even the flower of evil can bear the fruit of good, if carefully nurtured and cultivated.  

> Primagen，这头被囚禁于地底之城的远古宇宙生灵，其毁灭宇宙的狂想，本质是对权力极致贪婪的终极投射。  
> 科技一旦挣脱道德的缰绳 —— 无论是核能、基因工程，还是如今的 AI Agent—— 都将从文明的利刃，蜕变为自我毁灭的凶器。  
> 可真正的启示，不在于镇压这头远古巨兽，而在于驯化与引导：压制其恶，唤醒其善，世界便会走向截然不同的未来。  
> Primagen 映照的，从来不是外星怪物，而是人性深处的幽暗本源。  
> 恶本是天生的种子，但若以理性与良知浇灌，恶之花，亦可结出善之果。  

This is a pure C implementation of the original nanobot project.

## Project Structure
- `src/`: Source code
  - `agent/`: Agent loop
  - `bus/`: Message bus
  - `context/`: Context builder
  - `memory/`: Memory management (Long-term/Short-term Memory, not RAM)
  - `providers/`: LLM providers
  - `session/`: Session management
  - `tools/`: Tool registry
  - `subagent/`: Subagent manager
  - `cron/`: Cron service
  - `common.h/c`, `message.h/c`, `main.c`: Common files
- `build/`: Build directory for object files and executable
- `Makefile`: Build script
- `README.md`: This file

## Features Implemented
- Agent Loop with ReAct Paradigm
- MessageBus for decoupling channels and agent core
- ContextBuilder for assembling prompts
- Tool registration mechanism (Filesystem, Shell, Web, Subagent, Cron)
- Session persistence in JSONL format
- Memory management (short and long term context)
- Subagent Manager for background tasks
- Cron Service for scheduled tasks
- Real LLM Provider (OpenAI/Brave integration)

**Note on Channels**: Currently, only the CLI (Command Line Interface) channel is implemented. Configuration structures for Telegram/WhatsApp exist but are not yet connected to real APIs.

**Note on CLI Arguments**: The application currently loads configuration from `.nanobot/config.json` by default and does not yet support command-line arguments override (unlike the original nanobot).

## Compilation
```bash
make
```

## Running
```bash
# Ensure API keys are set
export OPENAI_API_KEY="sk-..."
export BRAVE_API_KEY="your-brave-key"

./build/primagen
```

The program runs an interactive CLI agent loop.

## Verification
Compare the logic with the original Python code. The agent loop processes messages, builds context, calls LLM, executes tools, and responds.

## Notes
- **Memory**: Refers to the AI's "Memory" (Context/Knowledge), distinct from system RAM.
- **LLM**: Uses `libcurl` to make real requests to OpenAI API.
- **Tools**: Includes `exec`, `read_file`, `write_file`, `web_search` etc.
