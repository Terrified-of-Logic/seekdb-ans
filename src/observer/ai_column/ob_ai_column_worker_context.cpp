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

#include "observer/ai_column/ob_ai_column_worker_context.h"

#include <new>
#include "observer/ob_inner_sql_connection.h"
#include "observer/ob_server.h"
#include "share/rc/ob_server_runtime.h"
#include "share/ob_define.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_schema_struct.h"
#include "share/system_variable/ob_sys_var_class_type.h"
#include "sql/executor/ob_worker_session_guard.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/session/ob_sql_session_mgr.h"
#include "common/sql_mode/ob_sql_mode.h"
#include "common/mysqlclient/ob_mysql_result.h"

namespace oceanbase
{
using namespace common;
using namespace sql;
using namespace share;
using namespace share::schema;

namespace observer
{

ObAIColumnWorkerContext::ObAIColumnWorkerContext()
    : inited_(false),
      free_session_ctx_(),
      session_info_(nullptr),
      worker_session_guard_(nullptr),
      conn_guard_(),
      conn_(nullptr),
      allocator_()
{
}

ObAIColumnWorkerContext::~ObAIColumnWorkerContext()
{
  destroy();
}

int ObAIColumnWorkerContext::create_session_()
{
  int ret = OB_SUCCESS;
  uint32_t sid = ObSQLSessionInfo::INVALID_SESSID;
  ObSQLSessionMgr &session_mgr = OBSERVER.get_sql_session_mgr();
  if (OB_FAIL(session_mgr.create_sessid(sid))) {
    LOG_WARN("fail to create sessid", KR(ret));
  } else if (OB_FAIL(session_mgr.create_session(sid, session_info_))) {
    session_info_ = nullptr;
    LOG_WARN("fail to create session", KR(ret));
  } else if (OB_ISNULL(session_info_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("null session info", KR(ret));
  } else {
    free_session_ctx_.sessid_ = sid;
  }
  return ret;
}

int ObAIColumnWorkerContext::init_session_env_()
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard schema_guard;
  ObSEArray<const ObUserInfo *, 1> user_infos;
  const ObUserInfo *user_info = nullptr;
  const ObDatabaseSchema *database_schema = nullptr;
  ObPrivSet db_priv_set = OB_PRIV_SET_EMPTY;
  ObObj sql_mode;
  sql_mode.set_uint(ObUInt64Type, DEFAULT_MYSQL_MODE);
  if (OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("schema service not ready", KR(ret));
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(schema_guard))) {
    LOG_WARN("fail to get runtime schema guard", KR(ret));
  } else if (OB_FAIL(schema_guard.get_user_info(ObString("root"), user_infos))) {
    LOG_WARN("fail to get root user", KR(ret));
  } else if (user_infos.count() >= 1 && OB_NOT_NULL(user_infos.at(0))) {
    user_info = user_infos.at(0);
  } else if (OB_FAIL(schema_guard.get_user_info(ObString("admin"), user_infos))) {
    LOG_WARN("fail to get admin user", KR(ret));
  } else if (user_infos.count() >= 1 && OB_NOT_NULL(user_infos.at(0))) {
    user_info = user_infos.at(0);
  } else {
    ret = OB_USER_NOT_EXIST;
    LOG_WARN("no privileged user for ai column worker", KR(ret));
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(schema_guard.get_database_schema(
                 ObString(OB_SYS_DATABASE_NAME), database_schema))) {
    LOG_WARN("fail to get oceanbase database schema", KR(ret));
  } else if (OB_ISNULL(database_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("null database schema", KR(ret));
  } else if (OB_ISNULL(session_info_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("session info not created", KR(ret));
  } else {
    const bool print_info_log = true;
    const bool use_server_defaults = true;
    session_info_->set_inner_session();
    OZ (session_info_->load_default_sys_variable(print_info_log, use_server_defaults));
    OZ (session_info_->update_max_packet_size());
    OZ (session_info_->init_runtime(OB_SERVER_RUNTIME_NAME));
    OZ (session_info_->load_all_sys_vars(schema_guard));
    OZ (session_info_->update_sys_variable(SYS_VAR_SQL_MODE, sql_mode));
    OZ (session_info_->set_default_database(database_schema->get_database_name_str()));
    OX (session_info_->set_database_id(database_schema->get_database_id()));
    OZ (session_info_->set_user(user_info->get_user_name(),
                                user_info->get_host_name_str(),
                                user_info->get_user_id()));
    OX (session_info_->set_priv_user_id(user_info->get_user_id()));
    OX (session_info_->set_user_priv_set(user_info->get_priv_set()));
    OZ (schema_guard.get_db_priv_set(user_info->get_user_id(),
                                     database_schema->get_database_name_str(),
                                     db_priv_set));
    OX (session_info_->set_db_priv_set(db_priv_set));
    OX (session_info_->get_enable_role_array().reuse());
    for (int64_t i = 0; OB_SUCC(ret) && i < user_info->get_role_id_array().count(); ++i) {
      if (0 == user_info->get_disable_option(user_info->get_role_id_option_array().at(i))) {
        OZ (session_info_->get_enable_role_array().push_back(
            user_info->get_role_id_array().at(i)));
      }
    }
  }
  return ret;
}

int ObAIColumnWorkerContext::init()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("worker context already inited", KR(ret));
  } else if (OB_FAIL(create_session_())) {
    LOG_WARN("fail to create worker session", KR(ret));
  } else if (OB_FAIL(init_session_env_())) {
    LOG_WARN("fail to init worker session env, will retry later", KR(ret));
    destroy();
  } else {
    inited_ = true;
  }
  return ret;
}

void ObAIColumnWorkerContext::destroy()
{
  end_op();
  if (OB_NOT_NULL(session_info_)) {
    session_info_->set_session_sleep();
    ObSQLSessionMgr *session_mgr = ::oceanbase::share::server_service<ObSQLSessionMgr>();
    if (OB_NOT_NULL(session_mgr)) {
      session_mgr->revert_session(session_info_);
      session_mgr->free_session(free_session_ctx_);
    }
    session_info_ = nullptr;
  }
  inited_ = false;
}

int ObAIColumnWorkerContext::begin_op()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("worker context not inited", KR(ret));
  } else if (OB_ISNULL(session_info_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("null session info", KR(ret));
  } else if (OB_NOT_NULL(worker_session_guard_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("op already begun", KR(ret));
  } else {
    worker_session_guard_ = new (std::nothrow) ObWorkerSessionGuard(session_info_);
    if (OB_ISNULL(worker_session_guard_)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("fail to alloc worker session guard", KR(ret));
    } else if (OB_FAIL(ObInnerSQLConnection::create_spi_connection_with_external_session(
                   session_info_, conn_guard_))) {
      LOG_WARN("fail to create spi connection", KR(ret));
    } else {
      conn_ = static_cast<ObInnerSQLConnection *>(conn_guard_.get_ptr());
      if (OB_ISNULL(conn_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("null spi connection", KR(ret));
      } else {
        conn_->set_check_priv(false);
      }
    }
  }
  return ret;
}

void ObAIColumnWorkerContext::end_op()
{
  if (OB_NOT_NULL(conn_)) {
    conn_->set_check_priv(false);
  }
  conn_ = nullptr;
  conn_guard_.reset();
  if (OB_NOT_NULL(worker_session_guard_)) {
    delete worker_session_guard_;
    worker_session_guard_ = nullptr;
  }
}

int ObAIColumnWorkerContext::execute_write(const common::ObString &sql,
                                           int64_t &affected_rows)
{
  int ret = OB_SUCCESS;
  affected_rows = 0;
  if (OB_ISNULL(conn_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("spi connection not ready", KR(ret));
  } else if (OB_FAIL(conn_->execute_write(sql, affected_rows))) {
    LOG_WARN("fail to execute write sql", KR(ret), K(sql));
  }
  return ret;
}

int ObAIColumnWorkerContext::execute_read(const common::ObString &sql,
                                          common::ObISQLClient::ReadResult &res)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(conn_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("spi connection not ready", KR(ret));
  } else if (OB_FAIL(conn_->execute_read(sql, res))) {
    LOG_WARN("fail to execute read sql", KR(ret), K(sql));
  }
  return ret;
}

int ObAIColumnWorkerContext::get_runtime_schema_guard(
    share::schema::ObSchemaGetterGuard &guard) const
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("schema service not ready", KR(ret));
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
    LOG_WARN("fail to get runtime schema guard", KR(ret));
  }
  return ret;
}

} // namespace observer
} // namespace oceanbase
