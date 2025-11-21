import json
import os
import re
from pathlib import Path

def is_valid_entity(entity):
    # 1. 长度检查
    if len(entity) < 2:
        return False
    if len(entity) > 50:  # 太长通常是句子或乱码
        return False

    # 2. 噪声字符检查
    # 如果包含大量非中英文字符（如制表符、竖线）
    if re.search(r'[\|\uff5c\u2500-\u257f]', entity):  # 表格线
        return False
    
    # 3. 纯数字/百分比/年份排除
    if re.match(r'^[\d\.\%\s]+$', entity):  # 如 "60%", "2021"
        return False
    if re.match(r'^\d+：', entity):  # 如 "62：可比公司" (去掉序号)
        return False

    # 4. 特定停用词 (大小写不敏感)
    stopwords = {
        "text", "chart", "rate", "yoy", "capita", "wind", "source", "figure", 
        "table", "total", "note", "unit", "page", "end", "bar", "line",
        "图片", "图表", "数据来源", "资料来源", "页脚", "说明", "单位",
        "中游速冻食品规模化", "中游速冻食品标准化" # 一些奇怪的长短语
    }
    if entity.lower() in stopwords:
        return False
    
    # 5. 必须包含至少一个汉字或字母
    if not re.search(r'[\u4e00-\u9fa5a-zA-Z]', entity):
        return False

    return True

def clean_entity_name(entity):
    # 去除首尾空白和换行
    entity = entity.strip()
    # 去除前面的序号 (如 "62：可比公司" -> "可比公司")
    entity = re.sub(r'^\d+[：:]\s*', '', entity)
    return entity

def process_directory(cache_root):
    cache_path = Path(cache_root)
    for doc_dir in cache_path.iterdir():
        if not doc_dir.is_dir():
            continue
            
        index_file = doc_dir / "index.json"
        if not index_file.exists():
            continue

        print(f"Processing {doc_dir.name}...")
        
        with open(index_file, 'r', encoding='utf-8') as f:
            try:
                index_data = json.load(f)
            except json.JSONDecodeError:
                print(f"  Error decoding {index_file}")
                continue

        original_count = len(index_data)
        new_index = {}
        
        for entity, chunks in index_data.items():
            cleaned_entity = clean_entity_name(entity)
            
            if is_valid_entity(cleaned_entity):
                # 如果清洗后名字变了（如去掉了序号），合并chunk list
                if cleaned_entity in new_index:
                    new_index[cleaned_entity] = list(set(new_index[cleaned_entity] + chunks))
                else:
                    new_index[cleaned_entity] = chunks
        
        new_count = len(new_index)
        print(f"  Cleaned: {original_count} -> {new_count} entities (-{original_count - new_count})")

        # 备份原文件
        if not (doc_dir / "index.json.bak").exists():
            os.rename(index_file, doc_dir / "index.json.bak")
            
        # 写入新文件
        with open(index_file, 'w', encoding='utf-8') as f:
            json.dump(new_index, f, indent=4, ensure_ascii=False)

if __name__ == "__main__":
    cache_dir = "/home/tian/seekdb/rag_e2/cache/seekdb_competition"
    process_directory(cache_dir)
