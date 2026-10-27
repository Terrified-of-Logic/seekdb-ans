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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/dml/ob_ai_column_dml_handler.h"

#include <atomic>
#include "lib/time/ob_time_utility.h"
#include "share/ai_column/ob_ai_column_define.h"
#include "share/ob_dml_sql_splicer.h"
#include "share/ai_column/ob_ai_column_proxy.h"
#include "share/ai_column/ob_ai_column_struct.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "sql/engine/dml/ob_dml_ctx_define.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/ob_sql_context.h"
#include "sql/session/ob_inner_sql_connection.h"
#include "sql/session/ob_sql_session_info.h"
#include "common/mysqlclient/ob_isql_connection.h"
#include "query/session/ob_inner_sql_connection_access.h"

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace share::schema;

namespace sql
{

namespace
{

const int64_t AI_COLUMN_VALUE_BUF_LEN = 512;

static thread_local int64_t g_ai_column_dml_depth = 0;

class ObAIColumnDMLRecursionGuard
{
public:
  ObAIColumnDMLRecursionGuard() { ++g_ai_column_dml_depth; }
  ~ObAIColumnDMLRecursionGuard() { --g_ai_column_dml_depth; }
};

inline bool in_ai_column_dml()
{
  return g_ai_column_dml_depth > 0;
}

// Runs the AI column maintenance SQL in the current session transaction through
// the DML operator's inner connection. The nested session saves/restores the
// session runtime state, exactly like foreign key checks do.
class ObAIColumnRequestCtx
{
public:
  ObAIColumnRequestCtx()
    : op_(NULL), exec_ctx_(NULL), conn_(NULL), nested_begun_(false)
  { }
  ~ObAIColumnRequestCtx()
  {
    if (nested_begun_ && OB_NOT_NULL(op_)) {
      const int tmp_ret = op_->end_nested_session();
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN_RET(tmp_ret, "failed to end ai column nested session");
      }
    }
  }
  int begin(ObDMLModifyRowNode &modify_row)
  {
    int ret = OB_SUCCESS;
    if (OB_ISNULL(op_ = modify_row.dml_op_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_ISNULL(exec_ctx_ = &op_->get_exec_ctx())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(op_->ensure_inner_conn())) {
    } else if (OB_FAIL(op_->begin_nested_session(false /*skip_cur_stmt_tables*/))) {
    } else if (OB_ISNULL(conn_ = op_->get_inner_conn())) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      nested_begun_ = true;
    }
    return ret;
  }
  ObExecContext &exec_ctx() { return *exec_ctx_; }
  sqlclient::ObISQLConnection &conn() { return *conn_; }
  bool has_conn() const { return OB_NOT_NULL(conn_); }

private:
  ObTableModifyOp *op_;
  ObExecContext *exec_ctx_;
  sqlclient::ObISQLConnection *conn_;
  bool nested_begun_;
};

bool is_ai_hidden_column_name(const ObString &name)
{
  const char *prefixes[3] = {OB_AI_COLUMN_STATE_PREFIX,
                             OB_AI_COLUMN_REQUEST_PREFIX,
                             OB_AI_COLUMN_RESULT_SPEC_PREFIX};
  bool match = false;
  for (int64_t i = 0; !match && i < 3; ++i) {
    const int64_t len = static_cast<int64_t>(strlen(prefixes[i]));
    match = (name.length() >= len && 0 == strncmp(name.ptr(), prefixes[i], len));
  }
  return match;
}

int load_ai_columns(ObExecContext &exec_ctx,
                    const ObTableSchema &table_schema,
                    ObDMLBaseRtDef &dml_rtdef,
                    sqlclient::ObISQLConnection &conn)
{
  int ret = OB_SUCCESS;
  if (dml_rtdef.ai_columns_loaded_) {
    // already loaded for this statement
  } else {
    bool has_ai_column = false;
    for (int64_t i = 0; !has_ai_column && i < table_schema.get_column_count(); ++i) {
      const ObColumnSchemaV2 *column = table_schema.get_column_schema_by_idx(i);
      if (OB_NOT_NULL(column) && column->is_hidden()
          && is_ai_hidden_column_name(column->get_column_name_str())) {
        has_ai_column = true;
      }
    }
    if (has_ai_column) {
      common::ObSEArray<ObAIColumnSchema, 4> ai_columns;
      if (OB_FAIL(ObAIColumnProxy::select_ai_columns_by_table(
                   dml_rtdef.ai_column_allocator_, conn,
                   static_cast<int64_t>(table_schema.get_table_id()), ai_columns))) {
        LOG_WARN("failed to load ai column metadata", K(ret),
                 K(table_schema.get_table_id()));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
          if (OB_FAIL(dml_rtdef.ai_columns_.push_back(ai_columns.at(i)))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      dml_rtdef.ai_columns_loaded_ = true;
    }
  }
  return ret;
}

// Predicate text used by the task table primary key: "`id` = 2".
int build_rowkey_predicate(const ObTableSchema &table_schema,
                           const ObChunkDatumStore::StoredRow &row,
                           const UIntFixedArray &column_ids,
                           ObIAllocator &allocator,
                           ObString &predicate)
{
  int ret = OB_SUCCESS;
  const ObRowkeyInfo &rowkey_info = table_schema.get_rowkey_info();
  ObSqlString predicate_sql;
  for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_info.get_size(); ++i) {
    ObRowkeyColumn rowkey_column;
    const ObColumnSchemaV2 *column_schema = NULL;
    int64_t row_idx = -1;
    if (OB_FAIL(rowkey_info.get_column(i, rowkey_column))) {
    } else if (OB_ISNULL(column_schema =
                           table_schema.get_column_schema(rowkey_column.column_id_))) {
      ret = OB_ERR_UNEXPECTED;
    }
    for (int64_t j = 0; OB_SUCC(ret) && -1 == row_idx && j < column_ids.count(); ++j) {
      if (column_ids.at(j) == rowkey_column.column_id_) {
        row_idx = j;
      }
    }
    if (OB_SUCC(ret) && (row_idx < 0 || row_idx >= row.cnt_)) {
      ret = OB_ERR_UNEXPECTED;
    }
    if (OB_SUCC(ret)) {
      ObObj value;
      char buf[AI_COLUMN_VALUE_BUF_LEN];
      int64_t pos = 0;
      if (OB_FAIL(row.cells()[row_idx].to_obj(value, column_schema->get_meta_type()))) {
      } else if (OB_FAIL(value.print_sql_literal(buf, sizeof(buf), pos))) {
      } else {
        const ObString &column_name = column_schema->get_column_name_str();
        if (OB_FAIL(predicate_sql.append_fmt("%s`%.*s` = %.*s",
                                             (0 == i ? "" : " and "),
                                             column_name.length(), column_name.ptr(),
                                             static_cast<int32_t>(pos), buf))) {
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    ret = ob_write_string(allocator, predicate_sql.string(), predicate);
  }
  return ret;
}

// Parse the JSON array text of dependency column names: ["content","title"].
int parse_dep_columns(const ObString &dep_columns,
                      common::ObIArray<ObString> &column_names)
{
  int ret = OB_SUCCESS;
  const char *ptr = dep_columns.ptr();
  const int64_t len = dep_columns.length();
  int64_t i = 0;
  while (OB_SUCC(ret) && i < len) {
    if ('"' != ptr[i]) {
      i++;
    } else {
      int64_t begin = ++i;
      while (i < len && '"' != ptr[i]) {
        i++;
      }
      if (OB_UNLIKELY(i >= len)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(column_names.push_back(
                   ObString(static_cast<int32_t>(i - begin), ptr + begin)))) {
      } else {
        i++;
      }
    }
  }
  return ret;
}

int find_column_index(const UIntFixedArray &column_ids,
                      const int64_t column_id,
                      int64_t &index)
{
  int ret = OB_SUCCESS;
  index = -1;
  for (int64_t i = 0; OB_SUCC(ret) && -1 == index && i < column_ids.count(); ++i) {
    if (column_ids.at(i) == static_cast<uint64_t>(column_id)) {
      index = i;
    }
  }
  return ret;
}

int get_db_table_name(const ObTableSchema &table_schema,
                      ObExecContext &exec_ctx,
                      common::ObSqlString &db_table_name)
{
  int ret = OB_SUCCESS;
  const ObDatabaseSchema *db_schema = NULL;
  ObSqlCtx *sql_ctx = NULL;
  ObSchemaGetterGuard *schema_guard = NULL;
  if (OB_ISNULL(sql_ctx = exec_ctx.get_sql_ctx())
      || OB_ISNULL(schema_guard = sql_ctx->schema_guard_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(schema_guard->get_database_schema(
                       static_cast<uint64_t>(table_schema.get_database_id()), db_schema))) {
  } else if (OB_ISNULL(db_schema)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(db_table_name.assign_fmt("%.*s.%.*s",
               db_schema->get_database_name_str().length(),
               db_schema->get_database_name_str().ptr(),
               table_schema.get_table_name_str().length(),
               table_schema.get_table_name_str().ptr()))) {
  }
  return ret;
}

int execute_write_sql(sqlclient::ObISQLConnection &conn, const ObString &sql)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  if (sql.empty()) {
    // nothing to do
  } else if (OB_FAIL(conn.execute_write(sql.ptr(), affected_rows))) {
    // The inner SQL execution may report the nested "result close" error after the
    // statement has already been executed successfully (known inner SQL behavior).
    if (OB_ERR_UNEXPECTED == ret && affected_rows >= 0) {
      ret = OB_SUCCESS;
    } else {
      LOG_WARN("failed to execute ai column dml sql", K(ret), K(affected_rows), K(sql));
    }
  }
  return ret;
}

// Reset one visible result column to NULL for the given row (same transaction).
int reset_result_column(sqlclient::ObISQLConnection &conn,
                        ObExecContext &exec_ctx,
                        const ObTableSchema &table_schema,
                        const ObString &result_column_name,
                        const ObString &rowkey_predicate)
{
  int ret = OB_SUCCESS;
  ObSqlString db_table;
  ObSqlString sql;
  if (OB_FAIL(get_db_table_name(table_schema, exec_ctx, db_table))) {
  } else if (OB_FAIL(sql.append_fmt("UPDATE %s SET `%.*s` = NULL WHERE %.*s",
                                    db_table.ptr(),
                                    result_column_name.length(), result_column_name.ptr(),
                                    rowkey_predicate.length(), rowkey_predicate.ptr()))) {
  } else if (OB_FAIL(execute_write_sql(conn, sql.string()))) {
  }
  return ret;
}

uint64_t alloc_request_id()
{
  static std::atomic<uint64_t> seq(0);
  const uint64_t s = static_cast<uint64_t>(seq.fetch_add(1));
  return (static_cast<uint64_t>(ObTimeUtility::current_time()) << 8) | (s & 0xFFULL);
}

int build_tasks_for_row(const int64_t table_id,
                        const ObIArray<ObAIColumnSchema> &ai_columns,
                        const ObString &rowkey_predicate,
                        const int64_t now,
                        common::ObIArray<ObAIColumnTask> &tasks)
{
  int ret = OB_SUCCESS;
  const uint64_t request_id = alloc_request_id();
  for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
    const ObAIColumnSchema &ai_column = ai_columns.at(i);
    ObAIColumnTask task;
    task.table_id_ = table_id;
    task.column_id_ = ai_column.column_id_;
    task.request_id_ = static_cast<int64_t>(request_id);
    task.rowkey_predicate_ = rowkey_predicate;
    task.eligible_ts_ = now;
    task.state_ = static_cast<int64_t>(ObAIColumnTaskState::PENDING);
    task.retry_count_ = 0;
    task.lease_owner_ = 0;
    task.lease_expire_ts_ = 0;
    if (OB_FAIL(tasks.push_back(task))) {
    }
  }
  return ret;
}

// Replace all task rows of one data row: delete then batch insert (upsert).
int upsert_tasks_for_row(sqlclient::ObISQLConnection &conn,
                         const int64_t table_id,
                         const ObString &rowkey_predicate,
                         const ObIArray<ObAIColumnTask> &tasks)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObAIColumnProxy::delete_ai_column_tasks_by_row(
               conn, table_id, rowkey_predicate))) {
  } else if (OB_FAIL(ObAIColumnProxy::insert_ai_column_tasks(conn, tasks))) {
    LOG_WARN("failed to insert ai column tasks", K(ret), K(tasks.count()));
  }
  return ret;
}

} // namespace

