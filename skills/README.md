# Skills

Skills are modular prompt extensions that enhance the agent's capabilities. Each skill is a Markdown file (`SKILL.md`) containing instructions, examples, and domain knowledge that gets injected into the agent's system prompt when loaded.

## How Skills Work

1. **Loading**: The agent calls the `skill` tool with `{"action": "load", "name": "skill-name"}` or you load it via WebUI → Skills tab
2. **Injection**: The skill's content is added to the system prompt by the Context Builder
3. **Effect**: The LLM follows the skill's instructions for the remainder of the conversation
4. **Unloading**: Skills can be unloaded via the `skill` tool (`{"action": "unload", "name": "..."}`) or WebUI

## Skill Sources

Skills are loaded from three sources (configured in `config.json`):

| Source | Location | Description |
|--------|----------|-------------|
| **Local** | `.primagen/skills/{name}/SKILL.md` | Manually installed or created skills |
| **SkyPilot** | sky-pilot/skills (GitHub) | Community skills repository |
| **ClawHub** | claudehub.com | Online skill marketplace |

To enable remote sources:
```jsonc
{
  "skills": {
    "sources": {
      "sky_pilot": { "enabled": true },
      "claw_hub": { "enabled": true }
    }
  }
}
```

## Skill File Format

```markdown
# Skill: skill-name

## Description
A brief description of what this skill does and when to use it.

## Instructions
Detailed instructions for the LLM on how to use this domain knowledge.
- Focus on actionable guidance
- Include concrete examples
- Define expected output formats

## Examples
Show example inputs and expected outputs.

## Constraints
Any limitations or safety rules the LLM must follow.
```

### Example: `code-review` skill

```markdown
# Skill: code-review

## Description
Expert code review assistant. Use when user asks for code review or feedback.

## Instructions
When reviewing code, analyze the following aspects in order:
1. **Security**: SQL injection, XSS, hardcoded secrets, unsafe deserialization
2. **Performance**: N+1 queries, unnecessary allocations, blocking operations
3. **Correctness**: Edge cases, error handling, null checks
4. **Readability**: Naming, structure, comments

## Output Format
```
## Code Review

### Summary
[1-2 sentence overview]

### Issues Found
1. **[Severity] Title** (file:line)
   - Problem: ...
   - Fix: ...

### Recommendations
- ...
```
```

## Managing Skills via WebUI

The WebUI Control Panel → Skills tab provides:

- **Browse**: View all available skills (local + remote)
- **Load/Unload**: Toggle skills for the current session
- **Content Viewer**: Read skill content before loading
- **Edit/Save**: Modify local skills in-place
- **Import**: Import skills from GitHub URLs or raw content

## Managing Skills Programmatically

### List available skills
```json
{"action": "list"}
```

### Load a skill
```json
{"action": "load", "name": "code-review"}
```

### Unload a skill
```json
{"action": "unload", "name": "code-review"}
```

## Creating a Local Skill

```bash
# Create skill directory
mkdir -p .primagen/skills/my-skill

# Write the SKILL.md
cat > .primagen/skills/my-skill/SKILL.md << 'EOF'
# Skill: my-skill

## Description
Your skill description here.

## Instructions
Your detailed instructions for the LLM.
EOF
```

The skill becomes immediately available in the WebUI and via the `skill` tool.

## Troubleshooting

| Problem | Solution |
|---------|----------|
| Skill not appearing | Verify SKILL.md exists at `.primagen/skills/{name}/SKILL.md` |
| Remote source not loading | Check `enabled: true` in config.json; verify network connectivity |
| Skill content not applied | Ensure the skill is loaded (WebUI → Skills → check loaded state) |
| Import fails | Verify the URL points to a valid SKILL.md; check `agent.log` for details |