# Web Tools

Provides web search and web fetch capabilities to the agent.

## Tools

| Tool | Description | Parameters |
|------|-------------|------------|
| `web_search` | Search the web via a search API | `query` (required), `num_results` (optional) |
| `web_fetch` | Fetch and extract text content from a URL | `url` (required), `raw` (optional boolean) |

## Configuration

```jsonc
{
  "plugins": {
    "web_tools": {
      "enabled": false,
      "config": {
        "search_api": "duckduckgo",
        "search_api_key": "",
        "proxy": "",
        "timeout": 30,
        "max_results": 5
      }
    }
  }
}
```

| Field | Description |
|-------|-------------|
| `search_api` | Search backend: `duckduckgo` (free), `serpapi`, `brave` |
| `search_api_key` | API key for paid search backends |
| `proxy` | HTTP/HTTPS proxy URL (e.g., `http://127.0.0.1:8080`) |
| `timeout` | Request timeout in seconds (default: 30) |
| `max_results` | Maximum search results returned (default: 5) |

## Environment Variables

```
PRIMAGEN_TOOLS_WEB_PROXY=http://127.0.0.1:8080
PRIMAGEN_TOOLS_WEB_SEARCH_API_KEY=sk-xxxxxxxxxxxxxxxx
```

## Troubleshooting

| Problem | Solution |
|---------|----------|
| Search returns no results | Verify network access; try setting a `proxy` if behind firewall |
| Rate limited | DuckDuckGo has rate limits; switch to a paid API backend |
| Fetch fails with timeout | Increase `timeout`; check URL accessibility |