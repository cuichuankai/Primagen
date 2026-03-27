#include "../../../src/include/channel.h"
#include "../../../src/include/common.h"
#include "../../../src/include/logger.h"
#include "../../../src/include/plugin.h"
#include "../../../src/plugin/plugin_manager.h"
#include "../../../src/vendor/cJSON/cJSON.h"
#include "../../../src/vendor/mongoose/mongoose.h"
#include <ctype.h>
#include <dlfcn.h>
#include <dirent.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

typedef struct ReplyNode {
    char* chat_id;
    char* content;
    time_t created_at;
    struct ReplyNode* next;
} ReplyNode;

typedef struct {
    MessageBus* bus;
    Config* cfg;
    PluginConfig* plugin_cfg;
    LoadedPlugin* plugin;
    pthread_t server_thread;
    pthread_mutex_t lock;
    bool running;
    int port;
    char config_path[512];
    struct mg_mgr mgr;
    struct mg_connection* listener;
    ReplyNode* replies_head;
    ReplyNode* replies_tail;
} WebUIChannelData;

static LoadedPlugin* g_plugin_instance = NULL;

static const char* WEBUI_HTML =
"<!doctype html>"
"<html data-theme='light'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Primagen Admin</title>"
"<style>"
"*{box-sizing:border-box}:root{--bg:#e9eef6;--bg-grad:#f6f8fc;--panel:#f3f6fbcc;--panel-2:#eef3f9cc;--text:#0f172a;--muted:#5b6475;--line:#d6deea;--line-strong:#bcc8db;--table-head-bg:#e9eef7;--table-row-bg:#f7fafe;--primary:#4f6bff;--primary-soft:#e8edff;--shadow:0 12px 30px rgba(30,41,59,.08)}html[data-theme='dark']{--bg:#0e1420;--bg-grad:#121a2a;--panel:#1a2335cc;--panel-2:#1e2940cc;--text:#dbe4f6;--muted:#9aa8c2;--line:#33415e;--line-strong:#4a5c82;--table-head-bg:#25314c;--table-row-bg:#1b2438;--primary:#8a9bff;--primary-soft:#2a3558;--shadow:0 12px 28px rgba(0,0,0,.45)}body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,Roboto,Helvetica,Arial,sans-serif;margin:0;background:linear-gradient(140deg,var(--bg-grad),var(--bg));color:var(--text)}"
".layout{display:flex;height:100vh}.sidebar{width:240px;background:var(--panel);backdrop-filter:blur(10px);border-right:1px solid var(--line);display:flex;flex-direction:column;transition:width .2s ease;position:relative}"
".sidebar.collapsed{width:72px}.brand{display:flex;align-items:center;justify-content:space-between;padding:14px 12px;border-bottom:1px solid var(--line);height:64px}.brand-main{display:flex;align-items:center;gap:10px}.logo{width:30px;height:30px;border-radius:9px;background:linear-gradient(135deg,var(--primary),#7ea0ff);color:#fff;display:flex;align-items:center;justify-content:center;font-weight:700}"
".brand-text{font-size:18px;font-weight:700;color:var(--text);white-space:nowrap}.sidebar.collapsed .brand-text{display:none}.menu{padding:12px 8px;overflow:auto}"
".group-title{font-size:12px;color:var(--muted);padding:8px 10px}.sidebar.collapsed .group-title{display:none}.item,.subitem{display:flex;align-items:center;gap:10px;padding:10px 12px;border-radius:10px;cursor:pointer;color:var(--text);margin-bottom:4px;user-select:none}"
".item:hover,.subitem:hover{background:var(--panel-2)}.item.active,.subitem.active{background:var(--primary-soft);color:var(--primary);font-weight:600}.icon{width:20px;height:20px;display:flex;align-items:center;justify-content:center;flex:none}"
".icon svg{width:18px;height:18px;stroke:currentColor;stroke-width:1.9;fill:none;stroke-linecap:round;stroke-linejoin:round}.label{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.sidebar.collapsed .label{display:none}.caret{margin-left:auto;color:var(--muted)}.sidebar.collapsed .caret{display:none}"
".sublist{margin-left:8px;padding-left:8px;border-left:1px dashed var(--line);display:none}.sublist.open{display:block}.sidebar.collapsed .sublist{display:none!important}"
".icon-btn{width:30px;height:30px;border:1px solid var(--line);background:var(--panel-2);color:var(--text);border-radius:9px;display:flex;align-items:center;justify-content:center;cursor:pointer}.icon-btn svg{width:16px;height:16px;stroke:currentColor;stroke-width:2;fill:none;stroke-linecap:round;stroke-linejoin:round}"
".content{flex:1;display:flex;flex-direction:column;min-width:0;height:100vh}.topbar{position:sticky;top:0;z-index:9;height:56px;display:flex;align-items:center;justify-content:space-between;padding:10px 18px;background:var(--panel);backdrop-filter:blur(10px);border-bottom:1px solid var(--line)}"
".path{font-size:13px;color:var(--muted);background:var(--panel-2);padding:7px 10px;border-radius:10px;border:1px solid var(--line)}.path strong{color:var(--text)}.menu-btn,.theme-btn{border:1px solid var(--line);background:var(--panel-2);color:var(--text);border-radius:10px;padding:7px 10px;cursor:pointer;display:inline-flex;align-items:center;gap:8px}.theme-btn:hover{border-color:var(--line-strong);box-shadow:0 0 0 3px color-mix(in srgb,var(--primary) 18%, transparent)}.menu-btn{display:none}"
".view{padding:16px 20px;overflow:auto;min-height:0}.title{font-size:22px;font-weight:700}.subtitle{color:var(--muted);font-size:13px}"
".row{display:flex;gap:12px;flex-wrap:wrap}.card{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:14px;box-shadow:var(--shadow);margin-bottom:12px}"
".item,.subitem,.card,button,input,textarea,select,.theme-btn,.icon-btn{transition:all .15s ease}.card:hover{transform:translateY(-1px);box-shadow:0 16px 32px rgba(30,41,59,.12)}"
".list-grid{display:grid;grid-template-columns:1fr;gap:12px}.list-item-card{background:var(--panel-2);border:1px solid var(--line);border-radius:12px;padding:12px;box-shadow:var(--shadow)}.list-item-card:hover{border-color:var(--line-strong);transform:translateY(-1px)}.list-item-head{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:8px}.list-item-title{font-weight:700;color:var(--text)}.card-title{font-size:18px;font-weight:700;color:var(--text);margin-bottom:14px}.list-kv{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:8px 0;border-top:1px dashed var(--line)}.list-kv strong{display:flex;align-items:center;justify-content:flex-end;font-size:13px;color:var(--text);font-weight:600;word-break:break-all}.switch{position:relative;width:42px;height:24px;border-radius:999px;border:1px solid var(--line-strong);background:var(--panel);padding:0;display:inline-flex;align-items:center;cursor:pointer;transition:all .15s ease}.switch-slider{position:absolute;left:3px;width:18px;height:18px;border-radius:50%;background:var(--muted);transition:all .15s ease}.switch.on{background:var(--primary-soft);border-color:var(--primary)}.switch.on .switch-slider{left:21px;background:var(--primary)}.switch.disabled{opacity:.5;cursor:not-allowed}.card-head{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:10px}.card-head.actions-right{justify-content:flex-end}.plus-btn{background:var(--primary);color:#fff;border-color:var(--primary);font-size:16px;font-weight:700}.modal-mask{position:fixed;inset:0;background:rgba(9,15,24,.45);backdrop-filter:blur(2px);display:none;align-items:center;justify-content:center;z-index:40}.modal-mask.open{display:flex}.modal-card{width:min(980px,94vw);max-height:92vh;overflow:auto;background:var(--panel);border:1px solid var(--line-strong);border-radius:14px;padding:16px;box-shadow:var(--shadow)}.modal-head-actions{display:flex;align-items:center;gap:8px}.form-row{display:flex;align-items:flex-start;gap:12px;margin-bottom:10px}.form-label{width:72px;padding-top:10px;color:var(--muted);font-size:13px}.form-field{flex:1}.input-one-line,.input-two-line,.content-area{width:100%}.input-one-line{height:40px}.input-two-line{height:72px;min-height:72px;resize:vertical}.content-area{min-height:240px}.form-actions{display:flex;gap:10px;justify-content:flex-end}"
".metric{min-width:180px;flex:1}.metric .k{font-size:12px;color:var(--muted)}.metric .v{font-size:26px;font-weight:700;color:var(--text);margin-top:4px}"
".list-item-card{background:linear-gradient(145deg,color-mix(in srgb,var(--panel-2) 78%, transparent),var(--panel));border:1px solid color-mix(in srgb,var(--line) 80%, var(--primary));border-radius:14px;padding:14px 15px;box-shadow:0 10px 24px rgba(30,41,59,.09)}.list-item-card:hover{border-color:var(--primary);transform:translateY(-2px);box-shadow:0 18px 34px rgba(30,41,59,.16)}.list-item-head{align-items:flex-start;gap:12px}.list-item-title{font-size:19px;line-height:1.25;font-weight:800;letter-spacing:.2px}.list-item-actions{display:flex;align-items:center;gap:8px}.list-item-body{margin-top:10px;display:flex;flex-direction:column;gap:8px}.list-item-desc{font-size:14px;line-height:1.6;color:var(--text);opacity:.94}.list-item-meta{display:flex;align-items:center;gap:8px;flex-wrap:wrap}.meta-chip{display:inline-flex;align-items:center;height:24px;padding:0 9px;border-radius:999px;background:var(--primary-soft);border:1px solid color-mix(in srgb,var(--primary) 50%, var(--line));font-size:12px;color:var(--primary);font-weight:700;text-transform:uppercase}.meta-id{display:inline-flex;align-items:center;height:24px;padding:0 9px;border-radius:999px;background:var(--panel);border:1px solid var(--line);font-size:12px;color:var(--muted)}"
"input,textarea,button,select{background:var(--panel-2);color:var(--text);border:1px solid var(--line-strong);border-radius:10px;padding:8px;font-size:14px}input,textarea{overflow-wrap:anywhere;word-break:break-word}"
"textarea{min-height:130px;width:100%}button{cursor:pointer;background:var(--primary);color:#fff;border-color:var(--primary)}button.secondary{background:var(--panel-2);color:var(--text);border-color:var(--line)}"
"pre{white-space:pre-wrap;word-break:break-word;background:var(--panel-2);border:1px solid var(--line);padding:10px;border-radius:8px;max-height:280px;overflow:auto}"
".tag{display:inline-block;padding:3px 8px;border-radius:999px;font-size:12px;background:#ffe8cc;color:#a14d00;border:1px solid #ffd5a5}"
".placeholder-box{border:1px dashed var(--line);border-radius:10px;padding:12px;background:var(--panel-2)}.muted{color:var(--muted);font-size:13px}"
".list-item-head .list-kv{margin-left:auto;border-top:none;padding:0;gap:6px;align-items:center;justify-content:flex-end}.list-item-head .list-kv span{font-size:12px;color:var(--muted)}.list-item-head .list-kv strong{display:flex;align-items:center;justify-content:flex-end}.card-title.with-action{display:flex;align-items:center;justify-content:space-between;gap:10px}.view.chat-mode{padding:0;overflow:hidden}.chat-shell{height:calc(100vh - 56px);display:flex;flex-direction:column}.chat-messages{flex:1;overflow:auto;padding:20px 18px;display:flex;flex-direction:column;gap:12px}.chat-msg{display:flex}.chat-msg.user{justify-content:flex-end}.chat-msg.assistant{justify-content:flex-start}.msg-bubble{max-width:min(860px,78%);padding:12px 14px;border-radius:14px;border:1px solid var(--line);background:var(--panel);box-shadow:var(--shadow);line-height:1.6}.chat-msg.user .msg-bubble{background:var(--primary);border-color:var(--primary);color:#fff}.chat-msg.assistant .msg-bubble{background:var(--panel-2)}.msg-bubble p{margin:0}.msg-bubble p+p{margin-top:8px}.msg-bubble pre{margin:8px 0;background:rgba(15,23,42,.08)}html[data-theme='dark'] .msg-bubble pre{background:rgba(148,163,184,.16)}.msg-bubble code{font-family:ui-monospace,SFMono-Regular,Menlo,monospace}.msg-bubble img{display:block;max-width:100%;height:auto;border-radius:10px;border:1px solid var(--line);margin:8px 0}.msg-bubble ul{margin:6px 0 6px 18px;padding:0}.msg-bubble h1,.msg-bubble h2,.msg-bubble h3,.msg-bubble h4,.msg-bubble h5,.msg-bubble h6{margin:8px 0 6px}.chat-compose{padding:12px 18px 16px;border-top:1px solid var(--line);background:color-mix(in srgb,var(--panel) 88%, transparent);backdrop-filter:blur(8px)}.chat-compose-box{display:flex;gap:10px;align-items:flex-end;max-width:980px;margin:0 auto}.chat-input{flex:1;min-height:44px;max-height:180px;resize:vertical;border-radius:12px}.send-btn{height:44px;padding:0 16px;border-radius:12px;font-weight:600}.chat-empty{color:var(--muted);font-size:13px;text-align:center;padding:10px}"
".floating{position:absolute;left:72px;min-width:190px;background:var(--panel);border:1px solid var(--line);border-radius:10px;box-shadow:var(--shadow);padding:8px;z-index:20;display:none}"
".floating.open{display:block}"
".overview-hero{display:grid;grid-template-columns:1.4fr 1fr;gap:12px;margin-bottom:12px}.hero-panel{background:linear-gradient(140deg,color-mix(in srgb,var(--primary) 24%, var(--panel)),var(--panel));border:1px solid color-mix(in srgb,var(--primary) 34%, var(--line));border-radius:14px;padding:16px;box-shadow:var(--shadow)}.hero-title{font-size:20px;font-weight:700}.hero-sub{font-size:13px;color:var(--muted);margin-top:6px}.quick-actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:14px}.quick-actions button{height:36px;padding:0 12px;border-radius:10px}.overview-kpi{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:10px;margin-bottom:12px}.kpi{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:12px}.kpi .k{font-size:12px;color:var(--muted)}.kpi .v{font-size:22px;font-weight:700;margin-top:4px}.chat-toolbar{display:flex;align-items:center;justify-content:space-between;gap:10px;padding:10px 18px;border-bottom:1px solid var(--line);background:var(--panel)}.chat-toolbar .toolbar-actions{display:flex;gap:8px}.chat-toolbar .toolbar-actions .secondary{height:34px;padding:0 12px;border-radius:9px}.config-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}.field{display:flex;flex-direction:column;gap:6px}.field label{font-size:12px;color:var(--muted)}.status-hint{font-size:12px;color:var(--muted)}.status-hint.dirtyHint{color:#d97706}.status-hint.saved{color:#16a34a}.status-hint.error{color:#dc2626}.op-history{margin-top:12px}.op-history .list-kv{border-top:1px dashed var(--line)}"
"@media(max-width:980px){.sidebar{position:fixed;z-index:30;height:100vh;left:0;top:0;transform:translateX(0)}.sidebar.mobile-hidden{transform:translateX(-100%)}.menu-btn{display:inline-flex}}"
"</style>"
"</head><body>"
"<div class='layout'>"
"<aside id='sidebar' class='sidebar'>"
"<div class='brand'><div class='brand-main'><div class='logo'>P</div><div class='brand-text'>Primagen</div></div><button id='sidebarToggle' class='icon-btn' aria-label='toggle sidebar'><span id='sidebarToggleIcon' class='icon'></span></button></div>"
"<div class='menu' id='menu'></div>"
""
"<div id='floating' class='floating'></div>"
"</aside>"
"<main class='content'>"
"<div class='topbar'><div style='display:flex;align-items:center;gap:10px'><button id='mobileBtn' class='menu-btn'>☰</button><div id='routePath' class='path'><strong>概览</strong></div></div><button id='themeBtn' class='theme-btn'><span id='themeIcon' class='icon'></span><span id='themeText'>Dark</span></button></div>"
"<div id='viewRoot' class='view'><div id='pageHeader'><div id='pageTitle' class='title'>Overview</div><div id='pageSubtitle' class='subtitle'></div></div><div id='pageRoot'></div></div>"
"</main></div>"
"<script>"
"const sidebar=document.getElementById('sidebar');const sidebarToggle=document.getElementById('sidebarToggle');const sidebarToggleIcon=document.getElementById('sidebarToggleIcon');const menuEl=document.getElementById('menu');const pageRoot=document.getElementById('pageRoot');const pageTitle=document.getElementById('pageTitle');const pageSubtitle=document.getElementById('pageSubtitle');const pageHeader=document.getElementById('pageHeader');const viewRoot=document.getElementById('viewRoot');const routePath=document.getElementById('routePath');const themeBtn=document.getElementById('themeBtn');const themeIcon=document.getElementById('themeIcon');const themeText=document.getElementById('themeText');const floating=document.getElementById('floating');"
"let collapsed=false;let mobileHidden=window.innerWidth<=980;let openGroups={control:true,settings:true};"
"const menus=["
"{k:'overview',icon:'home',label:'概览',path:'#/overview'},"
"{k:'chat',icon:'chat',label:'聊天',path:'#/chat'},"
"{k:'control',icon:'grid',label:'控制',children:[{k:'channels',icon:'channels',label:'频道',path:'#/control/channels'},{k:'tools',icon:'tools',label:'工具',path:'#/control/tools'},{k:'commands',icon:'commands',label:'命令',path:'#/control/commands'},{k:'skills',icon:'skills',label:'技能',path:'#/control/skills'},{k:'sessions',icon:'sessions',label:'会话',path:'#/control/sessions'},{k:'usage',icon:'usage',label:'使用情况',path:'#/control/usage'},{k:'cron',icon:'cron',label:'定时任务',path:'#/control/cron'}]},"
"{k:'settings',icon:'settings',label:'设置',children:[{k:'config',icon:'config',label:'配置',path:'#/settings/config'},{k:'logs',icon:'logs',label:'日志',path:'#/settings/logs'},{k:'docs',icon:'docs',label:'文档',path:'#/settings/docs'}]}"
"];"
"function route(){if(!location.hash)location.hash='#/overview';return location.hash}"
"function activePath(){return route().replace(/^#/, '')}"
"function isActive(path){return activePath()===path.replace(/^#/, '')}"
"function routeLabel(path){const m={'/overview':'概览','/chat':'聊天','/control/channels':'控制 / 频道','/control/tools':'控制 / 工具','/control/commands':'控制 / 命令','/control/skills':'控制 / 技能','/control/sessions':'控制 / 会话','/control/usage':'控制 / 使用情况','/control/cron':'控制 / 定时任务','/settings/config':'设置 / 配置','/settings/logs':'设置 / 日志','/settings/docs':'设置 / 文档'};return m[path]||path}"
"function esc(s){return String(s||'').replace(/[&<>]/g,m=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[m]))}"
"function icon(name){const m={home:'<svg viewBox=\"0 0 24 24\"><path d=\"M3 10.5L12 3l9 7.5\"></path><path d=\"M5 9.5V20h14V9.5\"></path></svg>',chat:'<svg viewBox=\"0 0 24 24\"><path d=\"M4 5h16v11H8l-4 4z\"></path></svg>',grid:'<svg viewBox=\"0 0 24 24\"><rect x=\"3\" y=\"3\" width=\"8\" height=\"8\"></rect><rect x=\"13\" y=\"3\" width=\"8\" height=\"8\"></rect><rect x=\"3\" y=\"13\" width=\"8\" height=\"8\"></rect><rect x=\"13\" y=\"13\" width=\"8\" height=\"8\"></rect></svg>',settings:'<svg viewBox=\"0 0 24 24\"><path d=\"M12 8a4 4 0 100 8 4 4 0 000-8z\"></path><path d=\"M3 12h3m12 0h3M12 3v3m0 12v3M5.6 5.6l2.1 2.1m8.6 8.6l2.1 2.1m0-12.8l-2.1 2.1m-8.6 8.6l-2.1 2.1\"></path></svg>',channels:'<svg viewBox=\"0 0 24 24\"><path d=\"M4 7h16M4 12h16M4 17h10\"></path></svg>',tools:'<svg viewBox=\"0 0 24 24\"><path d=\"M14 4l6 6\"></path><path d=\"M3 21l7-7\"></path><path d=\"M13 11l-2-2 4-4 2 2z\"></path></svg>',commands:'<svg viewBox=\"0 0 24 24\"><path d=\"M8 8l-4 4 4 4\"></path><path d=\"M16 8l4 4-4 4\"></path></svg>',skills:'<svg viewBox=\"0 0 24 24\"><path d=\"M12 3l3 6 6 .9-4.5 4.3 1 6.3L12 17l-5.5 3.5 1-6.3L3 9.9 9 9z\"></path></svg>',sessions:'<svg viewBox=\"0 0 24 24\"><rect x=\"3\" y=\"4\" width=\"18\" height=\"14\" rx=\"2\"></rect><path d=\"M8 20h8\"></path></svg>',usage:'<svg viewBox=\"0 0 24 24\"><path d=\"M5 19V9\"></path><path d=\"M12 19V5\"></path><path d=\"M19 19v-7\"></path></svg>',cron:'<svg viewBox=\"0 0 24 24\"><circle cx=\"12\" cy=\"12\" r=\"8\"></circle><path d=\"M12 8v5l3 2\"></path></svg>',config:'<svg viewBox=\"0 0 24 24\"><path d=\"M4 7h16\"></path><path d=\"M4 12h10\"></path><path d=\"M4 17h16\"></path></svg>',logs:'<svg viewBox=\"0 0 24 24\"><path d=\"M6 4h12v16H6z\"></path><path d=\"M9 8h6M9 12h6M9 16h4\"></path></svg>',docs:'<svg viewBox=\"0 0 24 24\"><path d=\"M6 3h9l3 3v15H6z\"></path><path d=\"M15 3v3h3\"></path></svg>',sun:'<svg viewBox=\"0 0 24 24\"><circle cx=\"12\" cy=\"12\" r=\"4\"></circle><path d=\"M12 2v2m0 16v2M2 12h2m16 0h2M4.9 4.9l1.4 1.4m11.4 11.4l1.4 1.4m0-14.2l-1.4 1.4M6.3 17.7l-1.4 1.4\"></path></svg>',moon:'<svg viewBox=\"0 0 24 24\"><path d=\"M20 14.5A8.5 8.5 0 119.5 4a7 7 0 1010.5 10.5z\"></path></svg>',chevronLeft:'<svg viewBox=\"0 0 24 24\"><path d=\"M15 18l-6-6 6-6\"></path></svg>',chevronRight:'<svg viewBox=\"0 0 24 24\"><path d=\"M9 18l6-6-6-6\"></path></svg>'};return m[name]||m.grid}"
"function applyTheme(t){document.documentElement.setAttribute('data-theme',t);localStorage.setItem('primagen.theme',t);themeIcon.innerHTML=icon(t==='dark'?'sun':'moon');themeText.textContent=t==='dark'?'Light':'Dark'}"
"function setTitle(t,sub){pageTitle.textContent=t;pageSubtitle.textContent=sub||''}"
"function card(title,body){return '<div class=\"card\"><div class=\"card-title\">'+title+'</div>'+body+'</div>'}"
"function placeholder(title){return card(title,'<div class=\"row\"><span class=\"tag\">待接入</span></div><div class=\"placeholder-box\"><div class=\"muted\">该模块已完成导航与页面结构，后续接入后端接口。</div><ul><li>列表与筛选</li><li>详情与操作</li><li>状态统计卡片</li></ul></div>')}"
"async function jget(u){const r=await fetch(u);if(r.status===204)return null;if(!r.ok)throw new Error(await r.text());return r.json()}"
"async function jpost(u,d){const r=await fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)});if(!r.ok)throw new Error(await r.text());return r.json()}"
"function pluginKindFromPath(p){if(p==='/control/channels')return 'channel';if(p==='/control/tools')return 'tool';if(p==='/control/commands')return 'command';return ''}"
"function encodeBase64(buf){let s='';const bytes=new Uint8Array(buf);const chunk=32768;for(let i=0;i<bytes.length;i+=chunk){s+=String.fromCharCode.apply(null,bytes.subarray(i,i+chunk))}return btoa(s)}"
"async function uploadPluginFromPage(path){const kind=pluginKindFromPath(path);if(!kind)return;const input=document.createElement('input');input.type='file';input.accept='.so';input.onchange=async()=>{const file=input.files&&input.files[0];if(!file)return;if(!file.name.toLowerCase().endsWith('.so')){alert('Only .so file is supported');return}try{const buf=await file.arrayBuffer();const data_base64=encodeBase64(buf);await jpost('/api/control/plugins/upload',{kind,filename:file.name,data_base64});alert('Upload success');render()}catch(e){alert('Upload failed: '+(e&&e.message?e.message:'unknown_error'))}};input.click()}"
"function ensureUploadAction(){const path=activePath();if(!pluginKindFromPath(path))return;const title=pageRoot.querySelector('.card-title');if(!title||title.getAttribute('data-upload-ready')==='1')return;title.classList.add('with-action');title.setAttribute('data-upload-ready','1');const text=title.textContent||'';title.textContent='';const label=document.createElement('span');label.textContent=text;const btn=document.createElement('button');btn.className='icon-btn plus-btn';btn.type='button';btn.textContent='+';btn.setAttribute('aria-label','upload plugin');btn.onclick=()=>uploadPluginFromPage(path);title.appendChild(label);title.appendChild(btn)}"
"function closeFloating(){floating.classList.remove('open');floating.innerHTML=''}"
"function alignEnableToRight(){const kind=pluginKindFromPath(activePath());if(!kind)return;document.querySelectorAll('.list-item-card').forEach(card=>{if(card.getAttribute('data-card-polished')==='1')return;const head=card.querySelector('.list-item-head');if(!head)return;const rows=[...card.querySelectorAll('.list-kv')];let source='';let desc='';let pluginId='';let switchBtn=null;rows.forEach(row=>{const key=(row.getAttribute('data-k')||'').trim().toLowerCase();const valEl=row.querySelector('strong');const val=valEl?valEl.textContent.trim():'';if(key==='source')source=val;else if(key==='description')desc=val;else if(key==='plugin_id')pluginId=val;if(key==='enabled')switchBtn=row.querySelector('button.switch')});rows.forEach(row=>row.remove());if(switchBtn){const actions=document.createElement('div');actions.className='list-item-actions';actions.appendChild(switchBtn);head.appendChild(actions)}const body=document.createElement('div');body.className='list-item-body';if(desc&&desc!=='-'){const descEl=document.createElement('div');descEl.className='list-item-desc';descEl.textContent=desc;body.appendChild(descEl)}const meta=document.createElement('div');meta.className='list-item-meta';if(source){const chip=document.createElement('span');chip.className='meta-chip';chip.textContent=source;meta.appendChild(chip)}if(pluginId){const idTag=document.createElement('span');idTag.className='meta-id';idTag.textContent=pluginId;meta.appendChild(idTag)}if(meta.childNodes.length>0)body.appendChild(meta);if(body.childNodes.length>0)card.appendChild(body);card.setAttribute('data-card-polished','1')});ensureUploadAction()}"
"function renderMenu(){"
"let h='';"
"h+='<div class=\"group-title\">Navigation</div>';"
"for(const item of menus){"
"if(!item.children){h+='<div class=\"item '+(isActive(item.path)?'active':'')+'\" data-path=\"'+item.path+'\"><span class=\"icon\">'+icon(item.icon)+'</span><span class=\"label\">'+item.label+'</span></div>';continue}"
"const childActive=item.children.some(c=>isActive(c.path));const open=!!openGroups[item.k];h+='<div class=\"item '+(childActive?'active':'')+'\" data-group=\"'+item.k+'\"><span class=\"icon\">'+icon(item.icon)+'</span><span class=\"label\">'+item.label+'</span><span class=\"caret\">'+(open?'▾':'▸')+'</span></div>';"
"h+='<div class=\"sublist '+(open?'open':'')+'\">';for(const c of item.children){h+='<div class=\"subitem '+(isActive(c.path)?'active':'')+'\" data-path=\"'+c.path+'\"><span class=\"icon\">'+icon(c.icon||'grid')+'</span><span class=\"label\">'+c.label+'</span></div>'}h+='</div>'"
"}"
"menuEl.innerHTML=h;"
"menuEl.querySelectorAll('[data-path]').forEach(el=>el.onclick=()=>{location.hash=el.getAttribute('data-path');if(window.innerWidth<=980){mobileHidden=true;applySidebar()}});"
"menuEl.querySelectorAll('[data-group]').forEach(el=>el.onclick=(e)=>{const g=el.getAttribute('data-group');if(collapsed){openFloating(g,e.currentTarget);return}openGroups[g]=!openGroups[g];renderMenu()});"
"}"
"function openFloating(group,anchor){const item=menus.find(m=>m.k===group);if(!item||!item.children)return;const rect=anchor.getBoundingClientRect();let h='';for(const c of item.children){h+='<div class=\"subitem '+(isActive(c.path)?'active':'')+'\" data-path=\"'+c.path+'\"><span class=\"icon\">'+icon(c.icon||'grid')+'</span><span class=\"label\">'+c.label+'</span></div>'}floating.innerHTML=h;floating.style.top=(rect.top+window.scrollY)+'px';floating.classList.add('open');floating.querySelectorAll('[data-path]').forEach(el=>el.onclick=()=>{location.hash=el.getAttribute('data-path');closeFloating()})}"
"function applySidebar(){sidebar.classList.toggle('collapsed',collapsed);sidebar.classList.toggle('mobile-hidden',mobileHidden);sidebarToggleIcon.innerHTML=icon(collapsed?'chevronRight':'chevronLeft');if(!collapsed)closeFloating()}"
"function prefKey(name){return 'webui.pref.'+name}"
"function initPrefs(){const ver=localStorage.getItem('webui.pref.version');if(ver!=='1'){localStorage.setItem('webui.pref.version','1');if(!localStorage.getItem('webui.pref.density'))localStorage.setItem('webui.pref.density','comfortable')}document.body.setAttribute('data-density',localStorage.getItem(prefKey('density'))||'comfortable')}"
"function bindGlobal(){sidebarToggle.onclick=()=>{collapsed=!collapsed;applySidebar();renderMenu()};document.getElementById('mobileBtn').onclick=()=>{mobileHidden=!mobileHidden;applySidebar()};themeBtn.onclick=()=>applyTheme(document.documentElement.getAttribute('data-theme')==='dark'?'light':'dark');document.addEventListener('click',(e)=>{if(!floating.contains(e.target)&&!e.target.closest('[data-group]'))closeFloating()})}"
"async function renderOverview(){setTitle('概览','运营总览与系统关键状态');pageRoot.innerHTML='<div class=\"overview-hero\"><div class=\"hero-panel\"><div class=\"hero-title\">Primagen Operations Center</div><div class=\"hero-sub\">统一监控、控制与配置入口</div><div class=\"quick-actions\"><button class=\"secondary\" data-go=\"#/chat\">进入工作台</button><button class=\"secondary\" data-go=\"#/settings/config\">修改配置</button><button class=\"secondary\" data-go=\"#/control/tools\">工具管理</button></div></div><div class=\"card\"><div class=\"card-title\">最近日志摘要</div><div id=\"recentLogs\" class=\"status-hint\">Loading...</div></div></div><div class=\"overview-kpi\"><div class=\"kpi\"><div class=\"k\">服务健康</div><div id=\"kpiHealth\" class=\"v\">--</div></div><div class=\"kpi\"><div class=\"k\">Web Port</div><div id=\"kpiPort\" class=\"v\">--</div></div><div class=\"kpi\"><div class=\"k\">Log Level</div><div id=\"kpiLog\" class=\"v\">--</div></div></div>'+card('系统状态','<div id=\"health\">Loading...</div>')+card('关键配置','<div id=\"cfgsum\">Loading...</div>')+'<div id=\"opHistory\" class=\"op-history\"></div>';pageRoot.querySelectorAll('[data-go]').forEach(btn=>btn.onclick=()=>{location.hash=btn.getAttribute('data-go')});try{const h=await jget('/api/health');document.getElementById('health').innerHTML='<pre>'+esc(JSON.stringify(h,null,2))+'</pre>';document.getElementById('kpiHealth').textContent=h&&h.ok?'Healthy':'Check'}catch(e){document.getElementById('health').innerHTML='<div class=\"tag\">Error</div><div>'+esc(e.message)+'</div>';document.getElementById('kpiHealth').textContent='Error'}try{const c=await jget('/api/config');const s={model:c.agent.model,apiBase:c.agent.apiBase,temperature:c.agent.temperature,max_tokens:c.agent.max_tokens,port:c.webui.port,log_level:c.log.level};document.getElementById('cfgsum').innerHTML='<pre>'+esc(JSON.stringify(s,null,2))+'</pre>';document.getElementById('kpiPort').textContent=String(c.webui.port||'--');document.getElementById('kpiLog').textContent=String(c.log.level||'--')}catch(e){document.getElementById('cfgsum').innerHTML='<div class=\"tag\">Error</div><div>'+esc(e.message)+'</div>'}try{const logs=await jget('/api/settings/logs');const lines=String((logs&&logs.logs)||'').split('\\n').filter(Boolean).slice(-3);document.getElementById('recentLogs').innerHTML=lines.length?('<pre>'+esc(lines.join('\\n'))+'</pre>'):'<span class=\"muted\">No logs</span>'}catch(e){document.getElementById('recentLogs').textContent='Unavailable'}}"
"function markdownToHtml(md){let t=esc(String(md||'')).replace(/\\r\\n/g,'\\n');const blocks=[];t=t.replace(/```([\\w-]*)\\n([\\s\\S]*?)```/g,(_,lang,code)=>{const k='@@CODE'+blocks.length+'@@';blocks.push('<pre><code'+(lang?' data-lang=\"'+lang+'\"':'')+'>'+code+'</code></pre>');return k});t=t.replace(/^######\\s+(.+)$/gm,'<h6>$1</h6>').replace(/^#####\\s+(.+)$/gm,'<h5>$1</h5>').replace(/^####\\s+(.+)$/gm,'<h4>$1</h4>').replace(/^###\\s+(.+)$/gm,'<h3>$1</h3>').replace(/^##\\s+(.+)$/gm,'<h2>$1</h2>').replace(/^#\\s+(.+)$/gm,'<h1>$1</h1>');t=t.replace(/^>\\s+(.+)$/gm,'<blockquote>$1</blockquote>');t=t.replace(/\\*\\*([^*]+)\\*\\*/g,'<strong>$1</strong>').replace(/\\*([^*]+)\\*/g,'<em>$1</em>').replace(/`([^`]+)`/g,'<code>$1</code>');t=t.replace(/!\\[([^\\]]*)\\]\\(((?:https?:\\/\\/|data:image\\/)[^\\s\"'()<>]+)(?:\\s+\"[^\"]*\")?\\)/g,'<img src=\"$2\" alt=\"$1\" loading=\"lazy\" referrerpolicy=\"no-referrer\">');t=t.replace(/\\[([^\\]]+)\\]\\((https?:\\/\\/[^\\s)]+)\\)/g,'<a href=\"$2\" target=\"_blank\" rel=\"noopener noreferrer\">$1</a>');t=t.replace(/^(?:-|\\*)\\s+(.+)$/gm,'<li>$1</li>').replace(/(<li>.*<\\/li>\\n?)+/g,m=>'<ul>'+m.replace(/\\n/g,'')+'</ul>');t=t.split(/\\n{2,}/).map(x=>{const s=x.trim();if(!s)return'';if(/^<(h\\d|ul|ol|pre|blockquote)/.test(s))return s;return '<p>'+s.replace(/\\n/g,'<br>')+'</p>'}).join('');blocks.forEach((h,i)=>{t=t.replace('@@CODE'+i+'@@',h)});return t||'<p></p>'}"
"function appendChatMessage(role,text){const box=document.getElementById('chatMessages');if(!box)return;const row=document.createElement('div');row.className='chat-msg '+role;const bubble=document.createElement('div');bubble.className='msg-bubble';bubble.innerHTML=role==='assistant'?markdownToHtml(text):('<p>'+esc(String(text||'')).replace(/\\n/g,'<br>')+'</p>');row.appendChild(bubble);box.appendChild(row);box.scrollTop=box.scrollHeight}"
"function renderChat(){const chatId=(window._chatId&&String(window._chatId))||'web-user';window._chatId=chatId;pageRoot.innerHTML='<div class=\"chat-shell\"><div class=\"chat-toolbar\"><div><strong>AI 工作台</strong><div class=\"status-hint\">支持轮询控制与快速重试</div></div><div class=\"toolbar-actions\"><button id=\"stopPollBtn\" class=\"secondary\">停止轮询</button><button id=\"retryBtn\" class=\"secondary\">重试</button><button id=\"clearBtn\" class=\"secondary\">清空</button></div></div><div id=\"chatMessages\" class=\"chat-messages\"><div class=\"chat-empty\">Start chatting with Primagen</div></div><div class=\"chat-compose\"><div class=\"chat-compose-box\"><textarea id=\"msgInput\" class=\"chat-input\" placeholder=\"Type your message...\"></textarea><button id=\"sendBtn\" class=\"send-btn\">发送</button></div></div></div>';const box=document.getElementById('chatMessages');const input=document.getElementById('msgInput');let pollEnabled=true;let lastUserInput='';const clearEmpty=()=>{const e=box.querySelector('.chat-empty');if(e)e.remove()};const send=async(text)=>{const message=(text!==undefined?text:input.value||'').trim();if(!message)return;lastUserInput=message;clearEmpty();appendChatMessage('user',message);input.value='';try{await jpost('/api/chat/send',{chat_id:chatId,message})}catch(e){appendChatMessage('assistant','Send failed: '+(e&&e.message?e.message:'unknown_error'))}};document.getElementById('sendBtn').onclick=()=>send();document.getElementById('retryBtn').onclick=()=>send(lastUserInput);document.getElementById('clearBtn').onclick=()=>{box.innerHTML='<div class=\"chat-empty\">Start chatting with Primagen</div>'};document.getElementById('stopPollBtn').onclick=(e)=>{pollEnabled=!pollEnabled;e.target.textContent=pollEnabled?'停止轮询':'恢复轮询'};input.onkeydown=(e)=>{if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();send()}};if(window._chatTimer)clearInterval(window._chatTimer);window._chatTimer=setInterval(async()=>{try{if(!pollEnabled||activePath()!='/chat')return;const r=await fetch('/api/chat/poll?chat_id='+encodeURIComponent(chatId));if(r.status===204)return;if(!r.ok)return;const x=await r.json();if(x&&x.message){clearEmpty();appendChatMessage('assistant',x.message)}}catch(_e){}},1200)}"
"async function renderConfig(){setTitle('配置','读取与更新系统配置');pageRoot.innerHTML=card('配置中心','<div class=\"config-grid\"><div class=\"field\"><label>Model</label><input id=\"model\"></div><div class=\"field\"><label>API Base</label><input id=\"apiBase\"></div><div class=\"field\"><label>Temperature</label><input id=\"temperature\" type=\"number\" step=\"0.1\"></div><div class=\"field\"><label>Max Tokens</label><input id=\"maxTokens\" type=\"number\"></div><div class=\"field\"><label>Web Port</label><input id=\"port\" type=\"number\"></div><div class=\"field\"><label>Log Level</label><select id=\"logLevel\"><option>DEBUG</option><option>INFO</option><option>WARN</option><option>ERROR</option></select></div><div class=\"field\"><label>界面密度</label><select id=\"density\"><option value=\"comfortable\">comfortable</option><option value=\"compact\">compact</option></select></div></div><div class=\"row\" style=\"margin-top:12px\"><button id=\"saveBtn\">保存</button><span id=\"saveHint\" class=\"status-hint\">Ready</span></div><div id=\"dirtyHint\" class=\"status-hint\">未修改</div><pre id=\"cfg\"></pre>');const markDirty=()=>{const el=document.getElementById('dirtyHint');el.className='status-hint dirtyHint';el.textContent='有未保存修改'};['model','apiBase','temperature','maxTokens','port','logLevel','density'].forEach(id=>{setTimeout(()=>{const node=document.getElementById(id);if(node)node.addEventListener('input',markDirty)},0)});document.getElementById('density').value=localStorage.getItem(prefKey('density'))||'comfortable';try{const c=await jget('/api/config');document.getElementById('cfg').textContent=JSON.stringify(c,null,2);document.getElementById('model').value=c.agent.model||'';document.getElementById('apiBase').value=c.agent.apiBase||'';document.getElementById('temperature').value=c.agent.temperature;document.getElementById('maxTokens').value=c.agent.max_tokens;document.getElementById('port').value=c.webui.port;document.getElementById('logLevel').value=c.log.level||'INFO'}catch(e){document.getElementById('cfg').textContent='Error: '+e.message}document.getElementById('saveBtn').onclick=async()=>{const hint=document.getElementById('saveHint');hint.textContent='Saving...';const payload={model:document.getElementById('model').value,apiBase:document.getElementById('apiBase').value,temperature:Number(document.getElementById('temperature').value),max_tokens:Number(document.getElementById('maxTokens').value),port:Number(document.getElementById('port').value),log_level:document.getElementById('logLevel').value,save:true};try{const r=await jpost('/api/config',payload);document.getElementById('cfg').textContent=JSON.stringify(r,null,2);localStorage.setItem(prefKey('density'),document.getElementById('density').value);document.body.setAttribute('data-density',document.getElementById('density').value);document.getElementById('dirtyHint').className='status-hint saved';document.getElementById('dirtyHint').textContent='已保存';hint.textContent='Saved'}catch(e){document.getElementById('cfg').textContent='Error: '+e.message;document.getElementById('dirtyHint').className='status-hint error';document.getElementById('dirtyHint').textContent='保存失败';hint.textContent='Failed'}}}"
"async function renderRemoteList(title,subtitle,endpoint,toggleKind){setTitle(title,subtitle);pageRoot.innerHTML=card(title,'<div id=\"listRoot\">Loading...</div>');try{const d=await jget(endpoint);const items=(d&&Array.isArray(d.items))?d.items:[];if(items.length===0){document.getElementById('listRoot').innerHTML='<div class=\"placeholder-box\"><div class=\"muted\">No records</div></div>';return}let cards='';for(const it of items){const keys=Object.keys(it||{});const primary=keys.includes('name')?'name':keys[0];const titleVal=esc(it[primary]===undefined?'':it[primary]);let kv='';for(const k of keys){if(k===primary)continue;if(k==='enabled'){const enabled=!!it[k];const pluginId=it.plugin_id===undefined?'':String(it.plugin_id);const canToggle=!!toggleKind&&String(it.source||'')==='plugin'&&pluginId.length>0;const switchBtn='<button type=\"button\" class=\"switch '+(enabled?'on ':'')+(canToggle?'':'disabled')+'\" '+(canToggle?'data-enable-toggle=\"1\" data-toggle-kind=\"'+esc(toggleKind)+'\" data-plugin-id=\"'+esc(pluginId)+'\" data-next-enabled=\"'+(enabled?'false':'true')+'\"':'disabled')+' aria-label=\"toggle enabled\"><span class=\"switch-slider\"></span></button>';kv+='<div class=\"list-kv\" data-k=\"enabled\"><strong>'+switchBtn+'</strong></div>';continue}kv+='<div class=\"list-kv\" data-k=\"'+esc(k)+'\"><strong>'+esc(it[k]===undefined?'':it[k])+'</strong></div>'}cards+='<div class=\"list-item-card\"><div class=\"list-item-head\"><div class=\"list-item-title\">'+titleVal+'</div></div>'+kv+'</div>'}document.getElementById('listRoot').innerHTML='<div class=\"list-grid\">'+cards+'</div>';if(toggleKind){document.getElementById('listRoot').querySelectorAll('[data-enable-toggle]').forEach(btn=>btn.onclick=async()=>{const kind=btn.getAttribute('data-toggle-kind')||'';const plugin_id=btn.getAttribute('data-plugin-id')||'';const enabled=btn.getAttribute('data-next-enabled')==='true';if(!kind||!plugin_id)return;btn.disabled=true;try{await jpost('/api/control/plugin-enable',{kind,plugin_id,enabled});await renderRemoteList(title,subtitle,endpoint,toggleKind)}catch(e){btn.disabled=false;document.getElementById('listRoot').insertAdjacentHTML('afterbegin','<div class=\"tag\">Error</div><div>'+esc(e.message)+'</div>')}})}alignEnableToRight()}catch(e){document.getElementById('listRoot').innerHTML='<div class=\"tag\">Error</div><div>'+esc(e.message)+'</div>'}}"
"async function renderSkillsPage(){setTitle('技能管理','列举、编辑、新建与导入技能');pageRoot.innerHTML='<div class=\"card\"><div class=\"card-title with-action\"><span>技能列表</span><button id=\"newSkillBtn\" class=\"icon-btn plus-btn\">➕</button></div><div id=\"skillsTable\">Loading...</div></div>'+'<div id=\"skillModal\" class=\"modal-mask\"><div class=\"modal-card\"><div class=\"card-head\"><div id=\"skillModalTitle\" class=\"list-item-title\">新建技能</div><div class=\"modal-head-actions\"><button id=\"modalImportBtn\" class=\"secondary\">导入技能(zip)</button><button id=\"closeSkillModal\" class=\"icon-btn\">✕</button></div></div><div class=\"form-row\"><div class=\"form-label\">技能名</div><div class=\"form-field\"><input id=\"skillName\" class=\"input-one-line\" placeholder=\"skill-name\"></div></div><div class=\"form-row\"><div class=\"form-label\">描述</div><div class=\"form-field\"><textarea id=\"skillDesc\" class=\"input-two-line\" placeholder=\"what + when to invoke\"></textarea></div></div><div class=\"form-row\"><div class=\"form-label\">内容</div><div class=\"form-field\"><textarea id=\"skillContent\" class=\"content-area\" placeholder=\"# Skill\\n...\\n\"></textarea></div></div><div class=\"form-row\"><div class=\"form-label\"></div><div class=\"form-field\"><div class=\"form-actions\"><button id=\"tmplSkillBtn\" class=\"secondary\">插入模板</button><input id=\"importZip\" type=\"file\" accept=\".zip\" style=\"display:none\"><button id=\"saveSkillBtn\">保存技能</button></div></div></div></div></div>';const modal=document.getElementById('skillModal');const importBtn=document.getElementById('modalImportBtn');const openModal=(title)=>{const isEdit=(title||'')==='编辑技能';document.getElementById('skillModalTitle').textContent=title||'新建技能';importBtn.style.display=isEdit?'none':'';modal.classList.add('open')};const closeModal=()=>modal.classList.remove('open');async function openSkillEditor(name){const detail=await jget('/api/control/skills/content?name='+encodeURIComponent(name));document.getElementById('skillName').value=detail.name||name;document.getElementById('skillDesc').value=detail.description||'';document.getElementById('skillContent').value=detail.content||'';openModal('编辑技能')}async function loadSkills(){try{const d=await jget('/api/control/skills');const items=(d&&Array.isArray(d.items))?d.items:[];if(items.length===0){document.getElementById('skillsTable').innerHTML='<div class=\"placeholder-box\"><div class=\"muted\">No skills</div></div>';return}let cards='';for(const it of items){cards+='<div class=\"list-item-card\"><div class=\"list-item-head\"><div class=\"list-item-title\">'+esc(it.name)+'</div><button class=\"secondary\" data-sname=\"'+it.name+'\">编辑</button></div><div class=\"list-item-body\"><div class=\"list-item-desc\">'+esc(it.description||'')+'</div></div></div>'}document.getElementById('skillsTable').innerHTML='<div class=\"list-grid\">'+cards+'</div>';document.querySelectorAll('[data-sname]').forEach(btn=>btn.onclick=async()=>{try{const n=btn.getAttribute('data-sname');await openSkillEditor(n)}catch(e){}})}catch(e){document.getElementById('skillsTable').innerHTML='<div class=\"tag\">Error</div><div>'+esc(e.message)+'</div>'}}document.getElementById('newSkillBtn').onclick=()=>{document.getElementById('skillName').value='';document.getElementById('skillDesc').value='';document.getElementById('skillContent').value='';openModal('新建技能')};document.getElementById('closeSkillModal').onclick=closeModal;modal.onclick=(e)=>{if(e.target===modal)closeModal()};document.getElementById('tmplSkillBtn').onclick=()=>{const n=(document.getElementById('skillName').value||'new-skill').trim()||'new-skill';if(!document.getElementById('skillDesc').value.trim()){document.getElementById('skillDesc').value='Describe what this skill does and when to invoke it.'}if(!document.getElementById('skillContent').value.trim()){document.getElementById('skillContent').value='# '+n+'\\n\\nUse this skill when ...\\n\\n## Steps\\n1. ...\\n2. ...\\n\\n## Examples\\n- ...\\n'}};document.getElementById('saveSkillBtn').onclick=async()=>{const payload={name:document.getElementById('skillName').value.trim(),description:document.getElementById('skillDesc').value,content:document.getElementById('skillContent').value};if(!payload.name)return;if(!payload.description.trim())return;if(!payload.content.trim())return;try{await jpost('/api/control/skills/save',payload);closeModal();await loadSkills()}catch(e){}};document.getElementById('modalImportBtn').onclick=()=>document.getElementById('importZip').click();document.getElementById('importZip').onchange=async(ev)=>{const f=ev.target.files&&ev.target.files[0];if(!f)return;const rd=new FileReader();rd.onload=async()=>{try{const result=String(rd.result||'');const b64=result.includes(',')?result.split(',')[1]:result;const importResp=await jpost('/api/control/skills/import',{filename:f.name,data_base64:b64});await loadSkills();if(importResp&&importResp.imported_name){await openSkillEditor(importResp.imported_name)}}catch(e){}finally{ev.target.value=''}};rd.readAsDataURL(f)};await loadSkills()}"
"function renderPlaceholderByPath(p){if(p==='/control/skills'){return renderSkillsPage()}const map={'/control/channels':['频道管理','/api/control/channels','channel'],'/control/tools':['工具管理','/api/control/tools','tool'],'/control/commands':['命令管理','/api/control/commands','command'],'/control/sessions':['会话管理','/api/control/sessions',''],'/control/usage':['使用情况','/api/control/usage',''],'/control/cron':['定时任务','/api/control/cron',''],'/settings/logs':['日志中心','/api/settings/logs',''],'/settings/docs':['文档中心','/api/settings/docs','']};if(map[p]){return renderRemoteList(map[p][0],'列表视图',map[p][1],map[p][2])}const t='模块';setTitle(t,'页面结构已就绪，等待后端接入');pageRoot.innerHTML=placeholder(t)}"
"function render(){renderMenu();const p=activePath();const chatMode=p==='/chat';routePath.innerHTML='<strong>'+esc(routeLabel(p))+'</strong>';if(pageHeader)pageHeader.style.display=chatMode?'none':'';if(viewRoot)viewRoot.classList.toggle('chat-mode',chatMode);if(p==='/overview')return renderOverview();if(chatMode)return renderChat();if(p==='/settings/config')return renderConfig();return renderPlaceholderByPath(p)}"
"window.addEventListener('hashchange',render);window.addEventListener('resize',()=>{mobileHidden=window.innerWidth<=980?true:false;applySidebar()});bindGlobal();applySidebar();if(location.pathname==='/chat')location.hash='#/chat';if(location.pathname==='/config')location.hash='#/settings/config';applyTheme(localStorage.getItem('primagen.theme')||'light');initPrefs();render();setInterval(alignEnableToRight,200);alignEnableToRight();"
"</script></body></html>";

