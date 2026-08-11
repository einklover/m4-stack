import os
import sys
import re
import random
from ebooklib import epub
from PIL import Image, ImageDraw, ImageFont

# 封面颜色方案（背景色，文字色）
COVER_COLOR_SCHEMES = [
    ('#2c3e50', '#ecf0f1'),  # 深蓝灰
    ('#34495e', '#bdc3c7'),  # 灰蓝色
    ('#16a085', '#ffffff'),  # 青绿色
    ('#27ae60', '#ffffff'),  # 绿色
    ('#2980b9', '#ffffff'),  # 蓝色
    ('#8e44ad', '#ffffff'),  # 紫色
    ('#c0392b', '#ffffff'),  # 红色
    ('#d35400', '#ffffff'),  # 橙色
    ('#7f8c8d', '#ffffff'),  # 灰色
    ('#1abc9c', '#ffffff'),  # 绿松石色
]

# 封面背景图案样式
COVER_PATTERNS = ['solid', 'gradient', 'stripes', 'dots']


def generate_random_cover(title, author, output_path, width=600, height=800):
    """
    生成随机风格的 EPUB 封面

    Args:
        title (str): 书籍标题
        author (str): 作者名
        output_path (str): 输出封面图片路径
        width (int): 封面宽度
        height (int): 封面高度
    """
    # 随机选择颜色方案
    bg_color, text_color = random.choice(COVER_COLOR_SCHEMES)
    pattern = random.choice(COVER_PATTERNS)
    
    # 解析颜色值为 RGB 元组
    def hex_to_rgb(hex_color):
        hex_color = hex_color.lstrip('#')
        return tuple(int(hex_color[i:i+2], 16) for i in (0, 2, 4))
    
    def rgba_to_hex(hex_color, alpha):
        """将十六进制颜色转换为带透明度的 RGBA"""
        rgb = hex_to_rgb(hex_color)
        return (*rgb, int(alpha * 255))

    # 创建图像
    img = Image.new('RGB', (width, height), color=bg_color)
    draw = ImageDraw.Draw(img)

    # 绘制背景图案
    if pattern == 'gradient':
        # 渐变效果
        bg_rgb = hex_to_rgb(bg_color)
        for y in range(height):
            # 创建从深到浅的渐变
            factor = y / height
            r = int(bg_rgb[0] * (1 - factor * 0.3))
            g = int(bg_rgb[1] * (1 - factor * 0.3))
            b = int(bg_rgb[2] * (1 - factor * 0.3))
            draw.line([(0, y), (width, y)], fill=(r, g, b))
    elif pattern == 'stripes':
        # 条纹效果
        stripe_height = random.randint(20, 50)
        text_rgb = hex_to_rgb(text_color)
        for y in range(0, height, stripe_height * 2):
            draw.rectangle([(0, y), (width, y + stripe_height)], fill=(*text_rgb, 64))
    elif pattern == 'dots':
        # 圆点效果
        dot_spacing = 30
        text_rgb = hex_to_rgb(text_color)
        for x in range(0, width, dot_spacing):
            for y in range(0, height, dot_spacing):
                if random.random() > 0.7:
                    draw.ellipse([(x-3, y-3), (x+3, y+3)], fill=(*text_rgb, 48))
    
    # 绘制标题（居中）
    title_y = height // 3
    
    # 尝试加载中文字体
    font_paths = [
        'C:/Windows/Fonts/simhei.ttf',      # 黑体
        'C:/Windows/Fonts/simsun.ttc',      # 宋体
        'C:/Windows/Fonts/msyh.ttc',        # 微软雅黑
        '/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc',  # 文泉驿
        '/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf',
    ]
    
    title_font = None
    author_font = None
    
    for font_path in font_paths:
        if os.path.exists(font_path):
            try:
                title_font = ImageFont.truetype(font_path, 48)
                author_font = ImageFont.truetype(font_path, 28)
                break
            except:
                continue
    
    # 如果没有中文字体，使用默认字体
    if title_font is None:
        title_font = ImageFont.load_default()
        author_font = ImageFont.load_default()
    
    # 计算标题位置（居中）
    title_bbox = draw.textbbox((0, 0), title, font=title_font)
    title_width = title_bbox[2] - title_bbox[0]
    title_x = (width - title_width) // 2

    # 绘制标题（带阴影效果）
    shadow_offset = 3
    draw.text((title_x + shadow_offset, title_y + shadow_offset), title, fill=(0, 0, 0, 128), font=title_font)
    text_rgb = hex_to_rgb(text_color)
    draw.text((title_x, title_y), title, fill=text_rgb, font=title_font)

    # 绘制作者名
    if author:
        author_bbox = draw.textbbox((0, 0), author, font=author_font)
        author_width = author_bbox[2] - author_bbox[0]
        author_x = (width - author_width) // 2
        author_y = title_y + 70
        draw.text((author_x, author_y), author, fill=(*text_rgb, 220), font=author_font)

    # 绘制底部装饰线
    line_height = height - 100
    draw.line([(50, line_height), (width - 50, line_height)], fill=(*text_rgb, 96), width=3)
    
    # 保存封面
    img.save(output_path, 'JPEG', quality=90)
    print(f"已生成封面：{os.path.basename(output_path)} (风格：{pattern}, 背景：{bg_color})")
    return output_path


