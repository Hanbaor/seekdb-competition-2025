#!/usr/bin/env bash
set -euo pipefail

HOST=${HOST:-127.0.0.1}
PORT=${PORT:-2881}
USER=${USER:-root}
DB=${DB:-test}
PASSWORD=${PASSWORD:-''}
TABLE_NAME=${TABLE_NAME:-items_debug_ft}

OBCLIENT="obclient -h${HOST} -P${PORT} -u${USER} -D${DB} -A"
if [[ -n ${PASSWORD} && ${PASSWORD} != "''" ]]; then
  OBCLIENT+=" -p${PASSWORD}"
fi

read -r -d '' SQL <<SQL
DROP TABLE IF EXISTS ${TABLE_NAME};
CREATE TABLE ${TABLE_NAME} (
  id BIGINT AUTO_INCREMENT PRIMARY KEY,
  base_id VARCHAR(255) NOT NULL,
  docid_col VARCHAR(255) NOT NULL,
  fulltext_col LONGTEXT
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT INTO ${TABLE_NAME} (base_id, docid_col, fulltext_col) VALUES
('base_id_1', 'doc_1', 'OceanBase FTS cache smoke test entry one'),
('base_id_2', 'doc_2', 'OceanBase FTS cache smoke test entry two'),
('base_id_3', 'doc_3', 'OceanBase cache lookup doc three'),
('base_id_1', 'doc_4', 'Another entry mentioning cache hit and miss'),
('base_id_2', 'doc_5', 'Fulltext search demo row five'),
('base_id_3', 'doc_6', 'Shielding logic verification record');

CREATE FULLTEXT INDEX ft_debug_fulltext ON ${TABLE_NAME}(fulltext_col);

SELECT docid_col,
       MATCH(fulltext_col) AGAINST('cache hit cache miss' IN NATURAL LANGUAGE MODE) AS _score
FROM ${TABLE_NAME}
WHERE MATCH(fulltext_col) AGAINST('cache hit cache miss' IN NATURAL LANGUAGE MODE)
  AND base_id IN ('base_id_1','base_id_2','base_id_3')
  AND id < 1000
ORDER BY _score DESC
LIMIT 10;
SQL

echo "Running mini FTS scenario on ${TABLE_NAME}..."
echo "-------------------------"
eval "${OBCLIENT} -e \"${SQL//$/\\$}\""
echo "-------------------------"
echo "Done. Check observer.log for FUNC DATA / FTS_CACHE_TRACE entries."
