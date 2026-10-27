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

#define USING_LOG_PREFIX SERVER

#include "observer/ai_column/ob_ai_column_worker_service.h"

#include "observer/ai_column/ob_ai_column_ai_executor.h"
#include "observer/ai_column/ob_ai_column_worker_context.h"
#include "share/ai_column/ob_ai_column_define.h"
#include "share/ai_column/ob_ai_column_proxy.h"
#include "share/schema/ob_schema_struct.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "common/mysqlclient/ob_isql_client.h"
#include "common/sql_mode/ob_sql_mode.h"
#include "lib/time/ob_time_utility.h"
#include "lib/utility/utility.h"
#include "lib/utility/ob_print_utils.h"

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace share::schema;

namespace observer
{

const int64_t ObAIColumnWorkerService::WORKER_THREAD_CNT;
const int64_t ObAIColumnWorkerService::WORKER_IDLE_TIME_US;
const int64_t ObAIColumnWorkerService::TASK_LEASE_TIME_US;
const int64_t ObAIColumnWorkerService::MAX_TASK_BATCH;
const int64_t ObAIColumnWorkerService::MIN_RETRY_BACKOFF_US;
const int64_t ObAIColumnWorkerService::MAX_RETRY_BACKOFF_US;

namespace
{

const char *const TASK_SELECT_COLUMNS =
    "table_id, column_id, request_id, rowkey_predicate, eligible_ts, state, "
    "retry_count, lease_owner, lease_expire_ts";

int build_task_from_row(ObArenaAllocator &allocator,
                        sqlclient::ObMySQLResult &result,
                        ObAIColumnTask &task)
{
  int ret = OB_SUCCESS;
  ObString rowkey_predicate;
  task.reset();
  EXTRACT_INT_FIELD_MYSQL_SKIP_RET(result, "table_id", task.table_id_, int64_t);
  EXTRACT_INT_FIELD_MYSQL_SKIP_RET(result, "column_id", task.column_id_, int64_t);
  EXTRACT_INT_FIELD_MYSQL_SKIP_RET(result, "request_id", task.request_id_, int64_t);
  EXTRACT_VARCHAR_FIELD_MYSQL_SKIP_RET(result, "rowkey_predicate", rowkey_predicate);
  EXTRACT_TIMESTAMP_FIELD_MYSQL_SKIP_RET(result, "eligible_ts", task.eligible_ts_);
  EXTRACT_INT_FIELD_MYSQL_SKIP_RET(result, "state", task.state_, int64_t);
  EXTRACT_INT_FIELD_MYSQL_SKIP_RET(result, "retry_count", task.retry_count_, int64_t);
  EXTRACT_INT_FIELD_MYSQL_SKIP_RET(result, "lease_owner", task.lease_owner_, int64_t);
  EXTRACT_TIMESTAMP_FIELD_MYSQL_SKIP_RET(result, "lease_expire_ts", task.lease_expire_ts_);
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(deep_copy_ob_string(allocator, rowkey_predicate,
                                         task.rowkey_predicate_))) {
    LOG_WARN("fail to copy rowkey predicate", KR(ret));
  } else if (OB_UNLIKELY(!task.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid task row", KR(ret), K(task));
  }
  return ret;
}

} // anonymous namespace

ObAIColumnWorkerService::ObAIColumnWorkerService()
    : inited_(false),
      stop_flag_(true),
      worker_id_(0)
{
}

ObAIColumnWorkerService::~ObAIColumnWorkerService()
{
  destroy();
}

int ObAIColumnWorkerService::server_module_init(ObAIColumnWorkerService *&service)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(service)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("null worker service", KR(ret));
  } else if (OB_FAIL(service->init())) {
    LOG_WARN("fail to init ai column worker service", KR(ret));
  }
  return ret;
}

int ObAIColumnWorkerService::init()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("ai column worker service already inited", KR(ret));
  } else if (OB_FAIL(ObServerThreadHelper::create("AICOLW", WORKER_THREAD_CNT))) {
    LOG_WARN("fail to create ai column worker threads", KR(ret));
  } else {
    worker_id_ = ObTimeUtility::current_time();
    stop_flag_ = true;
    inited_ = true;
    LOG_INFO("[AICOLW] ai column worker service init success", K_(worker_id),
             "thread_cnt", WORKER_THREAD_CNT);
  }
  return ret;
}

