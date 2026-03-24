#!/bin/bash
#
# 综合科技新闻 RSS Feed 抓取脚本
# 支持多个科技媒体 RSS 源：36kr、虎嗅网、艾瑞网、爱范儿、效率火箭
#

# 默认参数
DEFAULT_SOURCE="all"
HOURS=${2:-24}
LIMIT=${3:-10}

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# 获取 RSS URL 和名称
get_source_info() {
    local key=$1
    case $key in
        "36kr")
            echo "https://rss.aishort.top/?type=36kr|36 氪"
            ;;
        "huxiu")
            echo "https://rss.aishort.top/?type=huxiu|虎嗅网"
            ;;
        "iresearch")
            echo "https://rss.aishort.top/?type=iresearch|艾瑞网"
            ;;
        "appsolution")
            echo "https://rss.aishort.top/?type=AppSolution|爱范儿"
            ;;
        "xlrocket")
            echo "https://rss.aishort.top/?type=xlrocket|效率火箭"
            ;;
        *)
            echo ""
            ;;
    esac
}

# 获取所有新闻源键列表
get_all_sources() {
    echo "36kr huxiu iresearch appsolution xlrocket"
}

# 显示帮助信息
show_help() {
    echo -e "${GREEN}综合科技新闻抓取工具${NC}"
    echo ""
    echo "用法：$0 [SOURCE] [HOURS] [LIMIT]"
    echo ""
    echo "参数说明："
    echo "  SOURCE    新闻源 (可选): 36kr, huxiu, iresearch, appsolution, xlrocket, all"
    echo "  HOURS     时间范围 (可选): 最近多少小时，默认 24"
    echo "  LIMIT     文章数量 (可选): 最多显示多少篇，默认 10"
    echo ""
    echo "可用新闻源："
    echo -e "  ${CYAN}36kr${NC} - 36 氪"
    echo -e "  ${CYAN}huxiu${NC} - 虎嗅网"
    echo -e "  ${CYAN}iresearch${NC} - 艾瑞网"
    echo -e "  ${CYAN}appsolution${NC} - 爱范儿"
    echo -e "  ${CYAN}xlrocket${NC} - 效率火箭"
    echo -e "  ${CYAN}all${NC} - 抓取所有新闻源"
    echo ""
    echo "示例："
    echo "  $0                    # 抓取 36 氪最近 24 小时的 10 篇新闻"
    echo "  $0 huxiu              # 抓取虎嗅网最近 24 小时的 10 篇新闻"
    echo "  $0 iresearch 12 5     # 抓取艾瑞网最近 12 小时的 5 篇新闻"
    echo "  $0 all 24 20          # 抓取所有新闻源最近 24 小时的各 20 篇新闻"
    echo ""
    exit 0
}

# 检查帮助参数
if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    show_help
fi

# 获取新闻源
SOURCE_KEY=${1:-$DEFAULT_SOURCE}

# 检查依赖
check_dependencies() {
    if ! command -v curl &> /dev/null; then
        echo -e "${RED}错误：需要安装 curl${NC}"
        exit 1
    fi
    
    if ! command -v xmllint &> /dev/null; then
        echo -e "${RED}错误：需要安装 xmllint (libxml2-utils)${NC}"
        exit 1
    fi
}

check_dependencies

# 获取当前时间戳
current_timestamp=$(date +%s)
threshold_timestamp=$((current_timestamp - HOURS * 3600))

