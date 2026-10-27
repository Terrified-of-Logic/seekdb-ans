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

#define USING_LOG_PREFIX SHARE

#include "share/ai_column/ob_ai_column_proxy.h"

#include "share/ai_column/ob_ai_column_define.h"
#include "share/inner_table/ob_inner_table_schema_constants.h"
#include "share/ob_dml_sql_splicer.h"
#include "share/ob_share_util.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_isql_client.h"
#include "common/mysqlclient/ob_isql_connection.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "common/object/ob_object.h"
#include "lib/time/ob_time_utility.h"
#include "lib/utility/ob_smart_var.h"
#include "lib/utility/utility.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace share
{

namespace
{

// The AI column inner tables live in the "oceanbase" database. Qualify
// them explicitly because SPI connections created from a user session run
// with the user database as the current database.
const char *const OB_AI_COLUMN_TABLE_FULL_NAME = "oceanbase.__all_ai_column";
const char *const OB_AI_COLUMN_TASK_TABLE_FULL_NAME = "oceanbase.__all_ai_column_task";

const char *const AI_COLUMN_SELECT_COLUMNS =
    "table_id, column_id, column_name, normalized_expr, max_retries, sync_mode, "
    "system_prompt, unfinished_count, state_column_id, request_column_id, "
    "result_spec_column_id, dep_columns, ai_model_name, user_expr";

const char *const AI_COLUMN_TASK_SELECT_COLUMNS =
    "table_id, column_id, request_id, rowkey_predicate, eligible_ts, state, "
    "retry_count, lease_owner, lease_expire_ts, error_msg";

} // anonymous namespace

// ------------------------------------------------------------------
// __all_ai_column
// ------------------------------------------------------------------

int ObAIColumnProxy::splice_ai_column_(ObDMLSqlSplicer &dml,
                                       const ObAIColumnSchema &ai_column)
{
  int ret = OB_SUCCESS;
  ObString sync_mode = ai_column.sync_mode_;
  if (OB_UNLIKELY(!ai_column.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid ai column schema", KR(ret), K(ai_column));
  } else if (sync_mode.empty()) {
    sync_mode = ObString::make_string(OB_AI_COLUMN_DEFAULT_SYNC_MODE);
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(dml.add_pk_column("table_id", ai_column.table_id_))) {
  } else if (OB_FAIL(dml.add_pk_column("column_id", ai_column.column_id_))) {
  } else if (OB_FAIL(dml.add_column("column_name",
                                    ObHexEscapeSqlStr(ai_column.column_name_)))) {
  } else if (OB_FAIL(dml.add_column("normalized_expr",
                                    ObHexEscapeSqlStr(ai_column.normalized_expr_)))) {
  } else if (OB_FAIL(dml.add_column("max_retries", ai_column.max_retries_))) {
  } else if (OB_FAIL(dml.add_column("sync_mode", ObHexEscapeSqlStr(sync_mode)))) {
  } else if (OB_FAIL(dml.add_column("unfinished_count", 0))) {
  } else if (OB_FAIL(dml.add_column("state_column_id", ai_column.state_column_id_))) {
  } else if (OB_FAIL(dml.add_column("request_column_id", ai_column.request_column_id_))) {
  } else if (OB_FAIL(dml.add_column("result_spec_column_id",
                                    ai_column.result_spec_column_id_))) {
  }
  // NOTE: the optional columns below MUST NOT be chained with `else if` on an
  // "is empty -> add NULL" branch: the first empty optional column would skip all
  // following ones in the batch insert column list.
  if (OB_SUCC(ret)) {
    if (ai_column.system_prompt_.empty()) {
      ret = dml.add_column(true /*is_null*/, "system_prompt");
    } else {
      ret = dml.add_column("system_prompt",
                           ObHexEscapeSqlStr(ai_column.system_prompt_));
    }
  }
  if (OB_SUCC(ret)) {
    if (ai_column.dep_columns_.empty()) {
      ret = dml.add_column(true /*is_null*/, "dep_columns");
    } else {
      ret = dml.add_column("dep_columns",
                           ObHexEscapeSqlStr(ai_column.dep_columns_));
    }
  }
  if (OB_SUCC(ret)) {
    if (ai_column.ai_model_name_.empty()) {
      ret = dml.add_column(true /*is_null*/, "ai_model_name");
    } else {
      ret = dml.add_column("ai_model_name",
                           ObHexEscapeSqlStr(ai_column.ai_model_name_));
    }
  }
  if (OB_SUCC(ret)) {
    if (ai_column.user_expr_.empty()) {
      ret = dml.add_column(true /*is_null*/, "user_expr");
    } else {
      ret = dml.add_column("user_expr",
                           ObHexEscapeSqlStr(ai_column.user_expr_));
    }
  }
  return ret;
}

int ObAIColumnProxy::insert_ai_column(ObMySQLTransaction &trans,
                                      const ObAIColumnSchema &ai_column)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObDMLSqlSplicer dml;
  ObSqlString sql;
  if (OB_FAIL(splice_ai_column_(dml, ai_column))) {
  } else if (OB_FAIL(dml.add_gmt_create())) {
  } else if (OB_FAIL(dml.add_gmt_modified())) {
  } else if (OB_FAIL(dml.splice_insert_sql(OB_AI_COLUMN_TABLE_FULL_NAME, sql))) {
    LOG_WARN("fail to splice insert sql", KR(ret));
  } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
    LOG_WARN("fail to write ai column metadata", KR(ret), K(sql));
  } else if (1 != affected_rows) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected affected rows", KR(ret), K(affected_rows), K(sql));
  }
  if (OB_ERR_PRIMARY_KEY_DUPLICATE == ret) {
    // Stable error code for the DDL caller when metadata already exists.
    ret = OB_ENTRY_EXIST;
    LOG_WARN("ai column metadata already exists", K(ai_column));
  }
  return ret;
}