static void free_reply_node(ReplyNode* node) {
    if (!node) return;
    free(node->chat_id);
    free(node->content);
    free(node);
}

static void enqueue_reply(WebUIChannelData* data, const char* chat_id, const char* content) {
    if (!data || !chat_id || !content) return;
    ReplyNode* node = calloc(1, sizeof(ReplyNode));
    if (!node) return;
    node->chat_id = strdup(chat_id);
    node->content = strdup(content);
    node->created_at = time(NULL);
    if (!node->chat_id || !node->content) {
        free_reply_node(node);
        return;
    }
    pthread_mutex_lock(&data->lock);
    if (!data->replies_head) {
        data->replies_head = node;
        data->replies_tail = node;
    } else {
        data->replies_tail->next = node;
        data->replies_tail = node;
    }
    pthread_mutex_unlock(&data->lock);
}

static ReplyNode* dequeue_reply(WebUIChannelData* data, const char* chat_id) {
    if (!data || !chat_id) return NULL;
    pthread_mutex_lock(&data->lock);
    ReplyNode* prev = NULL;
    ReplyNode* cur = data->replies_head;
    while (cur) {
        if (strcmp(cur->chat_id, chat_id) == 0) {
            if (prev) prev->next = cur->next;
            else data->replies_head = cur->next;
            if (data->replies_tail == cur) data->replies_tail = prev;
            cur->next = NULL;
            pthread_mutex_unlock(&data->lock);
            return cur;
        }
        prev = cur;
        cur = cur->next;
    }
    pthread_mutex_unlock(&data->lock);
    return NULL;
}

