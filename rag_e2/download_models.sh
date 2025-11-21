#!/bin/bash

# 下载并保存模型到项目本地

echo "=== 开始下载模型 ==="

# 检查是否在 conda 环境中
if [ -z "$CONDA_DEFAULT_ENV" ]; then
    echo "⚠️  警告：未检测到 conda 环境，请先运行: conda activate rag"
    exit 1
fi

echo "当前 Python: $(which python)"
echo "当前环境: $CONDA_DEFAULT_ENV"

# 创建模型目录
mkdir -p models/bert-base-chinese
mkdir -p models/zh_core_web_sm

# 1. 下载 bert-base-chinese
echo "下载 bert-base-chinese..."
python << 'EOF'
from transformers import AutoTokenizer
import shutil

tokenizer = AutoTokenizer.from_pretrained("bert-base-chinese")
tokenizer.save_pretrained("./models/bert-base-chinese")
print("✅ bert-base-chinese 下载完成")
EOF

# 2. 下载 zh_core_web_sm
echo "下载 zh_core_web_sm..."
python -m spacy download zh_core_web_sm

# 复制 spaCy 模型到项目目录
python << 'EOF'
import spacy
import shutil
import os

# 加载模型获取路径
nlp = spacy.load("zh_core_web_sm")
model_path = nlp._path

# 复制到项目目录
target_path = "./models/zh_core_web_sm"
if os.path.exists(target_path):
    shutil.rmtree(target_path)
shutil.copytree(model_path, target_path)
print(f"✅ zh_core_web_sm 复制完成: {target_path}")
EOF

echo ""
echo "=== 下载完成 ==="
echo "模型已保存到:"
echo "  - ./models/bert-base-chinese (~110 KB)"
echo "  - ./models/zh_core_web_sm (~44 MB)"
echo ""
echo "总大小:"
du -sh ./models/
