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

#ifndef OCEANBASE_OBSERVER_AI_COLUMN_OB_AI_COLUMN_WORKER_CONTEXT_H_
#define OCEANBASE_OBSERVER_AI_COLUMN_OB_AI_COLUMN_WORKER_CONTEXT_H_

#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "lib/allocator/page_arena.h"
#include "common/mysqlclient/ob_isql_connection.h"
#include "query/session/ob_free_session_ctx.h"

namespace oceanbase
{
namespace sql
{
class ObSQLSessionInfo;
class ObWorkerSessionGuard;
}
namespace common
{
class ObISQLClient;
}
namespace share
{
namespace schema
{
class ObSchemaGetterGuard;
}
}
namespace observer
{

// Per-worker-thread execution context: owns one inner SQL session plus an SPI
// connection bound to it, so the worker can run SQL in the tenant without
// touching the user's session/transaction. Modeled after
// ObDBMSSchedJobExecutor (observer/dbms_scheduler).
class ObAIColumnWorkerContext
{
public:
  ObAIColumnWorkerContext();
  ~ObAIColumnWorkerContext();

  int init();
  void destroy();
  bool is_inited() const { return inited_; }

  // Create the SPI connection (inside a worker-session guard). Must be paired
  // with end_op() before the next round.
  int begin_op();
  void end_op();

  int execute_write(const common::ObString &sql, int64_t &affected_rows);
  int execute_read(const common::ObString &sql,
                   common::ObISQLClient::ReadResult &res);

  int get_runtime_schema_guard(share::schema::ObSchemaGetterGuard &guard) const;

  common::sqlclient::ObISQLConnection *get_sql_conn() const { return conn_; }
  common::ObArenaAllocator &get_allocator() { return allocator_; }
  void reuse_allocator() { allocator_.reuse(); }

private:
  int create_session_();
  int init_session_env_();

private:
  bool inited_;
  sql::ObFreeSessionCtx free_session_ctx_;
  sql::ObSQLSessionInfo *session_info_;
  sql::ObWorkerSessionGuard *worker_session_guard_;
  common::sqlclient::ObISQLConnectionGuard conn_guard_;
  common::sqlclient::ObISQLConnection *conn_;
  common::ObArenaAllocator allocator_;
  DISALLOW_COPY_AND_ASSIGN(ObAIColumnWorkerContext);
};

} // namespace observer
} // namespace oceanbase

#endif // OCEANBASE_OBSERVER_AI_COLUMN_OB_AI_COLUMN_WORKER_CONTEXT_H_
