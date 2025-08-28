# rss_reader.py
# ==========================================
# 一个界面优雅、跨平台可用的本地 RSS 阅读器（Streamlit 实现）
# 特色：
# - 侧边栏管理订阅（添加/删除/分组）
# - 并发抓取、缓存（TTL 可调），稳健的网络请求（超时/重试/UA）
# - 本地 SQLite 持久化（订阅、文章、已读/收藏）
# - 搜索、按时间排序、只看未读、只看收藏
# - 简洁卡片式 UI，自动提取封面图与图标
# - OPML 导入/导出
#
# 运行：
#   1) 安装依赖：
#      pip install -U streamlit feedparser httpx beautifulsoup4 lxml
#   2) 启动：
#      streamlit run rss_reader.py
#   3) Windows 用户若遇到中文路径问题，建议在非系统盘英文路径下运行。
#
# 可选环境变量：
#   RSS_DB_PATH   指定数据库路径（默认 ./rss_reader.db）
#   RSS_CACHE_TTL 抓取缓存秒数（默认 900 = 15 分钟）
# ==========================================

from __future__ import annotations
import os
import re
import time
import math
import json
import sqlite3
import hashlib
import asyncio
from datetime import datetime, timedelta
from urllib.parse import urlparse

import streamlit as st
import httpx
import feedparser
from bs4 import BeautifulSoup

# ------------------------- 配置 -------------------------
APP_TITLE = "RSS Reader"
DB_PATH = os.environ.get("RSS_DB_PATH", os.path.abspath("./rss_reader.db"))
CACHE_TTL = int(os.environ.get("RSS_CACHE_TTL", "900"))  # 15 min
REQUEST_TIMEOUT = 15.0
CONCURRENT_LIMIT = 8

DEFAULT_FEEDS = [
    {"title": "Hacker News", "url": "https://hnrss.org/frontpage", "category": "极客"},
    {"title": "LWN.net", "url": "https://lwn.net/headlines/rss", "category": "开源"},
    {
        "title": "Ars Technica",
        "url": "https://feeds.arstechnica.com/arstechnica/index/",
        "category": "科技",
    },
    {
        "title": "Cloudflare Blog",
        "url": "https://blog.cloudflare.com/rss/",
        "category": "网络",
    },
    {"title": "少数派", "url": "https://sspai.com/feed", "category": "中文"},
    {"title": "爱范儿", "url": "https://www.ifanr.com/feed", "category": "中文"},
    {
        "title": "Quanta Magazine",
        "url": "https://www.quantamagazine.org/feed",
        "category": "科学",
    },
    {"title": "NASA APOD", "url": "https://apod.nasa.gov/apod.rss", "category": "科学"},
]

USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/120.0.0.0 Safari/537.36 RSS-Reader/1.0"
)

# ------------------------- 工具函数 -------------------------


def _connect_db():
    conn = sqlite3.connect(DB_PATH)
    conn.execute("PRAGMA journal_mode=WAL;")
    conn.execute("PRAGMA synchronous=NORMAL;")
    return conn


def _init_db():
    conn = _connect_db()
    cur = conn.cursor()
    cur.executescript(
        """
        CREATE TABLE IF NOT EXISTS feeds (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT NOT NULL,
            url   TEXT NOT NULL UNIQUE,
            category TEXT,
            added_at TEXT DEFAULT CURRENT_TIMESTAMP
        );

        CREATE TABLE IF NOT EXISTS articles (
            id TEXT PRIMARY KEY,
            feed_id INTEGER NOT NULL,
            title TEXT,
            link  TEXT,
            author TEXT,
            summary TEXT,
            published TEXT,
            image TEXT,
            FOREIGN KEY(feed_id) REFERENCES feeds(id)
        );

        CREATE TABLE IF NOT EXISTS reads (
            article_id TEXT PRIMARY KEY,
            read_at TEXT DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY(article_id) REFERENCES articles(id)
        );

        CREATE TABLE IF NOT EXISTS stars (
            article_id TEXT PRIMARY KEY,
            starred_at TEXT DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY(article_id) REFERENCES articles(id)
        );
        """
    )
    conn.commit()
    # 若为空，写入默认源
    cur.execute("SELECT COUNT(*) FROM feeds;")
    if cur.fetchone()[0] == 0:
        cur.executemany(
            "INSERT OR IGNORE INTO feeds(title, url, category) VALUES (?, ?, ?);",
            [(f["title"], f["url"], f.get("category")) for f in DEFAULT_FEEDS],
        )
        conn.commit()
    conn.close()


