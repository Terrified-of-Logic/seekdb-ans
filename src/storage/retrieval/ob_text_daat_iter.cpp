/*
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

#define USING_LOG_PREFIX STORAGE

#include "ob_text_daat_iter.h"

namespace oceanbase
{
namespace storage
{
int ObTextDaaTIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRDaaTIterImpl::init(*param.base_param_, *param.dim_iters_,
                                            *param.allocator_, *param.relevance_collector_))) {
  } else if (param.base_param_->need_calc_relevance()
             && OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  } else {
    mode_flag_ = param.mode_flag_;
    function_lookup_mode_ = param.function_lookup_mode_;
    min_should_match_ = param.min_should_match_;
  }
  return ret;
}

void ObTextDaaTIter::reset()
{
  id_only_state_ = ID_ONLY_UNCHECKED;
  id_only_cursors_ = nullptr;
  id_only_keys_ = nullptr;
  id_only_out_bytes_ = nullptr;
  id_only_binary_ = false;
  id_only_batch_size_ = 0;
  bm25_param_estimator_.reset();
  // this class can know the type of dim_iters_, but ObSRDaaTIterImpl maybe not
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRDaaTIterImpl::reset();
}

void ObTextDaaTIter::reuse(const bool switch_tablet)
{
  if ((OB_NOT_NULL(dim_iters_) && 0 == dim_iters_->count())) {
    // do nothing
  } else {
    bm25_param_estimator_.reuse(switch_tablet);
  }
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reuse();
    }
  }
  reset_id_only_cursors();
  ObSRDaaTIterImpl::reuse(switch_tablet);
}

int ObTextDaaTIter::pre_process()
{
  int ret = OB_SUCCESS;
  if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (iter_param_->need_project_relevance()) {
    if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
    }
  }
  return ret;
}

int ObTextDaaTIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  count = 0;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(iter_param_) || OB_ISNULL(dim_iters_)
             || OB_ISNULL(iter_param_->limit_param_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ID_ONLY_UNCHECKED == id_only_state_ && OB_FAIL(check_id_only_mode())) {
  } else if (OB_UNLIKELY(0 == capacity)) {
  } else if (iter_param_->limit_param_->is_valid()
             && output_row_cnt_ >= iter_param_->limit_param_->limit_) {
    // the limit pushed into the retrieval has already been satisfied
    ret = OB_ITER_END;
  } else if (0 == dim_iters_->count()) {
    ret = OB_ITER_END;
  } else if (ID_ONLY_ENABLED == id_only_state_) {
    ret = merge_id_only_rows(capacity, count);
  } else {
    ret = ObSRDaaTIterImpl::get_next_rows(capacity, count);
  }
  return ret;
}

// The id-only union merge scans every posting-list cursor twice per output row (find the
// minimum, then advance the matched ones), so its per-row cost is linear in the number of
// MATCH terms, while the heap based generic DaaT merge is logarithmic. The fast path is
// therefore only enabled for short queries (the benchmark and the common search shapes use
// 1-3 terms); deeper queries keep the generic, proven merge path.
static constexpr int64_t K_ID_ONLY_MAX_DIM_CNT = 8;

// The union fast path is only taken for the plain natural language MATCH predicate, where the
// matched rows are exactly the union of the posting lists with no observable score, no filter on
// it and no pushed down topk. Every other shape keeps the generic DaaT implementation.
int ObTextDaaTIter::check_id_only_mode()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(iter_param_) || OB_ISNULL(iter_param_->eval_ctx_) || OB_ISNULL(dim_iters_)
      || OB_ISNULL(iter_param_->id_proj_expr_) || OB_ISNULL(iter_allocator_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (!iter_param_->need_calc_relevance()
             && !iter_param_->need_project_relevance()
             && !iter_param_->need_filter()
             && !iter_param_->need_pushdown_topk()
             && !function_lookup_mode_
             && sql::ObMatchAgainstMode::NATURAL_LANGUAGE_MODE == mode_flag_
             && dim_iters_->count() > 0
             && dim_iters_->count() <= K_ID_ONLY_MAX_DIM_CNT
             && iter_param_->eval_ctx_->is_vectorized()
             && ObTextDocIdKey::is_supported(iter_param_->id_proj_expr_->datum_meta_)) {
    const int64_t batch_size = OB_MAX(iter_param_->max_batch_size_,
                                      iter_param_->eval_ctx_->max_batch_size_);
    // heap tables expose the document id as a fixed 16 byte binary value instead of an integer
    const bool is_binary_id = common::ObUInt64Type != iter_param_->id_proj_expr_->datum_meta_.type_;
    if (OB_UNLIKELY(batch_size <= 0)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_ISNULL(id_only_cursors_ = static_cast<IdOnlyCursor *>(
                   iter_allocator_->alloc(sizeof(IdOnlyCursor) * dim_iters_->count())))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_ISNULL(id_only_keys_ = static_cast<ObTextDocIdKey *>(
                   iter_allocator_->alloc(sizeof(ObTextDocIdKey) * batch_size)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (is_binary_id
               && OB_ISNULL(id_only_out_bytes_ = static_cast<char *>(iter_allocator_->alloc(
                     ObTextDocIdKey::BINARY_ID_LEN * batch_size)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      id_only_binary_ = is_binary_id;
      id_only_batch_size_ = batch_size;
      id_only_state_ = ID_ONLY_ENABLED;
      reset_id_only_cursors();
      LOG_DEBUG("score-free MATCH uses the id only union path",
                K(dim_iters_->count()), K(batch_size), K(is_binary_id), KPC(iter_param_));
    }
  } else {
    id_only_state_ = ID_ONLY_DISABLED;
  }
  return ret;
}

void ObTextDaaTIter::reset_id_only_cursors()
{
  if (OB_NOT_NULL(id_only_cursors_) && OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      id_only_cursors_[i].keys_ = nullptr;
      id_only_cursors_[i].count_ = 0;
      id_only_cursors_[i].pos_ = 0;
      id_only_cursors_[i].end_ = false;
    }
  }
}

int ObTextDaaTIter::load_id_only_cursor(const int64_t iter_idx)
{
  int ret = OB_SUCCESS;
  ObTextRetrievalDaaTTokenIter *dim_iter = nullptr;
  const ObTextDocIdKey *keys = nullptr;
  int64_t id_cnt = 0;
  if (OB_ISNULL(id_only_cursors_) || OB_ISNULL(dim_iters_)
      || OB_UNLIKELY(iter_idx < 0 || iter_idx >= dim_iters_->count())) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    IdOnlyCursor &cursor = id_only_cursors_[iter_idx];
    cursor.keys_ = nullptr;
    cursor.count_ = 0;
    cursor.pos_ = 0;
    if (OB_ISNULL(dim_iter = static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(iter_idx)))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(dim_iter->get_next_id_key_batch(keys, id_cnt))) {
      if (OB_UNLIKELY(OB_ITER_END != ret)) {
        LOG_WARN("failed to load posting list document ids", K(ret), K(iter_idx));
      } else {
        cursor.end_ = true;
        ret = OB_SUCCESS;
      }
    } else if (OB_UNLIKELY(id_cnt <= 0 || nullptr == keys)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      cursor.keys_ = keys;
      cursor.count_ = id_cnt;
    }
  }
  return ret;
}

int ObTextDaaTIter::merge_id_only_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  const int64_t dim_cnt = dim_iters_->count();
  // Same output batch bound as the generic DaaT path, so the projected row indexes stay
  // inside the datum arrays of the evaluation context.
  const int64_t batch_size = MIN(capacity, iter_param_->max_batch_size_);
  const common::ObLimitParam *limit_param = iter_param_->limit_param_;
  const bool limited = limit_param->is_valid();
  count = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < dim_cnt; ++i) {
    const IdOnlyCursor &cursor = id_only_cursors_[i];
    if (!cursor.end_ && cursor.pos_ >= cursor.count_ && OB_FAIL(load_id_only_cursor(i))) {
    }
  }
  while (OB_SUCC(ret) && count < batch_size) {
    ObTextDocIdKey min_key;
    min_key.hi_ = 0;
    min_key.lo_ = 0;
    bool found = false;
    for (int64_t i = 0; i < dim_cnt; ++i) {
      const IdOnlyCursor &cursor = id_only_cursors_[i];
      if (!cursor.end_) {
        const ObTextDocIdKey &key = cursor.keys_[cursor.pos_];
        if (!found || key < min_key) {
          min_key = key;
          found = true;
        }
      }
    }
    if (OB_UNLIKELY(!found)) {
      ret = OB_ITER_END;
    } else {
      int64_t matched_dim_cnt = 0;
      for (int64_t i = 0; OB_SUCC(ret) && i < dim_cnt; ++i) {
        IdOnlyCursor &cursor = id_only_cursors_[i];
        if (!cursor.end_ && cursor.keys_[cursor.pos_] == min_key) {
          ++matched_dim_cnt;
          if (++cursor.pos_ >= cursor.count_ && OB_FAIL(load_id_only_cursor(i))) {
          }
        }
      }
      if (OB_FAIL(ret)) {
      } else if (min_should_match_ > 0 && matched_dim_cnt < min_should_match_) {
        // the same document has to occur in enough posting lists to be a result
      } else if (FALSE_IT(++input_row_cnt_)) {
      } else if (limited && input_row_cnt_ <= limit_param->offset_) {
        // skipped by the offset of the pushed down limit
      } else {
        id_only_keys_[count++] = min_key;
        ++output_row_cnt_;
        if (limited && output_row_cnt_ >= limit_param->limit_) {
          ret = OB_ITER_END;
        }
      }
    }
  }
  if ((OB_SUCC(ret) || OB_ITER_END == ret) && count > 0) {
    ObEvalCtx *eval_ctx = iter_param_->eval_ctx_;
    ObExpr *id_proj_expr = iter_param_->id_proj_expr_;
    ObEvalCtx::BatchInfoScopeGuard guard(*eval_ctx);
    sql::ObBitVector &evaluated_flags = id_proj_expr->get_evaluated_flags(*eval_ctx);
    for (int64_t i = 0; i < count; ++i) {
      guard.set_batch_idx(i);
      ObDatum &id_datum = id_proj_expr->locate_datum_for_write(*eval_ctx);
      if (id_only_binary_) {
        // the projection buffer has to live until the batch has been consumed, so every row of
        // the batch gets its own slot
        char *buf = id_only_out_bytes_ + i * ObTextDocIdKey::BINARY_ID_LEN;
        uint64_t hi = id_only_keys_[i].hi_;
        uint64_t lo = id_only_keys_[i].lo_;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        hi = __builtin_bswap64(hi);
        lo = __builtin_bswap64(lo);
#endif
        MEMCPY(buf, &hi, sizeof(uint64_t));
        MEMCPY(buf + sizeof(uint64_t), &lo, sizeof(uint64_t));
        id_datum.set_string(buf, ObTextDocIdKey::BINARY_ID_LEN);
      } else {
        id_datum.set_uint(id_only_keys_[i].lo_);
      }
      evaluated_flags.set(i);
    }
    id_proj_expr->set_evaluated_projected(*eval_ctx);
  }
  return ret;
}

ObTextBMWIter::ObTextBMWIter()
  : ObSRBMWIterImpl(),
    bm25_param_estimator_() {}

void ObTextBMWIter::reuse(const bool switch_tablet)
{
  bm25_param_estimator_.reuse(switch_tablet);
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRBMWIterImpl::reuse(switch_tablet);
}

void ObTextBMWIter::reset()
{
  bm25_param_estimator_.reset();
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRBMWIterImpl::reset();
}

int ObTextBMWIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRBMWIterImpl::init(*param.base_param_, *param.dim_iters_,
                                           *param.allocator_, *param.relevance_collector_))) {
  } else if (param.base_param_->need_calc_relevance()
             && OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  }
  return ret;
}

int ObTextBMWIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (iter_param_->need_calc_relevance()
             && OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
  }

  if (FAILEDx(ObSRBMWIterImpl::get_next_rows(capacity, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }
  return ret;
}

int ObTextBMWIter::init_before_wand_process()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!bm25_param_estimator_.is_estimated())) {
    ret = OB_ERR_UNEXPECTED;
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    ObTextRetrievalBlockMaxIter *block_max_iter = static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i));
    if (OB_FAIL(block_max_iter->init_block_max_iter(
        bm25_param_estimator_.get_total_doc_cnt(), bm25_param_estimator_.get_avg_doc_token_cnt()))) {
    }
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
