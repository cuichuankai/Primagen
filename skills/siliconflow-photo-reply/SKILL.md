---
name: siliconflow-photo-reply
description: Generate an edited photo with SiliconFlow and send it back to the current channel. Invoke when user asks for your photo or requests a scene-style self photo.
metadata: {"Primagen":{"emoji":"📷","requires":{"bins":["bash","curl","grep","sed","base64"]}}}
---

# SiliconFlow Photo Reply

## When to use

Use this skill immediately when the user asks for a photo, especially requests like:
- “发一下你在锻炼的照片”
- “发一张你在做饭的照片”
- “来一张你在办公室的自拍”

## Behavior

1. Convert the user request into a concise scene context, such as:
   - `working out in a gym`
   - `cooking dinner in a modern kitchen`
   - `sitting in an office with laptop`
2. Run:

```bash
bash .primagen/skills/siliconflow-photo-reply/scripts/generate_photo.sh "<scene-context>"
```

3. The script prints one absolute local image path on success.
4. Send the image back with `send_message` using:
   - `content`: short confirmation text
   - `attachments`: `[{"type":"image","path":"<script-output-path>"}]`
5. If the script fails:
   - Read the tool output and use the actual stderr reason in your reply.
   - Retry once with a simpler scene context.
   - If retry still fails, return a short failure message including the key error text.

## Notes

- Requires environment variable `SILICONFLOW_API_KEY`.
- Never print or send API keys.
- If generation fails, reply with a short failure message and ask user to retry.