int ObAIColumnWorkerService::start()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ai column worker service not inited", KR(ret));
  } else {
    stop_flag_ = false;
    if (OB_FAIL(ObServerThreadHelper::start())) {
      stop_flag_ = true;
      LOG_WARN("fail to start ai column worker threads", KR(ret));
    } else {
      LOG_INFO("[AICOLW] ai column worker service started");
    }
  }
  return ret;
}

void ObAIColumnWorkerService::stop()
{
  if (inited_) {
    stop_flag_ = true;
    ObServerThreadHelper::wakeup();
    ObServerThreadHelper::stop();
    LOG_INFO("[AICOLW] ai column worker service stop");
  }
}

void ObAIColumnWorkerService::wait()
{
  if (inited_) {
    ObServerThreadHelper::wait();
  }
}

void ObAIColumnWorkerService::destroy()
{
  if (inited_) {
    stop_flag_ = true;
    ObServerThreadHelper::wakeup();
    ObServerThreadHelper::destroy();
    inited_ = false;
  }
}

int ObAIColumnWorkerService::activate()
{
  return start();
}

void ObAIColumnWorkerService::deactivate()
{
  stop();
}

void ObAIColumnWorkerService::do_work()
{
  int ret = OB_SUCCESS;
  ObAIColumnWorkerContext ctx;
  if (OB_UNLIKELY(!inited_)) {
    LOG_WARN("[AICOLW] service not inited, worker thread exit");
    return;
  }
  LOG_INFO("[AICOLW] worker thread start", "worker_id", worker_id_);
  while (!stop_flag_) {
    if (!ctx.is_inited()) {
      // The worker service starts with the server, while the tenant schema
      // (users / oceanbase database) may still be bootstrapping: retry instead
      // of giving up, otherwise the worker would be dead for the whole run.
      int init_ret = ctx.init();
      if (OB_SUCCESS != init_ret) {
        ObServerThreadHelper::idle(200 * 1000);
        continue;
      }
      LOG_INFO("[AICOLW] worker context ready", "worker_id", worker_id_);
    }
    int tmp_ret = process_round_(ctx);
    if (OB_SUCCESS != tmp_ret) {
      LOG_WARN("[AICOLW] process round failed", KR(tmp_ret));
    }
    ObServerThreadHelper::idle(WORKER_IDLE_TIME_US);
  }
  ctx.destroy();
  LOG_INFO("[AICOLW] worker thread exit", "worker_id", worker_id_);
}

int ObAIColumnWorkerService::process_round_(ObAIColumnWorkerContext &ctx)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObAIColumnTask, MAX_TASK_BATCH> polled_tasks;
  ctx.reuse_allocator();
  if (OB_FAIL(ctx.begin_op())) {
    LOG_WARN("fail to begin worker op", KR(ret));
  } else {
    if (OB_FAIL(poll_tasks_(ctx, polled_tasks))) {
      LOG_WARN("fail to poll ai column tasks", KR(ret));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < polled_tasks.count(); ++i) {
      const ObAIColumnTask &seed = polled_tasks.at(i);
      bool duplicated = false;
      for (int64_t k = 0; k < i; ++k) {
        if (polled_tasks.at(k).table_id_ == seed.table_id_
            && polled_tasks.at(k).request_id_ == seed.request_id_) {
          duplicated = true;
          break;
        }
      }
      if (duplicated) {
        continue;
      }
      ObSEArray<ObAIColumnTask, 4> group_tasks;
      bool claimed = false;
      int tmp_ret = claim_group_(ctx, seed.table_id_, seed.request_id_, group_tasks, claimed);
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN("[AICOLW] fail to claim task group", KR(tmp_ret),
                 K(seed.table_id_), K(seed.request_id_));
        continue;
      } else if (!claimed) {
        continue;
      }
      tmp_ret = process_group_(ctx, seed.table_id_, seed.request_id_, group_tasks);
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN("[AICOLW] ai column task group processing failed", KR(tmp_ret),
                 K(seed.table_id_), K(seed.request_id_));
      }
    }
  }
  ctx.end_op();
  return ret;
}