int ObAIColumnProxy::insert_ai_columns(ObMySQLTransaction &trans,
                                       const ObIArray<ObAIColumnSchema> &ai_columns)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObDMLSqlSplicer dml;
  ObSqlString sql;
  if (ai_columns.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("empty ai column array", KR(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
    if (OB_FAIL(splice_ai_column_(dml, ai_columns.at(i)))) {
    } else if (OB_FAIL(dml.add_gmt_create())) {
    } else if (OB_FAIL(dml.add_gmt_modified())) {
    } else if (OB_FAIL(dml.finish_row())) {
      LOG_WARN("fail to finish row", KR(ret), K(i));
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(dml.splice_batch_insert_sql(OB_AI_COLUMN_TABLE_FULL_NAME, sql))) {
    LOG_WARN("fail to splice batch insert sql", KR(ret));
  } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
    LOG_WARN("fail to write ai column metadata", KR(ret), K(sql));
  } else if (affected_rows != ai_columns.count()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected affected rows", KR(ret), K(affected_rows),
             K(ai_columns.count()));
  }
  return ret;
}

int ObAIColumnProxy::insert_update_ai_column(ObMySQLTransaction &trans,
                                             const ObAIColumnSchema &ai_column)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObDMLSqlSplicer dml;
  ObSqlString sql;
  if (OB_FAIL(splice_ai_column_(dml, ai_column))) {
  } else if (OB_FAIL(dml.add_gmt_create())) {
  } else if (OB_FAIL(dml.add_gmt_modified())) {
  } else if (OB_FAIL(dml.splice_insert_update_sql(OB_AI_COLUMN_TABLE_FULL_NAME, sql))) {
    LOG_WARN("fail to splice insert update sql", KR(ret));
  } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
    LOG_WARN("fail to write ai column metadata", KR(ret), K(sql));
  }
  return ret;
}

int ObAIColumnProxy::delete_ai_column(ObMySQLTransaction &trans,
                                      const int64_t table_id,
                                      const int64_t column_id)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObSqlString sql;
  if (OB_UNLIKELY(table_id <= 0 || column_id <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(table_id), K(column_id));
  } else if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE table_id = %ld AND column_id = %ld",
                                    OB_AI_COLUMN_TABLE_FULL_NAME, table_id, column_id))) {
    LOG_WARN("fail to assign sql", KR(ret));
  } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
    LOG_WARN("fail to delete ai column metadata", KR(ret), K(sql));
  } else if (affected_rows > 1) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected affected rows", KR(ret), K(affected_rows), K(sql));
  }
  return ret;
}

int ObAIColumnProxy::delete_ai_columns_by_table(ObMySQLTransaction &trans,
                                                const int64_t table_id)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObSqlString sql;
  if (OB_UNLIKELY(table_id <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(table_id));
  } else if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE table_id = %ld",
                                    OB_AI_COLUMN_TABLE_FULL_NAME, table_id))) {
    LOG_WARN("fail to assign sql", KR(ret));
  } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
    LOG_WARN("fail to delete ai column metadata", KR(ret), K(sql));
  }
  return ret;
}