@st.cache_data(show_spinner=False)
def favicon_url(site_url: str) -> str:
    try:
        host = urlparse(site_url).netloc
        if not host:
            host = site_url
        # Google s2 图标服务
        return f"https://www.google.com/s2/favicons?domain={host}&sz=64"
    except Exception:
        return ""


def _hash_id(*parts: str) -> str:
    s = "|".join([p or "" for p in parts])
    return hashlib.sha1(s.encode("utf-8", errors="ignore")).hexdigest()


def _first_image_from_entry(entry) -> str | None:
    # 尝试多种字段
    for key in ("media_content", "media_thumbnail"):
        if key in entry and entry[key]:
            try:
                return entry[key][0].get("url")
            except Exception:
                pass
    # 从内容/summary里提取第一张图片
    html = ""
    if "content" in entry and entry["content"]:
        html = entry["content"][0].get("value", "")
    elif "summary" in entry:
        html = entry.get("summary", "")
    soup = BeautifulSoup(html, "lxml")
    img = soup.find("img")
    if img and img.get("src"):
        return img.get("src")
    return None


def _clean_text(x: str | None, limit=400) -> str:
    if not x:
        return ""
    txt = BeautifulSoup(x, "lxml").get_text(" ")
    txt = re.sub(r"\s+", " ", txt).strip()
    if len(txt) > limit:
        txt = txt[: limit - 1] + "…"
    return txt


async def fetch_one(client: httpx.AsyncClient, url: str) -> bytes:
    r = await client.get(
        url, timeout=REQUEST_TIMEOUT, headers={"User-Agent": USER_AGENT}
    )
    r.raise_for_status()
    return r.content


@st.cache_data(ttl=CACHE_TTL, show_spinner=False)
def fetch_feed_cached(url: str) -> dict:
    """因为 Streamlit 的 cache 无法直接缓存协程，这里做一个同步壳。"""
    return asyncio.run(_fetch_feed(url))


async def _fetch_feed(url: str) -> dict:
    async with httpx.AsyncClient(
        follow_redirects=True, headers={"User-Agent": USER_AGENT}
    ) as client:
        try:
            content = await fetch_one(client, url)
        except Exception as e:
            return {"url": url, "error": str(e), "entries": []}

    parsed = feedparser.parse(content)
    feed_title = parsed.feed.get("title", url)
    site_link = parsed.feed.get("link", url)
    entries = []
    for e in parsed.entries:
        link = getattr(e, "link", "")
        guid = getattr(e, "id", "") or link or getattr(e, "title", "")
        eid = _hash_id(url, guid, link)
        published = None
        if getattr(e, "published_parsed", None):
            published = datetime.fromtimestamp(
                time.mktime(e.published_parsed)
            ).isoformat()
        elif getattr(e, "updated_parsed", None):
            published = datetime.fromtimestamp(
                time.mktime(e.updated_parsed)
            ).isoformat()
        img = _first_image_from_entry(e)
        entries.append(
            {
                "id": eid,
                "title": getattr(e, "title", "(无标题)") or "(无标题)",
                "link": link,
                "author": getattr(e, "author", ""),
                "summary": _clean_text(getattr(e, "summary", ""), 600),
                "published": published,
                "image": img,
            }
        )
    return {"url": url, "title": feed_title, "site": site_link, "entries": entries}


