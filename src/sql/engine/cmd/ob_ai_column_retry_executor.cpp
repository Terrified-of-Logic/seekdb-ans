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

#define USING_LOG_PREFIX SQL_EXE

#include "sql/engine/cmd/ob_ai_column_retry_executor.h"

#include <atomic>
#include "lib/time/ob_time_utility.h"
#include "share/ai_column/ob_ai_column_define.h"
#include "share/ai_column/ob_ai_column_proxy.h"
#include "share/ai_column/ob_ai_column_struct.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_table_schema.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/ob_sql_context.h"
#include "sql/resolver/ddl/ob_alter_table_stmt.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "sql/session/ob_inner_sql_connection.h"
#include "sql/session/ob_sql_session_info.h"
#include "common/mysqlclient/ob_isql_connection.h"
#include "common/mysqlclient/ob_mysql_result.h"

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace share::schema;

namespace sql
{

namespace
{

uint64_t alloc_retry_request_id()
{
  static std::atomic<uint64_t> seq(0);
  const uint64_t s = static_cast<uint64_t>(seq.fetch_add(1));
  return (static_cast<uint64_t>(ObTimeUtility::current_time()) << 8) | (s & 0xFFULL);
}

bool is_numeric_type(const ObObjType type)
{
  return ob_is_integer_type(type) || ob_is_number_tc(type) || ob_is_double_tc(type)
      || ob_is_float_tc(type) || ob_is_numeric_type(type);
}

int create_retry_spi_conn(ObExecContext &exec_ctx,
                          sqlclient::ObISQLConnectionGuard &conn_guard,
                          sqlclient::ObISQLConnection *&conn)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = NULL;
  conn = NULL;
  if (OB_ISNULL(session = exec_ctx.get_my_session())
      || OB_ISNULL(exec_ctx.get_sql_proxy())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(query::ObInnerSQLConnectionAccess::
                       create_spi_connection_with_external_session(session, conn_guard))) {
  } else if (OB_ISNULL(conn = conn_guard.get_ptr())) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

// The nested inner SQL execution may report a close error after the statement has
// been executed successfully; the affected row count is the authoritative result.
int execute_retry_write(sqlclient::ObISQLConnection &conn, const ObString &sql)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  if (sql.empty()) {
  } else if (OB_FAIL(conn.execute_write(sql.ptr(), affected_rows))) {
    if (OB_ERR_UNEXPECTED == ret && affected_rows >= 0) {
      ret = OB_SUCCESS;
    } else {
      LOG_WARN("failed to execute ai column retry sql", K(ret), K(affected_rows), K(sql));
    }
  }
  return ret;
}

int build_rowkey_predicate(const ObTableSchema &table_schema,
                           const ObIArray<ObString> &rowkey_values,
                           const ObIArray<ObObjType> &rowkey_types,
                           ObIAllocator &allocator,
                           ObString &predicate)
{
  int ret = OB_SUCCESS;
  ObSqlString predicate_sql;
  const ObRowkeyInfo &rowkey_info = table_schema.get_rowkey_info();
  if (OB_UNLIKELY(rowkey_values.count() != rowkey_info.get_size())) {
    ret = OB_ERR_UNEXPECTED;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_info.get_size(); ++i) {
    ObRowkeyColumn rowkey_column;
    const ObColumnSchemaV2 *column_schema = NULL;
    if (OB_FAIL(rowkey_info.get_column(i, rowkey_column))) {
    } else if (OB_ISNULL(column_schema =
                           table_schema.get_column_schema(rowkey_column.column_id_))) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      const ObString &value = rowkey_values.at(i);
      if (is_numeric_type(rowkey_types.at(i))) {
        if (OB_FAIL(predicate_sql.append_fmt("%s`%.*s` = %.*s",
                                             (0 == i ? "" : " and "),
                                             column_schema->get_column_name_str().length(),
                                             column_schema->get_column_name_str().ptr(),
                                             value.length(), value.ptr()))) {
        }
      } else {
        if (OB_FAIL(predicate_sql.append_fmt("%s`%.*s` = '%.*s'",
                                             (0 == i ? "" : " and "),
                                             column_schema->get_column_name_str().length(),
                                             column_schema->get_column_name_str().ptr(),
                                             value.length(), value.ptr()))) {
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    ret = ob_write_string(allocator, predicate_sql.string(), predicate);
  }
  return ret;
}

} // namespace

int ObAIColumnRetryExecutor::execute(ObExecContext &ctx, ObAlterTableStmt &stmt)
{
  int ret = OB_SUCCESS;
  obcall::ObAlterTableArg &arg = stmt.get_alter_table_arg();
  ObSchemaGetterGuard schema_guard;
  ObSqlCtx *sql_ctx = NULL;
  const ObTableSchema *table_schema = NULL;
  const ObDatabaseSchema *db_schema = NULL;
  const int64_t table_id = arg.ai_retry_table_id_;
  const int64_t column_id = arg.ai_retry_column_id_;
  if (OB_UNLIKELY(table_id <= 0 || column_id <= 0
                  || arg.ai_retry_column_name_.empty())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(sql_ctx = ctx.get_sql_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(share::schema::ObMultiVersionSchemaService::get_instance()
                       .get_runtime_schema_guard(schema_guard))) {
  } else if (OB_FAIL(schema_guard.get_table_schema(
                       static_cast<uint64_t>(table_id), table_schema))) {
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(schema_guard.get_database_schema(
                       static_cast<uint64_t>(table_schema->get_database_id()), db_schema))) {
  } else if (OB_ISNULL(db_schema)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    sqlclient::ObISQLConnection *conn = NULL;
    sqlclient::ObISQLConnectionGuard conn_guard;
    ObSqlString db_table_name;
    ObString where_sql = arg.ai_retry_where_sql_;
    const ObRowkeyInfo &rowkey_info = table_schema->get_rowkey_info();
    common::ObSEArray<ObString, 4> rowkey_predicates;
    common::ObSEArray<int64_t, 4> request_ids;
    if (where_sql.empty()) {
      where_sql = ObString::make_string("1 = 1");
    }
    ret = create_retry_spi_conn(ctx, conn_guard, conn);
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(db_table_name.append_fmt("%.*s.%.*s",
                 db_schema->get_database_name_str().length(),
                 db_schema->get_database_name_str().ptr(),
                 table_schema->get_table_name_str().length(),
                 table_schema->get_table_name_str().ptr()))) {
    } else {
      // 1. select the rowkeys of the selected rows first
      ObSqlString select_sql;
      if (OB_FAIL(select_sql.append("SELECT "))) {
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_info.get_size(); ++i) {
        ObRowkeyColumn rowkey_column;
        const ObColumnSchemaV2 *column_schema = NULL;
        if (OB_FAIL(rowkey_info.get_column(i, rowkey_column))) {
        } else if (OB_ISNULL(column_schema =
                               table_schema->get_column_schema(rowkey_column.column_id_))) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(select_sql.append_fmt("%s`%.*s`",
                            (0 == i ? "" : ", "),
                            column_schema->get_column_name_str().length(),
                            column_schema->get_column_name_str().ptr()))) {
        }
      }
      if (OB_SUCC(ret)
          && OB_FAIL(select_sql.append_fmt(" FROM %s WHERE %.*s",
                      db_table_name.ptr(),
                      where_sql.length(), where_sql.ptr()))) {
      }
      if (OB_SUCC(ret)) {
        ObISQLClient::ReadResult read_result;
        sqlclient::ObMySQLResult *result = NULL;
        if (OB_FAIL(conn->execute_read(select_sql.string(), read_result))) {
          LOG_WARN("failed to select rows for ai column retry", K(ret), K(select_sql));
        } else if (OB_ISNULL(result = read_result.get_result())) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          while (OB_SUCC(ret)) {
            const int tmp_ret = result->next();
            if (OB_ITER_END == tmp_ret) {
              break;
            } else if (OB_SUCCESS != tmp_ret) {
              ret = tmp_ret;
              LOG_WARN("failed to iterate ai column retry rows", K(ret));
            } else {
              common::ObSEArray<ObString, 4> rowkey_values;
              common::ObSEArray<ObObjType, 4> rowkey_types;
              for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_info.get_size(); ++i) {
                ObRowkeyColumn rowkey_column;
                const ObColumnSchemaV2 *column_schema = NULL;
                if (OB_FAIL(rowkey_info.get_column(i, rowkey_column))) {
                } else if (OB_ISNULL(column_schema =
                               table_schema->get_column_schema(rowkey_column.column_id_))) {
                  ret = OB_ERR_UNEXPECTED;
                } else {
                  const ObObjType type = column_schema->get_data_type();
                  ObString value;
                  if (is_numeric_type(type)) {
                    int64_t int_value = 0;
                    ObSqlString value_sql;
                    if (OB_FAIL(result->get_int(i, int_value))) {
                    } else if (OB_FAIL(value_sql.append_fmt("%ld", int_value))) {
                    } else if (OB_FAIL(ob_write_string(ctx.get_allocator(),
                                                       value_sql.string(), value))) {
                    }
                  } else {
                    ObString raw_value;
                    if (OB_FAIL(result->get_varchar(i, raw_value))) {
                    } else if (OB_FAIL(ob_write_string(ctx.get_allocator(),
                                                       raw_value, value))) {
                    }
                  }
                  if (OB_SUCC(ret) && OB_FAIL(rowkey_values.push_back(value))) {
                  } else if (OB_SUCC(ret) && OB_FAIL(rowkey_types.push_back(type))) {
                  }
                }
              }
              if (OB_SUCC(ret)) {
                ObString rowkey_predicate;
                if (OB_FAIL(build_rowkey_predicate(*table_schema, rowkey_values,
                                                   rowkey_types,
                                                   ctx.get_allocator(),
                                                   rowkey_predicate))) {
                } else if (OB_FAIL(rowkey_predicates.push_back(rowkey_predicate))) {
                } else if (OB_FAIL(request_ids.push_back(
                             static_cast<int64_t>(alloc_retry_request_id())))) {
                }
              }
            }
          }
        }
      }
    }
    if (OB_SUCC(ret) && !rowkey_predicates.empty()) {
      const int64_t now = ObTimeUtility::current_time();
      // 2. build the new tasks (one request id per row) BEFORE invalidating results:
      //    `status = 'READY'` predicates are evaluated while result still holds the value.
      common::ObSEArray<ObAIColumnTask, 4> tasks;
      for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_predicates.count(); ++i) {
        ObAIColumnTask task;
        task.table_id_ = table_id;
        task.column_id_ = column_id;
        task.request_id_ = request_ids.at(i);
        task.rowkey_predicate_ = rowkey_predicates.at(i);
        task.eligible_ts_ = now;
        task.state_ = static_cast<int64_t>(ObAIColumnTaskState::PENDING);
        task.retry_count_ = 0;
        task.lease_owner_ = 0;
        task.lease_expire_ts_ = 0;
        if (OB_FAIL(tasks.push_back(task))) {
        }
      }
      if (OB_SUCC(ret)
          && OB_FAIL(ObAIColumnProxy::insert_ai_column_tasks(*conn, tasks))) {
        LOG_WARN("failed to insert ai column retry tasks", K(ret), K(tasks.count()));
      }
      // 3. invalidate the target AI result column of the selected rows
      for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_predicates.count(); ++i) {
        ObSqlString update_sql;
        if (OB_FAIL(update_sql.append_fmt(
              "UPDATE %s SET `%.*s` = NULL WHERE %.*s",
              db_table_name.ptr(),
              arg.ai_retry_column_name_.length(), arg.ai_retry_column_name_.ptr(),
              rowkey_predicates.at(i).length(), rowkey_predicates.at(i).ptr()))) {
        } else if (OB_FAIL(execute_retry_write(*conn, update_sql.string()))) {
        }
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