static void cleanup_old_replies(WebUIChannelData* data) {
    if (!data) return;
    time_t now = time(NULL);
    pthread_mutex_lock(&data->lock);
    ReplyNode* prev = NULL;
    ReplyNode* cur = data->replies_head;
    while (cur) {
        if ((now - cur->created_at) > 600) {
            ReplyNode* old = cur;
            cur = cur->next;
            if (prev) prev->next = cur;
            else data->replies_head = cur;
            if (data->replies_tail == old) data->replies_tail = prev;
            free_reply_node(old);
            continue;
        }
        prev = cur;
        cur = cur->next;
    }
    pthread_mutex_unlock(&data->lock);
}

static void resolve_config_path(WebUIChannelData* data) {
    if (!data) return;
    data->config_path[0] = '\0';
    if (!data->plugin || !data->plugin->path) return;
    const char* marker = strstr(data->plugin->path, "/.primagen/plugins/");
    if (!marker) return;
    size_t prefix_len = (size_t)(marker - data->plugin->path);
    if (prefix_len + strlen("/.primagen/config.json") + 1 >= sizeof(data->config_path)) return;
    memcpy(data->config_path, data->plugin->path, prefix_len);
    data->config_path[prefix_len] = '\0';
    strcat(data->config_path, "/.primagen/config.json");
}

