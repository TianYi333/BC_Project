import docx, sys, re
p = r"C:\Users\Lenovo\Documents\WXWork\1688855715881236\Cache\File\2026-10\控制节点固件升级系统设计说明文档V2.1.docx"
d = docx.Document(p)
print("=== headings ===")
for para in d.paragraphs:
    if para.style.name.startswith('Heading') and para.text.strip():
        print(para.style.name, "::", para.text.strip())
print("\n=== protocol keywords context ===")
kw = re.compile(r'upgrade|upgrade_ack|upgrade_status|upgrade_result|MD5|CRC|filename|文件(名|大小|长度)|tftp_server|tftp_port|校验|状态|错误码', re.I)
body = []
for para in d.paragraphs:
    t = para.text.strip()
    if t and kw.search(t):
        body.append(t)
# also scan tables
for ti, table in enumerate(d.tables):
    for row in table.rows:
        cells = [c.text.strip() for c in row.cells]
        line = " | ".join(cells)
        if line and kw.search(line):
            body.append("[TABLE%d] %s" % (ti, line))
seen=set()
for t in body:
    if t not in seen:
        seen.add(t)
        print(t)
PYEOF_MARKER = True