# 抓取单个 RSS 源的函数
fetch_rss() {
    local source_key=$1
    local source_info=$(get_source_info "$source_key")
    
    if [ -z "$source_info" ]; then
        echo -e "${RED}错误：未知的新闻源 '${source_key}'${NC}"
        return 1
    fi
    
    local rss_url="${source_info%|*}"
    local source_name="${source_info#*|}"
    
    echo -e "${GREEN}正在抓取 ${source_name} 最近 ${HOURS} 小时内的新闻...${NC}\n"
    
    # 获取 RSS 内容
    rss_content=$(curl -sL --fail --retry 2 --retry-delay 1 --connect-timeout 10 \
        -A "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36" "$rss_url")
    
    if [ $? -ne 0 ]; then
        echo -e "${RED}错误：无法获取 ${source_name} 的 RSS Feed${NC}\n"
        return 1
    fi
    
    # 提取 Feed 信息
    feed_title=$(echo "$rss_content" | xmllint --xpath "string(//*[local-name()='channel']/*[local-name()='title'])" - 2>/dev/null)
    feed_link=$(echo "$rss_content" | xmllint --xpath "string(//*[local-name()='channel']/*[local-name()='link'])" - 2>/dev/null)
    if [ -z "$feed_link" ]; then
        feed_link=$(echo "$rss_content" | xmllint --xpath "string((//*[local-name()='link']/@href)[1])" - 2>/dev/null)
    fi
    
    echo "Feed 标题：${feed_title:-$source_name}"
    echo "Feed 链接：${feed_link:-未知}"
    echo ""
    echo "============================================================"
    
    # 提取所有 item
    items_count=$(echo "$rss_content" | xmllint --xpath "count(//*[local-name()='item'])" - 2>/dev/null | awk '{printf("%d",$1)}')
    
    if [ -z "$items_count" ] || [ "$items_count" -le 0 ]; then
        echo -e "${RED}错误：无法解析 RSS 内容${NC}\n"
        return 1
    fi
    
    # 计数器
    count=0
    found=0
    
    # 逐个处理 item
    for i in $(seq 1 "$items_count"); do
        title=$(echo "$rss_content" | xmllint --xpath "string((//*[local-name()='item'])[$i]/*[local-name()='title'])" - 2>/dev/null)
        pubdate=$(echo "$rss_content" | xmllint --xpath "string((//*[local-name()='item'])[$i]/*[local-name()='pubDate'])" - 2>/dev/null)
        
        # 转换时间为时间戳
        if [ -n "$pubdate" ]; then
            item_timestamp=$(python3 - "$pubdate" <<'PY'
import datetime
import email.utils
import sys
s = sys.argv[1]
for fn in (
    lambda x: email.utils.parsedate_to_datetime(x),
    lambda x: datetime.datetime.fromisoformat(x.replace("Z", "+00:00")),
):
    try:
        dt = fn(s)
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=datetime.timezone.utc)
        print(int(dt.timestamp()))
        break
    except Exception:
        pass
PY
)
            if [ -n "$item_timestamp" ] && [ "$item_timestamp" -ge "$threshold_timestamp" ]; then
                found=$((found + 1))
                count=$((count + 1))
                
                echo ""
                echo -e "${YELLOW}${count}. ${title}${NC}"
                echo ""
                
                # 检查是否达到限制
                if [ "$count" -ge "$LIMIT" ]; then
                    break
                fi
            fi
        fi
    done
    
    # 如果没有找到文章
    if [ "$found" -eq 0 ]; then
        echo ""
        echo -e "${YELLOW}抱歉，${source_name} 最近 ${HOURS} 小时没有找到新的文章哦～${NC}\n"
    else
        echo -e "${GREEN}${source_name} 共找到 ${found} 篇符合条件的文章${NC}\n"
    fi
    
    return 0
}

# 主逻辑
echo ""
echo -e "${BLUE}╔══════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║                   综合科技新闻                             ║${NC}"
echo -e "${BLUE}╚══════════════════════════════════════════════════════════╝${NC}"
echo ""

if [ "$SOURCE_KEY" = "all" ]; then
    # 抓取所有新闻源
    all_sources=$(get_all_sources)
    total_sources=5
    current_source=0
    
    for key in $all_sources; do
        current_source=$((current_source + 1))
        source_info=$(get_source_info "$key")
        source_name="${source_info#*|}"
        echo -e "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        echo -e "${CYAN}【${current_source}/${total_sources}】${source_name}${NC}"
        echo -e "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}\n"
        fetch_rss "$key"
    done
else
    # 检查新闻源是否存在
    source_info=$(get_source_info "$SOURCE_KEY")
    if [ -z "$source_info" ]; then
        echo -e "${RED}错误：未知的新闻源 '${SOURCE_KEY}'${NC}"
        echo ""
        echo "可用的新闻源："
        echo -e "  ${CYAN}36kr${NC} - 36 氪"
        echo -e "  ${CYAN}huxiu${NC} - 虎嗅网"
        echo -e "  ${CYAN}iresearch${NC} - 艾瑞网"
        echo -e "  ${CYAN}appsolution${NC} - 爱范儿"
        echo -e "  ${CYAN}xlrocket${NC} - 效率火箭"
        echo -e "  ${CYAN}all${NC} - 抓取所有新闻源"
        echo ""
        echo "使用 -h 或 --help 查看更多帮助信息"
        exit 1
    fi
    
    # 抓取指定新闻源
    fetch_rss "$SOURCE_KEY"
fi

echo -e "${GREEN}抓取完成！${NC}"