static void send_json(struct mg_connection* c, int status, const char* json) {
    mg_http_reply(c, status, "Content-Type: application/json\r\n", "%s", json);
}

static void send_html(struct mg_connection* c, const char* html) {
    mg_http_reply(c, 200, "Content-Type: text/html; charset=utf-8\r\n", "%s", html);
}

static int read_int_from_plugin_cfg(WebUIChannelData* data, const char* key, int default_val) {
    if (!data || !data->plugin_cfg || !data->plugin_cfg->config || !key) return default_val;
    cJSON* item = cJSON_GetObjectItem(data->plugin_cfg->config, key);
    if (!cJSON_IsNumber(item)) return default_val;
    return item->valueint;
}

static void write_int_to_plugin_cfg(WebUIChannelData* data, const char* key, int value) {
    if (!data || !data->plugin_cfg || !data->plugin_cfg->config || !key) return;
    cJSON_DeleteItemFromObject(data->plugin_cfg->config, key);
    cJSON_AddNumberToObject(data->plugin_cfg->config, key, value);
}

static void handle_health(struct mg_connection* c, WebUIChannelData* data) {
    char json[256];
    snprintf(json, sizeof(json), "{\"ok\":true,\"channel\":\"webui\",\"port\":%d}", data->port);
    send_json(c, 200, json);
}

