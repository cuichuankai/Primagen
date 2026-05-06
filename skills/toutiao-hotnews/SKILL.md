---
name: toutiao-hotnews
description: 抓取多个科技媒体 24 小时内热点新闻的综合技能。支持 36 氪、虎嗅网、艾瑞网、爱范儿、效率火箭等 RSS 源，可自定义时间范围和文章数量。当用户需要查看科技资讯、行业动态或热点新闻时使用此技能。
---

# 综合科技热点新闻抓取

## 功能说明

本技能用于从多个科技媒体 RSS Feed 抓取指定时间范围内的热点新闻文章，支持以下新闻源：

- **36kr** - 36 氪
- **huxiu** - 虎嗅网
- **iresearch** - 艾瑞网
- **appsolution** - 爱范儿
- **xlrocket** - 效率火箭
- **all** - 一次性抓取所有新闻源

## 使用方法

### 基本用法

执行脚本抓取 36 氪最近 24 小时的新闻：

```bash
# 抓取 36 氪 (默认)
skills/toutiao-hotnews/scripts/rss_fetch.sh

```

### 指定新闻源

```bash
# 抓取虎嗅网
skills/toutiao-hotnews/scripts/rss_fetch.sh huxiu

# 抓取艾瑞网
skills/toutiao-hotnews/scripts/rss_fetch.sh iresearch

# 抓取爱范儿
skills/toutiao-hotnews/scripts/rss_fetch.sh appsolution

```

### 自定义时间范围和文章数量

```bash
# 语法：脚本 [新闻源] [小时数] [文章数量]

# 抓取 36 氪最近 12 小时的 5 篇新闻
skills/toutiao-hotnews/scripts/rss_fetch.sh 36kr 12 5

# 抓取虎嗅网最近 3 天 (72 小时) 的 20 篇新闻
skills/toutiao-hotnews/scripts/rss_fetch.sh huxiu 72 20

# 抓取所有新闻源最近 24 小时的各 10 篇新闻
skills/toutiao-hotnews/scripts/rss_fetch.sh all 24 10

# 抓取所有新闻源最近一周 (168 小时) 的各 15 篇新闻
skills/toutiao-hotnews/scripts/rss_fetch.sh all 168 15

```

### 查看帮助信息

```bash
skills/toutiao-hotnews/scripts/rss_fetch.sh --help
```

### 输出格式

脚本会输出：

- 新闻源名称
- 文章标题
- 发布时间
- 文章链接
- 简要摘要

## 可用新闻源列表

| 源标识 | 媒体名称 | RSS 地址 |
|--------|----------|----------|
| 36kr | 36 氪 | https://rss.aishort.top/?type=36kr |
| huxiu | 虎嗅网 | https://rss.aishort.top/?type=huxiu |
| iresearch | 艾瑞网 | https://rss.aishort.top/?type=iresearch |
| appsolution | 爱范儿 | https://rss.aishort.top/?type=AppSolution |
| xlrocket | 效率火箭 | https://rss.aishort.top/?type=xlrocket |

## 脚本说明

详见 [脚本使用说明](references/script-usage.md)

## 注意事项

1. 需要网络连接才能获取 RSS Feed
2. 时间基于 RSS 中的 pubDate 字段
3. 默认抓取最近 24 小时内的文章
4. 部分 RSS 地址可能会变化，如遇问题请检查最新地址
5. 使用 `all` 参数时会依次抓取所有新闻源，耗时较长

## 示例输出

```
╔══════════════════════════════════════════════════════════╗
║          综合科技新闻抓取工具                            ║
╚══════════════════════════════════════════════════════════╝

正在抓取 36 氪 最近 24 小时内的新闻...

Feed 标题：36 氪
Feed 链接：https://36kr.com

============================================================

1. 某科技新闻标题
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