int ObAIColumnWorkerService::claim_group_(ObAIColumnWorkerContext &ctx,
                                          const int64_t table_id,
                                          const int64_t request_id,
                                          ObIArray<ObAIColumnTask> &group_tasks,
                                          bool &claimed)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObSqlString sql;
  claimed = false;
  if (OB_FAIL(sql.assign_fmt(
          "UPDATE oceanbase.%s SET state = 2, lease_owner = %ld, "
          "lease_expire_ts = date_add(now(6), interval %ld microsecond), error_msg = NULL "
          "WHERE table_id = %ld AND request_id = %ld AND "
          "(state IN (1, 3) OR (state = 2 AND (lease_expire_ts IS NULL "
          "OR lease_expire_ts <= now(6))))",
          "__all_ai_column_task", worker_id_, TASK_LEASE_TIME_US,
          table_id, request_id))) {
    LOG_WARN("fail to build claim group sql", KR(ret));
  } else if (OB_FAIL(ctx.execute_write(sql.string(), affected_rows))) {
    LOG_WARN("fail to claim task group", KR(ret), K(table_id), K(request_id));
  } else if (affected_rows <= 0) {
    // Another worker thread owns this group (or it was already finished).
    claimed = false;
  } else if (OB_FAIL(load_group_tasks_(ctx, table_id, request_id, group_tasks))) {
  } else if (group_tasks.empty()) {
    claimed = false;
  } else if (group_tasks.count() == affected_rows) {
    bool all_ours = true;
    for (int64_t i = 0; all_ours && i < group_tasks.count(); ++i) {
      if (group_tasks.at(i).lease_owner_ != worker_id_) {
        all_ours = false;
      }
    }
    if (all_ours) {
      claimed = true;
    } else {
      int tmp_ret = release_our_claims_(ctx, table_id, request_id);
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN("fail to release partial claims", KR(tmp_ret));
      }
    }
  } else {
    // Only part of the group was claimable (the rest is being processed by
    // another thread): give back our rows and let that thread finish the group.
    int tmp_ret = release_our_claims_(ctx, table_id, request_id);
    if (OB_SUCCESS != tmp_ret) {
      LOG_WARN("fail to release partial claims", KR(tmp_ret));
    }
  }
  return ret;
}

int ObAIColumnWorkerService::release_our_claims_(ObAIColumnWorkerContext &ctx,
                                                 const int64_t table_id,
                                                 const int64_t request_id)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObSqlString sql;
  if (OB_FAIL(sql.assign_fmt(
          "UPDATE oceanbase.%s SET state = 1, eligible_ts = now(6), lease_owner = 0, "
          "lease_expire_ts = NULL WHERE table_id = %ld AND request_id = %ld "
          "AND state = 2 AND lease_owner = %ld",
          "__all_ai_column_task", table_id, request_id, worker_id_))) {
  } else if (OB_FAIL(ctx.execute_write(sql.string(), affected_rows))) {
    LOG_WARN("fail to release our claims", KR(ret), K(table_id), K(request_id));
  }
  return ret;
}

int ObAIColumnWorkerService::poll_tasks_(ObAIColumnWorkerContext &ctx,
                                         ObIArray<ObAIColumnTask> &tasks)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.assign_fmt(
          "SELECT %s FROM oceanbase.%s "
          "WHERE (state IN (1, 3) AND (eligible_ts IS NULL OR eligible_ts <= now(6))) "
          "   OR (state = 2 AND (lease_expire_ts IS NULL OR lease_expire_ts <= now(6))) "
          "ORDER BY eligible_ts LIMIT %ld",
          TASK_SELECT_COLUMNS, "__all_ai_column_task", MAX_TASK_BATCH))) {
    LOG_WARN("fail to build poll sql", KR(ret));
  } else {
    SMART_VAR(ObISQLClient::ReadResult, res) {
      sqlclient::ObMySQLResult *result = nullptr;
      if (OB_FAIL(ctx.execute_read(sql.string(), res))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("null poll result", KR(ret));
      } else {
        while (OB_SUCC(ret)) {
          ObAIColumnTask task;
          if (OB_FAIL(result->next())) {
            if (OB_ITER_END == ret) {
              ret = OB_SUCCESS;
              break;
            }
            LOG_WARN("fail to get next task row", KR(ret));
          } else if (OB_FAIL(build_task_from_row(ctx.get_allocator(), *result, task))) {
          } else if (OB_FAIL(tasks.push_back(task))) {
            LOG_WARN("fail to push task", KR(ret));
          }
        }
      }
    }
  }
  return ret;
}