static void handle_chat_send(struct mg_connection* c, struct mg_http_message* hm, WebUIChannelData* data) {
    cJSON* req = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
    if (!req) {
        send_json(c, 400, "{\"error\":\"invalid_json\"}");
        return;
    }
    cJSON* chat_id = cJSON_GetObjectItem(req, "chat_id");
    cJSON* message = cJSON_GetObjectItem(req, "message");
    if (!cJSON_IsString(chat_id) || !cJSON_IsString(message) || !chat_id->valuestring || !message->valuestring) {
        cJSON_Delete(req);
        send_json(c, 400, "{\"error\":\"invalid_params\"}");
        return;
    }
    if (strlen(message->valuestring) == 0 || strlen(message->valuestring) > 8192) {
        cJSON_Delete(req);
        send_json(c, 400, "{\"error\":\"message_length_invalid\"}");
        return;
    }
    InboundMessage* inbound = inbound_message_new("webui", chat_id->valuestring, message->valuestring);
    if (!inbound) {
        cJSON_Delete(req);
        send_json(c, 500, "{\"error\":\"alloc_failed\"}");
        return;
    }
    message_bus_send_inbound(data->bus, inbound);
    cJSON_Delete(req);
    send_json(c, 200, "{\"queued\":true}");
}

static void handle_chat_poll(struct mg_connection* c, struct mg_http_message* hm, WebUIChannelData* data) {
    char chat_id[256] = {0};
    if (mg_http_get_var(&hm->query, "chat_id", chat_id, sizeof(chat_id)) <= 0) {
        send_json(c, 400, "{\"error\":\"chat_id_required\"}");
        return;
    }
    cleanup_old_replies(data);
    ReplyNode* node = dequeue_reply(data, chat_id);
    if (!node) {
        mg_http_reply(c, 204, "", "");
        return;
    }
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "chat_id", node->chat_id);
    cJSON_AddStringToObject(resp, "message", node->content);
    char* s = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    free_reply_node(node);
    if (!s) {
        send_json(c, 500, "{\"error\":\"encode_failed\"}");
        return;
    }
    send_json(c, 200, s);
    free(s);
}

static void handle_get_config(struct mg_connection* c, WebUIChannelData* data) {
    cJSON* resp = cJSON_CreateObject();
    cJSON* agent = cJSON_CreateObject();
    cJSON* log = cJSON_CreateObject();
    cJSON* webui = cJSON_CreateObject();
    cJSON_AddStringToObject(agent, "model", data->cfg->agent.model ? data->cfg->agent.model : "");
    cJSON_AddStringToObject(agent, "apiBase", data->cfg->agent.api_base ? data->cfg->agent.api_base : "");
    cJSON_AddNumberToObject(agent, "temperature", data->cfg->agent.temperature);
    cJSON_AddNumberToObject(agent, "max_tokens", data->cfg->agent.max_tokens);
    cJSON_AddNumberToObject(agent, "max_tool_iterations", data->cfg->agent.max_tool_iterations);
    cJSON_AddNumberToObject(agent, "memory_window", data->cfg->agent.memory_window);
    cJSON_AddStringToObject(agent, "reasoning_effort", data->cfg->agent.reasoning_effort ? data->cfg->agent.reasoning_effort : "");
    cJSON_AddStringToObject(log, "level", data->cfg->log.level ? data->cfg->log.level : "INFO");
    cJSON_AddBoolToObject(log, "consoleOutput", data->cfg->log.console_output);
    cJSON_AddNumberToObject(webui, "port", data->port);
    cJSON_AddBoolToObject(webui, "enabled", true);
    cJSON_AddItemToObject(resp, "agent", agent);
    cJSON_AddItemToObject(resp, "log", log);
    cJSON_AddItemToObject(resp, "webui", webui);
    char* s = cJSON_Print(resp);
    cJSON_Delete(resp);
    if (!s) {
        send_json(c, 500, "{\"error\":\"encode_failed\"}");
        return;
    }
    send_json(c, 200, s);
    free(s);
}

static bool update_string(char** target, cJSON* item) {
    if (!target || !cJSON_IsString(item) || !item->valuestring) return false;
    char* v = strdup(item->valuestring);
    if (!v) return false;
    free(*target);
    *target = v;
    return true;
}

static void handle_update_config(struct mg_connection* c, struct mg_http_message* hm, WebUIChannelData* data) {
    cJSON* req = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
    if (!req) {
        send_json(c, 400, "{\"error\":\"invalid_json\"}");
        return;
    }
    cJSON* item = NULL;
    item = cJSON_GetObjectItem(req, "model");
    if (cJSON_IsString(item)) update_string(&data->cfg->agent.model, item);
    item = cJSON_GetObjectItem(req, "apiBase");
    if (cJSON_IsString(item)) update_string(&data->cfg->agent.api_base, item);
    item = cJSON_GetObjectItem(req, "temperature");
    if (cJSON_IsNumber(item)) data->cfg->agent.temperature = item->valuedouble;
    item = cJSON_GetObjectItem(req, "max_tokens");
    if (cJSON_IsNumber(item)) data->cfg->agent.max_tokens = item->valueint;
    item = cJSON_GetObjectItem(req, "max_tool_iterations");
    if (cJSON_IsNumber(item)) data->cfg->agent.max_tool_iterations = item->valueint;
    item = cJSON_GetObjectItem(req, "memory_window");
    if (cJSON_IsNumber(item)) data->cfg->agent.memory_window = item->valueint;
    item = cJSON_GetObjectItem(req, "reasoning_effort");
    if (cJSON_IsString(item)) update_string(&data->cfg->agent.reasoning_effort, item);
    item = cJSON_GetObjectItem(req, "log_level");
    if (cJSON_IsString(item)) update_string(&data->cfg->log.level, item);
    item = cJSON_GetObjectItem(req, "console_output");
    if (cJSON_IsBool(item)) data->cfg->log.console_output = cJSON_IsTrue(item);
    item = cJSON_GetObjectItem(req, "port");
    if (cJSON_IsNumber(item) && item->valueint > 0 && item->valueint < 65536) {
        data->port = item->valueint;
        write_int_to_plugin_cfg(data, "port", data->port);
    }
    bool should_save = true;
    item = cJSON_GetObjectItem(req, "save");
    if (cJSON_IsBool(item)) should_save = cJSON_IsTrue(item);
    bool saved = false;
    if (should_save && data->config_path[0] != '\0') {
        saved = config_save_to_file(data->cfg, data->config_path);
    }
    cJSON_Delete(req);
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "port", data->port);
    cJSON_AddBoolToObject(resp, "saved", saved);
    if (!saved && should_save) cJSON_AddStringToObject(resp, "warning", "save_failed_restart_required");
    char* s = cJSON_Print(resp);
    cJSON_Delete(resp);
    if (!s) {
        send_json(c, 500, "{\"error\":\"encode_failed\"}");
        return;
    }
    send_json(c, 200, s);
    free(s);
}

static bool get_primagen_dir(WebUIChannelData* data, char* out, size_t out_len) {
    if (!data || !out || out_len == 0) return false;
    out[0] = '\0';
    if (data->config_path[0] != '\0') {
        size_t len = strlen(data->config_path);
        const char* suffix = "/config.json";
        size_t suffix_len = strlen(suffix);
        if (len > suffix_len && strcmp(data->config_path + len - suffix_len, suffix) == 0) {
            size_t base_len = len - suffix_len;
            if (base_len + 1 < out_len) {
                memcpy(out, data->config_path, base_len);
                out[base_len] = '\0';
                return true;
            }
        }
    }
    if (data->plugin && data->plugin->path) {
        const char* marker = strstr(data->plugin->path, "/.primagen/plugins/");
        if (marker) {
            size_t prefix_len = (size_t)(marker - data->plugin->path);
            const char* part = "/.primagen";
            size_t need = prefix_len + strlen(part);
            if (need + 1 < out_len) {
                memcpy(out, data->plugin->path, prefix_len);
                out[prefix_len] = '\0';
                strcat(out, part);
                return true;
            }
        }
    }
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd))) {
        if (snprintf(out, out_len, "%s/.primagen", cwd) < (int)out_len) {
            struct stat st;
            if (stat(out, &st) == 0 && S_ISDIR(st.st_mode)) {
                return true;
            }
        }
    }
    return false;
}

static bool ensure_dir_exists(const char* path) {
    if (!path || path[0] == '\0') return false;
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return mkdir(path, 0755) == 0;
}

static bool is_valid_so_filename(const char* filename) {
    if (!filename || filename[0] == '\0') return false;
    if (strchr(filename, '/') || strchr(filename, '\\')) return false;
    if (strstr(filename, "..")) return false;
    size_t len = strlen(filename);
    if (len <= 3 || strcmp(filename + len - 3, ".so") != 0) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)filename[i];
        if (!(isalnum(ch) || ch == '_' || ch == '-' || ch == '.')) return false;
    }
    return true;
}

static bool get_plugin_upload_dir(WebUIChannelData* data, const char* kind, char* out, size_t out_len) {
    if (!data || !kind || !out || out_len == 0) return false;
    char primagen_dir[1024];
    if (!get_primagen_dir(data, primagen_dir, sizeof(primagen_dir))) return false;
    char plugins_root[1200];
    if (snprintf(plugins_root, sizeof(plugins_root), "%s/plugins", primagen_dir) >= (int)sizeof(plugins_root)) return false;
    const char* subdir = NULL;
    if (strcmp(kind, "channel") == 0) subdir = "channels";
    else if (strcmp(kind, "tool") == 0) subdir = "tools";
    else if (strcmp(kind, "command") == 0) subdir = "commands";
    if (!subdir) return false;
    if (!ensure_dir_exists(plugins_root)) return false;
    if (snprintf(out, out_len, "%s/%s", plugins_root, subdir) >= (int)out_len) return false;
    return ensure_dir_exists(out);
}

static void handle_control_plugin_upload(struct mg_connection* c, struct mg_http_message* hm, WebUIChannelData* data) {
    cJSON* req = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
    if (!req) {
        send_json(c, 400, "{\"error\":\"invalid_json\"}");
        return;
    }
    cJSON* kind = cJSON_GetObjectItem(req, "kind");
    cJSON* filename = cJSON_GetObjectItem(req, "filename");
    cJSON* b64 = cJSON_GetObjectItem(req, "data_base64");
    if (!cJSON_IsString(kind) || !kind->valuestring ||
        !cJSON_IsString(filename) || !filename->valuestring ||
        !cJSON_IsString(b64) || !b64->valuestring) {
        cJSON_Delete(req);
        send_json(c, 400, "{\"error\":\"invalid_params\"}");
        return;
    }
    if (strcmp(kind->valuestring, "channel") != 0 &&
        strcmp(kind->valuestring, "tool") != 0 &&
        strcmp(kind->valuestring, "command") != 0) {
        cJSON_Delete(req);
        send_json(c, 400, "{\"error\":\"invalid_kind\"}");
        return;
    }
    if (!is_valid_so_filename(filename->valuestring)) {
        cJSON_Delete(req);
        send_json(c, 400, "{\"error\":\"invalid_filename\"}");
        return;
    }
    size_t in_len = strlen(b64->valuestring);
    if (in_len == 0 || in_len > 24 * 1024 * 1024) {
        cJSON_Delete(req);
        send_json(c, 400, "{\"error\":\"invalid_size\"}");
        return;
    }
    char target_dir[1400];
    if (!get_plugin_upload_dir(data, kind->valuestring, target_dir, sizeof(target_dir))) {
        cJSON_Delete(req);
        send_json(c, 500, "{\"error\":\"plugin_dir_unavailable\"}");
        return;
    }
    char target_path[1800];
    if (snprintf(target_path, sizeof(target_path), "%s/%s", target_dir, filename->valuestring) >= (int)sizeof(target_path)) {
        cJSON_Delete(req);
        send_json(c, 400, "{\"error\":\"path_too_long\"}");
        return;
    }
    char* decoded = malloc(in_len + 4);
    if (!decoded) {
        cJSON_Delete(req);
        send_json(c, 500, "{\"error\":\"alloc_failed\"}");
        return;
    }
    size_t out_len = mg_base64_decode(b64->valuestring, in_len, decoded, in_len + 3);
    if (out_len == 0) {
        free(decoded);
        cJSON_Delete(req);
        send_json(c, 400, "{\"error\":\"base64_decode_failed\"}");
        return;
    }
    FILE* fp = fopen(target_path, "wb");
    if (!fp) {
        free(decoded);
        cJSON_Delete(req);
        send_json(c, 500, "{\"error\":\"open_failed\"}");
        return;
    }
    size_t written = fwrite(decoded, 1, out_len, fp);
    fclose(fp);
    free(decoded);
    cJSON_Delete(req);
    if (written != out_len) {
        send_json(c, 500, "{\"error\":\"write_failed\"}");
        return;
    }
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "kind", kind->valuestring);
    cJSON_AddStringToObject(resp, "filename", filename->valuestring);
    char* s = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!s) {
        send_json(c, 500, "{\"error\":\"encode_failed\"}");
        return;
    }
    send_json(c, 200, s);
    free(s);
}

