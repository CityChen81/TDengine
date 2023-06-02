/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "inc/tsdbIter.h"

// STsdbIter ================
struct STsdbIter {
  struct {
    bool noMoreData;
  } ctx[1];
  SRowInfo    row[1];
  SRBTreeNode node[1];
  EIterType   type;
  union {
    struct {
      SSttSegReader      *reader;
      const TSttBlkArray *sttBlkArray;
      int32_t             sttBlkArrayIdx;
      SBlockData          bData[1];
      int32_t             iRow;
    } stt[1];
    struct {
      SDataFileReader      *reader;
      const TBlockIdxArray *blockIdxArray;
      int32_t               blockIdxArrayIdx;
      const TDataBlkArray  *dataBlkArray;
      int32_t               dataBlkArrayIdx;
      SBlockData            bData[1];
      int32_t               iRow;
    } data[1];
    struct {
      SMemTable  *memt;
      TSDBKEY     from[1];
      SRBTreeIter iter[1];
      STbData    *tbData;
      STbDataIter tbIter[1];
    } memt[1];
  };
};

static int32_t tsdbSttIterNext(STsdbIter *iter, const TABLEID *tbid) {
  while (!iter->ctx->noMoreData) {
    for (; iter->stt->iRow < iter->stt->bData->nRow; iter->stt->iRow++) {
      iter->row->suid = iter->stt->bData->suid;
      iter->row->uid = iter->stt->bData->uid ? iter->stt->bData->uid : iter->stt->bData->aUid[iter->stt->iRow];

      if (tbid && iter->row->suid == tbid->suid && iter->row->uid == tbid->uid) {
        continue;
      }

      iter->row->row = tsdbRowFromBlockData(iter->stt->bData, iter->stt->iRow);
      iter->stt->iRow++;
      goto _exit;
    }

    if (iter->stt->sttBlkArrayIdx >= TARRAY2_SIZE(iter->stt->sttBlkArray)) {
      iter->ctx->noMoreData = true;
      break;
    }

    for (; iter->stt->sttBlkArrayIdx < TARRAY2_SIZE(iter->stt->sttBlkArray); iter->stt->sttBlkArrayIdx++) {
      const SSttBlk *sttBlk = TARRAY2_GET_PTR(iter->stt->sttBlkArray, iter->stt->sttBlkArrayIdx);

      if (tbid && tbid->suid == sttBlk->suid && tbid->uid == sttBlk->minUid && tbid->uid == sttBlk->maxUid) {
        continue;
      }

      int32_t code = tsdbSttFileReadDataBlock(iter->stt->reader, sttBlk, iter->stt->bData);
      if (code) return code;

      iter->stt->iRow = 0;
      iter->stt->sttBlkArrayIdx++;
      break;
    }
  }

_exit:
  return 0;
}

static int32_t tsdbDataIterNext(STsdbIter *iter, const TABLEID *tbid) {
  int32_t code;

  while (!iter->ctx->noMoreData) {
    for (;;) {
      for (; iter->data->iRow < iter->data->bData->nRow; iter->data->iRow++) {
        if (tbid && tbid->suid == iter->data->bData->suid && tbid->uid == iter->data->bData->uid) {
          iter->data->iRow = iter->data->bData->nRow;
          break;
        }

        iter->row->row = tsdbRowFromBlockData(iter->data->bData, iter->data->iRow);
        iter->data->iRow++;
        goto _exit;
      }

      if (iter->data->dataBlkArray == NULL || iter->data->dataBlkArrayIdx >= TARRAY2_SIZE(iter->data->dataBlkArray)) {
        break;
      }

      for (; iter->data->dataBlkArray && iter->data->dataBlkArrayIdx < TARRAY2_SIZE(iter->data->dataBlkArray);
           iter->data->dataBlkArrayIdx++) {
        const SDataBlk *dataBlk = TARRAY2_GET_PTR(iter->data->dataBlkArray, iter->data->dataBlkArrayIdx);

        if (tbid) {
          const SBlockIdx *blockIdx = TARRAY2_GET_PTR(iter->data->blockIdxArray, iter->data->blockIdxArrayIdx - 1);

          if (tbid->suid == blockIdx->suid && tbid->uid == blockIdx->uid) {
            iter->data->dataBlkArrayIdx = TARRAY2_SIZE(iter->data->dataBlkArray);
            break;
          }
        }

        code = tsdbDataFileReadDataBlock(iter->data->reader, dataBlk, iter->data->bData);
        if (code) return code;

        iter->data->iRow = 0;
        iter->data->dataBlkArrayIdx++;
        break;
      }
    }

    if (iter->data->blockIdxArrayIdx >= TARRAY2_SIZE(iter->data->blockIdxArray)) {
      iter->ctx->noMoreData = true;
      break;
    }

    for (; iter->data->blockIdxArrayIdx < TARRAY2_SIZE(iter->data->blockIdxArray); iter->data->blockIdxArrayIdx++) {
      const SBlockIdx *blockIdx = TARRAY2_GET_PTR(iter->data->blockIdxArray, iter->data->blockIdxArrayIdx);

      if (tbid && tbid->suid == blockIdx->suid && tbid->uid == blockIdx->uid) {
        continue;
      }

      code = tsdbDataFileReadDataBlk(iter->data->reader, blockIdx, &iter->data->dataBlkArray);
      if (code) return code;

      iter->row->suid = blockIdx->suid;
      iter->row->uid = blockIdx->uid;
      iter->data->dataBlkArrayIdx = 0;
      iter->data->blockIdxArrayIdx++;
      break;
    }
  }

_exit:
  return 0;
}

