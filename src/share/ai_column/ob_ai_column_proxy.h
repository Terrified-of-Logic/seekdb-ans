/**
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_PROXY_H_
#define OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_PROXY_H_

#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "lib/container/ob_iarray.h"
#include "share/ai_column/ob_ai_column_struct.h"

namespace oceanbase
{
namespace common
{
class ObArenaAllocator;
class ObISQLClient;
class ObMySQLTransaction;
class ObSqlString;
namespace sqlclient
{
class ObISQLConnection;
class ObMySQLResult;
}
}
namespace share
{

class ObDMLSqlSplicer;

// Read/write helper for the AI column inner tables (see contract.md §3.1/§3.2):
//   __all_ai_column       one row per AI column
//   __all_ai_column_task  one row per affected data row x AI column
//
// The write APIs take an ObMySQLTransaction so that DDL (metadata) and DML
// (task rows) can join the caller's transaction: AI column metadata / tasks must
// be committed or rolled back atomically with the statement that created them.
//
// The read APIs take an allocator that owns the memory of the string fields of
// the returned structs (same convention as ObAiServiceProxy::select_ai_endpoint).
class ObAIColumnProxy
{
public:
  ObAIColumnProxy() = default;
  ~ObAIColumnProxy() = default;

  // -------------------- __all_ai_column --------------------
  // Insert one metadata row in the caller's transaction. A duplicate primary key
  // is reported as OB_ENTRY_EXIST; otherwise affected_rows must be exactly 1.
  static int insert_ai_column(common::ObMySQLTransaction &trans,
                              const ObAIColumnSchema &ai_column);
  // Batch insert (one INSERT for all AI columns of one table).
  static int insert_ai_columns(common::ObMySQLTransaction &trans,
                               const common::ObIArray<ObAIColumnSchema> &ai_columns);
  // INSERT ... ON DUPLICATE KEY UPDATE (idempotent rewrite of the metadata row).
  static int insert_update_ai_column(common::ObMySQLTransaction &trans,
                                     const ObAIColumnSchema &ai_column);
  static int delete_ai_column(common::ObMySQLTransaction &trans,
                              const int64_t table_id,
                              const int64_t column_id);
  // Drop-table cleanup: delete all AI column metadata of one table.
  static int delete_ai_columns_by_table(common::ObMySQLTransaction &trans,
                                        const int64_t table_id);
  // Read APIs: string fields of returned structs are deep copied into allocator.
  static int select_ai_columns_by_table(common::ObArenaAllocator &allocator,
                                        common::ObISQLClient &sql_proxy,
                                        const int64_t table_id,
                                        common::ObIArray<ObAIColumnSchema> &ai_columns);
  static int select_ai_column(common::ObArenaAllocator &allocator,
                              common::ObISQLClient &sql_proxy,
                              const int64_t table_id,
                              const int64_t column_id,
                              ObAIColumnSchema &ai_column);

  // ------------------ __all_ai_column_task ------------------
  // NOTE: reserved for P2 (DML task generation) / P3 (worker); task rows must be
  // written in the DML transaction (contract.md §5.4).
  static int insert_ai_column_task(common::ObMySQLTransaction &trans,
                                   const ObAIColumnTask &task);
  static int insert_ai_column_tasks(common::ObMySQLTransaction &trans,
                                    const common::ObIArray<ObAIColumnTask> &tasks);
  static int insert_update_ai_column_task(common::ObMySQLTransaction &trans,
                                          const ObAIColumnTask &task);
  static int delete_ai_column_task(common::ObMySQLTransaction &trans,
                                   const int64_t table_id,
                                   const int64_t column_id,
                                   const common::ObString &rowkey_predicate);
  // Delete all task rows of one AI column (e.g. ALTER TABLE ... DROP COLUMN).
  static int delete_ai_column_tasks_by_column(common::ObMySQLTransaction &trans,
                                              const int64_t table_id,
                                              const int64_t column_id);
  // Delete all task rows of one data row (e.g. DELETE of the row / primary key change).
  static int delete_ai_column_tasks_by_row(common::ObMySQLTransaction &trans,
                                           const int64_t table_id,
                                           const common::ObString &rowkey_predicate);
  static int delete_ai_column_tasks_by_table(common::ObMySQLTransaction &trans,
                                             const int64_t table_id);
  static int select_ai_column_tasks_by_table(common::ObArenaAllocator &allocator,
                                             common::ObISQLClient &sql_proxy,
                                             const int64_t table_id,
                                             common::ObIArray<ObAIColumnTask> &tasks);

  // ---------------- SPI variants (current session transaction) ----------------
  // Used by the DML handler: task rows must live in the user's transaction so a
  // ROLLBACK revokes them (contract.md §5.4). `conn` is the session's
  // ObInnerSQLConnection (execute_write/execute_read), NOT a new transaction.
  static int insert_ai_column_tasks(common::sqlclient::ObISQLConnection &conn,
                                    const common::ObIArray<ObAIColumnTask> &tasks);
  static int delete_ai_column_tasks_by_row(common::sqlclient::ObISQLConnection &conn,
                                           const int64_t table_id,
                                           const common::ObString &rowkey_predicate);
  static int select_ai_columns_by_table(common::ObArenaAllocator &allocator,
                                        common::sqlclient::ObISQLConnection &conn,
                                        const int64_t table_id,
                                        common::ObIArray<ObAIColumnSchema> &ai_columns);

private:
  // SQL assembly helpers shared by the ObMySQLTransaction and SPI variants.
  static int build_select_ai_columns_sql_(const int64_t table_id,
                                          common::ObSqlString &sql);
  static int build_batch_insert_tasks_sql_(const common::ObIArray<ObAIColumnTask> &tasks,
                                           common::ObSqlString &sql);
  static int build_delete_tasks_by_row_sql_(const int64_t table_id,
                                            const common::ObString &rowkey_predicate,
                                            common::ObSqlString &sql);
  static int splice_ai_column_(ObDMLSqlSplicer &dml,
                               const ObAIColumnSchema &ai_column);
  static int build_ai_column_(common::ObArenaAllocator &allocator,
                              common::sqlclient::ObMySQLResult &result,
                              ObAIColumnSchema &ai_column);
  static int splice_ai_column_task_(ObDMLSqlSplicer &dml,
                                    const ObAIColumnTask &task);
  static int build_ai_column_task_(common::ObArenaAllocator &allocator,
                                   common::sqlclient::ObMySQLResult &result,
                                   ObAIColumnTask &task);
};

} // namespace share
} // namespace oceanbase

#endif // OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_PROXY_H_
