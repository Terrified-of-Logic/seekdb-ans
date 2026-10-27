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

#ifndef OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_STRUCT_H_
#define OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_STRUCT_H_

#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_print_utils.h"
#include "lib/utility/ob_unify_serialize.h"

namespace oceanbase
{
namespace share
{

// AI column meta info, one row per AI column in __all_ai_column.
// NOTE: fields marked with [test] are directly queried by the public test suite;
//       others are required by our own implementation (see contract.md §3.1).
class ObAIColumnSchema
{
public:
  OB_UNIS_VERSION(1);
public:

  static const int64_t INVALID_ID = -1;

  ObAIColumnSchema() { reset(); }
  ~ObAIColumnSchema() = default;

  void reset()
  {
    table_id_ = INVALID_ID;
    column_id_ = INVALID_ID;
    column_name_.reset();
    normalized_expr_.reset();
    sync_mode_.reset();
    system_prompt_.reset();
    dep_columns_.reset();
    ai_model_name_.reset();
    user_expr_.reset();
    max_retries_ = 0;
    state_column_id_ = INVALID_ID;
    request_column_id_ = INVALID_ID;
    result_spec_column_id_ = INVALID_ID;
  }

  bool is_valid() const
  {
    return INVALID_ID != table_id_
        && INVALID_ID != column_id_
        && !column_name_.empty()
        && !normalized_expr_.empty();
  }

  TO_STRING_KV(K_(table_id), K_(column_id), K_(column_name), K_(normalized_expr),
               K_(sync_mode), K_(system_prompt), K_(dep_columns), K_(ai_model_name),
               K_(user_expr), K_(max_retries),
               K_(state_column_id), K_(request_column_id), K_(result_spec_column_id));

public:
  int64_t table_id_;             // [test]
  int64_t column_id_;            // [test] id of the user-visible result column
  common::ObString column_name_; // [test] result column name
  common::ObString normalized_expr_;  // [test] normalized AI expression text
  common::ObString sync_mode_;        // WITH(sync_mode='async')
  common::ObString system_prompt_;    // typed system prompt (json/bigint columns)
  common::ObString dep_columns_;      // JSON array text of dependency column names
  common::ObString ai_model_name_;    // AI func 1st arg: model name literal (resolver fills)
  common::ObString user_expr_;        // AI func 2nd arg: prompt expression text (resolver fills)
  int64_t max_retries_;               // [test]
  int64_t state_column_id_;           // [test] hidden state column id
  int64_t request_column_id_;         // [test] hidden request column id
  int64_t result_spec_column_id_;     // [test] hidden result spec column id
};

// Row-level AI column state, persisted in the hidden state column.
// The retry statement pseudo column `status` maps these to strings
// ('READY' / 'FAILED' / 'PENDING' ...), see contract.md §5.1.
struct ObAIColumnRowState
{
  enum State : int64_t
  {
    PENDING = 0,    // inserted/invalidated, waiting for worker
    RUNNING = 1,    // leased by worker
    READY = 2,      // result written
    FAILED = 3,     // terminal failure, result stays NULL
    RETRY_WAIT = 4, // failure, waiting for retry backoff
    MAX_STATE = 5
  };

  static const char *get_state_str(int64_t state)
  {
    const char *str = "UNKNOWN";
    switch (state) {
      case PENDING: str = "PENDING"; break;
      case RUNNING: str = "RUNNING"; break;
      case READY: str = "READY"; break;
      case FAILED: str = "FAILED"; break;
      case RETRY_WAIT: str = "RETRY_WAIT"; break;
      default: break;
    }
    return str;
  }

  static bool is_ready(int64_t state) { return READY == state; }
  // FAILED is "finished" from worker's point of view, but strict read treats it
  // as unfinished (see contract.md §5.3).
  static bool is_finished(int64_t state) { return READY == state || FAILED == state; }
};

// Task-level state, persisted in __all_ai_column_task.state.
// Only one value is hard-pinned by the public tests: state=3 (RETRY_WAIT) with
// retry_count=1, observed within a 100ms polling window. See contract.md §5.2.
struct ObAIColumnTaskState
{
  enum State : int64_t
  {
    INIT = 0,
    PENDING = 1,       // eligible for worker
    RUNNING = 2,       // leased by worker
    RETRY_WAIT = 3,    // failed, waiting for backoff (tests pin this value)
    DONE = 4,          // success (task rows are deleted in the same txn)
    TERMINAL_FAILED = 5, // retries exhausted (task rows are deleted)
    MAX_STATE = 6
  };
};

// One task row of __all_ai_column_task (one AI column of one affected row).
// [test] fields are directly queried by the public test suite.
class ObAIColumnTask
{
public:
  static const int64_t INVALID_ID = -1;

  ObAIColumnTask() { reset(); }
  ~ObAIColumnTask() = default;

  void reset()
  {
    tenant_id_ = INVALID_ID;
    table_id_ = INVALID_ID;
    column_id_ = INVALID_ID;
    request_id_ = INVALID_ID;
    rowkey_predicate_.reset();
    error_msg_.reset();
    eligible_ts_ = 0;
    state_ = ObAIColumnTaskState::INIT;
    retry_count_ = 0;
    lease_owner_ = 0;
    lease_expire_ts_ = 0;
  }

  bool is_valid() const
  {
    return INVALID_ID != table_id_ && INVALID_ID != column_id_ && !rowkey_predicate_.empty();
  }

  TO_STRING_KV(K_(tenant_id), K_(table_id), K_(column_id), K_(request_id),
               K_(rowkey_predicate), K_(eligible_ts), K_(state), K_(retry_count),
               K_(lease_owner), K_(lease_expire_ts), K_(error_msg));

public:
  int64_t tenant_id_;
  int64_t table_id_;          // [test]
  int64_t column_id_;         // which AI column this task belongs to
  int64_t request_id_;        // [test] task group id, shared by all AI columns of one row
  common::ObString rowkey_predicate_;  // [test] e.g. "`id` = 2"
  common::ObString error_msg_;         // last error message (for FAILED / debug)
  int64_t eligible_ts_;       // [test] microseconds; task eligible time
  int64_t state_;             // [test] ObAIColumnTaskState
  int64_t retry_count_;       // [test] failed attempt count
  int64_t lease_owner_;       // worker instance identity (for crash recovery)
  int64_t lease_expire_ts_;   // lease expiration (microseconds)
};

} // namespace share
} // namespace oceanbase

#endif // OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_STRUCT_H_