int ObAIColumnWorkerService::load_group_tasks_(ObAIColumnWorkerContext &ctx,
                                               const int64_t table_id,
                                               const int64_t request_id,
                                               ObIArray<ObAIColumnTask> &tasks)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.assign_fmt(
          "SELECT %s FROM oceanbase.%s WHERE table_id = %ld AND request_id = %ld",
          TASK_SELECT_COLUMNS, "__all_ai_column_task", table_id, request_id))) {
    LOG_WARN("fail to build load group sql", KR(ret));
  } else {
    SMART_VAR(ObISQLClient::ReadResult, res) {
      sqlclient::ObMySQLResult *result = nullptr;
      if (OB_FAIL(ctx.execute_read(sql.string(), res))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("null group result", KR(ret));
      } else {
        while (OB_SUCC(ret)) {
          ObAIColumnTask task;
          if (OB_FAIL(result->next())) {
            if (OB_ITER_END == ret) {
              ret = OB_SUCCESS;
              break;
            }
            LOG_WARN("fail to get next group task", KR(ret));
          } else if (OB_FAIL(build_task_from_row(ctx.get_allocator(), *result, task))) {
          } else if (OB_FAIL(tasks.push_back(task))) {
            LOG_WARN("fail to push group task", KR(ret));
          }
        }
      }
    }
  }
  return ret;
}

int ObAIColumnWorkerService::append_identifier_(ObSqlString &sql,
                                                const ObString &name) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("`"))) {
  } else if (OB_FAIL(sql.append(name))) {
  } else if (OB_FAIL(sql.append("`"))) {
  }
  return ret;
}

int ObAIColumnWorkerService::append_string_literal_(ObSqlString &sql,
                                                    const ObString &str) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("'"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < str.length(); ++i) {
    const char c = str.ptr()[i];
    if ('\'' == c || '\\' == c) {
      if (OB_FAIL(sql.append_fmt("\\%c", c))) {
        LOG_WARN("fail to append escaped char", KR(ret));
      }
    } else if (OB_FAIL(sql.append_fmt("%c", c))) {
      LOG_WARN("fail to append char", KR(ret));
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(sql.append("'"))) {
    LOG_WARN("fail to append quote", KR(ret));
  }
  return ret;
}

int ObAIColumnWorkerService::claim_task_(ObAIColumnWorkerContext &ctx,
                                         const ObAIColumnTask &task,
                                         bool &claimed)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObSqlString sql;
  claimed = false;
  if (OB_FAIL(sql.assign_fmt(
          "UPDATE oceanbase.%s SET state = 2, lease_owner = %ld, "
          "lease_expire_ts = date_add(now(6), interval %ld microsecond), error_msg = NULL "
          "WHERE table_id = %ld AND column_id = %ld AND rowkey_predicate = ",
          "__all_ai_column_task", worker_id_, TASK_LEASE_TIME_US,
          task.table_id_, task.column_id_))) {
  } else if (OB_FAIL(append_string_literal_(sql, task.rowkey_predicate_))) {
  } else if (OB_FAIL(sql.append_fmt(
                 " AND request_id = %ld AND (state IN (1, 3) OR "
                 "(state = 2 AND (lease_expire_ts IS NULL OR lease_expire_ts <= now(6))))",
                 task.request_id_))) {
  } else if (OB_FAIL(ctx.execute_write(sql.string(), affected_rows))) {
    LOG_WARN("fail to claim task", KR(ret), K(sql));
  } else {
    claimed = (1 == affected_rows);
  }
  return ret;
}

