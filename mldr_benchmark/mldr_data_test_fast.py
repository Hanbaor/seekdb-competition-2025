#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MLDR 数据集快速测试版本
跳过数据插入和预热阶段，只进行1轮测试以快速查看QPS
"""

import os
import sys
import time
import logging
import tempfile
import platform
import subprocess
import io
import re
from contextlib import redirect_stdout
from typing import Tuple, Optional

# 导入其他模块的功能
from test_insert_fast import test_simple_insert
from get_search_rrf_oceanbase import OceanBaseClientForSearch, ModelArgs
from evaluate_results_oceanbase import evaluate, check_qrels_files, map_metric
from pyserini.util import download_evaluation_script

# 设置日志
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)


class MLDRDataTestFast:
    """MLDR 数据集快速测试类 - 跳过数据插入和预热，只运行1轮测试"""
    
    def __init__(self, 
                 mldr_data_test_dir: str,
                 lang: str = 'en',
                 query_types: str = 'bm25',
                 db_port: int = 2881,
                 query_result_dir: Optional[str] = None):
        """
        初始化 MLDR 测试
        
        Args:
            mldr_data_test_dir: mldr-data-test 目录路径
            lang: 测试语言，默认 'en'
            query_types: 查询类型，默认 'bm25'
            db_port: 数据库端口，默认 2881
            query_result_dir: 查询结果保存目录，如果为 None 则使用临时目录
        """
        self.mldr_data_test_dir = mldr_data_test_dir
        self.lang = lang
        self.query_types = query_types
        self.db_port = db_port
        self.query_result_dir = query_result_dir or os.path.join(tempfile.gettempdir(), 'mldr_query_results')
        
        # 确保结果目录存在
        os.makedirs(self.query_result_dir, exist_ok=True)
        
        # 测试结果
        self.recall_at_10: float = 0.0
        self.qps: float = 0.0
        self.total_queries: int = 0
        self.total_time: float = 0.0
        
        logger.info(f'MLDRDataTestFast initialized: lang={lang}, query_types={query_types}, db_port={db_port}')
        logger.info(f'Query result directory: {self.query_result_dir}')

    def step1_insert_data(self) -> bool:
        """
        步骤1: 插入数据并创建索引，与完整版脚本保持一致，确保 HashMap/索引数据可用
        """
        logger.info(f"开始插入数据并创建索引: lang={self.lang}")
        test_simple_insert(self.lang)
        logger.info("✓ Data insertion and index creation complete (fast mode)")
        return True

    def step3_evaluate(self) -> float:
        """
        步骤3: 评估搜索结果
        
        Returns:
            float: recall@10 值
        """
        logger.info("=" * 60)
        logger.info("step3: Evaluate search results")
        logger.info("=" * 60)
        
        try:
            # 检查qrels文件
            qrels_dir = './qrels'
            if not check_qrels_files(qrels_dir, [self.lang]):
                error_msg = f"step3 evaluate failed: qrels file not found for lang={self.lang}"
                logger.error(error_msg)
                raise RuntimeError(error_msg)
            
            # 下载TREC评估工具
            logger.info("下载TREC评估工具...")
            script_path = download_evaluation_script('trec_eval')
            logger.info(f"TREC评估工具路径: {script_path}")
            
            # 评估结果文件
            query_result_path = os.path.join(self.query_result_dir, f"{self.lang}_{self.query_types}.txt")
            if not os.path.exists(query_result_path):
                error_msg = f"step3 evaluate failed: result file not found: {query_result_path}"
                logger.error(error_msg)
                raise RuntimeError(error_msg)
            
            qrels_path = os.path.join(qrels_dir, f"qrels.mldr-v1.0-{self.lang}-test-filtered.tsv")
            metrics = ['recall@10']
            
            # 执行评估
            result = evaluate(script_path, qrels_path, query_result_path, metrics)
            
            # 提取 recall@10
            recall_at_10 = result.get('recall@10', 0.0)
            if isinstance(recall_at_10, str):
                # 如果解析失败，尝试从字符串中提取
                import re
                match = re.search(r'(\d+\.\d+)', recall_at_10)
                if match:
                    recall_at_10 = float(match.group(1))
                else:
                    recall_at_10 = 0.0
            
            if recall_at_10 == 0.0:
                error_msg = "step3 evaluate failed: cannot extract recall@10 from result"
                logger.error(error_msg)
                logger.error(f"Result: {result}")
                raise RuntimeError(error_msg)
            
            logger.info(f"✓ Evaluation complete: recall@10={recall_at_10:.4f}")
            return recall_at_10
            
        except Exception as e:
            error_msg = f"step3 evaluate failed: {str(e)}"
            logger.error(error_msg)
            raise RuntimeError(error_msg) from e
    
    def run_fast_test(self) -> Tuple[float, float]:
        """
        运行快速测试流程（跳过数据插入和预热，只运行1轮测试）
        
        Returns:
            Tuple[float, float]: (recall@10, QPS)
        """
        logger.info("=" * 60)
        logger.info("Starting MLDR fast test (1 run, no data insertion, no warmup)")
        logger.info("=" * 60)
        
        try:
            # 先执行索引创建，避免 observer 重启后 HashMap 为空
            self.step1_insert_data()

            # 直接进行正式测试：只执行1次搜索和评估
            logger.info("=" * 60)
            logger.info("Running 1 search and evaluation")
            logger.info("=" * 60)
            
            recall_threshold = 0.95
            
            # 清理之前的结果文件
            expected_file = os.path.join(self.query_result_dir, f"{self.lang}_{self.query_types}.txt")
            if os.path.exists(expected_file):
                try:
                    os.remove(expected_file)
                    logger.info(f"Cleaned previous result file: {expected_file}")
                except Exception as e:
                    logger.warning(f"Failed to clean result file: {e}")
            
            # 执行搜索
            logger.info(f"开始搜索: lang={self.lang}, query_types={self.query_types}")
            
            # 创建搜索客户端
            oceanbase_client = OceanBaseClientForSearch(with_colbert=False)
            
            # 创建模型参数
            model_args = ModelArgs()
            
            # 执行搜索（捕获输出以提取查询时间）
            query_types_list = [self.query_types] if isinstance(self.query_types, str) else self.query_types
            output_buffer = io.StringIO()
            
            with redirect_stdout(output_buffer):
                oceanbase_client.main(
                    languages=[self.lang],
                    query_types=query_types_list,
                    model_args=model_args,
                    save_dir=self.query_result_dir
                )
            
            # 获取输出内容
            search_output = output_buffer.getvalue()
            # 同时打印到日志
            logger.info("Search output:")
            for line in search_output.split('\n'):
                if line.strip():
                    logger.info(line)
            
            # 关闭连接
            if oceanbase_client.connection:
                oceanbase_client.connection.close()
            
            # 释放客户端对象
            del oceanbase_client
            import gc
            gc.collect()
            
            # 从输出中提取平均查询时间和查询总数
            avg_query_time_ms = 0.0
            total_queries = 0
            
            # 提取平均查询时间
            avg_time_pattern = r'平均查询时间[：:]\s*(\d+\.?\d*)\s*ms'
            match = re.search(avg_time_pattern, search_output)
            if match:
                avg_query_time_ms = float(match.group(1))
                logger.info(f'Extracted average query time: {avg_query_time_ms} ms')
            
            # 提取查询总数
            query_total_pattern = r'查询总数[：:]\s*(\d+)'
            match = re.search(query_total_pattern, search_output)
            if match:
                total_queries = int(match.group(1))
                logger.info(f'Extracted total queries: {total_queries}')
            
            # 如果无法从输出中提取，从结果文件中统计查询数
            if total_queries == 0:
                expected_file = os.path.join(self.query_result_dir, f"{self.lang}_{self.query_types}.txt")
                if os.path.exists(expected_file):
                    try:
                        with open(expected_file, 'r') as f:
                            lines = f.readlines()
                            # TREC 格式，每行一个查询结果，通过查询ID判断查询数
                            query_ids = set()
                            for line in lines:
                                parts = line.strip().split()
                                if len(parts) >= 1:
                                    query_ids.add(parts[0])
                            total_queries = len(query_ids)
                            logger.info(f'Found total queries from result file: {total_queries}')
                    except Exception as e:
                        logger.warning(f'Failed to count queries from result file: {e}')
            
            # 计算 QPS：QPS = 1000 / 平均查询时间(ms)
            if avg_query_time_ms > 0:
                qps = 1000.0 / avg_query_time_ms
                logger.info(f"Search QPS: {qps:.2f} (1000 / {avg_query_time_ms:.2f}ms)")
            else:
                qps = 0.0
                logger.warning(f"Cannot calculate QPS: average query time not found in output")
            
            # 执行评估
            recall_at_10 = self.step3_evaluate()
            logger.info(f"Evaluation recall@10: {recall_at_10:.4f}")
            
            # 保存结果
            self.recall_at_10 = recall_at_10
            self.qps = qps
            
            # 输出结果
            logger.info("=" * 60)
            logger.info("MLDR fast test complete")
            logger.info(f"recall@10: {recall_at_10:.4f}")
            logger.info(f"QPS: {qps:.2f}")
            
            if recall_at_10 >= recall_threshold:
                logger.info(f"✓ recall@10={recall_at_10:.4f} >= {recall_threshold} threshold")
            else:
                logger.warning(f"✗ recall@10={recall_at_10:.4f} < {recall_threshold} threshold")
            
            logger.info("=" * 60)
            
            return recall_at_10, qps
            
        except Exception as e:
            logger.error(f"MLDR fast test failed: {e}")
            import traceback
            logger.error(traceback.format_exc())
            raise


if __name__ == "__main__":
    import argparse
    
    parser = argparse.ArgumentParser(description='MLDR数据集快速测试程序（跳过数据插入和预热，只运行1轮）')
    parser.add_argument('--lang', type=str, default='en', 
                       help='测试语言，默认 en')
    parser.add_argument('--query_types', type=str, default='bm25',
                       help='查询类型，默认 bm25')
    parser.add_argument('--db_port', type=int, default=2881,
                       help='数据库端口，默认 2881')
    parser.add_argument('--query_result_dir', type=str, default=None,
                       help='查询结果保存目录，如果为 None 则使用临时目录')
    
    args = parser.parse_args()
    
    # 获取当前脚本所在目录作为 mldr_data_test_dir
    mldr_data_test_dir = os.path.dirname(os.path.abspath(__file__))
    
    # 创建测试实例
    test = MLDRDataTestFast(
        mldr_data_test_dir=mldr_data_test_dir,
        lang=args.lang,
        query_types=args.query_types,
        db_port=args.db_port,
        query_result_dir=args.query_result_dir
    )
    
    # 运行快速测试
    try:
        recall, qps = test.run_fast_test()
        print(f"\n{'='*60}")
        print(f"最终结果:")
        print(f"  recall@10: {recall:.4f}")
        print(f"  QPS: {qps:.2f}")
        print(f"{'='*60}")
    except Exception as e:
        logger.error(f"测试失败: {e}")
        import traceback
        logger.error(traceback.format_exc())
        sys.exit(1)