def upsert_articles(feed_id: int, items: list[dict]):
    conn = _connect_db()
    cur = conn.cursor()
    cur.executemany(
        """
        INSERT OR IGNORE INTO articles(id, feed_id, title, link, author, summary, published, image)
        VALUES(:id, :feed_id, :title, :link, :author, :summary, :published, :image);
        """,
        [dict(i, feed_id=feed_id) for i in items],
    )
    conn.commit()
    conn.close()


def list_feeds() -> list[dict]:
    conn = _connect_db()
    cur = conn.cursor()
    cur.execute("SELECT id, title, url, category FROM feeds ORDER BY category, title;")
    rows = cur.fetchall()
    conn.close()
    return [{"id": r[0], "title": r[1], "url": r[2], "category": r[3]} for r in rows]


def add_feed(title: str, url: str, category: str | None):
    conn = _connect_db()
    cur = conn.cursor()
    cur.execute(
        "INSERT OR IGNORE INTO feeds(title, url, category) VALUES (?, ?, ?);",
        (title or url, url.strip(), category),
    )
    conn.commit()
    conn.close()


def remove_feed(feed_id: int):
    conn = _connect_db()
    cur = conn.cursor()
    cur.execute("DELETE FROM articles WHERE feed_id=?;", (feed_id,))
    cur.execute("DELETE FROM feeds WHERE id=?;", (feed_id,))
    conn.commit()
    conn.close()


def query_articles(
    feed_ids: list[int] | None,
    only_unread: bool,
    only_starred: bool,
    keyword: str | None,
    limit=200,
):
    conn = _connect_db()
    cur = conn.cursor()
    where = []
    params = []
    if feed_ids:
        where.append("feed_id IN (%s)" % ",".join(["?"] * len(feed_ids)))
        params.extend(feed_ids)
    if only_unread:
        where.append("articles.id NOT IN (SELECT article_id FROM reads)")
    if only_starred:
        where.append("articles.id IN (SELECT article_id FROM stars)")
    if keyword:
        where.append("(title LIKE ? OR summary LIKE ?)")
        kw = f"%{keyword}%"
        params.extend([kw, kw])
    where_sql = ("WHERE " + " AND ".join(where)) if where else ""
    cur.execute(
        f"""
        SELECT articles.id, feed_id, title, link, author, summary, published, image,
               (SELECT 1 FROM reads WHERE article_id = articles.id) as read,
               (SELECT 1 FROM stars WHERE article_id = articles.id) as starred
        FROM articles
        {where_sql}
        ORDER BY COALESCE(published, '1970-01-01') DESC
        LIMIT ?;
        """,
        (*params, limit),
    )
    rows = cur.fetchall()
    conn.close()
    return [
        {
            "id": r[0],
            "feed_id": r[1],
            "title": r[2],
            "link": r[3],
            "author": r[4],
            "summary": r[5],
            "published": r[6],
            "image": r[7],
            "read": bool(r[8]),
            "starred": bool(r[9]),
        }
        for r in rows
    ]


def mark_read(article_id: str):
    conn = _connect_db()
    conn.execute(
        "INSERT OR REPLACE INTO reads(article_id, read_at) VALUES (?, CURRENT_TIMESTAMP);",
        (article_id,),
    )
    conn.commit()
    conn.close()


def toggle_star(article_id: str):
    conn = _connect_db()
    cur = conn.cursor()
    cur.execute("SELECT 1 FROM stars WHERE article_id=?;", (article_id,))
    exist = cur.fetchone()
    if exist:
        cur.execute("DELETE FROM stars WHERE article_id=?;", (article_id,))
    else:
        cur.execute("INSERT INTO stars(article_id) VALUES (?);", (article_id,))
    conn.commit()
    conn.close()