int ObAIColumnWorkerService::release_claim_(ObAIColumnWorkerContext &ctx,
                                            const ObAIColumnTask &task)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObSqlString sql;
  if (OB_FAIL(sql.assign_fmt(
          "UPDATE oceanbase.%s SET state = 1, eligible_ts = now(6), lease_owner = 0, "
          "lease_expire_ts = NULL WHERE table_id = %ld AND column_id = %ld "
          "AND request_id = %ld AND state = 2 AND lease_owner = %ld AND rowkey_predicate = ",
          "__all_ai_column_task", task.table_id_, task.column_id_, task.request_id_,
          worker_id_))) {
  } else if (OB_FAIL(append_string_literal_(sql, task.rowkey_predicate_))) {
  } else if (OB_FAIL(ctx.execute_write(sql.string(), affected_rows))) {
    LOG_WARN("fail to release claim", KR(ret));
  }
  return ret;
}

int ObAIColumnWorkerService::read_user_content_(ObAIColumnWorkerContext &ctx,
                                                const ObString &db_name,
                                                const ObString &table_name,
                                                const ObString &user_expr,
                                                const ObString &rowkey_predicate,
                                                ObString &content)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  content.reset();
  if (OB_FAIL(sql.assign_fmt("SELECT "))) {
  } else if (OB_FAIL(sql.append(user_expr))) {
  } else if (OB_FAIL(sql.append(" FROM "))) {
  } else if (OB_FAIL(append_identifier_(sql, db_name))) {
  } else if (OB_FAIL(sql.append("."))) {
  } else if (OB_FAIL(append_identifier_(sql, table_name))) {
  } else if (OB_FAIL(sql.append(" WHERE "))) {
  } else if (OB_FAIL(sql.append(rowkey_predicate))) {
    LOG_WARN("fail to append rowkey predicate", KR(ret));
  } else {
    SMART_VAR(ObISQLClient::ReadResult, res) {
      sqlclient::ObMySQLResult *result = nullptr;
      ObString tmp;
      if (OB_FAIL(ctx.execute_read(sql.string(), res))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("null user content result", KR(ret));
      } else if (OB_FAIL(result->next())) {
        LOG_WARN("fail to get user content row", KR(ret), K(sql));
      } else if (OB_FAIL(result->get_varchar(static_cast<int64_t>(0), tmp))) {
        LOG_WARN("fail to get user content", KR(ret), K(sql));
      } else if (OB_FAIL(deep_copy_ob_string(ctx.get_allocator(), tmp, content))) {
        LOG_WARN("fail to copy user content", KR(ret));
      }
    }
  }
  return ret;
}

int ObAIColumnWorkerService::check_task_still_valid_(ObAIColumnWorkerContext &ctx,
                                                     const ObAIColumnTask &task,
                                                     bool &still_valid)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t cur_request_id = -1;
  still_valid = false;
  if (OB_FAIL(sql.assign_fmt(
          "SELECT request_id FROM oceanbase.%s WHERE table_id = %ld AND column_id = %ld "
          "AND rowkey_predicate = ",
          "__all_ai_column_task", task.table_id_, task.column_id_))) {
  } else if (OB_FAIL(append_string_literal_(sql, task.rowkey_predicate_))) {
  } else {
    SMART_VAR(ObISQLClient::ReadResult, res) {
      sqlclient::ObMySQLResult *result = nullptr;
      if (OB_FAIL(ctx.execute_read(sql.string(), res))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("null valid check result", KR(ret));
      } else if (OB_FAIL(result->next())) {
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
          still_valid = false;
        } else {
          LOG_WARN("fail to get task row for valid check", KR(ret));
        }
      } else if (OB_FAIL(result->get_int(static_cast<int64_t>(0), cur_request_id))) {
        LOG_WARN("fail to get request id", KR(ret));
      } else {
        still_valid = (cur_request_id == task.request_id_);
      }
    }
  }
  return ret;
}

