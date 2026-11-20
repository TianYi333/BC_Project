# -*- coding: utf-8 -*-
"""将 OTA 说明 HTML 转换为真正的 .docx（保留标题/表格/代码块/列表/引用）。"""
import sys
from html.parser import HTMLParser
from docx import Document
from docx.shared import Pt, RGBColor, Inches
from docx.oxml.ns import qn
from docx.oxml import OxmlElement

SRC = "d:/Desktop/H743_VS_GCC_IDE/output/OTA升级协议与流程说明.html"
OUT = "d:/Desktop/H743_VS_GCC_IDE/output/OTA升级协议与流程说明.docx"

CODE_COLOR = RGBColor(0xB4, 0x30, 0x3C)
MUTED_COLOR = RGBColor(0x52, 0x60, 0x6D)
BRAND_FILL = "0B6E4F"


def shade_para(p, fill):
    pPr = p._p.get_or_add_pPr()
    shd = OxmlElement('w:shd')
    shd.set(qn('w:val'), 'clear')
    shd.set(qn('w:color'), 'auto')
    shd.set(qn('w:fill'), fill)
    pPr.append(shd)


def shade_cell(cell, fill):
    tcPr = cell._tc.get_or_add_tcPr()
    shd = OxmlElement('w:shd')
    shd.set(qn('w:val'), 'clear')
    shd.set(qn('w:color'), 'auto')
    shd.set(qn('w:fill'), fill)
    tcPr.append(shd)