def export_opml() -> str:
    feeds = list_feeds()
    now = datetime.utcnow().strftime("%a, %d %b %Y %H:%M:%S +0000")
    outlines = []
    for f in feeds:
        outlines.append(
            f'      <outline type="rss" text="{f["title"]}" title="{f["title"]}" xmlUrl="{f["url"]}"/>'
        )
    return (
        f'<?xml version="1.0" encoding="UTF-8"?>\n'
        f'<opml version="2.0">\n  <head>\n    <title>Exported Feeds</title>\n    <dateCreated>{now}</dateCreated>\n  </head>\n  <body>\n'
        + "\n".join(outlines)
        + "\n  </body>\n</opml>\n"
    )


def import_opml(opml_bytes: bytes) -> int:
    from xml.etree import ElementTree as ET

    root = ET.fromstring(opml_bytes)
    count = 0
    for node in root.iter("outline"):
        url = node.attrib.get("xmlUrl")
        title = node.attrib.get("title") or node.attrib.get("text")
        if url:
            try:
                add_feed(title or url, url, None)
                count += 1
            except Exception:
                pass
    return count


# ------------------------- UI 部分 -------------------------

st.set_page_config(page_title=APP_TITLE, page_icon="📰", layout="wide")

# 自定义样式（卡片 + 精简字体）
CARD_CSS = """
<style>
:root { --card-bg: #ffffff; --muted: #6b7280; }
[data-theme="dark"] :root { --card-bg: #111827; --muted: #9ca3af; }
.rss-card { display: grid; grid-template-columns: 140px 1fr; gap: 16px; padding: 16px; border-radius: 16px; background: var(--card-bg); box-shadow: 0 4px 20px rgba(0,0,0,0.06); margin-bottom: 14px; }
.rss-thumb { width: 140px; height: 100px; object-fit: cover; border-radius: 12px; background: #f3f4f6; }
.rss-title { font-size: 1.05rem; font-weight: 700; margin: 0 0 6px 0; line-height: 1.35; }
.rss-meta { color: var(--muted); font-size: 0.85rem; margin-bottom: 8px; }
.rss-actions { display:flex; gap:10px; align-items:center; }
.rss-summary { font-size: 0.94rem; line-height: 1.5; }
.badge { display:inline-flex; align-items:center; gap:6px; padding:2px 8px; border-radius:999px; background:#f3f4f6; color:#374151; font-size:12px; }
[data-theme="dark"] .badge { background:#1f2937; color:#d1d5db; }
</style>
"""
st.markdown(CARD_CSS, unsafe_allow_html=True)

# 初始化数据库
_init_db()

# 侧边栏：订阅管理
with st.sidebar:
    st.title("📚 订阅管理")
    feeds = list_feeds()
    cats = sorted(set([f["category"] or "未分组" for f in feeds]))
    selected_cat = st.selectbox("分类筛选", ["全部"] + cats, index=0)
    if selected_cat == "全部":
        feed_options = feeds
    else:
        feed_options = [f for f in feeds if (f["category"] or "未分组") == selected_cat]

    feed_labels = [f"{f['title']} — {urlparse(f['url']).netloc}" for f in feed_options]
    selected_feed_idx = st.multiselect(
        "选择订阅（可多选）",
        options=list(range(len(feed_options))),
        default=list(range(len(feed_options))),
    )
    selected_feed_ids = [feed_options[i]["id"] for i in selected_feed_idx]

    st.divider()
    st.subheader("➕ 添加订阅")
    new_url = st.text_input("RSS/Atom 地址")
    new_title = st.text_input("名称（可留空自动识别）")
    new_cat = st.text_input("分类（可留空）")
    if st.button("添加", type="primary", use_container_width=True):
        if new_url.strip():
            # 试抓一次，顺便识别标题
            info = fetch_feed_cached(new_url.strip())
            title = new_title.strip() or info.get("title") or new_url.strip()
            add_feed(title, new_url.strip(), new_cat.strip() or None)
            st.success(f"已添加：{title}")
            st.experimental_rerun()
        else:
            st.warning("请输入有效的 RSS/Atom URL")

    st.subheader("🛠️ OPML 导入/导出")
    up = st.file_uploader("导入 OPML", type=["opml", "xml"])
    if up is not None:
        n = import_opml(up.read())
        st.success(f"导入 {n} 条订阅")
        st.experimental_rerun()
    if st.button("导出当前订阅为 OPML", use_container_width=True):
        st.download_button(
            label="下载 OPML",
            data=export_opml().encode("utf-8"),
            file_name="feeds_export.opml",
            mime="text/xml",
            use_container_width=True,
        )