int ObAIColumnProxy::build_select_ai_columns_sql_(const int64_t table_id,
                                                  ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(table_id <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(table_id));
  } else if (OB_FAIL(sql.assign_fmt("SELECT %s FROM %s WHERE table_id = %ld",
                                    AI_COLUMN_SELECT_COLUMNS,
                                    OB_AI_COLUMN_TABLE_FULL_NAME, table_id))) {
    LOG_WARN("fail to assign sql", KR(ret));
  }
  return ret;
}

int ObAIColumnProxy::select_ai_columns_by_table(ObArenaAllocator &allocator,
                                                ObISQLClient &sql_proxy,
                                                const int64_t table_id,
                                                ObIArray<ObAIColumnSchema> &ai_columns)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(build_select_ai_columns_sql_(table_id, sql))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = nullptr;
      if (OB_FAIL(sql_proxy.read(res, sql.ptr()))) {
        LOG_WARN("fail to read ai column metadata", KR(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("null result", KR(ret), K(sql));
      } else {
        while (OB_SUCC(ret)) {
          ObAIColumnSchema ai_column;
          if (OB_FAIL(result->next())) {
            if (OB_ITER_END == ret) {
              ret = OB_SUCCESS;
              break;
            } else {
              LOG_WARN("fail to get next row", KR(ret), K(sql));
            }
          } else if (OB_FAIL(build_ai_column_(allocator, *result, ai_column))) {
          } else if (OB_FAIL(ai_columns.push_back(ai_column))) {
            LOG_WARN("fail to push back ai column", KR(ret));
          }
        }
      }
    }
  }
  return ret;
}

int ObAIColumnProxy::select_ai_column(ObArenaAllocator &allocator,
                                      ObISQLClient &sql_proxy,
                                      const int64_t table_id,
                                      const int64_t column_id,
                                      ObAIColumnSchema &ai_column)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ai_column.reset();
  if (OB_UNLIKELY(table_id <= 0 || column_id <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(table_id), K(column_id));
  } else if (OB_FAIL(sql.assign_fmt("SELECT %s FROM %s WHERE table_id = %ld AND column_id = %ld",
                                    AI_COLUMN_SELECT_COLUMNS,
                                    OB_AI_COLUMN_TABLE_FULL_NAME, table_id, column_id))) {
    LOG_WARN("fail to assign sql", KR(ret));
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = nullptr;
      if (OB_FAIL(sql_proxy.read(res, sql.ptr()))) {
        LOG_WARN("fail to read ai column metadata", KR(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("null result", KR(ret), K(sql));
      } else if (OB_FAIL(result->next())) {
        if (OB_ITER_END == ret) {
          ret = OB_ENTRY_NOT_EXIST;
        } else {
          LOG_WARN("fail to get next row", KR(ret), K(sql));
        }
      } else if (OB_FAIL(build_ai_column_(allocator, *result, ai_column))) {
      }
    }
  }
  return ret;
}