int ObAIColumnDMLHandler::handle_after_row(ObDMLModifyRowNode &modify_row)
{
  int ret = OB_SUCCESS;
  if (in_ai_column_dml()) {
    // nested SQL of the AI column maintenance
  } else if (OB_ISNULL(modify_row.dml_op_) || OB_ISNULL(modify_row.dml_ctdef_)
             || OB_ISNULL(modify_row.dml_rtdef_)) {
    // not a normal DML row
  } else if (!modify_row.dml_ctdef_->is_primary_index_) {
    // only maintain on the primary table (not on index tables)
  } else {
    ObExecContext &filter_ctx = modify_row.dml_op_->get_exec_ctx();
    ObSQLSessionInfo *filter_session = filter_ctx.get_my_session();
    if (OB_NOT_NULL(filter_session)
        && filter_session->is_inner()
        && !filter_session->is_user_session()) {
      // Worker / internal PL SPI writes must not create tasks or invalidate results.
    } else {
    ObAIColumnDMLRecursionGuard guard;
    switch (modify_row.dml_event_) {
      case ObDmlEventType::DE_INSERTING:
        ret = handle_insert(modify_row);
        break;
      case ObDmlEventType::DE_UPDATING:
        ret = handle_update(modify_row);
        break;
      case ObDmlEventType::DE_DELETING:
        ret = handle_delete(modify_row);
        break;
      default:
        break;
    }
    }
  }
  return ret;
}