int ObAIColumnWorkerService::write_result_row_(
    ObAIColumnWorkerContext &ctx,
    const ObString &db_name,
    const ObString &table_name,
    const ObString &rowkey_predicate,
    const ObIArray<ObAIColumnSchema> &all_metas,
    const ObIArray<int64_t> &column_ids,
    const ObIArray<ObString> &values,
    const ObIArray<bool> &is_embedding)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObSqlString sql;
  if (column_ids.count() != values.count() || column_ids.count() != is_embedding.count()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("column id/value count mismatch", KR(ret), K(column_ids.count()),
             K(values.count()));
  } else if (OB_FAIL(sql.assign_fmt("UPDATE "))) {
  } else if (OB_FAIL(append_identifier_(sql, db_name))) {
  } else if (OB_FAIL(sql.append("."))) {
  } else if (OB_FAIL(append_identifier_(sql, table_name))) {
  } else if (OB_FAIL(sql.append(" SET "))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < column_ids.count(); ++i) {
    const ObAIColumnSchema *meta = nullptr;
    for (int64_t j = 0; OB_SUCC(ret) && j < all_metas.count(); ++j) {
      if (all_metas.at(j).column_id_ == column_ids.at(i)) {
        meta = &all_metas.at(j);
        break;
      }
    }
    if (OB_ISNULL(meta)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai column metadata not found", KR(ret), K(column_ids.at(i)));
    } else if (i > 0 && OB_FAIL(sql.append(", "))) {
    } else if (OB_FAIL(append_identifier_(sql, meta->column_name_))) {
    } else if (OB_FAIL(sql.append(" = "))) {
    } else if (OB_FAIL(append_string_literal_(sql, values.at(i)))) {
      // NOTE: even embedding result is written as a string literal: the server
      // casts the '[1.0,2.0,...]' text to the vector column on assignment.
      LOG_WARN("fail to append result value", KR(ret));
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(sql.append(" WHERE "))) {
  } else if (OB_FAIL(sql.append(rowkey_predicate))) {
    LOG_WARN("fail to append rowkey predicate", KR(ret));
  } else if (OB_FAIL(ctx.execute_write(sql.string(), affected_rows))) {
    LOG_WARN("fail to write ai column results", KR(ret), K(sql));
  } else if (0 == affected_rows) {
    LOG_WARN("[AICOLW] result row no longer exists (stale)", K(sql));
  }
  return ret;
}

int ObAIColumnWorkerService::delete_group_tasks_(ObAIColumnWorkerContext &ctx,
                                                 const int64_t table_id,
                                                 const int64_t request_id)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObSqlString sql;
  if (OB_FAIL(sql.assign_fmt(
          "DELETE FROM oceanbase.%s WHERE table_id = %ld AND request_id = %ld",
          "__all_ai_column_task", table_id, request_id))) {
  } else if (OB_FAIL(ctx.execute_write(sql.string(), affected_rows))) {
    // Retry once: finished tasks must not linger (a transient row-lock conflict
    // with a competing claimer can otherwise leave the group unprocessed for a
    // full lease period).
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = ctx.execute_write(sql.string(), affected_rows))) {
      LOG_WARN("fail to delete task group after retry", KR(tmp_ret), K(sql));
      ret = tmp_ret;
    } else {
      LOG_INFO("[AICOLW] delete task group succeeded on retry", K(table_id), K(request_id));
      ret = OB_SUCCESS;
    }
  }
  return ret;
}

int ObAIColumnWorkerService::reset_task_to_pending_(ObAIColumnWorkerContext &ctx,
                                                    const ObAIColumnTask &task)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObSqlString sql;
  if (OB_FAIL(sql.assign_fmt(
          "UPDATE oceanbase.%s SET state = 1, eligible_ts = now(6), lease_owner = 0, "
          "lease_expire_ts = NULL WHERE table_id = %ld AND column_id = %ld "
          "AND request_id = %ld AND rowkey_predicate = ",
          "__all_ai_column_task", task.table_id_, task.column_id_, task.request_id_))) {
  } else if (OB_FAIL(append_string_literal_(sql, task.rowkey_predicate_))) {
  } else if (OB_FAIL(ctx.execute_write(sql.string(), affected_rows))) {
    LOG_WARN("fail to reset task to pending", KR(ret), K(sql));
  }
  return ret;
}