int ObAIColumnProxy::build_ai_column_(ObArenaAllocator &allocator,
                                      sqlclient::ObMySQLResult &result,
                                      ObAIColumnSchema &ai_column)
{
  int ret = OB_SUCCESS;
  ObString column_name;
  ObString normalized_expr;
  ObString sync_mode;
  ObString system_prompt;
  ObString dep_columns;
  ObString ai_model_name;
  ObString user_expr;
  ai_column.reset();
  EXTRACT_INT_FIELD_MYSQL_SKIP_RET(result, "table_id", ai_column.table_id_, int64_t);
  EXTRACT_INT_FIELD_MYSQL_SKIP_RET(result, "column_id", ai_column.column_id_, int64_t);
  EXTRACT_VARCHAR_FIELD_MYSQL_SKIP_RET(result, "column_name", column_name);
  EXTRACT_VARCHAR_FIELD_MYSQL_SKIP_RET(result, "normalized_expr", normalized_expr);
  EXTRACT_INT_FIELD_MYSQL_SKIP_RET(result, "max_retries", ai_column.max_retries_, int64_t);
  EXTRACT_VARCHAR_FIELD_MYSQL_SKIP_RET(result, "sync_mode", sync_mode);
  EXTRACT_VARCHAR_FIELD_MYSQL_SKIP_RET(result, "system_prompt", system_prompt);
  EXTRACT_VARCHAR_FIELD_MYSQL_WITH_DEFAULT_VALUE(result, "dep_columns", dep_columns,
                                                true /*skip_null_error*/,
                                                true /*skip_column_error*/,
                                                ObString());
  EXTRACT_VARCHAR_FIELD_MYSQL_WITH_DEFAULT_VALUE(result, "ai_model_name", ai_model_name,
                                                true /*skip_null_error*/,
                                                true /*skip_column_error*/,
                                                ObString());
  EXTRACT_VARCHAR_FIELD_MYSQL_WITH_DEFAULT_VALUE(result, "user_expr", user_expr,
                                                true /*skip_null_error*/,
                                                true /*skip_column_error*/,
                                                ObString());
  EXTRACT_INT_FIELD_MYSQL_SKIP_RET(result, "state_column_id",
                                   ai_column.state_column_id_, int64_t);
  EXTRACT_INT_FIELD_MYSQL_SKIP_RET(result, "request_column_id",
                                   ai_column.request_column_id_, int64_t);
  EXTRACT_INT_FIELD_MYSQL_SKIP_RET(result, "result_spec_column_id",
                                   ai_column.result_spec_column_id_, int64_t);
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(deep_copy_ob_string(allocator, column_name,
                                         ai_column.column_name_))) {
  } else if (OB_FAIL(deep_copy_ob_string(allocator, normalized_expr,
                                         ai_column.normalized_expr_))) {
  } else if (OB_FAIL(deep_copy_ob_string(allocator, sync_mode,
                                         ai_column.sync_mode_))) {
  } else if (OB_FAIL(deep_copy_ob_string(allocator, system_prompt,
                                         ai_column.system_prompt_))) {
  } else if (OB_FAIL(deep_copy_ob_string(allocator, dep_columns,
                                         ai_column.dep_columns_))) {
  } else if (OB_FAIL(deep_copy_ob_string(allocator, ai_model_name,
                                         ai_column.ai_model_name_))) {
  } else if (OB_FAIL(deep_copy_ob_string(allocator, user_expr,
                                         ai_column.user_expr_))) {
  }
  return ret;
}

// ------------------------------------------------------------------
// __all_ai_column_task
// ------------------------------------------------------------------

int ObAIColumnProxy::splice_ai_column_task_(ObDMLSqlSplicer &dml,
                                            const ObAIColumnTask &task)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!task.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid ai column task", KR(ret), K(task));
  } else if (OB_FAIL(dml.add_pk_column("table_id", task.table_id_))) {
  } else if (OB_FAIL(dml.add_pk_column("column_id", task.column_id_))) {
  } else if (OB_FAIL(dml.add_pk_column("rowkey_predicate",
                                       ObHexEscapeSqlStr(task.rowkey_predicate_)))) {
  } else if (OB_FAIL(dml.add_column("request_id", task.request_id_))) {
  } else if (OB_FAIL(dml.add_time_column("eligible_ts", task.eligible_ts_))) {
  } else if (OB_FAIL(dml.add_column("state", task.state_))) {
  } else if (OB_FAIL(dml.add_column("retry_count", task.retry_count_))) {
  } else if (OB_FAIL(dml.add_column("lease_owner", task.lease_owner_))) {
  } else if (task.lease_expire_ts_ > 0) {
    ret = dml.add_time_column("lease_expire_ts", task.lease_expire_ts_);
  } else {
    ret = dml.add_column(true /*is_null*/, "lease_expire_ts");
  }
  if (OB_FAIL(ret)) {
  } else if (task.error_msg_.empty()) {
    ret = dml.add_column(true /*is_null*/, "error_msg");
  } else {
    ret = dml.add_column("error_msg", ObHexEscapeSqlStr(task.error_msg_));
  }
  return ret;
}

int ObAIColumnProxy::insert_ai_column_task(ObMySQLTransaction &trans,
                                           const ObAIColumnTask &task)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObDMLSqlSplicer dml;
  ObSqlString sql;
  if (OB_FAIL(splice_ai_column_task_(dml, task))) {
  } else if (OB_FAIL(dml.add_gmt_create())) {
  } else if (OB_FAIL(dml.add_gmt_modified())) {
  } else if (OB_FAIL(dml.splice_insert_sql(OB_AI_COLUMN_TASK_TABLE_FULL_NAME, sql))) {
  } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
    LOG_WARN("fail to write ai column task", KR(ret), K(sql));
  } else if (1 != affected_rows) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected affected rows", KR(ret), K(affected_rows), K(sql));
  }
  if (OB_ERR_PRIMARY_KEY_DUPLICATE == ret) {
    ret = OB_ENTRY_EXIST;
    LOG_WARN("ai column task already exists", K(task));
  }
  return ret;
}

