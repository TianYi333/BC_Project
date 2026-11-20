import docx
d = docx.Document(r"C:\Users\Lenovo\Documents\WXWork\1688855715881236\Cache\File\2026-10\控制节点固件升级系统设计说明文档V2.1.docx")
print("=== total paragraphs:", len(d.paragraphs), "tables:", len(d.tables))
keys = ["upgrade", "upgrade_ack", "upgrade_status", "upgrade_result",
        "文件名", "文件大小", "crc", "md5", "tftp", "下发", "json",
        "状态", "错误码", "进度", "校验", "报文"]
buf = []
for i, p in enumerate(d.paragraphs):
    t = p.text.strip()
    if not t:
        continue
    if any(k.lower() in t.lower() for k in keys):
        style = p.style.name if p.style else ""
        buf.append((i, style, t))
print("=== matched paragraphs:", len(buf))
for i, style, t in buf[:160]:
    print(f"[{i}|{style}] {t}")