int ObAIColumnWorkerService::retry_or_terminal_(ObAIColumnWorkerContext &ctx,
                                                const ObAIColumnTask &task,
                                                const int64_t max_retries,
                                                const int ret_code)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  const int64_t new_retry_count = task.retry_count_ + 1;
  ObSqlString sql;
  if (new_retry_count > max_retries) {
    // Exhausted: keep result NULL, drop the task row (terminal failure).
    if (OB_FAIL(sql.assign_fmt(
            "DELETE FROM oceanbase.%s WHERE table_id = %ld AND column_id = %ld "
            "AND request_id = %ld AND rowkey_predicate = ",
            "__all_ai_column_task", task.table_id_, task.column_id_, task.request_id_))) {
    } else if (OB_FAIL(append_string_literal_(sql, task.rowkey_predicate_))) {
    } else if (OB_FAIL(ctx.execute_write(sql.string(), affected_rows))) {
      LOG_WARN("fail to delete terminal task", KR(ret));
    } else {
      LOG_INFO("[AICOLW] ai column task terminal failed", K(task.table_id_),
               K(task.column_id_), K(task.request_id_), K(ret_code));
    }
  } else {
    int64_t shift = new_retry_count - 1;
    if (shift > 4) {
      shift = 4;
    }
    int64_t backoff_us = MIN_RETRY_BACKOFF_US << shift;
    if (backoff_us > MAX_RETRY_BACKOFF_US) {
      backoff_us = MAX_RETRY_BACKOFF_US;
    }
    if (OB_FAIL(sql.assign_fmt(
            "UPDATE oceanbase.%s SET state = 3, retry_count = %ld, "
            "eligible_ts = date_add(now(6), interval %ld microsecond), "
            "lease_owner = 0, lease_expire_ts = NULL, error_msg = 'ret_code=%d' "
            "WHERE table_id = %ld AND column_id = %ld AND request_id = %ld "
            "AND rowkey_predicate = ",
            "__all_ai_column_task", new_retry_count, backoff_us, ret_code,
            task.table_id_, task.column_id_, task.request_id_))) {
    } else if (OB_FAIL(append_string_literal_(sql, task.rowkey_predicate_))) {
    } else if (OB_FAIL(ctx.execute_write(sql.string(), affected_rows))) {
      LOG_WARN("fail to schedule task retry", KR(ret), K(sql));
    } else {
      LOG_INFO("[AICOLW] ai column task scheduled for retry", K(task.table_id_),
               K(task.column_id_), K(task.request_id_), K(new_retry_count), K(backoff_us));
    }
  }
  return ret;
}