int ObAIColumnProxy::insert_ai_column_tasks(ObMySQLTransaction &trans,
                                            const ObIArray<ObAIColumnTask> &tasks)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObDMLSqlSplicer dml;
  ObSqlString sql;
  if (tasks.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("empty ai column task array", KR(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
    if (OB_FAIL(splice_ai_column_task_(dml, tasks.at(i)))) {
    } else if (OB_FAIL(dml.add_gmt_create())) {
    } else if (OB_FAIL(dml.add_gmt_modified())) {
    } else if (OB_FAIL(dml.finish_row())) {
      LOG_WARN("fail to finish row", KR(ret), K(i));
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(dml.splice_batch_insert_sql(OB_AI_COLUMN_TASK_TABLE_FULL_NAME, sql))) {
  } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
    LOG_WARN("fail to write ai column tasks", KR(ret), K(sql));
  } else if (affected_rows != tasks.count()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected affected rows", KR(ret), K(affected_rows), K(tasks.count()));
  }
  return ret;
}

int ObAIColumnProxy::insert_update_ai_column_task(ObMySQLTransaction &trans,
                                                  const ObAIColumnTask &task)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObDMLSqlSplicer dml;
  ObSqlString sql;
  if (OB_FAIL(splice_ai_column_task_(dml, task))) {
  } else if (OB_FAIL(dml.add_gmt_create())) {
  } else if (OB_FAIL(dml.add_gmt_modified())) {
  } else if (OB_FAIL(dml.splice_insert_update_sql(OB_AI_COLUMN_TASK_TABLE_FULL_NAME, sql))) {
  } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
    LOG_WARN("fail to write ai column task", KR(ret), K(sql));
  }
  return ret;
}

int ObAIColumnProxy::delete_ai_column_task(ObMySQLTransaction &trans,
                                           const int64_t table_id,
                                           const int64_t column_id,
                                           const ObString &rowkey_predicate)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObSqlString sql;
  if (OB_UNLIKELY(table_id <= 0 || column_id <= 0 || rowkey_predicate.empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(table_id), K(column_id),
             K(rowkey_predicate));
  } else if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE table_id = %ld AND column_id = %ld AND rowkey_predicate = ",
                                    OB_AI_COLUMN_TASK_TABLE_FULL_NAME, table_id, column_id))) {
  } else if (OB_FAIL(sql_append_hex_escape_str(rowkey_predicate, sql))) {
  } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
    LOG_WARN("fail to delete ai column task", KR(ret), K(sql));
  } else if (affected_rows > 1) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected affected rows", KR(ret), K(affected_rows), K(sql));
  }
  return ret;
}

int ObAIColumnProxy::delete_ai_column_tasks_by_row(ObMySQLTransaction &trans,
                                                   const int64_t table_id,
                                                   const ObString &rowkey_predicate)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObSqlString sql;
  if (OB_UNLIKELY(table_id <= 0 || rowkey_predicate.empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(table_id), K(rowkey_predicate));
  } else if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE table_id = %ld AND rowkey_predicate = ",
                                    OB_AI_COLUMN_TASK_TABLE_FULL_NAME, table_id))) {
  } else if (OB_FAIL(sql_append_hex_escape_str(rowkey_predicate, sql))) {
  } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
    LOG_WARN("fail to delete ai column tasks", KR(ret), K(sql));
  }
  return ret;
}

int ObAIColumnProxy::delete_ai_column_tasks_by_column(ObMySQLTransaction &trans,
                                                       const int64_t table_id,
                                                       const int64_t column_id)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObSqlString sql;
  if (OB_UNLIKELY(table_id <= 0 || column_id <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(table_id), K(column_id));
  } else if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE table_id = %ld AND column_id = %ld",
                                    OB_AI_COLUMN_TASK_TABLE_FULL_NAME, table_id, column_id))) {
  } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
    LOG_WARN("fail to delete ai column tasks by column", KR(ret), K(sql));
  }
  return ret;
}

