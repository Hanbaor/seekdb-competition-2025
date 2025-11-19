-- =============================================================
-- 内核赛题：items1 表测试数据初始化脚本
-- 用途：配合 kernel_scalar_fulltext_tests.sql 进行功能/性能自测
-- 注意：执行前请确认 items1 表已创建且无关键业务数据
-- =============================================================

-- 可选：清空旧数据
TRUNCATE TABLE items1;

-- 批量插入覆盖不同 base_id、ID 范围及关键文本特征的数据
INSERT INTO items1 (id, base_id, docid_col, fulltext_col) VALUES
  -- 覆盖“供应链 优化”关键词，命中测试用例 1
  (1,   'base_id_1',  'DOC-OPT-001', '公司持续推进供应链 优化，结合冷链调度策略提高履约效率。'),
  (2,   'base_id_2',  'DOC-OPT-002', '供应链 优化 专家提出新的库存模型以支撑旺季生产。'),
  (3,   'base_id_3',  'DOC-OPT-003', '团队在供应链 优化 上引入 AI 预测，显著降低损耗。'),
  (150, 'base_id_4',  'DOC-RISK-150', '支付失败 风险控制 规则迭代，可精确识别高风险交易。'),
  (180, 'base_id_5',  'DOC-RISK-180', '风控中心监控支付失败 风险控制 指标并联动客服。'),
  (220, 'base_id_1',  'DOC-RISK-220', '支付失败 风险控制 场景下叠加时间窗口过滤。'),

  -- 覆盖大量 base_id_10~base_id_19，用于“RAG pipeline”场景
  (10010, 'base_id_10', 'DOC-RAG-10', 'RAG pipeline 调整检索阶段，加入多通道 rerank。'),
  (10011, 'base_id_11', 'DOC-RAG-11', '团队记录 RAG pipeline 和数据校准方案。'),
  (10012, 'base_id_12', 'DOC-RAG-12', 'RAG pipeline 在混合检索中结合 BM25。'),
  (10013, 'base_id_13', 'DOC-RAG-13', '研发总结 RAG pipeline 调优经验。'),
  (10014, 'base_id_14', 'DOC-RAG-14', 'RAG pipeline 需要向量召回与全文过滤协同。'),
  (10015, 'base_id_15', 'DOC-RAG-15', '基于 RAG pipeline 的指标持续跟踪。'),
  (10016, 'base_id_16', 'DOC-RAG-16', 'RAG pipeline 引入动态 rerank 策略。'),
  (10017, 'base_id_17', 'DOC-RAG-17', '讨论 RAG pipeline 的缓存策略。'),
  (10018, 'base_id_18', 'DOC-RAG-18', 'RAG pipeline 接入多语言模型。'),
  (10019, 'base_id_19', 'DOC-RAG-19', 'RAG pipeline 的系统压测总结。'),

  -- “多模态 检索”场景保留空结果：不插入 non_exist_* base_id，确保测试 4 无命中

  -- 高偏移分页 + VIP 用户工单场景
  (300100, 'base_id_20', 'DOC-VIP-01', 'VIP 用户 工单 需要一键升级并记录 SLA。'),
  (300150, 'base_id_20', 'DOC-VIP-02', 'VIP 用户 工单 包含向量与全文摘要。'),
  (300200, 'base_id_21', 'DOC-VIP-03', '客服处理 VIP 用户 工单 时需结合语义检索。'),
  (300250, 'base_id_21', 'DOC-VIP-04', 'VIP 用户 工单 拥有专属跟进流程。');

-- 可选：再次插入若干冗余文本，便于模拟 OFFSET 100 的场景
INSERT INTO items1 (id, base_id, docid_col, fulltext_col)
SELECT 300300 + seq AS id,
       CASE WHEN seq % 2 = 0 THEN 'base_id_20' ELSE 'base_id_21' END AS base_id,
       CONCAT('DOC-VIP-BULK-', seq) AS docid_col,
       CONCAT('批量填充：VIP 用户 工单 自动生成片段 #', seq) AS fulltext_col
FROM (
    SELECT 1 AS seq UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4 UNION ALL
    SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL
    SELECT 9 UNION ALL SELECT 10 UNION ALL SELECT 11 UNION ALL SELECT 12 UNION ALL
    SELECT 13 UNION ALL SELECT 14 UNION ALL SELECT 15 UNION ALL SELECT 16 UNION ALL
    SELECT 17 UNION ALL SELECT 18 UNION ALL SELECT 19 UNION ALL SELECT 20 UNION ALL
    SELECT 21 UNION ALL SELECT 22 UNION ALL SELECT 23 UNION ALL SELECT 24 UNION ALL
    SELECT 25 UNION ALL SELECT 26 UNION ALL SELECT 27 UNION ALL SELECT 28 UNION ALL
    SELECT 29 UNION ALL SELECT 30 UNION ALL SELECT 31 UNION ALL SELECT 32 UNION ALL
    SELECT 33 UNION ALL SELECT 34 UNION ALL SELECT 35 UNION ALL SELECT 36 UNION ALL
    SELECT 37 UNION ALL SELECT 38 UNION ALL SELECT 39 UNION ALL SELECT 40 UNION ALL
    SELECT 41 UNION ALL SELECT 42 UNION ALL SELECT 43 UNION ALL SELECT 44 UNION ALL
    SELECT 45 UNION ALL SELECT 46 UNION ALL SELECT 47 UNION ALL SELECT 48 UNION ALL
    SELECT 49 UNION ALL SELECT 50
) AS filler;