int ObAIColumnWorkerService::process_group_(ObAIColumnWorkerContext &ctx,
                                            const int64_t table_id,
                                            const int64_t request_id,
                                            const ObIArray<ObAIColumnTask> &claimed_tasks)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator &allocator = ctx.get_allocator();
  ObSchemaGetterGuard schema_guard;
  const ObTableSchema *table_schema = nullptr;
  const ObDatabaseSchema *database_schema = nullptr;
  ObSEArray<ObAIColumnSchema, 4> all_metas;
  ObSEArray<int64_t, 4> column_ids;
  ObSEArray<ObString, 4> values;
  ObSEArray<bool, 4> is_embedding;
  bool any_failed = false;
  if (claimed_tasks.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("empty claimed tasks", KR(ret));
  } else if (OB_FAIL(ctx.get_runtime_schema_guard(schema_guard))) {
  } else if (OB_FAIL(schema_guard.get_table_schema(table_id, table_schema))) {
    LOG_WARN("fail to get table schema", KR(ret), K(table_id));
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("ai column table not exist", KR(ret), K(table_id));
  } else if (OB_FAIL(schema_guard.get_database_schema(
                 table_schema->get_database_id(), database_schema))) {
    LOG_WARN("fail to get database schema", KR(ret));
  } else if (OB_ISNULL(database_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("null database schema", KR(ret));
  } else if (OB_FAIL(ObAIColumnProxy::select_ai_columns_by_table(
                 allocator, *ctx.get_sql_conn(), table_id, all_metas))) {
    LOG_WARN("fail to read ai column metadata", KR(ret), K(table_id));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < claimed_tasks.count(); ++i) {
    const ObAIColumnTask &task = claimed_tasks.at(i);
    const ObAIColumnSchema *meta = nullptr;
    ObString content;
    ObString value;
    bool embedding = false;
    int tmp_ret = OB_SUCCESS;
    for (int64_t j = 0; OB_SUCC(ret) && j < all_metas.count(); ++j) {
      if (all_metas.at(j).column_id_ == task.column_id_) {
        meta = &all_metas.at(j);
        break;
      }
    }
    if (OB_FAIL(ret)) {
      break;
    } else if (OB_ISNULL(meta)) {
      tmp_ret = OB_ENTRY_NOT_EXIST;
      LOG_WARN("[AICOLW] ai column metadata not found", K(task.column_id_));
    } else if (OB_UNLIKELY(meta->ai_model_name_.empty() || meta->user_expr_.empty())) {
      tmp_ret = OB_INVALID_ARGUMENT;
      LOG_WARN("[AICOLW] ai column metadata incomplete", K(*meta));
    } else if (OB_FAIL(read_user_content_(ctx,
                                          database_schema->get_database_name_str(),
                                          table_schema->get_table_name_str(),
                                          meta->user_expr_,
                                          task.rowkey_predicate_,
                                          content))) {
      tmp_ret = ret;
      LOG_WARN("[AICOLW] fail to evaluate user expr", KR(tmp_ret), K(meta->user_expr_));
    } else if (OB_FAIL(ObAIColumnAIExecutor::execute(
                   allocator, schema_guard, *meta, content, embedding, value))) {
      tmp_ret = ret;
      LOG_WARN("[AICOLW] fail to execute ai column", KR(tmp_ret), K(meta->column_name_));
    } else if (OB_FAIL(column_ids.push_back(task.column_id_))) {
      tmp_ret = ret;
    } else if (OB_FAIL(values.push_back(value))) {
      tmp_ret = ret;
    } else if (OB_FAIL(is_embedding.push_back(embedding))) {
      tmp_ret = ret;
    }
    if (OB_SUCCESS != tmp_ret) {
      any_failed = true;
      int tmp2_ret = OB_SUCCESS;
      if (OB_SUCCESS != (tmp2_ret = retry_or_terminal_(
                             ctx, task, OB_ISNULL(meta) ? 0 : meta->max_retries_, tmp_ret))) {
        LOG_WARN("fail to handle task failure", KR(tmp2_ret));
      }
      ret = OB_SUCCESS;
    }
  }
  if (OB_FAIL(ret)) {
  } else if (any_failed) {
    // Group is not atomic if one column failed: put the succeeded ones back to
    // PENDING so the whole group is retried together (no partial visibility).
    for (int64_t i = 0; i < claimed_tasks.count(); ++i) {
      bool succeeded = false;
      for (int64_t j = 0; j < column_ids.count(); ++j) {
        if (column_ids.at(j) == claimed_tasks.at(i).column_id_) {
          succeeded = true;
          break;
        }
      }
      if (succeeded) {
        int tmp_ret = reset_task_to_pending_(ctx, claimed_tasks.at(i));
        if (OB_SUCCESS != tmp_ret) {
          LOG_WARN("fail to reset succeeded task to pending", KR(tmp_ret));
        }
      }
    }
  } else {
    bool still_valid = false;
    if (OB_FAIL(check_task_still_valid_(ctx, claimed_tasks.at(0), still_valid))) {
    } else if (!still_valid) {
      LOG_INFO("[AICOLW] stale task group detected, skip result write",
               K(table_id), K(request_id));
    } else if (OB_FAIL(write_result_row_(ctx,
                                         database_schema->get_database_name_str(),
                                         table_schema->get_table_name_str(),
                                         claimed_tasks.at(0).rowkey_predicate_,
                                         all_metas, column_ids, values, is_embedding))) {
    } else if (OB_FAIL(delete_group_tasks_(ctx, table_id, request_id))) {
      LOG_WARN("fail to delete task group after write", KR(ret));
    } else {
      LOG_INFO("[AICOLW] ai column group finished", K(table_id), K(request_id),
               "columns", column_ids.count());
    }
  }
  return ret;
}

} // namespace observer
} // namespace oceanbase