static void add_dir_entries(cJSON* items, const char* dir_path, int filter_mode) {
    if (!items || !dir_path) return;
    DIR* dir = opendir(dir_path);
    if (!dir) return;
    struct dirent* ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ent->d_name);
        struct stat st;
        if (stat(full_path, &st) != 0) continue;
        bool is_dir = S_ISDIR(st.st_mode);
        if ((filter_mode == 1 && !is_dir) || (filter_mode == 2 && is_dir)) continue;
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", ent->d_name);
        cJSON_AddStringToObject(obj, "path", full_path);
        cJSON_AddStringToObject(obj, "type", is_dir ? "dir" : "file");
        cJSON_AddNumberToObject(obj, "size", (double)st.st_size);
        cJSON_AddItemToArray(items, obj);
    }
    closedir(dir);
}

static void send_items_response(struct mg_connection* c, cJSON* items) {
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddItemToObject(resp, "items", items ? items : cJSON_CreateArray());
    char* s = cJSON_Print(resp);
    cJSON_Delete(resp);
    if (!s) {
        send_json(c, 500, "{\"error\":\"encode_failed\"}");
        return;
    }
    send_json(c, 200, s);
    free(s);
}

static const char* builtin_channel_description(const char* name) {
    if (!name) return "-";
    if (strcmp(name, "console") == 0) return "Local console input and output channel";
    return "-";
}

static const char* builtin_tool_description(const char* name) {
    if (!name) return "-";
    if (strcmp(name, "read_file") == 0) return "Read file contents from disk";
    if (strcmp(name, "write_file") == 0) return "Write content to a file";
    if (strcmp(name, "edit_file") == 0) return "Apply targeted edits to a file";
    if (strcmp(name, "list_dir") == 0) return "List directory entries";
    if (strcmp(name, "exec") == 0) return "Execute a shell command";
    if (strcmp(name, "send_message") == 0) return "Send message through channels";
    if (strcmp(name, "skill") == 0) return "Run and manage skill workflows";
    if (strcmp(name, "cron") == 0) return "Schedule delayed or periodic tasks";
    return "-";
}

static const char* builtin_command_description(const char* name) {
    if (!name) return "-";
    if (strcmp(name, "stop") == 0) return "Stop active tasks";
    if (strcmp(name, "restart") == 0) return "Restart the bot";
    if (strcmp(name, "new") == 0) return "Start a new conversation";
    if (strcmp(name, "help") == 0) return "Show available commands";
    if (strcmp(name, "tools") == 0) return "Show available tools (builtin/plugin/mcp)";
    if (strcmp(name, "reload-plugins") == 0) return "Reload all plugins from the plugins directory";
    return "-";
}

static bool get_plugin_so_path(WebUIChannelData* data, const char* kind, const char* plugin_id, char* out, size_t out_len) {
    if (!data || !kind || !plugin_id || !out || out_len == 0) return false;
    char primagen_dir[1024];
    if (!get_primagen_dir(data, primagen_dir, sizeof(primagen_dir))) return false;
    const char* subdir = NULL;
    if (strcmp(kind, "channel") == 0) subdir = "channels";
    else if (strcmp(kind, "tool") == 0) subdir = "tools";
    else if (strcmp(kind, "command") == 0) subdir = "commands";
    if (!subdir) return false;
    if (snprintf(out, out_len, "%s/plugins/%s/%s.so", primagen_dir, subdir, plugin_id) >= (int)out_len) return false;
    struct stat st;
    return stat(out, &st) == 0 && S_ISREG(st.st_mode);
}

static char* load_plugin_description_from_so(const char* so_path) {
    if (!so_path || so_path[0] == '\0') return NULL;
    void* handle = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) return NULL;
    PluginGetInfoFunc get_info = (PluginGetInfoFunc)dlsym(handle, "plugin_get_info");
    char* desc = NULL;
    if (get_info) {
        PluginInfo* info = get_info();
        if (info && info->description && info->description[0] != '\0') {
            desc = strdup(info->description);
        }
    }
    dlclose(handle);
    return desc;
}

static char* plugin_item_description(WebUIChannelData* data, const char* kind, const char* plugin_id) {
    char so_path[1600];
    if (!get_plugin_so_path(data, kind, plugin_id, so_path, sizeof(so_path))) return NULL;
    return load_plugin_description_from_so(so_path);
}

static char* plugin_command_name(WebUIChannelData* data, const char* plugin_id) {
    if (!data || !plugin_id) return NULL;
    char so_path[1600];
    if (!get_plugin_so_path(data, "command", plugin_id, so_path, sizeof(so_path))) return NULL;
    void* handle = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) return NULL;
    PluginGetInfoFunc get_info = (PluginGetInfoFunc)dlsym(handle, "plugin_get_info");
    char* name = NULL;
    if (get_info) {
        PluginInfo* info = get_info();
        if (info && info->type == PLUGIN_COMMAND && info->metadata) {
            CommandPluginDef* cmd = (CommandPluginDef*)info->metadata;
            if (cmd->name && cmd->name[0] != '\0') name = strdup(cmd->name);
        }
    }
    dlclose(handle);
    return name;
}

static void handle_control_channels(struct mg_connection* c, WebUIChannelData* data) {
    cJSON* items = cJSON_CreateArray();
    cJSON* console = cJSON_CreateObject();
    cJSON_AddStringToObject(console, "name", "console");
    cJSON_AddStringToObject(console, "source", "builtin");
    cJSON_AddStringToObject(console, "description", builtin_channel_description("console"));
    cJSON_AddBoolToObject(console, "enabled", true);
    cJSON_AddItemToArray(items, console);
    if (data && data->cfg) {
        for (size_t i = 0; i < data->cfg->plugins.count; i++) {
            PluginConfig* pc = &data->cfg->plugins.items[i];
            if (!pc->plugin_id) continue;
            if (!strstr(pc->plugin_id, "_channel")) continue;
            cJSON* obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "name", pc->plugin_id);
            cJSON_AddStringToObject(obj, "plugin_id", pc->plugin_id);
            cJSON_AddStringToObject(obj, "source", "plugin");
            char* desc = plugin_item_description(data, "channel", pc->plugin_id);
            cJSON_AddStringToObject(obj, "description", (desc && desc[0]) ? desc : "-");
            free(desc);
            cJSON_AddBoolToObject(obj, "enabled", pc->enabled);
            cJSON_AddItemToArray(items, obj);
        }
    }
    send_items_response(c, items);
}

static void handle_control_tools(struct mg_connection* c, WebUIChannelData* data) {
    cJSON* items = cJSON_CreateArray();
    const char* builtins[] = {"read_file", "write_file", "edit_file", "list_dir", "exec", "send_message", "skill", "cron"};
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", builtins[i]);
        cJSON_AddStringToObject(obj, "source", "builtin");
        cJSON_AddStringToObject(obj, "description", builtin_tool_description(builtins[i]));
        cJSON_AddBoolToObject(obj, "enabled", true);
        cJSON_AddItemToArray(items, obj);
    }
    if (data && data->cfg) {
        for (size_t i = 0; i < data->cfg->plugins.count; i++) {
            PluginConfig* pc = &data->cfg->plugins.items[i];
            if (!pc->plugin_id) continue;
            if (!strstr(pc->plugin_id, "_tool")) continue;
            cJSON* obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "name", pc->plugin_id);
            cJSON_AddStringToObject(obj, "plugin_id", pc->plugin_id);
            cJSON_AddStringToObject(obj, "source", "plugin");
            char* desc = plugin_item_description(data, "tool", pc->plugin_id);
            cJSON_AddStringToObject(obj, "description", (desc && desc[0]) ? desc : "-");
            free(desc);
            cJSON_AddBoolToObject(obj, "enabled", pc->enabled);
            cJSON_AddItemToArray(items, obj);
        }
    }
    send_items_response(c, items);
}

static void handle_control_commands(struct mg_connection* c, WebUIChannelData* data) {
    cJSON* items = cJSON_CreateArray();
    const char* builtins[] = {"stop", "restart", "new", "help", "tools", "reload-plugins"};
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", builtins[i]);
        cJSON_AddStringToObject(obj, "source", "builtin");
        cJSON_AddStringToObject(obj, "description", builtin_command_description(builtins[i]));
        cJSON_AddBoolToObject(obj, "enabled", true);
        cJSON_AddItemToArray(items, obj);
    }
    if (data && data->cfg) {
        for (size_t i = 0; i < data->cfg->plugins.count; i++) {
            PluginConfig* pc = &data->cfg->plugins.items[i];
            if (!pc->plugin_id) continue;
            if (!strstr(pc->plugin_id, "_command")) continue;
            cJSON* obj = cJSON_CreateObject();
            char* cmd_name = plugin_command_name(data, pc->plugin_id);
            cJSON_AddStringToObject(obj, "name", (cmd_name && cmd_name[0]) ? cmd_name : pc->plugin_id);
            cJSON_AddStringToObject(obj, "plugin_id", pc->plugin_id);
            cJSON_AddStringToObject(obj, "source", "plugin");
            char* desc = plugin_item_description(data, "command", pc->plugin_id);
            cJSON_AddStringToObject(obj, "description", (desc && desc[0]) ? desc : "-");
            free(cmd_name);
            free(desc);
            cJSON_AddBoolToObject(obj, "enabled", pc->enabled);
            cJSON_AddItemToArray(items, obj);
        }
    }
    send_items_response(c, items);
}

static bool get_skills_dir(WebUIChannelData* data, char* out, size_t out_len) {
    char primagen_dir[1024];
    if (!get_primagen_dir(data, primagen_dir, sizeof(primagen_dir))) return false;
    if (snprintf(out, out_len, "%s/skills", primagen_dir) >= (int)out_len) return false;
    struct stat st;
    if (stat(out, &st) != 0) {
        if (mkdir(out, 0755) != 0) return false;
    } else if (!S_ISDIR(st.st_mode)) {
        return false;
    }
    return true;
}

static bool is_valid_skill_name(const char* name) {
    if (!name || !name[0]) return false;
    size_t len = strlen(name);
    if (len > 128) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (isalnum(ch) || ch == '-' || ch == '_') continue;
        return false;
    }
    return true;
}

static bool is_blank_text(const char* s) {
    if (!s) return true;
    while (*s) {
        if (!isspace((unsigned char)*s)) return false;
        s++;
    }
    return true;
}

static char* read_file_to_string(const char* path, size_t max_len) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len < 0 || (size_t)len > max_len) {
        fclose(fp);
        return NULL;
    }
    char* buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    if (n != (size_t)len) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    return buf;
}

static char* extract_description_from_skill_md(const char* content) {
    if (!content) return strdup("");
    const char* key = "description:";
    const char* p = strstr(content, key);
    if (!p) return strdup("");
    p += strlen(key);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') p++;
    const char* end = p;
    while (*end && *end != '\n' && *end != '"') end++;
    size_t len = (size_t)(end - p);
    char* out = malloc(len + 1);
    if (!out) return strdup("");
    memcpy(out, p, len);
    out[len] = '\0';
    return out;
}

static char* extract_body_from_skill_md(const char* content) {
    if (!content) return strdup("");
    const char* p = strstr(content, "\n---");
    if (!p) return strdup(content);
    p += 4;
    while (*p == '\r' || *p == '\n') p++;
    return strdup(p);
}

static bool write_skill_md(const char* skill_md_path, const char* name, const char* description, const char* body) {
    FILE* fp = fopen(skill_md_path, "wb");
    if (!fp) return false;
    const char* desc = description ? description : "";
    const char* content = body ? body : "";
    fprintf(fp, "---\nname: \"%s\"\ndescription: \"%s\"\n---\n\n%s\n", name, desc, content);
    fclose(fp);
    return true;
}

static void handle_control_skills(struct mg_connection* c, WebUIChannelData* data) {
    cJSON* items = cJSON_CreateArray();
    char skills_dir[1024];
    if (!get_skills_dir(data, skills_dir, sizeof(skills_dir))) {
        send_items_response(c, items);
        return;
    }
    DIR* dir = opendir(skills_dir);
    if (!dir) {
        send_items_response(c, items);
        return;
    }
    struct dirent* ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char skill_dir_path[1200];
        snprintf(skill_dir_path, sizeof(skill_dir_path), "%s/%s", skills_dir, ent->d_name);
        struct stat st;
        if (stat(skill_dir_path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        char md_path[1400];
        snprintf(md_path, sizeof(md_path), "%s/SKILL.md", skill_dir_path);
        char* md = read_file_to_string(md_path, 1024 * 1024);
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", ent->d_name);
        cJSON_AddStringToObject(obj, "path", skill_dir_path);
        if (md) {
            char* desc = extract_description_from_skill_md(md);
            cJSON_AddStringToObject(obj, "description", desc ? desc : "");
            free(desc);
            free(md);
        } else {
            cJSON_AddStringToObject(obj, "description", "");
        }
        cJSON_AddItemToArray(items, obj);
    }
    closedir(dir);
    send_items_response(c, items);
}

static void handle_control_skill_content(struct mg_connection* c, struct mg_http_message* hm, WebUIChannelData* data) {
    char name[256] = {0};
    if (mg_http_get_var(&hm->query, "name", name, sizeof(name)) <= 0 || !is_valid_skill_name(name)) {
        send_json(c, 400, "{\"error\":\"invalid_skill_name\"}");
        return;
    }
    char skills_dir[1024];
    if (!get_skills_dir(data, skills_dir, sizeof(skills_dir))) {
        send_json(c, 500, "{\"error\":\"skills_dir_unavailable\"}");
        return;
    }
    char md_path[1400];
    snprintf(md_path, sizeof(md_path), "%s/%s/SKILL.md", skills_dir, name);
    char* md = read_file_to_string(md_path, 1024 * 1024);
    if (!md) {
        send_json(c, 404, "{\"error\":\"skill_not_found\"}");
        return;
    }
    char* desc = extract_description_from_skill_md(md);
    char* body = extract_body_from_skill_md(md);
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "name", name);
    cJSON_AddStringToObject(resp, "description", desc ? desc : "");
    cJSON_AddStringToObject(resp, "content", body ? body : "");
    char* s = cJSON_Print(resp);
    cJSON_Delete(resp);
    free(md);
    free(desc);
    free(body);
    if (!s) {
        send_json(c, 500, "{\"error\":\"encode_failed\"}");
        return;
    }
    send_json(c, 200, s);
    free(s);
}

