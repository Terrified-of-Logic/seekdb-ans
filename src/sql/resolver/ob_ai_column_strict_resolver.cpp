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

#define USING_LOG_PREFIX SQL_RESV

#include "sql/resolver/ob_ai_column_strict_resolver.h"

#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "share/ai_column/ob_ai_column_define.h"
#include "share/inner_table/ob_inner_table_schema_constants.h"
#include "share/schema/ob_schema_struct.h"
#include "share/schema/ob_table_schema.h"
#include "share/system_variable/ob_sys_var_class_type.h"
#include "sql/ob_sql_context.h"
#include "sql/resolver/dml/ob_dml_stmt.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/expr/ob_raw_expr.h"
#include "sql/resolver/ob_resolver_define.h"
#include "sql/resolver/ob_schema_checker.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace share::schema;

namespace sql
{

namespace
{

using AIColumnRef = ObAIColumnStrictResolver::AIColumnRef;

bool has_ref(const ObIArray<AIColumnRef> &refs, const uint64_t table_id,
             const uint64_t column_id)
{
  bool found = false;
  for (int64_t i = 0; !found && i < refs.count(); ++i) {
    found = (refs.at(i).table_id_ == table_id && refs.at(i).column_id_ == column_id);
  }
  return found;
}

// Collect every base table column referenced by the statement, including the ones
// referenced by its subqueries (a column referenced only inside a subquery still
// triggers the strict read check).
int collect_column_refs(ObDMLStmt &stmt, common::ObIArray<AIColumnRef> &refs)
{
  int ret = OB_SUCCESS;
  const ObIArray<ColumnItem> &column_items = stmt.get_column_items();
  for (int64_t i = 0; OB_SUCC(ret) && i < column_items.count(); ++i) {
    const ColumnItem &item = column_items.at(i);
    const uint64_t table_id = (OB_INVALID_ID != item.base_tid_)
                                  ? item.base_tid_ : item.table_id_;
    const uint64_t column_id = (OB_INVALID_ID != item.base_cid_)
                                   ? item.base_cid_ : item.column_id_;
    if (OB_INVALID_ID != table_id && OB_INVALID_ID != column_id
        && !has_ref(refs, table_id, column_id)) {
      if (OB_FAIL(refs.push_back(AIColumnRef(table_id, column_id)))) {
      }
    }
  }
  const ObIArray<ObQueryRefRawExpr *> &subqueries = stmt.get_subquery_exprs();
  for (int64_t i = 0; OB_SUCC(ret) && i < subqueries.count(); ++i) {
    ObQueryRefRawExpr *query_ref = subqueries.at(i);
    const ObSelectStmt *sub_stmt = NULL;
    if (OB_ISNULL(query_ref) || OB_ISNULL(sub_stmt = query_ref->get_ref_stmt())) {
      // ignore
    } else if (OB_FAIL(SMART_CALL(collect_column_refs(
                 *const_cast<ObSelectStmt *>(sub_stmt), refs)))) {
    }
  }
  return ret;
}

// Does a SELECT return at least one row?
int sql_exists(ObMySQLProxy &sql_proxy, const ObSqlString &sql, bool &exists)
{
  int ret = OB_SUCCESS;
  exists = false;
  ObMySQLProxy::MySQLResult res;
  sqlclient::ObMySQLResult *result = NULL;
  if (OB_FAIL(sql_proxy.read(res, sql.ptr()))) {
    LOG_WARN("failed to execute ai column strict read check", K(ret), K(sql));
  } else if (OB_ISNULL(result = res.get_result())) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const int tmp_ret = result->next();
    if (OB_SUCCESS == tmp_ret) {
      exists = true;
    } else if (OB_ITER_END == tmp_ret) {
      exists = false;
    } else {
      ret = tmp_ret;
      LOG_WARN("failed to fetch ai column strict read check result", K(ret));
    }
  }
  return ret;
}

} // namespace

int ObAIColumnStrictResolver::collect_refs(ObDMLStmt &stmt,
                                           common::ObIArray<ObAIColumnStrictResolver::AIColumnRef> &refs)
{
  return collect_column_refs(stmt, refs);
}