def read_file_with_encoding(file_path):
    """
    尝试多种编码格式读取文件

    Args:
        file_path (str): 文件路径

    Returns:
        str: 文件内容
    """
    encodings = ['utf-8', 'gbk', 'gb2312', 'latin-1']

    for encoding in encodings:
        try:
            with open(file_path, 'r', encoding=encoding) as f:
                content = f.read()
            print(f"使用 {encoding} 编码成功读取文件")
            return content
        except UnicodeDecodeError:
            continue

    # 如果所有编码都失败，尝试二进制模式读取
    try:
        with open(file_path, 'rb') as f:
            raw_data = f.read()
        # 尝试解码为文本
        content = raw_data.decode('utf-8', errors='ignore')
        print("使用 utf-8 忽略错误模式读取文件")
        return content
    except Exception:
        raise Exception("无法使用任何编码格式读取文件")


def process_text_preserve_chapters(text):
    """
    处理文本，保留章节标题的独立段落
    替换中文引号：" → 「，" → 」

    Args:
        text (str): 原始文本

    Returns:
        str: 处理后的文本
    """
    # 替换中文双引号为方括号引号
    text = text.replace('"', '「')
    text = text.replace('"', '」')

    # 定义章节标题正则表达式模式
    # 注意：由于字符类 [] 对中文字符不工作，使用更宽松的模式
    strict_chapter_patterns = [
        r'^第.+[章节回部卷篇]',      # 第一章 开始、第 1 章 等
        r'^[章节回部卷篇].+',        # 章一、节二等
        r'^Chapter\s*\d+',          # Chapter 1
        r'^Chapter\s+[IVXLCDM]+',   # Chapter IV
        r'^楔子',                   # 楔子
        r'^尾声',                   # 尾声
        r'^序章',                   # 序章
        r'^终章',                   # 终章
        r'^番外.+',                 # 番外篇
        r'^尾声.+$',                # 尾声
    ]

    # 合并所有严格章节模式
    combined_pattern = '|'.join(f'({pattern})' for pattern in strict_chapter_patterns)

    # 按行分割文本
    lines = text.split('\n')
    result_lines = []

    i = 0
    while i < len(lines):
        line = lines[i].strip()

        if line:  # 跳过空行
            # 检查是否匹配章节标题模式
            if re.match(combined_pattern, line):
                # 这是章节标题行
                result_lines.append('')  # 确保章节标题前有空行
                result_lines.append(line)  # 保留完整章节标题
                result_lines.append('')  # 确保章节标题后有空行，标记章节标题刚被添加
            else:
                # 非章节标题行，清除内部空白字符
                cleaned_line = re.sub(r'\s+', '', line)
                if cleaned_line:
                    # 只有当上一行存在、不是空行、且不是章节标题时，才连接到前一行
                    if (result_lines and 
                        result_lines[-1] != '' and  # 上一行不是空行
                        not re.match(combined_pattern, result_lines[-1])):  # 上一行不是章节标题
                        result_lines[-1] += cleaned_line
                    else:
                        result_lines.append(cleaned_line)
        i += 1

    # 清理结果：移除多余的空行
    result = '\n'.join(result_lines)
    result = re.sub(r'\n{3,}', '\n\n', result)  # 最多保留两个连续换行符
    result = result.strip()

    return result