static void handle_control_skill_save(struct mg_connection* c, struct mg_http_message* hm, WebUIChannelData* data) {
    cJSON* req = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
    if (!req) {
        send_json(c, 400, "{\"error\":\"invalid_json\"}");
        return;
    }
    cJSON* name = cJSON_GetObjectItem(req, "name");
    cJSON* description = cJSON_GetObjectItem(req, "description");
    cJSON* content = cJSON_GetObjectItem(req, "content");
    if (!cJSON_IsString(name) || !is_valid_skill_name(name->valuestring) ||
        !cJSON_IsString(description) || !cJSON_IsString(content)) {
        cJSON_Delete(req);
        send_json(c, 400, "{\"error\":\"invalid_params\"}");
        return;
    }
    if (is_blank_text(description->valuestring)) {
        cJSON_Delete(req);
        send_json(c, 400, "{\"error\":\"description_required\"}");
        return;
    }
    if (is_blank_text(content->valuestring)) {
        cJSON_Delete(req);
        send_json(c, 400, "{\"error\":\"content_required\"}");
        return;
    }
    char skills_dir[1024];
    if (!get_skills_dir(data, skills_dir, sizeof(skills_dir))) {
        cJSON_Delete(req);
        send_json(c, 500, "{\"error\":\"skills_dir_unavailable\"}");
        return;
    }
    char skill_dir[1400];
    snprintf(skill_dir, sizeof(skill_dir), "%s/%s", skills_dir, name->valuestring);
    struct stat st;
    if (stat(skill_dir, &st) != 0) {
        if (mkdir(skill_dir, 0755) != 0) {
            cJSON_Delete(req);
            send_json(c, 500, "{\"error\":\"mkdir_failed\"}");
            return;
        }
    }
    char md_path[1500];
    snprintf(md_path, sizeof(md_path), "%s/SKILL.md", skill_dir);
    bool ok = write_skill_md(md_path, name->valuestring, description->valuestring, content->valuestring);
    cJSON_Delete(req);
    if (!ok) {
        send_json(c, 500, "{\"error\":\"write_failed\"}");
        return;
    }
    send_json(c, 200, "{\"ok\":true}");
}

static void handle_control_skill_import(struct mg_connection* c, struct mg_http_message* hm, WebUIChannelData* data) {
    cJSON* req = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
    if (!req) {
        send_json(c, 400, "{\"error\":\"invalid_json\"}");
        return;
    }
    cJSON* filename = cJSON_GetObjectItem(req, "filename");
    cJSON* b64 = cJSON_GetObjectItem(req, "data_base64");
    if (!cJSON_IsString(filename) || !cJSON_IsString(b64) || !filename->valuestring || !b64->valuestring) {
        cJSON_Delete(req);
        send_json(c, 400, "{\"error\":\"invalid_params\"}");
        return;
    }
    const char* suffix = ".zip";
    size_t fn_len = strlen(filename->valuestring);
    size_t sf_len = strlen(suffix);
    if (fn_len < sf_len || strcmp(filename->valuestring + fn_len - sf_len, suffix) != 0) {
        cJSON_Delete(req);
        send_json(c, 400, "{\"error\":\"zip_required\"}");
        return;
    }
    char skills_dir[1024];
    if (!get_skills_dir(data, skills_dir, sizeof(skills_dir))) {
        cJSON_Delete(req);
        send_json(c, 500, "{\"error\":\"skills_dir_unavailable\"}");
        return;
    }
    size_t in_len = strlen(b64->valuestring);
    char* decoded = malloc(in_len + 4);
    if (!decoded) {
        cJSON_Delete(req);
        send_json(c, 500, "{\"error\":\"alloc_failed\"}");
        return;
    }
    size_t out_len = mg_base64_decode(b64->valuestring, in_len, decoded, in_len + 3);
    if (out_len == 0) {
        free(decoded);
        cJSON_Delete(req);
        send_json(c, 400, "{\"error\":\"base64_decode_failed\"}");
        return;
    }
    char tmp_template[] = "/tmp/primagen_skill_zip_XXXXXX";
    int fd = mkstemp(tmp_template);
    if (fd < 0) {
        free(decoded);
        cJSON_Delete(req);
        send_json(c, 500, "{\"error\":\"tmp_create_failed\"}");
        return;
    }
    FILE* fp = fdopen(fd, "wb");
    if (!fp) {
        close(fd);
        unlink(tmp_template);
        free(decoded);
        cJSON_Delete(req);
        send_json(c, 500, "{\"error\":\"tmp_open_failed\"}");
        return;
    }
    fwrite(decoded, 1, out_len, fp);
    fclose(fp);
    free(decoded);
    cJSON_Delete(req);
    char cmd[2600];
    snprintf(cmd, sizeof(cmd), "unzip -oq '%s' -d '%s' >/dev/null 2>&1", tmp_template, skills_dir);
    int rc = system(cmd);
    unlink(tmp_template);
    if (rc != 0) {
        send_json(c, 500, "{\"error\":\"unzip_failed\"}");
        return;
    }
    char imported_name[256] = {0};
    char root_skill_md[1400];
    snprintf(root_skill_md, sizeof(root_skill_md), "%s/SKILL.md", skills_dir);
    char* md = read_file_to_string(root_skill_md, 1024 * 1024);
    if (md) {
        const char* name_key = "name:";
        const char* p = strstr(md, name_key);
        if (p) {
            p += strlen(name_key);
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '"') p++;
            size_t i = 0;
            while (*p && *p != '\n' && *p != '"' && i + 1 < sizeof(imported_name)) {
                imported_name[i++] = *p++;
            }
            imported_name[i] = '\0';
        }
        if (is_valid_skill_name(imported_name)) {
            char imported_dir[1500];
            snprintf(imported_dir, sizeof(imported_dir), "%s/%s", skills_dir, imported_name);
            struct stat st;
            if (stat(imported_dir, &st) != 0) {
                mkdir(imported_dir, 0755);
            }
            char target_path[1700];
            snprintf(target_path, sizeof(target_path), "%s/SKILL.md", imported_dir);
            rename(root_skill_md, target_path);
        }
        free(md);
    }
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    if (is_valid_skill_name(imported_name)) {
        cJSON_AddStringToObject(resp, "imported_name", imported_name);
    }
    char* s = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!s) {
        send_json(c, 500, "{\"error\":\"encode_failed\"}");
        return;
    }
    send_json(c, 200, s);
    free(s);
}

static void handle_control_sessions(struct mg_connection* c, WebUIChannelData* data) {
    cJSON* items = cJSON_CreateArray();
    char primagen_dir[1024];
    if (get_primagen_dir(data, primagen_dir, sizeof(primagen_dir))) {
        char sessions_dir[1024];
        snprintf(sessions_dir, sizeof(sessions_dir), "%s/sessions", primagen_dir);
        add_dir_entries(items, sessions_dir, 2);
    }
    send_items_response(c, items);
}

static void handle_control_usage(struct mg_connection* c, WebUIChannelData* data) {
    cJSON* items = cJSON_CreateArray();
    cJSON* a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "name", "plugin_count");
    cJSON_AddNumberToObject(a, "value", data && data->cfg ? (double)data->cfg->plugins.count : 0);
    cJSON_AddItemToArray(items, a);
    cJSON* b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "name", "model");
    cJSON_AddStringToObject(b, "value", (data && data->cfg && data->cfg->agent.model) ? data->cfg->agent.model : "");
    cJSON_AddItemToArray(items, b);
    cJSON* d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "name", "webui_port");
    cJSON_AddNumberToObject(d, "value", data ? data->port : 0);
    cJSON_AddItemToArray(items, d);
    send_items_response(c, items);
}

static void handle_control_cron(struct mg_connection* c, WebUIChannelData* data) {
    cJSON* items = cJSON_CreateArray();
    char primagen_dir[1024];
    if (get_primagen_dir(data, primagen_dir, sizeof(primagen_dir))) {
        char cron_path[1024];
        snprintf(cron_path, sizeof(cron_path), "%s/cron_store.json", primagen_dir);
        FILE* fp = fopen(cron_path, "r");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long len = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (len > 0 && len < 4 * 1024 * 1024) {
                char* buf = malloc((size_t)len + 1);
                if (buf) {
                    fread(buf, 1, (size_t)len, fp);
                    buf[len] = '\0';
                    cJSON* root = cJSON_Parse(buf);
                    free(buf);
                    if (root) {
                        cJSON* jobs = cJSON_GetObjectItem(root, "jobs");
                        if (cJSON_IsArray(jobs)) {
                            cJSON* job = NULL;
                            cJSON_ArrayForEach(job, jobs) {
                                cJSON_AddItemToArray(items, cJSON_Duplicate(job, 1));
                            }
                        } else if (cJSON_IsArray(root)) {
                            cJSON* job = NULL;
                            cJSON_ArrayForEach(job, root) {
                                cJSON_AddItemToArray(items, cJSON_Duplicate(job, 1));
                            }
                        }
                        cJSON_Delete(root);
                    }
                }
            }
            fclose(fp);
        }
    }
    send_items_response(c, items);
}

static void handle_settings_logs(struct mg_connection* c, WebUIChannelData* data) {
    cJSON* items = cJSON_CreateArray();
    char primagen_dir[1024];
    if (get_primagen_dir(data, primagen_dir, sizeof(primagen_dir))) {
        char log_dir[1024];
        snprintf(log_dir, sizeof(log_dir), "%s/log", primagen_dir);
        add_dir_entries(items, log_dir, 2);
    }
    send_items_response(c, items);
}

static void handle_settings_docs(struct mg_connection* c, WebUIChannelData* data) {
    cJSON* items = cJSON_CreateArray();
    char primagen_dir[1024];
    if (get_primagen_dir(data, primagen_dir, sizeof(primagen_dir))) {
        add_dir_entries(items, primagen_dir, 2);
        char memory_dir[1024];
        snprintf(memory_dir, sizeof(memory_dir), "%s/memory", primagen_dir);
        add_dir_entries(items, memory_dir, 2);
    }
    send_items_response(c, items);
}

static void handle_control_plugin_enable(struct mg_connection* c, struct mg_http_message* hm, WebUIChannelData* data) {
    if (!data || !data->cfg) {
        send_json(c, 500, "{\"error\":\"invalid_state\"}");
        return;
    }
    cJSON* req = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
    if (!req) {
        send_json(c, 400, "{\"error\":\"invalid_json\"}");
        return;
    }
    cJSON* kind = cJSON_GetObjectItem(req, "kind");
    cJSON* plugin_id = cJSON_GetObjectItem(req, "plugin_id");
    cJSON* enabled = cJSON_GetObjectItem(req, "enabled");
    if (!cJSON_IsString(kind) || !kind->valuestring || !cJSON_IsString(plugin_id) || !plugin_id->valuestring || !cJSON_IsBool(enabled)) {
        cJSON_Delete(req);
        send_json(c, 400, "{\"error\":\"invalid_params\"}");
        return;
    }
    const char* type_tag = NULL;
    if (strcmp(kind->valuestring, "channel") == 0) type_tag = "_channel";
    else if (strcmp(kind->valuestring, "tool") == 0) type_tag = "_tool";
    else if (strcmp(kind->valuestring, "command") == 0) type_tag = "_command";
    if (!type_tag) {
        cJSON_Delete(req);
        send_json(c, 400, "{\"error\":\"invalid_kind\"}");
        return;
    }
    if (!strstr(plugin_id->valuestring, type_tag)) {
        cJSON_Delete(req);
        send_json(c, 400, "{\"error\":\"invalid_plugin_id\"}");
        return;
    }
    bool enabled_value = cJSON_IsTrue(enabled);
    bool found = false;
    for (size_t i = 0; i < data->cfg->plugins.count; i++) {
        PluginConfig* pc = &data->cfg->plugins.items[i];
        if (!pc->plugin_id) continue;
        if (strcmp(pc->plugin_id, plugin_id->valuestring) != 0) continue;
        pc->enabled = enabled_value;
        found = true;
        break;
    }
    if (!found) {
        cJSON_Delete(req);
        send_json(c, 404, "{\"error\":\"plugin_not_found\"}");
        return;
    }
    bool saved = false;
    if (data->config_path[0] != '\0') saved = config_save_to_file(data->cfg, data->config_path);
    cJSON_Delete(req);
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "enabled", enabled_value);
    cJSON_AddBoolToObject(resp, "saved", saved);
    if (!saved) cJSON_AddStringToObject(resp, "warning", "save_failed_restart_required");
    char* s = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!s) {
        send_json(c, 500, "{\"error\":\"encode_failed\"}");
        return;
    }
    send_json(c, 200, s);
    free(s);
}