class Converter(HTMLParser):
    def __init__(self, doc):
        super().__init__(convert_charrefs=True)
        self.doc = doc
        self.pending = []          # 当前块（段落级）的 run 缓冲
        self.fmt_stack = [{'bold': False, 'italic': False, 'code': False}]
        self.block = None          # 当前块名
        self.list_stack = []       # ul / ol 嵌套
        self.in_table = False
        self.tbl = []
        self.tbl_row = None
        self.in_cell = False
        self.cell_runs = []
        self.cell_is_header = False

    def eff(self):
        b = i = c = False
        for f in self.fmt_stack:
            b = b or f['bold']
            i = i or f['italic']
            c = c or f['code']
        return {'bold': b, 'italic': i, 'code': c}

    def add_text(self, text, br=False):
        if self.in_cell:
            self.cell_runs.append((text, self.eff(), br))
        else:
            self.pending.append((text, self.eff(), br))

    # ---- 元素进入 ----
    def handle_starttag(self, tag, attrs):
        d = dict(attrs)
        cls = d.get('class', '')
        if tag in ('strong', 'b'):
            top = self.fmt_stack[-1]
            self.fmt_stack.append({'bold': True, 'italic': top['italic'], 'code': top['code']})
        elif tag in ('em', 'i'):
            top = self.fmt_stack[-1]
            self.fmt_stack.append({'bold': top['bold'], 'italic': True, 'code': top['code']})
        elif tag == 'code':
            top = self.fmt_stack[-1]
            self.fmt_stack.append({'bold': top['bold'], 'italic': top['italic'], 'code': True})
        elif tag in ('h1', 'h2', 'h3'):
            self.block = tag
        elif tag == 'p':
            self.block = 'p'
        elif tag == 'li':
            self.block = 'li'
        elif tag == 'blockquote':
            self.block = 'blockquote'
        elif tag == 'pre':
            self.block = 'pre'
        elif tag == 'br':
            self.add_text('', br=True)
        elif tag in ('ul', 'ol'):
            self.list_stack.append(tag)
        elif tag == 'table':
            self.in_table = True
            self.tbl = []
        elif tag == 'tr':
            self.tbl_row = []
        elif tag in ('td', 'th'):
            self.in_cell = True
            self.cell_runs = []
            self.cell_is_header = (tag == 'th')
        elif tag == 'div':
            if 'meta' in cls:
                self.block = 'meta'
            elif 'note' in cls:
                self.block = 'note'
        elif tag == 'footer':
            self.block = 'footer'
        # header 及其他未知标签：忽略（继续解析子元素）

    # ---- 文本 ----
    def handle_data(self, data):
        if self.in_cell:
            if data.strip() != '' or self.cell_runs:
                self.add_text(data)
            return
        if self.block is None:
            return
        if self.block == 'pre':
            self.add_text(data)
            return
        if data.strip() == '':
            return
        self.add_text(data)

    # ---- 元素结束 ----
    def handle_endtag(self, tag):
        if tag in ('strong', 'b', 'em', 'i', 'code'):
            if len(self.fmt_stack) > 1:
                self.fmt_stack.pop()
            return
        if tag in ('h1', 'h2', 'h3', 'p', 'li', 'blockquote', 'pre'):
            self.flush_block(self.block)
            self.block = None
            return
        if tag in ('meta', 'note', 'footer'):
            self.flush_block(self.block)
            self.block = None
            return
        if tag in ('ul', 'ol'):
            if self.list_stack:
                self.list_stack.pop()
            return
        if tag in ('td', 'th'):
            if self.in_table:
                self.tbl_row.append((self.cell_runs, self.cell_is_header))
            self.in_cell = False
            return
        if tag == 'tr':
            if self.in_table and self.tbl_row is not None:
                self.tbl.append(self.tbl_row)
            self.tbl_row = None
            return
        if tag == 'table':
            self.build_table()
            self.in_table = False
            return
        if tag == 'div':
            if self.block in ('meta', 'note'):
                self.flush_block(self.block)
                self.block = None
            return
        # header 等：忽略
        return

    # ---- 生成 run ----
    def add_runs_to(self, p, items, mono=False):
        for (text, fmt, br) in items:
            if br:
                p.add_run('').add_break()
                continue
            lines = text.split('\n')
            for k, line in enumerate(lines):
                if k > 0:
                    p.add_run('').add_break()
                if line == '':
                    continue
                run = p.add_run(line)
                run.bold = fmt['bold']
                run.italic = fmt['italic']
                if mono or fmt['code']:
                    run.font.name = 'Consolas'
                    run.font.size = Pt(9.5)
                    if fmt['code'] and not mono:
                        run.font.color.rgb = CODE_COLOR

    def flush_block(self, block):
        if block == 'h1':
            p = self.doc.add_heading(level=1)
        elif block == 'h2':
            p = self.doc.add_heading(level=2)
        elif block == 'h3':
            p = self.doc.add_heading(level=3)
        elif block == 'li':
            st = 'List Bullet' if (self.list_stack and self.list_stack[-1] == 'ul') else 'List Number'
            p = self.doc.add_paragraph(style=st)
        elif block == 'blockquote':
            p = self.doc.add_paragraph()
            p.paragraph_format.left_indent = Inches(0.3)
            shade_para(p, 'F0F6FF')
        elif block == 'note':
            p = self.doc.add_paragraph()
            shade_para(p, 'FFF8E6')
        elif block == 'pre':
            p = self.doc.add_paragraph()
            shade_para(p, 'F5F7FA')
            p.paragraph_format.left_indent = Inches(0.1)
            self.add_runs_to(p, self.pending, mono=True)
            self.pending = []
            return
        elif block == 'meta':
            p = self.doc.add_paragraph()
        elif block == 'footer':
            p = self.doc.add_paragraph()
        else:
            p = self.doc.add_paragraph()
        self.add_runs_to(p, self.pending)
        if block in ('meta', 'footer'):
            for r in p.runs:
                r.font.size = Pt(9)
                r.font.color.rgb = MUTED_COLOR
        self.pending = []

    def build_table(self):
        if not self.tbl:
            return
        nrows = len(self.tbl)
        ncols = max(len(r) for r in self.tbl)
        t = self.doc.add_table(rows=nrows, cols=ncols)
        t.style = 'Table Grid'
        for ri, row in enumerate(self.tbl):
            for ci, (runs, is_header) in enumerate(row):
                cell = t.cell(ri, ci)
                cell.paragraphs[0].text = ''
                self.add_runs_to(cell.paragraphs[0], runs)
                if is_header:
                    for r in cell.paragraphs[0].runs:
                        r.bold = True
                        r.font.color.rgb = RGBColor(0xFF, 0xFF, 0xFF)
                    shade_cell(cell, BRAND_FILL)
        self.tbl = []


def main():
    doc = Document()
    normal = doc.styles['Normal']
    normal.font.name = 'Microsoft YaHei'
    normal.font.size = Pt(10.5)
    rpr = normal.element.get_or_add_rPr()
    rfonts = rpr.get_or_add_rFonts()
    rfonts.set(qn('w:eastAsia'), 'Microsoft YaHei')

    cp = doc.core_properties
    cp.title = 'OTA 升级协议与流程说明文档'
    cp.author = 'H743_VS_GCC_IDE'

    parser = Converter(doc)
    with open(SRC, encoding='utf-8') as f:
        parser.feed(f.read())
    doc.save(OUT)
    print("SAVED:", OUT)


if __name__ == '__main__':
    main()