def txt_to_epub(txt_content, title, output_path, cover_path=None):
    """
    将 TXT 内容转换为 EPUB 文件

    Args:
        txt_content (str): 处理后的 TXT 内容
        title (str): 书籍标题
        output_path (str): 输出 EPUB 文件路径
        cover_path (str): 封面图片路径（可选）
    """
    # 创建 EPUB 书籍
    book = epub.EpubBook()

    # 设置元数据
    book.set_identifier(f'id-{os.path.basename(output_path)}')
    book.set_title(title)
    book.set_language('zh')
    book.add_author('TXT Converter')

    # 添加封面（如果有）
    if cover_path and os.path.exists(cover_path):
        with open(cover_path, 'rb') as f:
            cover_data = f.read()
        
        # set_cover 会自动创建 cover.xhtml，无需手动添加
        book.set_cover('cover.jpg', cover_data)

    # 按章节分割内容
    chapter_patterns = [
        r'^第[一二三四五六七八九十百千\d]+[章节回部卷篇].*$',
        r'^[章节回部卷篇][一二三四五六七八九十百千\d]+.*$',
        r'^Chapter\s*\d+.*$',
        r'^Chapter\s+[IVXLCDM]+.*$',
    ]
    combined_pattern = '|'.join(f'({pattern})' for pattern in chapter_patterns)

    # 分割章节
    lines = txt_content.split('\n')
    chapters = []
    current_chapter = {'title': '正文', 'content': []}

    for line in lines:
        if re.match(combined_pattern, line.strip()):
            # 保存当前章节
            if current_chapter['content']:
                chapters.append(current_chapter)
            # 开始新章节
            current_chapter = {'title': line.strip(), 'content': []}
        else:
            current_chapter['content'].append(line)

    # 添加最后一章
    if current_chapter['content']:
        chapters.append(current_chapter)

    # 创建 EPUB 章节
    epub_chapters = []
    toc = []

    for i, chapter in enumerate(chapters):
        chapter_title = chapter['title']
        chapter_content = '\n'.join(chapter['content'])

        # 将文本转换为 HTML 段落
        html_content = '<h2>' + chapter_title + '</h2>' if i > 0 or chapter_title != '正文' else ''

        # 按空行分割段落
        paragraphs = re.split(r'\n\n+', chapter_content)
        for para in paragraphs:
            para = para.strip()
            if para:
                # 替换特殊字符
                para = para.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
                html_content += f'<p>{para}</p>'

        # 跳过空内容章节
        if not html_content.strip():
            continue

        # 创建章节
        c = epub.EpubHtml(title=chapter_title, file_name=f'chap_{i+1:02d}.xhtml', lang='zh')
        c.content = html_content
        book.add_item(c)
        epub_chapters.append(c)
        toc.append(c)

    # 如果没有检测到章节，创建单一章节
    if not epub_chapters:
        c = epub.EpubHtml(title='正文', file_name='chap_01.xhtml', lang='zh')
        html_content = '<h2>正文</h2>'
        paragraphs = re.split(r'\n\n+', txt_content)
        for para in paragraphs:
            para = para.strip()
            if para:
                para = para.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
                html_content += f'<p>{para}</p>'
        c.content = html_content
        book.add_item(c)
        epub_chapters.append(c)
        toc.append(c)

    # 添加默认 CSS
    style = '''
        body { margin: 0.5em; text-align: justify; line-height: 1.5; }
        h2 { text-align: center; margin: 1em 0; }
        p { margin: 0.3em 0; text-indent: 2em; }
        p:first-of-type { text-indent: 0; }
    '''
    css = epub.EpubItem(uid="style", file_name="style/main.css", media_type="text/css", content=style)
    book.add_item(css)

    # 为所有章节添加 CSS
    for c in epub_chapters:
        c.add_item(css)

    # 设置目录和书脊
    book.toc = toc
    book.spine = ['nav'] + epub_chapters

    # 添加导航文件
    book.add_item(epub.EpubNcx())
    book.add_item(epub.EpubNav())

    # 写入 EPUB 文件
    epub.write_epub(output_path, book, {})
    print(f"已生成 EPUB: {os.path.basename(output_path)}")


