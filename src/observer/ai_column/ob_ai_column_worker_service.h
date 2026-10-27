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

#ifndef OCEANBASE_OBSERVER_AI_COLUMN_OB_AI_COLUMN_WORKER_SERVICE_H_
#define OCEANBASE_OBSERVER_AI_COLUMN_OB_AI_COLUMN_WORKER_SERVICE_H_

#include "lib/ob_define.h"
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_sql_string.h"
#include "rootserver/ob_server_thread_helper.h"
#include "share/ai_column/ob_ai_column_struct.h"

namespace oceanbase
{
namespace observer
{

class ObAIColumnWorkerContext;

// Background worker service for AI columns (P2b).
//
// Each thread polls `__all_ai_column_task`, claims due tasks with an optimistic
// lease UPDATE, evaluates the row's user expression through an SPI inner SQL
// session, sends the AI request (system prompt from metadata), writes the user
// visible result column and finally deletes the task rows.
//
// It only touches normal columns (`result` columns are user visible) and the
// task inner table; hidden state/request columns are NOT written here (they are
// a P4 concern, see design/worker.md §10).
class ObAIColumnWorkerService : public rootserver::ObServerThreadHelper
{
public:
  static int server_module_init(ObAIColumnWorkerService *&service);

  ObAIColumnWorkerService();
  virtual ~ObAIColumnWorkerService();

  int init();
  int start();
  void do_work() override;
  void stop();
  void wait();
  void destroy();

  // for logservice::ObILocalLogHandler (inherited via ObServerThreadHelper)
  int activate() override;
  void deactivate() override;

public:
  static const int64_t WORKER_THREAD_CNT = 4;
  static const int64_t WORKER_IDLE_TIME_US = 50 * 1000;        // 50ms polling
  static const int64_t TASK_LEASE_TIME_US = 30 * 1000 * 1000;  // 30s lease
  static const int64_t MAX_TASK_BATCH = 4;
  static const int64_t MIN_RETRY_BACKOFF_US = 200 * 1000;      // >= 200ms
  static const int64_t MAX_RETRY_BACKOFF_US = 1000 * 1000;     // 1s cap

private:
  int process_round_(ObAIColumnWorkerContext &ctx);
  int poll_tasks_(ObAIColumnWorkerContext &ctx,
                  common::ObIArray<share::ObAIColumnTask> &tasks);
  int load_group_tasks_(ObAIColumnWorkerContext &ctx,
                        const int64_t table_id,
                        const int64_t request_id,
                        common::ObIArray<share::ObAIColumnTask> &tasks);
  int claim_task_(ObAIColumnWorkerContext &ctx,
                  const share::ObAIColumnTask &task,
                  bool &claimed);
  int claim_group_(ObAIColumnWorkerContext &ctx,
                   const int64_t table_id,
                   const int64_t request_id,
                   common::ObIArray<share::ObAIColumnTask> &group_tasks,
                   bool &claimed);
  int release_our_claims_(ObAIColumnWorkerContext &ctx,
                          const int64_t table_id,
                          const int64_t request_id);
  int release_claim_(ObAIColumnWorkerContext &ctx,
                     const share::ObAIColumnTask &task);
  int process_group_(ObAIColumnWorkerContext &ctx,
                     const int64_t table_id,
                     const int64_t request_id,
                     const common::ObIArray<share::ObAIColumnTask> &claimed_tasks);
  int read_user_content_(ObAIColumnWorkerContext &ctx,
                         const common::ObString &db_name,
                         const common::ObString &table_name,
                         const common::ObString &user_expr,
                         const common::ObString &rowkey_predicate,
                         common::ObString &content);
  int check_task_still_valid_(ObAIColumnWorkerContext &ctx,
                              const share::ObAIColumnTask &task,
                              bool &still_valid);
  int write_result_row_(ObAIColumnWorkerContext &ctx,
                        const common::ObString &db_name,
                        const common::ObString &table_name,
                        const common::ObString &rowkey_predicate,
                        const common::ObIArray<share::ObAIColumnSchema> &all_metas,
                        const common::ObIArray<int64_t> &column_ids,
                        const common::ObIArray<common::ObString> &values,
                        const common::ObIArray<bool> &is_embedding);
  int delete_group_tasks_(ObAIColumnWorkerContext &ctx,
                          const int64_t table_id,
                          const int64_t request_id);
  int retry_or_terminal_(ObAIColumnWorkerContext &ctx,
                         const share::ObAIColumnTask &task,
                         const int64_t max_retries,
                         const int ret_code);
  int reset_task_to_pending_(ObAIColumnWorkerContext &ctx,
                             const share::ObAIColumnTask &task);
  int append_string_literal_(common::ObSqlString &sql,
                             const common::ObString &str) const;
  int append_identifier_(common::ObSqlString &sql,
                         const common::ObString &name) const;

private:
  bool inited_;
  volatile bool stop_flag_;
  int64_t worker_id_;
  DISALLOW_COPY_AND_ASSIGN(ObAIColumnWorkerService);
};

} // namespace observer
} // namespace oceanbase

#endif // OCEANBASE_OBSERVER_AI_COLUMN_OB_AI_COLUMN_WORKER_SERVICE_H_