int ObAIColumnProxy::delete_ai_column_tasks_by_table(ObMySQLTransaction &trans,
                                                     const int64_t table_id)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObSqlString sql;
  if (OB_UNLIKELY(table_id <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(table_id));
  } else if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE table_id = %ld",
                                    OB_AI_COLUMN_TASK_TABLE_FULL_NAME, table_id))) {
  } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
    LOG_WARN("fail to delete ai column tasks", KR(ret), K(sql));
  }
  return ret;
}

int ObAIColumnProxy::select_ai_column_tasks_by_table(ObArenaAllocator &allocator,
                                                     ObISQLClient &sql_proxy,
                                                     const int64_t table_id,
                                                     ObIArray<ObAIColumnTask> &tasks)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_UNLIKELY(table_id <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(table_id));
  } else if (OB_FAIL(sql.assign_fmt("SELECT %s FROM %s WHERE table_id = %ld",
                                    AI_COLUMN_TASK_SELECT_COLUMNS,
                                    OB_AI_COLUMN_TASK_TABLE_FULL_NAME, table_id))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = nullptr;
      if (OB_FAIL(sql_proxy.read(res, sql.ptr()))) {
        LOG_WARN("fail to read ai column tasks", KR(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("null result", KR(ret), K(sql));
      } else {
        while (OB_SUCC(ret)) {
          ObAIColumnTask task;
          if (OB_FAIL(result->next())) {
            if (OB_ITER_END == ret) {
              ret = OB_SUCCESS;
              break;
            } else {
              LOG_WARN("fail to get next row", KR(ret), K(sql));
            }
          } else if (OB_FAIL(build_ai_column_task_(allocator, *result, task))) {
          } else if (OB_FAIL(tasks.push_back(task))) {
          }
        }
      }
    }
  }
  return ret;
}

int ObAIColumnProxy::build_ai_column_task_(ObArenaAllocator &allocator,
                                           sqlclient::ObMySQLResult &result,
                                           ObAIColumnTask &task)
{
  int ret = OB_SUCCESS;
  ObString rowkey_predicate;
  ObString error_msg;
  task.reset();
  EXTRACT_INT_FIELD_MYSQL_SKIP_RET(result, "table_id", task.table_id_, int64_t);
  EXTRACT_INT_FIELD_MYSQL_SKIP_RET(result, "column_id", task.column_id_, int64_t);
  EXTRACT_INT_FIELD_MYSQL_SKIP_RET(result, "request_id", task.request_id_, int64_t);
  EXTRACT_VARCHAR_FIELD_MYSQL_SKIP_RET(result, "rowkey_predicate", rowkey_predicate);
  EXTRACT_TIMESTAMP_FIELD_MYSQL_SKIP_RET(result, "eligible_ts", task.eligible_ts_);
  EXTRACT_INT_FIELD_MYSQL_SKIP_RET(result, "state", task.state_, int64_t);
  EXTRACT_INT_FIELD_MYSQL_SKIP_RET(result, "retry_count", task.retry_count_, int64_t);
  EXTRACT_INT_FIELD_MYSQL_SKIP_RET(result, "lease_owner", task.lease_owner_, int64_t);
  EXTRACT_TIMESTAMP_FIELD_MYSQL_SKIP_RET(result, "lease_expire_ts",
                                         task.lease_expire_ts_);
  EXTRACT_VARCHAR_FIELD_MYSQL_SKIP_RET(result, "error_msg", error_msg);
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(deep_copy_ob_string(allocator, rowkey_predicate,
                                         task.rowkey_predicate_))) {
  } else if (OB_FAIL(deep_copy_ob_string(allocator, error_msg, task.error_msg_))) {
  }
  return ret;
}


// ------------------------------------------------------------------
// SPI variants (current session transaction; used by the DML handler)
// ------------------------------------------------------------------