static void webui_http_handler(struct mg_connection* c, int ev, void* ev_data) {
    if (ev != MG_EV_HTTP_MSG) return;
    struct mg_http_message* hm = (struct mg_http_message*)ev_data;
    WebUIChannelData* data = (WebUIChannelData*)c->fn_data;
    if (!data) {
        send_json(c, 500, "{\"error\":\"server_not_ready\"}");
        return;
    }
    if (mg_match(hm->uri, mg_str("/api/health"), NULL)) {
        handle_health(c, data);
        return;
    }
    if (mg_match(hm->uri, mg_str("/api/chat/send"), NULL)) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            send_json(c, 405, "{\"error\":\"method_not_allowed\"}");
            return;
        }
        handle_chat_send(c, hm, data);
        return;
    }
    if (mg_match(hm->uri, mg_str("/api/chat/poll"), NULL)) {
        handle_chat_poll(c, hm, data);
        return;
    }
    if (mg_match(hm->uri, mg_str("/api/config"), NULL)) {
        if (mg_strcmp(hm->method, mg_str("GET")) == 0) {
            handle_get_config(c, data);
            return;
        }
        if (mg_strcmp(hm->method, mg_str("POST")) == 0) {
            handle_update_config(c, hm, data);
            return;
        }
        send_json(c, 405, "{\"error\":\"method_not_allowed\"}");
        return;
    }
    if (mg_match(hm->uri, mg_str("/api/control/channels"), NULL)) {
        handle_control_channels(c, data);
        return;
    }
    if (mg_match(hm->uri, mg_str("/api/control/tools"), NULL)) {
        handle_control_tools(c, data);
        return;
    }
    if (mg_match(hm->uri, mg_str("/api/control/commands"), NULL)) {
        handle_control_commands(c, data);
        return;
    }
    if (mg_match(hm->uri, mg_str("/api/control/plugin-enable"), NULL)) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            send_json(c, 405, "{\"error\":\"method_not_allowed\"}");
            return;
        }
        handle_control_plugin_enable(c, hm, data);
        return;
    }
    if (mg_match(hm->uri, mg_str("/api/control/plugins/upload"), NULL)) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            send_json(c, 405, "{\"error\":\"method_not_allowed\"}");
            return;
        }
        handle_control_plugin_upload(c, hm, data);
        return;
    }
    if (mg_match(hm->uri, mg_str("/api/control/skills/content"), NULL)) {
        if (mg_strcmp(hm->method, mg_str("GET")) != 0) {
            send_json(c, 405, "{\"error\":\"method_not_allowed\"}");
            return;
        }
        handle_control_skill_content(c, hm, data);
        return;
    }
    if (mg_match(hm->uri, mg_str("/api/control/skills/save"), NULL)) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            send_json(c, 405, "{\"error\":\"method_not_allowed\"}");
            return;
        }
        handle_control_skill_save(c, hm, data);
        return;
    }
    if (mg_match(hm->uri, mg_str("/api/control/skills/import"), NULL)) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            send_json(c, 405, "{\"error\":\"method_not_allowed\"}");
            return;
        }
        handle_control_skill_import(c, hm, data);
        return;
    }
    if (mg_match(hm->uri, mg_str("/api/control/skills"), NULL)) {
        handle_control_skills(c, data);
        return;
    }
    if (mg_match(hm->uri, mg_str("/api/control/sessions"), NULL)) {
        handle_control_sessions(c, data);
        return;
    }
    if (mg_match(hm->uri, mg_str("/api/control/usage"), NULL)) {
        handle_control_usage(c, data);
        return;
    }
    if (mg_match(hm->uri, mg_str("/api/control/cron"), NULL)) {
        handle_control_cron(c, data);
        return;
    }
    if (mg_match(hm->uri, mg_str("/api/settings/logs"), NULL)) {
        handle_settings_logs(c, data);
        return;
    }
    if (mg_match(hm->uri, mg_str("/api/settings/docs"), NULL)) {
        handle_settings_docs(c, data);
        return;
    }
    if (mg_match(hm->uri, mg_str("/"), NULL) ||
        mg_match(hm->uri, mg_str("/chat"), NULL) ||
        mg_match(hm->uri, mg_str("/config"), NULL)) {
        send_html(c, WEBUI_HTML);
        return;
    }
    send_json(c, 404, "{\"error\":\"not_found\"}");
}

static void* webui_server_thread(void* arg) {
    WebUIChannelData* data = (WebUIChannelData*)arg;
    char listen_addr[64];
    snprintf(listen_addr, sizeof(listen_addr), "http://0.0.0.0:%d", data->port);
    mg_mgr_init(&data->mgr);
    data->listener = mg_http_listen(&data->mgr, listen_addr, webui_http_handler, data);
    if (!data->listener) {
        log_error("[WebUIChannel] Failed to listen on %s", listen_addr);
        data->running = false;
        mg_mgr_free(&data->mgr);
        return NULL;
    }
    log_info("[WebUIChannel] Listening on port %d", data->port);
    while (data->running) {
        mg_mgr_poll(&data->mgr, 100);
    }
    mg_mgr_free(&data->mgr);
    log_info("[WebUIChannel] Server stopped");
    return NULL;
}

static bool webui_init(Channel* self, Config* cfg, MessageBus* bus) {
    WebUIChannelData* data = calloc(1, sizeof(WebUIChannelData));
    if (!data) return false;
    data->bus = bus;
    data->cfg = cfg;
    data->plugin = g_plugin_instance;
    data->plugin_cfg = config_get_plugin_config(cfg, "webui_channel");
    data->port = 16714;
    if (data->plugin_cfg && data->plugin_cfg->config) {
        int port = read_int_from_plugin_cfg(data, "port", 16714);
        if (port > 0 && port < 65536) data->port = port;
    }
    resolve_config_path(data);
    pthread_mutex_init(&data->lock, NULL);
    self->user_data = data;
    log_info("[WebUIChannel] Initialized on port %d", data->port);
    return true;
}

static void webui_start(Channel* self) {
    WebUIChannelData* data = (WebUIChannelData*)self->user_data;
    if (!data || data->running) return;
    data->running = true;
    if (pthread_create(&data->server_thread, NULL, webui_server_thread, data) != 0) {
        data->running = false;
        log_error("[WebUIChannel] Failed to create server thread");
    }
}

static void webui_stop(Channel* self) {
    WebUIChannelData* data = (WebUIChannelData*)self->user_data;
    if (!data || !data->running) return;
    data->running = false;
    pthread_join(data->server_thread, NULL);
}

static bool starts_with(const char* s, const char* prefix) {
    if (!s || !prefix) return false;
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

static bool has_image_suffix(const char* path) {
    if (!path) return false;
    const char* dot = strrchr(path, '.');
    if (!dot) return false;
    char ext[16];
    size_t len = strlen(dot + 1);
    if (len == 0 || len >= sizeof(ext)) return false;
    for (size_t i = 0; i < len; i++) ext[i] = (char)tolower((unsigned char)dot[1 + i]);
    ext[len] = '\0';
    return strcmp(ext, "png") == 0 || strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0 ||
           strcmp(ext, "gif") == 0 || strcmp(ext, "webp") == 0;
}

static const char* guess_image_mime(const char* path) {
    if (!path) return "image/png";
    const char* dot = strrchr(path, '.');
    if (!dot) return "image/png";
    char ext[16];
    size_t len = strlen(dot + 1);
    if (len == 0 || len >= sizeof(ext)) return "image/png";
    for (size_t i = 0; i < len; i++) ext[i] = (char)tolower((unsigned char)dot[1 + i]);
    ext[len] = '\0';
    if (strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, "gif") == 0) return "image/gif";
    if (strcmp(ext, "webp") == 0) return "image/webp";
    return "image/png";
}

static char* file_to_data_url(const char* path) {
    if (!path || !has_image_suffix(path)) return NULL;
    struct stat st;
    if (stat(path, &st) != 0) return NULL;
    if (st.st_size <= 0 || st.st_size > 5 * 1024 * 1024) return NULL;
    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;
    unsigned char* raw = malloc((size_t)st.st_size);
    if (!raw) {
        fclose(fp);
        return NULL;
    }
    size_t read_n = fread(raw, 1, (size_t)st.st_size, fp);
    fclose(fp);
    if (read_n != (size_t)st.st_size) {
        free(raw);
        return NULL;
    }
    size_t b64_cap = read_n * 2 + 8;
    char* b64 = malloc(b64_cap);
    if (!b64) {
        free(raw);
        return NULL;
    }
    size_t b64_len = mg_base64_encode(raw, read_n, b64, b64_cap);
    free(raw);
    if (b64_len == 0 || b64_len + 1 >= b64_cap) {
        free(b64);
        return NULL;
    }
    b64[b64_len] = '\0';
    const char* mime = guess_image_mime(path);
    size_t out_len = strlen("data:;base64,") + strlen(mime) + b64_len + 1;
    char* out = malloc(out_len);
    if (!out) {
        free(b64);
        return NULL;
    }
    snprintf(out, out_len, "data:%s;base64,%s", mime, b64);
    free(b64);
    return out;
}

static bool append_markdown_line(char** content, const char* line) {
    if (!content || !line) return false;
    const char* base = *content ? *content : "";
    size_t base_len = strlen(base);
    size_t line_len = strlen(line);
    size_t add = line_len + (base_len > 0 ? 2 : 0) + 1;
    char* merged = realloc(*content, base_len + add);
    if (!merged) return false;
    if (base_len > 0) {
        merged[base_len] = '\n';
        merged[base_len + 1] = '\n';
        memcpy(merged + base_len + 2, line, line_len + 1);
    } else {
        memcpy(merged, line, line_len + 1);
    }
    *content = merged;
    return true;
}

static char* attachment_to_markdown(const char* raw) {
    if (!raw || !*raw) return NULL;
    char* src = NULL;
    cJSON* parsed = cJSON_Parse(raw);
    if (parsed) {
        cJSON* type = cJSON_GetObjectItem(parsed, "type");
        cJSON* path = cJSON_GetObjectItem(parsed, "path");
        if (!cJSON_IsString(type) || !type->valuestring || strcmp(type->valuestring, "image") != 0 ||
            !cJSON_IsString(path) || !path->valuestring) {
            cJSON_Delete(parsed);
            return NULL;
        }
        src = strdup(path->valuestring);
        cJSON_Delete(parsed);
    } else {
        if (starts_with(raw, "http://") || starts_with(raw, "https://") || starts_with(raw, "data:image/")) {
            src = strdup(raw);
        } else if (has_image_suffix(raw)) {
            src = strdup(raw);
        } else {
            return NULL;
        }
    }
    if (!src) return NULL;
    char* resolved = NULL;
    if (starts_with(src, "http://") || starts_with(src, "https://") || starts_with(src, "data:image/")) {
        resolved = src;
    } else {
        resolved = file_to_data_url(src);
        free(src);
        if (!resolved) return NULL;
    }
    size_t out_len = strlen(resolved) + 8;
    char* md = malloc(out_len);
    if (!md) {
        free(resolved);
        return NULL;
    }
    snprintf(md, out_len, "![](%s)", resolved);
    free(resolved);
    return md;
}

static void webui_send(Channel* self, OutboundMessage* msg) {
    if (!self || !msg) return;
    WebUIChannelData* data = (WebUIChannelData*)self->user_data;
    if (!data || !msg->channel.data || !msg->chat_id.data) return;
    if (strcmp(msg->channel.data, "webui") != 0) return;
    char* merged = strdup(msg->content.data ? msg->content.data : "");
    if (!merged) return;
    if (msg->attachments.count > 0) {
        for (size_t i = 0; i < msg->attachments.count; i++) {
            const char* raw = msg->attachments.items[i].data;
            char* md = attachment_to_markdown(raw);
            if (!md) continue;
            append_markdown_line(&merged, md);
            free(md);
        }
    }
    enqueue_reply(data, msg->chat_id.data, merged);
    free(merged);
}

static void webui_destroy(Channel* self) {
    if (!self || !self->user_data) return;
    WebUIChannelData* data = (WebUIChannelData*)self->user_data;
    ReplyNode* cur = data->replies_head;
    while (cur) {
        ReplyNode* next = cur->next;
        free_reply_node(cur);
        cur = next;
    }
    pthread_mutex_destroy(&data->lock);
    free(data);
    self->user_data = NULL;
}

static Channel* webui_channel_create(void) {
    Channel* channel = calloc(1, sizeof(Channel));
    if (!channel) return NULL;
    channel->name = strdup("webui");
    channel->init = webui_init;
    channel->start = webui_start;
    channel->stop = webui_stop;
    channel->send = webui_send;
    channel->destroy = webui_destroy;
    channel->user_data = NULL;
    channel->plugin_ref = NULL;
    return channel;
}

PLUGIN_EXPORT int plugin_init(PluginManager* manager, void* context) {
    g_plugin_instance = (LoadedPlugin*)context;
    int ret = plugin_register_channel(manager, g_plugin_instance, "webui", webui_channel_create);
    if (ret != 0) {
        log_error("[Plugin:webui_channel] Failed to register webui channel");
        return -1;
    }
    log_info("[Plugin:webui_channel] Registered webui channel");
    return 0;
}

PLUGIN_EXPORT int plugin_cleanup(void) {
    return 0;
}

static PluginInfo g_plugin_info = {
    .version = 1,
    .type = PLUGIN_CHANNEL,
    .name = "webui_channel",
    .description = "Web UI channel with dashboard chat and config",
    .plugin_id = "webui_channel"
};

PLUGIN_EXPORT PluginInfo* plugin_get_info(void) {
    return &g_plugin_info;
}