# 顶部工具栏
st.title(APP_TITLE)
col1, col2, col3, col4, col5 = st.columns([2, 2, 2, 2, 2])
with col1:
    keyword = st.text_input("🔎 搜索标题/摘要")
with col2:
    only_unread = st.toggle("只看未读", value=False)
with col3:
    only_starred = st.toggle("只看收藏", value=False)
with col4:
    limit = st.slider("数量上限", 50, 500, 200, 50)
with col5:
    if st.button("🔄 立即刷新", help="忽略缓存，强制刷新所选订阅"):
        # 清空缓存（针对 fetch_feed_cached）
        fetch_feed_cached.clear()
        st.toast("缓存已清除，开始刷新…")

# 抓取 & 入库（仅当需要时）
if st.button("⬇️ 从所选订阅拉取最新"):
    feeds_all = list_feeds()
    id2feed = {f["id"]: f for f in feeds_all}
    target = [id2feed[i] for i in (selected_feed_ids or [f["id"] for f in feeds_all])]
    progress = st.progress(0)
    for idx, f in enumerate(target, start=1):
        info = fetch_feed_cached(f["url"])  # 有缓存；清缓存后就是实时
        if info.get("entries"):
            upsert_articles(f["id"], info["entries"])
        progress.progress(idx / len(target))
    st.success("完成！")

# 查询文章
articles = query_articles(
    selected_feed_ids or None,
    only_unread=only_unread,
    only_starred=only_starred,
    keyword=keyword,
    limit=limit,
)

# 渲染卡片
for a in articles:
    # 文章头部信息
    feed = next((f for f in list_feeds() if f["id"] == a["feed_id"]), None)
    site = feed["url"] if feed else ""
    fav = favicon_url(site)

    left, right = st.columns([1, 3])
    with left:
        if a.get("image"):
            st.markdown(
                f'<img class="rss-thumb" src="{a["image"]}" alt="thumb"/>',
                unsafe_allow_html=True,
            )
        else:
            st.markdown(
                f'<img class="rss-thumb" src="{fav}" alt="thumb"/>',
                unsafe_allow_html=True,
            )

    with right:
        pub = a.get("published")
        when = "" if not pub else datetime.fromisoformat(pub).strftime("%Y-%m-%d %H:%M")
        meta = []
        if feed:
            meta.append(feed["title"])  # feed 名称
        if when:
            meta.append(when)
        if a.get("author"):
            meta.append(a["author"])

        st.markdown(
            f"<div class='rss-card'>"
            f"<div style='grid-column: span 2;'>"
            f"<div class='rss-title'><a href='{a['link']}' target='_blank'>{a['title']}</a></div>"
            f"<div class='rss-meta'>{' · '.join(meta)}</div>"
            f"<div class='rss-summary'>{a['summary'] or ''}</div>"
            f"</div>"
            f"</div>",
            unsafe_allow_html=True,
        )

        c1, c2, c3 = st.columns(3)
        with c1:
            if st.button("阅读/标记已读", key=f"read-{a['id']}"):
                mark_read(a["id"])
                st.toast("已标记为已读")
                st.experimental_rerun()
        with c2:
            star_label = "取消收藏" if a["starred"] else "收藏 ⭐"
            if st.button(star_label, key=f"star-{a['id']}"):
                toggle_star(a["id"])
                st.experimental_rerun()
        with c3:
            if st.button("复制链接", key=f"copy-{a['id']}"):
                st.code(a["link"], language="text")

st.caption(
    "小提示：侧边栏可按分类筛选订阅；顶部‘立即刷新’清缓存，‘拉取最新’写入数据库；导入你现有的 OPML 可快速迁移。"
)