int ObAIColumnProxy::build_batch_insert_tasks_sql_(
    const ObIArray<ObAIColumnTask> &tasks,
    ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  ObDMLSqlSplicer dml;
  if (tasks.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("empty ai column task array", KR(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
    if (OB_FAIL(splice_ai_column_task_(dml, tasks.at(i)))) {
    } else if (OB_FAIL(dml.add_gmt_create())) {
    } else if (OB_FAIL(dml.add_gmt_modified())) {
    } else if (OB_FAIL(dml.finish_row())) {
      LOG_WARN("fail to finish row", KR(ret), K(i));
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(dml.splice_batch_insert_sql(OB_AI_COLUMN_TASK_TABLE_FULL_NAME, sql))) {
    LOG_WARN("fail to splice batch insert sql", KR(ret));
  }
  return ret;
}

int ObAIColumnProxy::build_delete_tasks_by_row_sql_(
    const int64_t table_id,
    const ObString &rowkey_predicate,
    ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(table_id <= 0 || rowkey_predicate.empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(table_id), K(rowkey_predicate));
  } else if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE table_id = %ld AND rowkey_predicate = ",
                                    OB_AI_COLUMN_TASK_TABLE_FULL_NAME, table_id))) {
  } else if (OB_FAIL(sql_append_hex_escape_str(rowkey_predicate, sql))) {
  }
  return ret;
}

int ObAIColumnProxy::insert_ai_column_tasks(sqlclient::ObISQLConnection &conn,
                                            const ObIArray<ObAIColumnTask> &tasks)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObSqlString sql;
  LOG_INFO("[aicol] spi insert tasks begin", K(tasks.count()));
  if (OB_FAIL(build_batch_insert_tasks_sql_(tasks, sql))) {
    LOG_INFO("[aicol] spi build task sql failed", K(ret), K(tasks.count()));
  } else {
    LOG_INFO("[aicol] spi task sql", K(sql));
    if (OB_FAIL(conn.execute_write(sql.string(), affected_rows))) {
      // Known seekdb behavior when an inner SQL write is issued inside an outer DML
      // statement: the write is applied (affected_rows reflects it) but closing the
      // inner result returns OB_ERR_UNEXPECTED. Treat it as success when the write
      // itself is confirmed by affected_rows.
      if (OB_ERR_UNEXPECTED == ret && affected_rows == tasks.count()) {
        LOG_INFO("[aicol] tolerate inner result close error after task insert",
                 K(ret), K(affected_rows), K(tasks.count()));
        ret = OB_SUCCESS;
      } else {
        LOG_INFO("[aicol] spi execute_write failed", K(ret), K(affected_rows), K(sql));
      }
    } else if (affected_rows != tasks.count()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_INFO("[aicol] spi affected rows mismatch", K(ret), K(affected_rows),
               K(tasks.count()), K(sql));
    } else {
      LOG_INFO("[aicol] spi insert tasks done", K(affected_rows));
    }
  }
  return ret;
}

int ObAIColumnProxy::delete_ai_column_tasks_by_row(sqlclient::ObISQLConnection &conn,
                                                   const int64_t table_id,
                                                   const ObString &rowkey_predicate)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObSqlString sql;
  if (OB_FAIL(build_delete_tasks_by_row_sql_(table_id, rowkey_predicate, sql))) {
  } else if (OB_FAIL(conn.execute_write(sql.string(), affected_rows))) {
    if (OB_ERR_UNEXPECTED == ret) {
      LOG_INFO("[aicol] tolerate inner result close error after task delete",
               K(ret), K(affected_rows), K(sql));
      ret = OB_SUCCESS;
    } else {
      LOG_INFO("[aicol] spi delete tasks failed", K(ret), K(affected_rows), K(sql));
    }
  }
  return ret;
}

int ObAIColumnProxy::select_ai_columns_by_table(ObArenaAllocator &allocator,
                                                sqlclient::ObISQLConnection &conn,
                                                const int64_t table_id,
                                                ObIArray<ObAIColumnSchema> &ai_columns)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(build_select_ai_columns_sql_(table_id, sql))) {
  } else {
    SMART_VAR(ObISQLClient::ReadResult, res) {
      sqlclient::ObMySQLResult *result = nullptr;
      if (OB_FAIL(conn.execute_read(sql.string(), res))) {
        LOG_WARN("fail to read ai column metadata via spi", KR(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("null result", KR(ret), K(sql));
      } else {
        while (OB_SUCC(ret)) {
          ObAIColumnSchema ai_column;
          if (OB_FAIL(result->next())) {
            if (OB_ITER_END == ret) {
              ret = OB_SUCCESS;
              break;
            } else {
              LOG_WARN("fail to get next row", KR(ret), K(sql));
            }
          } else if (OB_FAIL(build_ai_column_(allocator, *result, ai_column))) {
          } else if (OB_FAIL(ai_columns.push_back(ai_column))) {
          }
        }
      }
    }
  }
  return ret;
}

} // namespace share
} // namespace oceanbase
