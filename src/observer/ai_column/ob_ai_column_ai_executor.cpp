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

#include "observer/ai_column/ob_ai_column_ai_executor.h"

#include "observer/ob_server.h"
#include "share/ai_column/ob_ai_column_define.h"
#include "share/ai_service/ob_ai_service_struct.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_client.h"

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace sql;

namespace observer
{

int ObAIColumnAIExecutor::execute(common::ObIAllocator &allocator,
                                  share::schema::ObSchemaGetterGuard &schema_guard,
                                  const share::ObAIColumnSchema &ai_column,
                                  const common::ObString &user_content,
                                  bool &is_embedding,
                                  common::ObString &result)
{
  int ret = OB_SUCCESS;
  ObAIFuncExprInfo *info = nullptr;
  share::ObAiModelEndpointInfo endpoint_info;
  ObAIFuncClient client;
  ObArray<ObString> headers;
  ObJsonObject *body = nullptr;
  ObJsonObject *response = nullptr;
  ObIJsonBase *result_base = nullptr;
  ObString unencrypted_access_key;
  ObString request_model_name;
  ObString url;
  ObString system_prompt = ai_column.system_prompt_;
  ObString content = user_content;
  is_embedding = false;
  result.reset();
  if (OB_UNLIKELY(ai_column.ai_model_name_.empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("ai model name is empty in metadata", KR(ret), K(ai_column));
  } else if (OB_ISNULL(ai_column.user_expr_.ptr()) || ai_column.user_expr_.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("user expr is empty in metadata", KR(ret), K(ai_column));
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(
                 allocator, ai_column.ai_model_name_, schema_guard, info))) {
    LOG_WARN("fail to get ai func info", KR(ret), K(ai_column.ai_model_name_));
  } else if (OB_ISNULL(info)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("null ai func info", KR(ret));
  } else if (OB_FAIL(OBSERVER.resolve_by_model_name(
                 ai_column.ai_model_name_, allocator, endpoint_info, false /*check_access*/))) {
    LOG_WARN("fail to resolve ai model endpoint", KR(ret), K(ai_column.ai_model_name_));
  } else {
    request_model_name = endpoint_info.get_request_model_name();
    if (request_model_name.empty()) {
      request_model_name = info->model_;
    }
    url = endpoint_info.get_url();
  }
  if (OB_FAIL(ret)) {
  } else if (EndpointType::COMPLETION == info->type_) {
    ObAIFuncIComplete *complete_provider = nullptr;
    if (OB_FAIL(ObAIFuncUtils::get_complete_provider(
            allocator, endpoint_info.get_provider(), complete_provider))) {
      LOG_WARN("fail to get complete provider", KR(ret), K(endpoint_info.get_provider()));
    } else if (OB_FAIL(endpoint_info.get_unencrypted_access_key(
                   allocator, unencrypted_access_key))) {
      LOG_WARN("fail to get access key", KR(ret));
    } else if (OB_FAIL(complete_provider->get_header(
                   allocator, unencrypted_access_key, headers))) {
      LOG_WARN("fail to build request header", KR(ret));
    } else if (OB_FAIL(complete_provider->get_body(
                   allocator, request_model_name, system_prompt, content, nullptr, body))) {
      LOG_WARN("fail to build completion request body", KR(ret));
    } else if (OB_FAIL(client.send_post(allocator, url, headers, body, response))) {
      LOG_WARN("fail to send completion request", KR(ret), K(url));
    } else if (OB_FAIL(complete_provider->parse_output(allocator, response, result_base))) {
      LOG_WARN("fail to parse completion output", KR(ret));
    } else if (OB_FAIL(ObAIFuncJsonUtils::print_json_to_str(
                   allocator, result_base, result))) {
      LOG_WARN("fail to print completion result", KR(ret));
    }
  } else if (EndpointType::DENSE_EMBEDDING == info->type_) {
    ObAIFuncIEmbed *embed_provider = nullptr;
    ObArray<ObString> contents;
    is_embedding = true;
    if (OB_FAIL(contents.push_back(content))) {
      LOG_WARN("fail to push embedding content", KR(ret));
    } else if (OB_FAIL(ObAIFuncUtils::get_embed_provider(
                   allocator, endpoint_info.get_provider(), embed_provider))) {
      LOG_WARN("fail to get embed provider", KR(ret), K(endpoint_info.get_provider()));
    } else if (OB_FAIL(endpoint_info.get_unencrypted_access_key(
                   allocator, unencrypted_access_key))) {
      LOG_WARN("fail to get access key", KR(ret));
    } else if (OB_FAIL(embed_provider->get_header(
                   allocator, unencrypted_access_key, headers))) {
      LOG_WARN("fail to build request header", KR(ret));
    } else if (OB_FAIL(embed_provider->get_body(
                   allocator, request_model_name, contents, nullptr, body))) {
      LOG_WARN("fail to build embedding request body", KR(ret));
    } else if (OB_FAIL(client.send_post(allocator, url, headers, body, response))) {
      LOG_WARN("fail to send embedding request", KR(ret), K(url));
    } else if (OB_FAIL(embed_provider->parse_output(allocator, response, result_base))) {
      LOG_WARN("fail to parse embedding output", KR(ret));
    } else if (OB_ISNULL(result_base)
               || ObJsonNodeType::J_ARRAY != result_base->json_type()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected embedding result type", KR(ret));
    } else {
      ObJsonArray *result_array = static_cast<ObJsonArray *>(result_base);
      if (1 != result_array->element_count()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected embedding result count", KR(ret),
                 K(result_array->element_count()));
      } else if (OB_FAIL(ObAIFuncJsonUtils::print_json_to_str(
                     allocator, result_array->get_value(0), result))) {
        LOG_WARN("fail to print embedding result", KR(ret));
      }
    }
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("unsupported ai column type", KR(ret), K(info->type_));
  }
  return ret;
}

} // namespace observer
} // namespace oceanbase