def process_txt_files(directory_path):
    """
    处理指定目录下的所有 txt 文件，去除所有空白字符，转换为 EPUB 并生成随机封面

    Args:
        directory_path (str): 目录路径
    """
    # 检查目录是否存在
    if not os.path.exists(directory_path):
        print(f"错误：目录 '{directory_path}' 不存在")
        return

    # 获取目录下所有 txt 文件（排除已经处理过的_pure.txt 文件）
    txt_files = [f for f in os.listdir(directory_path)
                 if f.endswith('.txt')
                 and os.path.isfile(os.path.join(directory_path, f))]

    if not txt_files:
        print(f"目录 '{directory_path}' 中没有找到需要处理的 txt 文件")
        return

    print(f"找到 {len(txt_files)} 个需要处理的 txt 文件")

    # 创建 epub 输出子目录
    epub_output_dir = os.path.join(directory_path, 'epub')
    os.makedirs(epub_output_dir, exist_ok=True)
    print(f"EPUB 文件将保存到：{epub_output_dir}\n")

    # 处理每个 txt 文件
    for txt_file in txt_files:
        input_path = os.path.join(directory_path, txt_file)
        # 生成输出文件名：xxx_pure.txt
        name_without_ext = os.path.splitext(txt_file)[0]
        output_filename = f"{name_without_ext}_pure.txt"
        output_path = os.path.join(directory_path, output_filename)

        # EPUB 输出路径（保存到 epub 子目录）
        epub_filename = f"{name_without_ext}.epub"
        epub_path = os.path.join(epub_output_dir, epub_filename)

        # 封面输出路径（保存到 epub 子目录）
        cover_filename = f"{name_without_ext}_cover.jpg"
        cover_path = os.path.join(epub_output_dir, cover_filename)

        try:
            # 读取原文件内容（支持多种编码）
            content = read_file_with_encoding(input_path)

            # 处理文本，保留章节结构
            processed_content = process_text_preserve_chapters(content)

            # 写入 TXT 新文件
            with open(output_path, 'w', encoding='utf-8') as f:
                f.write(processed_content)

            print(f"已处理：{txt_file} -> {output_filename}")

            # 生成随机封面
            title = name_without_ext  # 使用文件名作为标题
            author = ""  # 可以后续从文件内容提取作者
            generate_random_cover(title, author, cover_path)

            # 转换为 EPUB（带封面）
            txt_to_epub(processed_content, title, epub_path, cover_path)

            print(f"✓ 完成：{txt_file} -> {epub_filename}\n")

        except Exception as e:
            print(f"处理文件 '{txt_file}' 时出错：{str(e)}")


def main():
    # 如果提供了命令行参数，使用参数作为目录路径
    if len(sys.argv) > 1:
        directory_path = sys.argv[1]
    else:
        # 否则使用当前目录
        directory_path = '.'

    print(f"正在处理目录：{os.path.abspath(directory_path)}")
    print("=" * 60)
    process_txt_files(directory_path)
    print("=" * 60)
    print("处理完成！")


if __name__ == "__main__":
    main()