int ObAIColumnStrictResolver::check_strict_read(ObResolverParams &params, ObDMLStmt &stmt)
{
  int ret = OB_SUCCESS;
  bool strict_read = false;
  ObObj strict_read_obj;
  ObSQLSessionInfo *session = params.session_info_;
  if (OB_ISNULL(session) || OB_ISNULL(params.schema_checker_)
      || OB_ISNULL(params.sql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (params.is_prepare_protocol_ || params.is_prepare_stage_
             || params.is_mock_prepare_) {
    // only check at real execution
  } else if (OB_FAIL(session->get_sys_variable(SYS_VAR_AI_COLUMN_STRICT_READ,
                                               strict_read_obj))) {
  } else if (FALSE_IT(strict_read = strict_read_obj.get_bool())) {
  } else if (!strict_read) {
    // strict read is off
  } else {
    ObSqlCtx *sql_ctx = NULL;
    ObExecContext *exec_ctx = session->get_cur_exec_ctx();
    common::ObSEArray<AIColumnRef, 8> refs;
    if (OB_NOT_NULL(exec_ctx)) {
      sql_ctx = exec_ctx->get_sql_ctx();
    }
    if (OB_FAIL(collect_refs(stmt, refs))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < refs.count(); ++i) {
      if (OB_FAIL(check_one_column(params, sql_ctx, refs.at(i).table_id_,
                                   refs.at(i).column_id_))) {
      }
    }
  }
  return ret;
}

int ObAIColumnStrictResolver::check_one_column(ObResolverParams &params,
                                               ObSqlCtx *sql_ctx,
                                               const uint64_t table_id,
                                               const uint64_t column_id)
{
  int ret = OB_SUCCESS;
  const ObTableSchema *table_schema = NULL;
  const ObDatabaseSchema *db_schema = NULL;
  const ObColumnSchemaV2 *result_column = NULL;
  if (OB_FAIL(params.schema_checker_->get_table_schema(table_id, table_schema))) {
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(result_column = table_schema->get_column_schema(column_id))) {
    // column dropped/renamed concurrently, ignore
  } else {
    ObSqlString state_column_name;
    const ObString &result_column_name = result_column->get_column_name_str();
    if (OB_FAIL(state_column_name.append_fmt("%s%.*s",
                                             OB_AI_COLUMN_STATE_PREFIX,
                                             result_column_name.length(),
                                             result_column_name.ptr()))) {
    } else if (OB_ISNULL(table_schema->get_column_schema(state_column_name.string()))) {
      // not an AI result column
    } else if (OB_FAIL(params.schema_checker_->get_database_schema(
                 table_schema->get_database_id(), db_schema))) {
    } else if (OB_ISNULL(db_schema)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ObSqlString null_sql;
      ObSqlString task_sql;
      bool has_null_result = false;
      bool has_task = false;
      if (OB_FAIL(null_sql.append_fmt(
              "SELECT 1 FROM `%.*s`.`%.*s` WHERE `%.*s` IS NULL LIMIT 1",
              db_schema->get_database_name_str().length(),
              db_schema->get_database_name_str().ptr(),
              table_schema->get_table_name_str().length(),
              table_schema->get_table_name_str().ptr(),
              result_column_name.length(), result_column_name.ptr()))) {
      } else if (OB_FAIL(task_sql.append_fmt(
                   "SELECT 1 FROM oceanbase.__all_ai_column_task "
                   "WHERE table_id = %lu AND column_id = %lu LIMIT 1",
                   static_cast<uint64_t>(table_id),
                   static_cast<uint64_t>(column_id)))) {
      } else if (OB_FAIL(sql_exists(*params.sql_proxy_, null_sql, has_null_result))) {
      } else if (OB_FAIL(sql_exists(*params.sql_proxy_, task_sql, has_task))) {
      }
      if (OB_SUCC(ret) && (has_null_result || has_task)) {
        ret = OB_AI_COLUMN_UNFINISHED_TASK;
        LOG_USER_ERROR(OB_AI_COLUMN_UNFINISHED_TASK,
                       static_cast<uint64_t>(table_id),
                       static_cast<uint64_t>(column_id));
        LOG_WARN("ai column has unfinished tasks in strict read mode", K(ret),
                 K(table_id), K(column_id), K(has_null_result), K(has_task));
      }
    }
  }
  UNUSED(sql_ctx);
  return ret;
}

} // namespace sql
} // namespace oceanbase
