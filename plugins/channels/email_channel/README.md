# Email Channel

Email channel. Monitors an IMAP inbox for new messages and sends agent replies via SMTP.

## Configuration

```jsonc
{
  "channels": {
    "email": {
      "enabled": false,
      "imap_host": "imap.gmail.com",
      "imap_port": 993,
      "imap_user": "user@gmail.com",
      "imap_pass": "xxxxxxxxxxxxxxxx",
      "smtp_host": "smtp.gmail.com",
      "smtp_port": 587,
      "smtp_user": "user@gmail.com",
      "smtp_pass": "xxxxxxxxxxxxxxxx",
      "smtp_from": "Primagen <user@gmail.com>",
      "poll_interval": 60
    }
  }
}
```

| Field | Description |
|-------|-------------|
| `imap_host` | IMAP server hostname |
| `imap_port` | IMAP port (usually 993 for SSL) |
| `imap_user` | IMAP login username/email |
| `imap_pass` | IMAP login password or app password |
| `smtp_host` | SMTP server hostname |
| `smtp_port` | SMTP port (usually 587 for TLS) |
| `smtp_user` | SMTP login username/email |
| `smtp_pass` | SMTP login password or app password |
| `smtp_from` | From address for sent emails |
| `poll_interval` | Seconds between IMAP inbox checks (default: 60) |

## Setup (Gmail)

1. Enable IMAP in Gmail settings
2. Generate an App Password at [Google Account Security](https://myaccount.google.com/security)
3. Use the app password for both `imap_pass` and `smtp_pass`

## Troubleshooting

| Problem | Solution |
|---------|----------|
| IMAP connection refused | Verify IMAP is enabled in email settings; check port |
| SMTP auth fails | Use App Password, not account password (for Gmail/Outlook) |
| Old emails re-processed | Channel tracks seen UIDs; avoid deleting UID cache |