static int32_t tsdbMemTableIterNext(STsdbIter *iter, const TABLEID *tbid) {
  SRBTreeNode *node;

  while (!iter->ctx->noMoreData) {
    for (TSDBROW *row; iter->memt->tbData && (row = tsdbTbDataIterGet(iter->memt->tbIter));) {
      if (tbid && tbid->suid == iter->memt->tbData->suid && tbid->uid == iter->memt->tbData->uid) {
        iter->memt->tbData = NULL;
        break;
      }
      iter->row->row = row[0];

      tsdbTbDataIterNext(iter->memt->tbIter);
      goto _exit;
    }

    for (;;) {
      node = tRBTreeIterNext(iter->memt->iter);
      if (!node) {
        iter->ctx->noMoreData = true;
        break;
      }

      iter->memt->tbData = TCONTAINER_OF(node, STbData, rbtn);
      if (tbid && tbid->suid == iter->memt->tbData->suid && tbid->uid == iter->memt->tbData->uid) {
        continue;
      } else {
        iter->row->suid = iter->memt->tbData->suid;
        iter->row->uid = iter->memt->tbData->uid;
        tsdbTbDataIterOpen(iter->memt->tbData, iter->memt->from, 0, iter->memt->tbIter);
        break;
      }
    }
  }

_exit:
  return 0;
}

static int32_t tsdbSttIterOpen(STsdbIter *iter) {
  int32_t code;

  code = tsdbSttFileReadSttBlk(iter->stt->reader, &iter->stt->sttBlkArray);
  if (code) return code;

  if (TARRAY2_SIZE(iter->stt->sttBlkArray) == 0) {
    iter->ctx->noMoreData = true;
    return 0;
  }

  iter->stt->sttBlkArrayIdx = 0;
  tBlockDataCreate(iter->stt->bData);
  iter->stt->iRow = 0;

  return tsdbSttIterNext(iter, NULL);
}

static int32_t tsdbDataIterOpen(STsdbIter *iter) {
  int32_t code;

  code = tsdbDataFileReadBlockIdx(iter->data->reader, &iter->data->blockIdxArray);
  if (code) return code;

  // SBlockIdx
  if (TARRAY2_SIZE(iter->data->blockIdxArray) == 0) {
    iter->ctx->noMoreData = true;
    return 0;
  }
  iter->data->blockIdxArrayIdx = 0;

  // SDataBlk
  iter->data->dataBlkArray = NULL;
  iter->data->dataBlkArrayIdx = 0;

  // SBlockData
  tBlockDataCreate(iter->data->bData);
  iter->data->iRow = 0;

  return tsdbDataIterNext(iter, NULL);
}

static int32_t tsdbMemTableIterOpen(STsdbIter *iter) {
  iter->memt->iter[0] = tRBTreeIterCreate(iter->memt->memt->tbDataTree, 1);
  return tsdbMemTableIterNext(iter, NULL);
}

static int32_t tsdbSttIterClose(STsdbIter *iter) {
  tBlockDataDestroy(iter->stt->bData);
  return 0;
}

static int32_t tsdbDataIterClose(STsdbIter *iter) {
  tBlockDataDestroy(iter->data->bData);
  return 0;
}

static int32_t tsdbMemTableIterClose(STsdbIter *iter) { return 0; }

int32_t tsdbIterOpen(const STsdbIterConfig *config, STsdbIter **iter) {
  int32_t code;

  iter[0] = taosMemoryCalloc(1, sizeof(*iter[0]));
  if (iter[0] == NULL) return TSDB_CODE_OUT_OF_MEMORY;

  iter[0]->type = config->type;
  iter[0]->ctx->noMoreData = false;
  switch (config->type) {
    case TSDB_ITER_TYPE_STT:
      iter[0]->stt->reader = config->sttReader;
      code = tsdbSttIterOpen(iter[0]);
      break;
    case TSDB_ITER_TYPE_DATA:
      iter[0]->data->reader = config->dataReader;
      code = tsdbDataIterOpen(iter[0]);
      break;
    case TSDB_ITER_TYPE_MEMT:
      iter[0]->memt->memt = config->memt;
      iter[0]->memt->from[0] = config->from[0];
      code = tsdbMemTableIterOpen(iter[0]);
      break;
    default:
      ASSERT(false);
  }

  if (code) {
    taosMemoryFree(iter[0]);
    iter[0] = NULL;
  }
  return code;
}

