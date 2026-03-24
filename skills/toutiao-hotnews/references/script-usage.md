# 脚本使用说明

## 依赖安装

### macOS
```bash
# 安装 curl (通常已预装)
brew install curl

# 安装 xmllint (libxml2)
brew install libxml2
```

### Ubuntu/Debian
```bash
sudo apt-get update
sudo apt-get install -y curl libxml2-utils python3
```

### CentOS/RHEL
```bash
sudo yum install -y curl libxml2 python3
```

## 参数说明

脚本支持三个可选参数：

1. **SOURCE** (可选): 新闻源标识
   - `36kr` - 36 氪 (默认)
   - `huxiu` - 虎嗅网
   - `iresearch` - 艾瑞网
   - `appsolution` - 爱范儿
   - `xlrocket` - 效率火箭
   - `all` - 抓取所有新闻源

2. **HOURS** (可选): 抓取最近多少小时内的新闻，默认 24 小时

3. **LIMIT** (可选): 每个新闻源最多显示多少篇文章，默认 10 篇

## 使用示例

```bash
# 获取 36 氪最近 24 小时的 10 篇新闻 (默认)
.primagen/skills/toutiao-hotnews/scripts/rss_fetch.sh

# 获取虎嗅网最近 24 小时的 10 篇新闻
.primagen/skills/toutiao-hotnews/scripts/rss_fetch.sh huxiu

# 获取艾瑞网最近 12 小时的 5 篇新闻
.primagen/skills/toutiao-hotnews/scripts/rss_fetch.sh iresearch 12 5

# 获取所有新闻源最近 24 小时的各 10 篇新闻
.primagen/skills/toutiao-hotnews/scripts/rss_fetch.sh all 24 10

# 获取所有新闻源最近 3 天 (72 小时) 的各 15 篇新闻
.primagen/skills/toutiao-hotnews/scripts/rss_fetch.sh all 72 15

# 获取爱范儿最近一周 (168 小时) 的 20 篇新闻
.primagen/skills/toutiao-hotnews/scripts/rss_fetch.sh appsolution 168 20

# 查看帮助信息
.primagen/skills/toutiao-hotnews/scripts/rss_fetch.sh --help
```

## 输出示例

### 单个新闻源
```
╔══════════════════════════════════════════════════════════╗
║          综合科技新闻抓取工具                            ║
╚══════════════════════════════════════════════════════════╝

正在抓取 36 氪 最近 24 小时内的新闻...

Feed 标题：36 氪
Feed 链接：https://36kr.com

============================================================

1. 某重大科技新闻标题
   发布时间：2024-01-15 10:30:00
   链接：https://36kr.com/p/xxxxx
   摘要：这是新闻的简要描述内容...

2. 另一条科技新闻
   发布时间：2024-01-15 09:15:00
   链接：https://36kr.com/p/yyyyy
   摘要：这是另一条新闻的简要描述...

36 氪 共找到 10 篇符合条件的文章

抓取完成！
```

### 所有新闻源
```
╔══════════════════════════════════════════════════════════╗
║          综合科技新闻抓取工具                            ║
╚══════════════════════════════════════════════════════════╝

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
【1/5】36 氪
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

正在抓取 36 氪 最近 24 小时内的新闻...
...

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
【2/5】虎嗅网
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

正在抓取 虎嗅网 最近 24 小时内的新闻...
...

(依次显示所有新闻源)

抓取完成！
```

## 故障排除

### 问题：无法获取 RSS Feed
- 检查网络连接
- 确认 RSS 地址是否正确
- 尝试增加重试次数
- 某些新闻源可能暂时不可用

### 问题：xmllint 命令未找到
- 安装 libxml2-utils (Ubuntu/Debian)
- 安装 libxml2 (CentOS/macOS)

### 问题：显示时间为空或错误
- 检查系统时区设置
- 确认 Python3 已安装

### 问题：某些新闻源无数据
- 该新闻源可能更新了 RSS 地址
- 该新闻源可能在指定时间内无新文章
- 尝试增加时间范围 (HOURS 参数)

## 性能提示

- 使用 `all` 参数时会依次抓取 5 个新闻源，耗时较长 (约 8-25 秒)
- 建议指定具体新闻源以提高效率
- 增加 LIMIT 参数会增加处理时间
- 网络状况不佳时可减少新闻源数量或文章数量