int ObAIColumnDMLHandler::handle_insert(ObDMLModifyRowNode &modify_row)
{
  int ret = OB_SUCCESS;
  const ObDMLBaseCtDef &ctdef = *modify_row.dml_ctdef_;
  ObDMLBaseRtDef &rtdef = *modify_row.dml_rtdef_;
  const int64_t table_id = static_cast<int64_t>(ctdef.das_base_ctdef_.index_tid_);
  ObAIColumnRequestCtx req_ctx;
  const ObTableSchema *table_schema = NULL;
  const ObChunkDatumStore::StoredRow *row = NULL;
  ret = req_ctx.begin(modify_row);
  if (OB_SUCC(ret)) {
    ret = get_table_schema(req_ctx.exec_ctx(), table_id, table_schema);
  }
  if (OB_SUCC(ret) && OB_ISNULL(table_schema)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_SUCC(ret) && OB_FAIL(load_ai_columns(req_ctx.exec_ctx(), *table_schema, rtdef,
                                    req_ctx.conn()))) {
  } else if (OB_SUCC(ret) && rtdef.ai_columns_.empty()) {
  } else if (OB_SUCC(ret) && OB_ISNULL(row = (OB_NOT_NULL(modify_row.new_row_)
                                ? modify_row.new_row_ : modify_row.full_row_))) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_SUCC(ret)) {
    ObString rowkey_predicate;
    common::ObSEArray<ObAIColumnTask, 4> tasks;
    const int64_t now = ObTimeUtility::current_time();
    if (OB_FAIL(build_rowkey_predicate(*table_schema, *row, ctdef.column_ids_,
                                       req_ctx.exec_ctx().get_allocator(),
                                       rowkey_predicate))) {
    } else if (OB_FAIL(build_tasks_for_row(table_id, rtdef.ai_columns_,
                                           rowkey_predicate, now, tasks))) {
    } else {
      ret = ObAIColumnProxy::insert_ai_column_tasks(req_ctx.conn(), tasks);
      if (OB_FAIL(ret)) {
      if (OB_ERR_PRIMARY_KEY_DUPLICATE == ret) {
        // a primary key move fires both delete(old) and insert(new) row events,
        // and both may create the tasks of the new row: keep the first one.
        LOG_INFO("ai column tasks already exist for this row", K(table_id),
                 K(rowkey_predicate));
        ret = OB_SUCCESS;
      }
      }
    }
  }
  return ret;
}

