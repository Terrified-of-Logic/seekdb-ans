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

// Local file scan -- logical operator.
//
// A local file table has no base table id, so the optimizer cannot use the
// generic access path machinery.  ObLogLocalFileScan carries the resolved
// ObLocalTableDef together with the set of raw expressions that read columns
// of the file.  Those expressions are produced by this operator itself; the
// physical plan later embeds the definition into ObLocalFileScanSpec so the
// executor can verify the file fingerprint before scanning it.

#ifndef OCEANBASE_SQL_OPTIMIZER_OB_LOG_LOCAL_FILE_SCAN_H_
#define OCEANBASE_SQL_OPTIMIZER_OB_LOG_LOCAL_FILE_SCAN_H_

#include "sql/optimizer/ob_logical_operator.h"
#include "sql/ob_local_file_common.h"

namespace oceanbase
{
namespace sql
{
class ObLogLocalFileScan : public ObLogicalOperator
{
public:
  explicit ObLogLocalFileScan(ObLogPlan &plan)
    : ObLogicalOperator(plan), local_table_id_(OB_INVALID_ID), local_table_name_(),
      table_desc_(nullptr), access_exprs_()
  {}
  virtual ~ObLogLocalFileScan() {}

  virtual uint64_t hash(uint64_t seed) const override;
  virtual int get_op_exprs(ObIArray<ObRawExpr *> &all_exprs) override;
  virtual int is_my_fixed_expr(const ObRawExpr *expr, bool &is_fixed) override;
  virtual int allocate_expr_post(ObAllocExprContext &ctx) override;
  virtual int get_plan_item_info(PlanText &plan_text, ObSqlPlanItem &plan_item) override;

  // Collects the column references that are satisfied by this operator.
  int collect_access_exprs();

  void set_table_id(const uint64_t table_id) { local_table_id_ = table_id; }
  uint64_t get_table_id() const { return local_table_id_; }
  void set_table_name(const common::ObString &table_name) { local_table_name_ = table_name; }
  const common::ObString &get_table_name() const { return local_table_name_; }
  void set_file_table_def(const ObLocalTableDef *def) { table_desc_ = def; }
  const ObLocalTableDef *get_file_table_def() const { return table_desc_; }
  ObIArray<ObRawExpr *> &get_access_exprs() { return access_exprs_; }

private:
  uint64_t local_table_id_;
  common::ObString local_table_name_;
  const ObLocalTableDef *table_desc_;
  common::ObSEArray<ObRawExpr *, 4, common::ModulePageAllocator, true> access_exprs_;
  DISALLOW_COPY_AND_ASSIGN(ObLogLocalFileScan);
};
} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_OPTIMIZER_OB_LOG_LOCAL_FILE_SCAN_H_
