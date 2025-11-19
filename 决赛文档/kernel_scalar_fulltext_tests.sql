-- =============================================================
-- 内核赛题：带标量过滤的全文检索测试 SQL 用例
-- 说明：覆盖不同标量过滤、排序、分页以及空结果等关键场景
-- 使用前确保已导入 MLDR 数据，并创建全文与标量索引。
-- =============================================================

-- 1. 基线混合查询：简单 IN + ID 上限过滤
SELECT docid_col,
       MATCH(fulltext_col) AGAINST ('供应链 优化' IN NATURAL LANGUAGE MODE) AS _score
FROM items1
WHERE MATCH(fulltext_col) AGAINST ('供应链 优化' IN NATURAL LANGUAGE MODE)
  AND base_id IN ('base_id_1', 'base_id_2', 'base_id_3')
  AND id < 2000
ORDER BY _score DESC
LIMIT 10;

-- 2. 扩大标量集合 + 精细排序 + 较小 LIMIT
SELECT docid_col,
       base_id,
       MATCH(fulltext_col) AGAINST ('支付失败 风险控制' IN BOOLEAN MODE) AS _score
FROM items1
WHERE MATCH(fulltext_col) AGAINST ('支付失败 风险控制' IN BOOLEAN MODE)
  AND base_id IN ('base_id_1','base_id_2','base_id_3','base_id_4','base_id_5')
  AND id BETWEEN 100 AND 500
ORDER BY _score DESC, id ASC
LIMIT 5;

-- 3. 大量 IN 值 + 高 LIMIT：测试位图展开与结果集压力
SELECT docid_col,
       base_id,
       MATCH(fulltext_col) AGAINST ('RAG pipeline' IN NATURAL LANGUAGE MODE) AS _score
FROM items1
WHERE MATCH(fulltext_col) AGAINST ('RAG pipeline' IN NATURAL LANGUAGE MODE)
  AND base_id IN (
      'base_id_10','base_id_11','base_id_12',
      'base_id_13','base_id_14','base_id_15',
      'base_id_16','base_id_17','base_id_18','base_id_19'
  )
  AND id < 100000
ORDER BY _score DESC
LIMIT 50;

-- 4. 空结果场景：验证无命中时的处理路径
SELECT docid_col,
       MATCH(fulltext_col) AGAINST ('多模态 检索' IN NATURAL LANGUAGE MODE) AS _score
FROM items1
WHERE MATCH(fulltext_col) AGAINST ('多模态 检索' IN NATURAL LANGUAGE MODE)
  AND base_id IN ('non_exist_1','non_exist_2')
  AND id < 10
ORDER BY _score DESC
LIMIT 10;

-- 5. 高偏移分页：考察排序稳定性与游标成本
SELECT docid_col,
       MATCH(fulltext_col) AGAINST ('VIP 用户 工单' IN NATURAL LANGUAGE MODE) AS _score
FROM items1
WHERE MATCH(fulltext_col) AGAINST ('VIP 用户 工单' IN NATURAL LANGUAGE MODE)
  AND base_id IN ('base_id_20','base_id_21')
  AND id < 500000
ORDER BY _score DESC
LIMIT 20 OFFSET 100;

-- 6. 预编译模板：便于脚本批量替换关键字/标量集合
PREPARE stmt FROM
'SELECT docid_col, MATCH(fulltext_col) AGAINST (? IN NATURAL LANGUAGE MODE) AS _score
  FROM items1
  WHERE MATCH(fulltext_col) AGAINST (? IN NATURAL LANGUAGE MODE)
    AND base_id IN (?, ?, ?)
    AND id < ?
  ORDER BY _score DESC
  LIMIT ?';
-- 示例执行：
-- EXECUTE stmt USING '关键词', '关键词', 'base_id_1','base_id_2','base_id_3', 10000, 10;
