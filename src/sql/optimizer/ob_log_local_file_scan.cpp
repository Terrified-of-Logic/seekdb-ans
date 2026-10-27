/*
 * Copyright (c) 2026 OceanBase.
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

// Local file scan -- logical operator implementation.

#define USING_LOG_PREFIX SQL_OPT
#include "sql/optimizer/ob_log_local_file_scan.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace sql
{
int ObLogLocalFileScan::collect_access_exprs()
{
  int ret = OB_SUCCESS;
  const ObDMLStmt *stmt = get_stmt();
  if (OB_ISNULL(stmt)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("logical file scan is not attached to a statement", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < stmt->get_column_size(); ++i) {
      const ColumnItem *column = stmt->get_column_item(i);
      if (OB_ISNULL(column) || OB_ISNULL(column->expr_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("column item of the local file table is null", K(ret), K(i));
      } else if (column->table_id_ == local_table_id_
                 && column->expr_->is_explicited_reference()) {
        if (OB_FAIL(access_exprs_.push_back(column->expr_))) {
          LOG_WARN("failed to record a local file access expression", K(ret), K(i));
        }
      }
    }
  }
  return ret;
}

int ObLogLocalFileScan::get_op_exprs(ObIArray<ObRawExpr *> &all_exprs)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(collect_access_exprs())) {
    LOG_WARN("failed to collect local file access expressions", K(ret));
  } else if (OB_FAIL(append(all_exprs, access_exprs_))) {
    LOG_WARN("failed to append local file access expressions", K(ret));
  } else if (OB_FAIL(ObLogicalOperator::get_op_exprs(all_exprs))) {
    LOG_WARN("failed to append logical operator expressions", K(ret));
  }
  return ret;
}

int ObLogLocalFileScan::allocate_expr_post(ObAllocExprContext &ctx)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < access_exprs_.count(); ++i) {
    ObRawExpr *expr = access_exprs_.at(i);
    if (OB_FAIL(mark_expr_produced(expr, branch_id_, id_, ctx))) {
      LOG_WARN("failed to mark a local file expression produced", K(ret));
    } else if (!is_plan_root() && OB_FAIL(add_var_to_array_no_dup(output_exprs_, expr))) {
      LOG_WARN("failed to add a local file output expression", K(ret));
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(ObLogicalOperator::allocate_expr_post(ctx))) {
    LOG_WARN("failed to allocate local file expressions", K(ret));
  }
  return ret;
}

int ObLogLocalFileScan::get_plan_item_info(PlanText &plan_text, ObSqlPlanItem &plan_item)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObLogicalOperator::get_plan_item_info(plan_text, plan_item))) {
    LOG_WARN("failed to describe the local file scan plan item", K(ret));
  } else {
    BUF_PRINT_OB_STR(local_table_name_.ptr(), local_table_name_.length(),
                     plan_item.object_alias_, plan_item.object_alias_len_);
    BUF_PRINT_STR("FILE SCAN", plan_item.object_type_, plan_item.object_type_len_);
    if (OB_NOT_NULL(table_desc_)) {
      // The canonical path is exposed as the "special predicate" column of the
      // plan so EXPLAIN shows which file the scan actually touches.
      BUF_PRINT_OB_STR(table_desc_->canonical_path_.ptr(),
                       table_desc_->canonical_path_.length(),
                       plan_item.special_predicates_,
                       plan_item.special_predicates_len_);
    }
  }
  return ret;
}

uint64_t ObLogLocalFileScan::hash(uint64_t seed) const
{
  seed = do_hash(local_table_name_, seed);
  if (OB_NOT_NULL(table_desc_)) {
    seed = do_hash(table_desc_->canonical_path_, seed);
    seed = do_hash(table_desc_->device_, seed);
    seed = do_hash(table_desc_->inode_, seed);
    seed = do_hash(table_desc_->file_size_, seed);
    seed = do_hash(table_desc_->modified_time_ns_, seed);
  }
  return ObLogicalOperator::hash(seed);
}

int ObLogLocalFileScan::is_my_fixed_expr(const ObRawExpr *expr, bool &is_fixed)
{
  is_fixed = ObOptimizerUtil::find_item(access_exprs_, expr);
  return OB_SUCCESS;
}
} // namespace sql
} // namespace oceanbase