int32_t tsdbIterClose(STsdbIter **iter) {
  switch (iter[0]->type) {
    case TSDB_ITER_TYPE_STT:
      tsdbSttIterClose(iter[0]);
      break;
    case TSDB_ITER_TYPE_DATA:
      tsdbDataIterClose(iter[0]);
      break;
    case TSDB_ITER_TYPE_MEMT:
      tsdbMemTableIterClose(iter[0]);
      break;
    default:
      ASSERT(false);
  }
  taosMemoryFree(iter[0]);
  iter[0] = NULL;
  return 0;
}

int32_t tsdbIterNext(STsdbIter *iter) {
  switch (iter->type) {
    case TSDB_ITER_TYPE_STT:
      return tsdbSttIterNext(iter, NULL);
    case TSDB_ITER_TYPE_DATA:
      return tsdbDataIterNext(iter, NULL);
    case TSDB_ITER_TYPE_MEMT:
      return tsdbMemTableIterNext(iter, NULL);
    default:
      ASSERT(false);
  }
  return 0;
}

static int32_t tsdbIterSkipTableData(STsdbIter *iter, const TABLEID *tbid) {
  switch (iter->type) {
    case TSDB_ITER_TYPE_STT:
      return tsdbSttIterNext(iter, tbid);
    case TSDB_ITER_TYPE_DATA:
      return tsdbDataIterNext(iter, tbid);
    case TSDB_ITER_TYPE_MEMT:
      return tsdbMemTableIterNext(iter, tbid);
    default:
      ASSERT(false);
  }
  return 0;
}

static int32_t tsdbIterCmprFn(const SRBTreeNode *n1, const SRBTreeNode *n2) {
  STsdbIter *iter1 = TCONTAINER_OF(n1, STsdbIter, node);
  STsdbIter *iter2 = TCONTAINER_OF(n2, STsdbIter, node);
  return tRowInfoCmprFn(&iter1->row, &iter2->row);
}

// SIterMerger ================
struct SIterMerger {
  STsdbIter *iter;
  SRBTree    iterTree[1];
};

int32_t tsdbIterMergerOpen(const TTsdbIterArray *iterArray, SIterMerger **merger) {
  STsdbIter   *iter;
  SRBTreeNode *node;

  merger[0] = taosMemoryCalloc(1, sizeof(*merger[0]));
  if (!merger[0]) return TSDB_CODE_OUT_OF_MEMORY;

  tRBTreeCreate(merger[0]->iterTree, tsdbIterCmprFn);
  TARRAY2_FOREACH(iterArray, iter) {
    if (iter->ctx->noMoreData) continue;
    node = tRBTreePut(merger[0]->iterTree, iter->node);
    ASSERT(node);
  }

  return tsdbIterMergerNext(merger[0]);
}

int32_t tsdbIterMergerClose(SIterMerger **merger) {
  if (merger[0]) {
    taosMemoryFree(merger[0]);
    merger[0] = NULL;
  }
  return 0;
}

int32_t tsdbIterMergerNext(SIterMerger *merger) {
  int32_t      code;
  int32_t      c;
  SRBTreeNode *node;

  if (merger->iter) {
    code = tsdbIterNext(merger->iter);
    if (code) return code;

    if (merger->iter->ctx->noMoreData) {
      merger->iter = NULL;
    } else if ((node = tRBTreeMin(merger->iterTree))) {
      c = tsdbIterCmprFn(merger->iter->node, node);
      ASSERT(c);
      if (c > 0) {
        node = tRBTreePut(merger->iterTree, merger->iter->node);
        ASSERT(node);
        merger->iter = NULL;
      }
    }
  }

  if (!merger->iter && (node = tRBTreeDropMin(merger->iterTree))) {
    merger->iter = TCONTAINER_OF(node, STsdbIter, node);
  }

  return 0;
}

SRowInfo *tsdbIterMergerGet(SIterMerger *merger) { return merger->iter ? merger->iter->row : NULL; }

int32_t tsdbIterMergerSkipTableData(SIterMerger *merger, const TABLEID *tbid) {
  int32_t      code;
  int32_t      c;
  SRBTreeNode *node;

  while (merger->iter && tbid->suid == merger->iter->row->suid && tbid->uid == merger->iter->row->uid) {
    int32_t code = tsdbIterSkipTableData(merger->iter, tbid);
    if (code) return code;

    if (merger->iter->ctx->noMoreData) {
      merger->iter = NULL;
    } else if ((node = tRBTreeMin(merger->iterTree))) {
      c = tsdbIterCmprFn(merger->iter->node, node);
      ASSERT(c);
      if (c > 0) {
        node = tRBTreePut(merger->iterTree, merger->iter->node);
        ASSERT(node);
        merger->iter = NULL;
      }
    }

    if (!merger->iter && (node = tRBTreeDropMin(merger->iterTree))) {
      merger->iter = TCONTAINER_OF(node, STsdbIter, node);
    }
  }

  return 0;
}