int ObAIColumnDMLHandler::handle_update(ObDMLModifyRowNode &modify_row)
{
  int ret = OB_SUCCESS;
  const ObDMLBaseCtDef &ctdef = *modify_row.dml_ctdef_;
  ObDMLBaseRtDef &rtdef = *modify_row.dml_rtdef_;
  const int64_t table_id = static_cast<int64_t>(ctdef.das_base_ctdef_.index_tid_);
  ObAIColumnRequestCtx req_ctx;
  const ObTableSchema *table_schema = NULL;
  const ObChunkDatumStore::StoredRow *old_row = modify_row.old_row_;
  const ObChunkDatumStore::StoredRow *new_row =
      (NULL != modify_row.full_row_) ? modify_row.full_row_ : modify_row.new_row_;
  ObSQLSessionInfo *session = NULL;
  if (OB_FAIL(req_ctx.begin(modify_row))) {
  } else if (OB_ISNULL(session = req_ctx.exec_ctx().get_my_session())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (session->is_inner() && !session->is_user_session()) {
    // writes issued by the AI column worker itself must not create new tasks
  } else if (OB_FAIL(get_table_schema(req_ctx.exec_ctx(), table_id, table_schema))) {
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(load_ai_columns(req_ctx.exec_ctx(), *table_schema, rtdef,
                                    req_ctx.conn()))) {
  } else if (rtdef.ai_columns_.empty()) {
    // ordinary table
  } else if (OB_ISNULL(new_row)) {
    // no new row image at all, nothing to maintain (e.g. worker side internal update)
  } else {
    // Which row image does this event carry?
    const bool full_row_only = (NULL == modify_row.old_row_) && (NULL == modify_row.new_row_);
    const UIntFixedArray &base_column_ids = ctdef.column_ids_;
    const UIntFixedArray &das_column_ids =
        static_cast<const ObDASUpdCtDef &>(ctdef.das_base_ctdef_).column_ids_;
    const UIntFixedArray &row_column_ids =
        full_row_only ? das_column_ids : base_column_ids;
    // updated column ids of this update statement
    const UIntFixedArray &updated_column_ids =
        static_cast<const ObUpdCtDef &>(ctdef).dupd_ctdef_.updated_column_ids_;
    {
      ObSqlString upd_ids;
      for (int64_t i = 0; i < updated_column_ids.count(); ++i) {
        upd_ids.append_fmt("%s%lu", 0 == i ? "" : ",",
                           updated_column_ids.at(i));
      }
    }
    // dependency column ids
    bool dep_changed = false;
    bool pk_changed = false;
    const ObRowkeyInfo &rowkey_info = table_schema->get_rowkey_info();
    for (int64_t i = 0; !pk_changed && i < rowkey_info.get_size(); ++i) {
      ObRowkeyColumn rowkey_column;
      if (OB_SUCC(rowkey_info.get_column(i, rowkey_column))) {
        for (int64_t j = 0; !pk_changed && j < updated_column_ids.count(); ++j) {
          pk_changed = (updated_column_ids.at(j) == rowkey_column.column_id_);
        }
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && !dep_changed && i < rtdef.ai_columns_.count(); ++i) {
      common::ObSEArray<ObString, 8> dep_columns;
      if (OB_FAIL(parse_dep_columns(rtdef.ai_columns_.at(i).dep_columns_, dep_columns))) {
      }
      for (int64_t j = 0; OB_SUCC(ret) && !dep_changed && j < dep_columns.count(); ++j) {
        const ObColumnSchemaV2 *dep_column =
            table_schema->get_column_schema(dep_columns.at(j));
        if (OB_ISNULL(dep_column)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          for (int64_t k = 0; !dep_changed && k < updated_column_ids.count(); ++k) {
            dep_changed = (updated_column_ids.at(k) == dep_column->get_column_id());
          }
        }
      }
    }
    if (OB_SUCC(ret) && (dep_changed || pk_changed)) {
      ObString old_predicate;
      ObString new_predicate;
      ObIAllocator &allocator = req_ctx.exec_ctx().get_allocator();
      if (OB_FAIL(build_rowkey_predicate(*table_schema, *new_row, row_column_ids,
                                         allocator, new_predicate))) {
      } else if (OB_NOT_NULL(old_row)
                 && OB_FAIL(build_rowkey_predicate(*table_schema, *old_row,
                                                   base_column_ids,
                                                   allocator, old_predicate))) {
      } else if (old_predicate.empty()) {
        old_predicate = new_predicate;
      } else {
      }
      const bool rowkey_changed = !old_predicate.empty()
          && !ObCharset::case_insensitive_equal(old_predicate, new_predicate);
      const int64_t now = ObTimeUtility::current_time();
      common::ObSEArray<ObAIColumnTask, 4> tasks;
      if (OB_SUCC(ret)
          && OB_FAIL(build_tasks_for_row(table_id, rtdef.ai_columns_,
                                         new_predicate, now, tasks))) {
      } else if (OB_SUCC(ret) && rowkey_changed
                 && OB_NOT_NULL(old_row)
                 && OB_FAIL(ObAIColumnProxy::delete_ai_column_tasks_by_row(
                              req_ctx.conn(), table_id, old_predicate))) {
      } else if (OB_SUCC(ret)
                 && OB_FAIL(upsert_tasks_for_row(req_ctx.conn(), table_id,
                                                 new_predicate, tasks))) {
      } else {
        // the visible result columns are invalidated until the worker finishes
        for (int64_t i = 0; OB_SUCC(ret) && i < rtdef.ai_columns_.count(); ++i) {
          if (OB_FAIL(reset_result_column(req_ctx.conn(), req_ctx.exec_ctx(),
                                          *table_schema,
                                          rtdef.ai_columns_.at(i).column_name_,
                                          new_predicate))) {
          }
        }
      }
    }
  }
  return ret;
}

int ObAIColumnDMLHandler::handle_delete(ObDMLModifyRowNode &modify_row)
{
  int ret = OB_SUCCESS;
  const ObDMLBaseCtDef &ctdef = *modify_row.dml_ctdef_;
  ObDMLBaseRtDef &rtdef = *modify_row.dml_rtdef_;
  const int64_t table_id = static_cast<int64_t>(ctdef.das_base_ctdef_.index_tid_);
  ObAIColumnRequestCtx req_ctx;
  const ObTableSchema *table_schema = NULL;
  if (OB_FAIL(req_ctx.begin(modify_row))) {
  } else if (OB_FAIL(get_table_schema(req_ctx.exec_ctx(), table_id, table_schema))) {
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(load_ai_columns(req_ctx.exec_ctx(), *table_schema, rtdef,
                                    req_ctx.conn()))) {
  } else if (rtdef.ai_columns_.empty()) {
    // ordinary table
  } else if (OB_ISNULL(modify_row.old_row_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObString rowkey_predicate;
    if (OB_FAIL(build_rowkey_predicate(*table_schema, *modify_row.old_row_,
                                       ctdef.column_ids_,
                                       req_ctx.exec_ctx().get_allocator(),
                                       rowkey_predicate))) {
    } else if (OB_FAIL(ObAIColumnProxy::delete_ai_column_tasks_by_row(
                 req_ctx.conn(), table_id, rowkey_predicate))) {
    }
  }
  return ret;
}

int ObAIColumnDMLHandler::get_table_schema(ObExecContext &exec_ctx,
                                           const int64_t table_id,
                                           const ObTableSchema *&table_schema)
{
  int ret = OB_SUCCESS;
  ObSqlCtx *sql_ctx = NULL;
  ObSchemaGetterGuard *schema_guard = NULL;
  table_schema = NULL;
  if (OB_ISNULL(sql_ctx = exec_ctx.get_sql_ctx())
      || OB_ISNULL(schema_guard = sql_ctx->schema_guard_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(schema_guard->get_table_schema(
                       static_cast<uint64_t>(table_id), table_schema))) {
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
