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

#define _DEFAULT_SOURCE
#include "sync.h"
#include "syncAppendEntries.h"
#include "syncAppendEntriesReply.h"
#include "syncCommit.h"
#include "syncElection.h"
#include "syncEnv.h"
#include "syncIndexMgr.h"
#include "syncInt.h"
#include "syncMessage.h"
#include "syncRaftCfg.h"
#include "syncRaftLog.h"
#include "syncRaftStore.h"
#include "syncReplication.h"
#include "syncRequestVote.h"
#include "syncRequestVoteReply.h"
#include "syncRespMgr.h"
#include "syncSnapshot.h"
#include "syncTimeout.h"
#include "syncUtil.h"
#include "syncVoteMgr.h"

static void    syncNodeEqPingTimer(void* param, void* tmrId);
static void    syncNodeEqElectTimer(void* param, void* tmrId);
static void    syncNodeEqHeartbeatTimer(void* param, void* tmrId);
static int32_t syncNodeEqNoop(SSyncNode* ths);
static int32_t syncNodeAppendNoop(SSyncNode* ths);
static void    syncNodeEqPeerHeartbeatTimer(void* param, void* tmrId);
static bool    syncIsConfigChanged(const SSyncCfg* pOldCfg, const SSyncCfg* pNewCfg);
static int32_t syncHbTimerInit(SSyncNode* pSyncNode, SSyncTimer* pSyncTimer, SRaftId destId);
static int32_t syncHbTimerStart(SSyncNode* pSyncNode, SSyncTimer* pSyncTimer);
static int32_t syncHbTimerStop(SSyncNode* pSyncNode, SSyncTimer* pSyncTimer);
static int32_t syncNodeUpdateNewConfigIndex(SSyncNode* ths, SSyncCfg* pNewCfg);
static bool    syncNodeInConfig(SSyncNode* pSyncNode, const SSyncCfg* config);
static void    syncNodeDoConfigChange(SSyncNode* pSyncNode, SSyncCfg* newConfig, SyncIndex lastConfigChangeIndex);
static bool    syncNodeIsOptimizedOneReplica(SSyncNode* ths, SRpcMsg* pMsg);

static bool    syncNodeCanChange(SSyncNode* pSyncNode);
static int32_t syncNodeLeaderTransfer(SSyncNode* pSyncNode);
static int32_t syncNodeLeaderTransferTo(SSyncNode* pSyncNode, SNodeInfo newLeader);
static int32_t syncDoLeaderTransfer(SSyncNode* ths, SRpcMsg* pRpcMsg, SSyncRaftEntry* pEntry);

static ESyncStrategy syncNodeStrategy(SSyncNode* pSyncNode);
static SyncIndex     syncNodeGetSnapshotConfigIndex(SSyncNode* pSyncNode, SyncIndex snapshotLastApplyIndex);

int64_t syncOpen(SSyncInfo* pSyncInfo) {
  SSyncNode* pSyncNode = syncNodeOpen(pSyncInfo);
  if (pSyncNode == NULL) {
    sError("vgId:%d, failed to open sync node", pSyncInfo->vgId);
    return -1;
  }

  pSyncNode->rid = syncNodeAdd(pSyncNode);
  if (pSyncNode->rid < 0) {
    syncNodeClose(pSyncNode);
    return -1;
  }

  pSyncNode->pingBaseLine = pSyncInfo->pingMs;
  pSyncNode->pingTimerMS = pSyncInfo->pingMs;
  pSyncNode->electBaseLine = pSyncInfo->electMs;
  pSyncNode->hbBaseLine = pSyncInfo->heartbeatMs;
  pSyncNode->heartbeatTimerMS = pSyncInfo->heartbeatMs;
  pSyncNode->msgcb = pSyncInfo->msgcb;
  return pSyncNode->rid;
}

void syncStart(int64_t rid) {
  SSyncNode* pSyncNode = syncNodeAcquire(rid);
  if (pSyncNode != NULL) {
    syncNodeStart(pSyncNode);
    syncNodeRelease(pSyncNode);
  }
}

void syncStop(int64_t rid) {
  SSyncNode* pSyncNode = syncNodeAcquire(rid);
  if (pSyncNode != NULL) {
    syncNodeRelease(pSyncNode);
    syncNodeRemove(rid);
  }
}

void syncPreStop(int64_t rid) {
  SSyncNode* pSyncNode = syncNodeAcquire(rid);
  if (pSyncNode != NULL) {
    syncNodePreClose(pSyncNode);
    syncNodeRelease(pSyncNode);
  }
}

static bool syncNodeCheckNewConfig(SSyncNode* pSyncNode, const SSyncCfg* pCfg) {
  if (!syncNodeInConfig(pSyncNode, pCfg)) return false;
  return abs(pCfg->replicaNum - pSyncNode->replicaNum) <= 1;
}

int32_t syncReconfig(int64_t rid, SSyncCfg* pNewCfg) {
  SSyncNode* pSyncNode = syncNodeAcquire(rid);
  if (pSyncNode == NULL) return -1;

  if (!syncNodeCheckNewConfig(pSyncNode, pNewCfg)) {
    syncNodeRelease(pSyncNode);
    terrno = TSDB_CODE_SYN_NEW_CONFIG_ERROR;
    sError("vgId:%d, failed to reconfig since invalid new config", pSyncNode->vgId);
    return -1;
  }

  syncNodeUpdateNewConfigIndex(pSyncNode, pNewCfg);
  syncNodeDoConfigChange(pSyncNode, pNewCfg, SYNC_INDEX_INVALID);

  if (pSyncNode->state == TAOS_SYNC_STATE_LEADER) {
    syncNodeStopHeartbeatTimer(pSyncNode);

    for (int32_t i = 0; i < TSDB_MAX_REPLICA; ++i) {
      syncHbTimerInit(pSyncNode, &pSyncNode->peerHeartbeatTimerArr[i], pSyncNode->replicasId[i]);
    }

    syncNodeStartHeartbeatTimer(pSyncNode);
    syncNodeReplicate(pSyncNode);
  }

  syncNodeRelease(pSyncNode);
  return 0;
}

int32_t syncProcessMsg(int64_t rid, SRpcMsg* pMsg) {
  int32_t code = -1;
  if (!syncIsInit()) return code;

  SSyncNode* pSyncNode = syncNodeAcquire(rid);
  if (pSyncNode == NULL) return code;

  switch (pMsg->msgType) {
    case TDMT_SYNC_HEARTBEAT:
      code = syncNodeOnHeartbeat(pSyncNode, pMsg);
      break;
    case TDMT_SYNC_HEARTBEAT_REPLY:
      code = syncNodeOnHeartbeatReply(pSyncNode, pMsg);
      break;
    case TDMT_SYNC_TIMEOUT:
      code = syncNodeOnTimeout(pSyncNode, pMsg);
      break;
    case TDMT_SYNC_CLIENT_REQUEST:
      code = syncNodeOnClientRequest(pSyncNode, pMsg, NULL);
      break;
    case TDMT_SYNC_REQUEST_VOTE:
      code = syncNodeOnRequestVote(pSyncNode, pMsg);
      break;
    case TDMT_SYNC_REQUEST_VOTE_REPLY:
      code = syncNodeOnRequestVoteReply(pSyncNode, pMsg);
      break;
    case TDMT_SYNC_APPEND_ENTRIES:
      code = syncNodeOnAppendEntries(pSyncNode, pMsg);
      break;
    case TDMT_SYNC_APPEND_ENTRIES_REPLY:
      code = syncNodeOnAppendEntriesReply(pSyncNode, pMsg);
      break;
    case TDMT_SYNC_SNAPSHOT_SEND:
      code = syncNodeOnSnapshot(pSyncNode, pMsg);
      break;
    case TDMT_SYNC_SNAPSHOT_RSP:
      code = syncNodeOnSnapshotReply(pSyncNode, pMsg);
      break;
    case TDMT_SYNC_LOCAL_CMD:
      code = syncNodeOnLocalCmd(pSyncNode, pMsg);
      break;
    default:
      sError("vgId:%d, failed to process msg:%p since invalid type:%s", pSyncNode->vgId, pMsg,
             TMSG_INFO(pMsg->msgType));
      code = -1;
  }

  syncNodeRelease(pSyncNode);
  return code;
}

int32_t syncLeaderTransfer(int64_t rid) {
  SSyncNode* pSyncNode = syncNodeAcquire(rid);
  if (pSyncNode == NULL) return -1;

  int32_t ret = syncNodeLeaderTransfer(pSyncNode);
  syncNodeRelease(pSyncNode);
  return ret;
}

SyncIndex syncMinMatchIndex(SSyncNode* pSyncNode) {
  SyncIndex minMatchIndex = SYNC_INDEX_INVALID;

  if (pSyncNode->peersNum > 0) {
    minMatchIndex = syncIndexMgrGetIndex(pSyncNode->pMatchIndex, &(pSyncNode->peersId[0]));
  }

  for (int32_t i = 1; i < pSyncNode->peersNum; ++i) {
    SyncIndex matchIndex = syncIndexMgrGetIndex(pSyncNode->pMatchIndex, &(pSyncNode->peersId[i]));
    if (matchIndex < minMatchIndex) {
      minMatchIndex = matchIndex;
    }
  }
  return minMatchIndex;
}

int32_t syncBeginSnapshot(int64_t rid, int64_t lastApplyIndex) {
  SSyncNode* pSyncNode = syncNodeAcquire(rid);
  if (pSyncNode == NULL) {
    sError("sync begin snapshot error");
    return -1;
  }

  int32_t code = 0;

  if (syncNodeIsMnode(pSyncNode)) {
    // mnode
    int64_t logRetention = SYNC_MNODE_LOG_RETENTION;

    SyncIndex beginIndex = pSyncNode->pLogStore->syncLogBeginIndex(pSyncNode->pLogStore);
    SyncIndex endIndex = pSyncNode->pLogStore->syncLogEndIndex(pSyncNode->pLogStore);
    int64_t   logNum = endIndex - beginIndex;
    bool      isEmpty = pSyncNode->pLogStore->syncLogIsEmpty(pSyncNode->pLogStore);

    if (isEmpty || (!isEmpty && logNum < logRetention)) {
      sNTrace(pSyncNode, "new-snapshot-index:%" PRId64 ", log-num:%" PRId64 ", empty:%d, do not delete wal",
              lastApplyIndex, logNum, isEmpty);
      syncNodeRelease(pSyncNode);
      return 0;
    }

    goto _DEL_WAL;

  } else {
    // vnode
    if (pSyncNode->replicaNum > 1) {
      // multi replicas

      if (pSyncNode->state == TAOS_SYNC_STATE_LEADER) {
        pSyncNode->minMatchIndex = syncMinMatchIndex(pSyncNode);

        for (int32_t i = 0; i < pSyncNode->peersNum; ++i) {
          int64_t matchIndex = syncIndexMgrGetIndex(pSyncNode->pMatchIndex, &(pSyncNode->peersId[i]));
          if (lastApplyIndex > matchIndex) {
            do {
              char     host[64];
              uint16_t port;
              syncUtilU642Addr(pSyncNode->peersId[i].addr, host, sizeof(host), &port);
              sNTrace(pSyncNode,
                      "new-snapshot-index:%" PRId64 " is greater than match-index:%" PRId64
                      " of %s:%d, do not delete wal",
                      lastApplyIndex, matchIndex, host, port);
            } while (0);

            syncNodeRelease(pSyncNode);
            return 0;
          }
        }

      } else if (pSyncNode->state == TAOS_SYNC_STATE_FOLLOWER) {
        if (lastApplyIndex > pSyncNode->minMatchIndex) {
          sNTrace(pSyncNode,
                  "new-snapshot-index:%" PRId64 " is greater than min-match-index:%" PRId64 ", do not delete wal",
                  lastApplyIndex, pSyncNode->minMatchIndex);
          syncNodeRelease(pSyncNode);
          return 0;
        }

      } else if (pSyncNode->state == TAOS_SYNC_STATE_CANDIDATE) {
        sNTrace(pSyncNode, "new-snapshot-index:%" PRId64 " candidate, do not delete wal", lastApplyIndex);
        syncNodeRelease(pSyncNode);
        return 0;

      } else {
        sNTrace(pSyncNode, "new-snapshot-index:%" PRId64 " unknown state, do not delete wal", lastApplyIndex);
        syncNodeRelease(pSyncNode);
        return 0;
      }

      goto _DEL_WAL;

    } else {
      // one replica

      goto _DEL_WAL;
    }
  }

_DEL_WAL:

  do {
    SyncIndex snapshottingIndex = atomic_load_64(&pSyncNode->snapshottingIndex);

    if (snapshottingIndex == SYNC_INDEX_INVALID) {
      atomic_store_64(&pSyncNode->snapshottingIndex, lastApplyIndex);
      pSyncNode->snapshottingTime = taosGetTimestampMs();

      SSyncLogStoreData* pData = pSyncNode->pLogStore->data;
      code = walBeginSnapshot(pData->pWal, lastApplyIndex);
      if (code == 0) {
        sNTrace(pSyncNode, "wal snapshot begin, index:%" PRId64 ", last apply index:%" PRId64,
                pSyncNode->snapshottingIndex, lastApplyIndex);
      } else {
        sNError(pSyncNode, "wal snapshot begin error since:%s, index:%" PRId64 ", last apply index:%" PRId64,
                terrstr(terrno), pSyncNode->snapshottingIndex, lastApplyIndex);
        atomic_store_64(&pSyncNode->snapshottingIndex, SYNC_INDEX_INVALID);
      }

    } else {
      sNTrace(pSyncNode, "snapshotting for %" PRId64 ", do not delete wal for new-snapshot-index:%" PRId64,
              snapshottingIndex, lastApplyIndex);
    }
  } while (0);

  syncNodeRelease(pSyncNode);
  return code;
}

int32_t syncEndSnapshot(int64_t rid) {
  SSyncNode* pSyncNode = syncNodeAcquire(rid);
  if (pSyncNode == NULL) {
    sError("sync end snapshot error");
    return -1;
  }

  int32_t code = 0;
  if (atomic_load_64(&pSyncNode->snapshottingIndex) != SYNC_INDEX_INVALID) {
    SSyncLogStoreData* pData = pSyncNode->pLogStore->data;
    code = walEndSnapshot(pData->pWal);
    if (code != 0) {
      sNError(pSyncNode, "wal snapshot end error since:%s", terrstr());
      syncNodeRelease(pSyncNode);
      return -1;
    } else {
      sNTrace(pSyncNode, "wal snapshot end, index:%" PRId64, atomic_load_64(&pSyncNode->snapshottingIndex));
      atomic_store_64(&pSyncNode->snapshottingIndex, SYNC_INDEX_INVALID);
    }
  }

  syncNodeRelease(pSyncNode);
  return code;
}

int32_t syncStepDown(int64_t rid, SyncTerm newTerm) {
  SSyncNode* pSyncNode = syncNodeAcquire(rid);
  if (pSyncNode == NULL) {
    sError("sync step down error");
    return -1;
  }

  syncNodeStepDown(pSyncNode, newTerm);
  syncNodeRelease(pSyncNode);
  return 0;
}

bool syncIsReadyForRead(int64_t rid) {
  SSyncNode* pSyncNode = syncNodeAcquire(rid);
  if (pSyncNode == NULL) {
    sError("sync ready for read error");
    return false;
  }

  if (pSyncNode->state == TAOS_SYNC_STATE_LEADER && pSyncNode->restoreFinish) {
    syncNodeRelease(pSyncNode);
    return true;
  }

  bool ready = false;
  if (pSyncNode->state == TAOS_SYNC_STATE_LEADER && !pSyncNode->restoreFinish) {
    if (!pSyncNode->pFsm->FpApplyQueueEmptyCb(pSyncNode->pFsm)) {
      // apply queue not empty
      ready = false;

    } else {
      if (!pSyncNode->pLogStore->syncLogIsEmpty(pSyncNode->pLogStore)) {
        SSyncRaftEntry* pEntry = NULL;
        int32_t         code = pSyncNode->pLogStore->syncLogGetEntry(
                    pSyncNode->pLogStore, pSyncNode->pLogStore->syncLogLastIndex(pSyncNode->pLogStore), &pEntry);
        if (code == 0 && pEntry != NULL) {
          if (pEntry->originalRpcType == TDMT_SYNC_NOOP && pEntry->term == pSyncNode->pRaftStore->currentTerm) {
            ready = true;
          }

          syncEntryDestory(pEntry);
        }
      }
    }
  }

  if (!ready) {
    if (pSyncNode->state != TAOS_SYNC_STATE_LEADER) {
      terrno = TSDB_CODE_SYN_NOT_LEADER;
    } else {
      terrno = TSDB_CODE_APP_NOT_READY;
    }
  }

  syncNodeRelease(pSyncNode);
  return ready;
}

int32_t syncNodeLeaderTransfer(SSyncNode* pSyncNode) {
  if (pSyncNode->peersNum == 0) {
    sDebug("only one replica, cannot leader transfer");
    terrno = TSDB_CODE_SYN_ONE_REPLICA;
    return -1;
  }

  int32_t ret = 0;
  if (pSyncNode->state == TAOS_SYNC_STATE_LEADER) {
    SNodeInfo newLeader = (pSyncNode->peersNodeInfo)[0];
    ret = syncNodeLeaderTransferTo(pSyncNode, newLeader);
  }

  return ret;
}

int32_t syncNodeLeaderTransferTo(SSyncNode* pSyncNode, SNodeInfo newLeader) {
  if (pSyncNode->replicaNum == 1) {
    sDebug("only one replica, cannot leader transfer");
    terrno = TSDB_CODE_SYN_ONE_REPLICA;
    return -1;
  }

  sNTrace(pSyncNode, "begin leader transfer to %s:%u", newLeader.nodeFqdn, newLeader.nodePort);

  SRpcMsg rpcMsg = {0};
  (void)syncBuildLeaderTransfer(&rpcMsg, pSyncNode->vgId);

  SyncLeaderTransfer* pMsg = rpcMsg.pCont;
  pMsg->newLeaderId.addr = syncUtilAddr2U64(newLeader.nodeFqdn, newLeader.nodePort);
  pMsg->newLeaderId.vgId = pSyncNode->vgId;
  pMsg->newNodeInfo = newLeader;

  return syncNodePropose(pSyncNode, &rpcMsg, false);
}

SSyncState syncGetState(int64_t rid) {
  SSyncState state = {.state = TAOS_SYNC_STATE_ERROR};

  SSyncNode* pSyncNode = syncNodeAcquire(rid);
  if (pSyncNode != NULL) {
    state.state = pSyncNode->state;
    state.restored = pSyncNode->restoreFinish;
    syncNodeRelease(pSyncNode);
  }

  return state;
}

#if 0
int32_t syncGetSnapshotByIndex(int64_t rid, SyncIndex index, SSnapshot* pSnapshot) {
  if (index < SYNC_INDEX_BEGIN) {
    return -1;
  }

  SSyncNode* pSyncNode = syncNodeAcquire(rid);
  if (pSyncNode == NULL) {
    return -1;
  }
  ASSERT(rid == pSyncNode->rid);

  SSyncRaftEntry* pEntry = NULL;
  int32_t         code = pSyncNode->pLogStore->syncLogGetEntry(pSyncNode->pLogStore, index, &pEntry);
  if (code != 0) {
    if (pEntry != NULL) {
      syncEntryDestory(pEntry);
    }
    syncNodeRelease(pSyncNode);
    return -1;
  }
  ASSERT(pEntry != NULL);

  pSnapshot->data = NULL;
  pSnapshot->lastApplyIndex = index;
  pSnapshot->lastApplyTerm = pEntry->term;
  pSnapshot->lastConfigIndex = syncNodeGetSnapshotConfigIndex(pSyncNode, index);

  syncEntryDestory(pEntry);
  syncNodeRelease(pSyncNode);
  return 0;
}

int32_t syncGetSnapshotMeta(int64_t rid, struct SSnapshotMeta* sMeta) {
  SSyncNode* pSyncNode = syncNodeAcquire(rid);
  if (pSyncNode == NULL) {
    return -1;
  }
  ASSERT(rid == pSyncNode->rid);
  sMeta->lastConfigIndex = pSyncNode->pRaftCfg->lastConfigIndex;

  sTrace("vgId:%d, get snapshot meta, lastConfigIndex:%" PRId64, pSyncNode->vgId, pSyncNode->pRaftCfg->lastConfigIndex);

  syncNodeRelease(pSyncNode);
  return 0;
}

int32_t syncGetSnapshotMetaByIndex(int64_t rid, SyncIndex snapshotIndex, struct SSnapshotMeta* sMeta) {
  SSyncNode* pSyncNode = syncNodeAcquire(rid);
  if (pSyncNode == NULL) {
    return -1;
  }
  ASSERT(rid == pSyncNode->rid);

  ASSERT(pSyncNode->pRaftCfg->configIndexCount >= 1);
  SyncIndex lastIndex = (pSyncNode->pRaftCfg->configIndexArr)[0];

  for (int32_t i = 0; i < pSyncNode->pRaftCfg->configIndexCount; ++i) {
    if ((pSyncNode->pRaftCfg->configIndexArr)[i] > lastIndex &&
        (pSyncNode->pRaftCfg->configIndexArr)[i] <= snapshotIndex) {
      lastIndex = (pSyncNode->pRaftCfg->configIndexArr)[i];
    }
  }
  sMeta->lastConfigIndex = lastIndex;
  sTrace("vgId:%d, get snapshot meta by index:%" PRId64 " lcindex:%" PRId64, pSyncNode->vgId, snapshotIndex,
         sMeta->lastConfigIndex);

  syncNodeRelease(pSyncNode);
  return 0;
}
#endif

SyncIndex syncNodeGetSnapshotConfigIndex(SSyncNode* pSyncNode, SyncIndex snapshotLastApplyIndex) {
  ASSERT(pSyncNode->pRaftCfg->configIndexCount >= 1);
  SyncIndex lastIndex = (pSyncNode->pRaftCfg->configIndexArr)[0];

  for (int32_t i = 0; i < pSyncNode->pRaftCfg->configIndexCount; ++i) {
    if ((pSyncNode->pRaftCfg->configIndexArr)[i] > lastIndex &&
        (pSyncNode->pRaftCfg->configIndexArr)[i] <= snapshotLastApplyIndex) {
      lastIndex = (pSyncNode->pRaftCfg->configIndexArr)[i];
    }
  }
  sTrace("vgId:%d, sync get last config index, index:%" PRId64 " lcindex:%" PRId64, pSyncNode->vgId,
         snapshotLastApplyIndex, lastIndex);

  return lastIndex;
}

void syncGetRetryEpSet(int64_t rid, SEpSet* pEpSet) {
  pEpSet->numOfEps = 0;

  SSyncNode* pSyncNode = syncNodeAcquire(rid);
  if (pSyncNode == NULL) return;

  for (int32_t i = 0; i < pSyncNode->pRaftCfg->cfg.replicaNum; ++i) {
    SEp* pEp = &pEpSet->eps[i];
    tstrncpy(pEp->fqdn, pSyncNode->pRaftCfg->cfg.nodeInfo[i].nodeFqdn, TSDB_FQDN_LEN);
    pEp->port = (pSyncNode->pRaftCfg->cfg.nodeInfo)[i].nodePort;
    pEpSet->numOfEps++;
    sInfo("vgId:%d, sync get retry epset, index:%d %s:%d", pSyncNode->vgId, i, pEp->fqdn, pEp->port);
  }
  if (pEpSet->numOfEps > 0) {
    pEpSet->inUse = (pSyncNode->pRaftCfg->cfg.myIndex + 1) % pEpSet->numOfEps;
  }

  sInfo("vgId:%d, sync get retry epset numOfEps:%d inUse:%d", pSyncNode->vgId, pEpSet->numOfEps, pEpSet->inUse);
  syncNodeRelease(pSyncNode);
}

int32_t syncPropose(int64_t rid, SRpcMsg* pMsg, bool isWeak) {
  SSyncNode* pSyncNode = syncNodeAcquire(rid);
  if (pSyncNode == NULL) {
    sError("sync propose error");
    return -1;
  }

  int32_t ret = syncNodePropose(pSyncNode, pMsg, isWeak);
  syncNodeRelease(pSyncNode);
  return ret;
}

int32_t syncNodePropose(SSyncNode* pSyncNode, SRpcMsg* pMsg, bool isWeak) {
  if (pSyncNode->state != TAOS_SYNC_STATE_LEADER) {
    terrno = TSDB_CODE_SYN_NOT_LEADER;
    sNError(pSyncNode, "sync propose not leader, %s, type:%s", syncStr(pSyncNode->state), TMSG_INFO(pMsg->msgType));
    return -1;
  }

  // not restored, vnode enable
  if (!pSyncNode->restoreFinish && pSyncNode->vgId != 1) {
    terrno = TSDB_CODE_SYN_PROPOSE_NOT_READY;
    sNError(pSyncNode, "failed to sync propose since not ready, type:%s, last:%" PRId64 ", cmt:%" PRId64,
            TMSG_INFO(pMsg->msgType), syncNodeGetLastIndex(pSyncNode), pSyncNode->commitIndex);
    return -1;
  }

  // optimized one replica
  if (syncNodeIsOptimizedOneReplica(pSyncNode, pMsg)) {
    SyncIndex retIndex;
    int32_t   code = syncNodeOnClientRequest(pSyncNode, pMsg, &retIndex);
    if (code == 0) {
      pMsg->info.conn.applyIndex = retIndex;
      pMsg->info.conn.applyTerm = pSyncNode->pRaftStore->currentTerm;
      sTrace("vgId:%d, propose optimized msg, index:%" PRId64 " type:%s", pSyncNode->vgId, retIndex,
             TMSG_INFO(pMsg->msgType));
      return 1;
    } else {
      terrno = TSDB_CODE_SYN_INTERNAL_ERROR;
      sError("vgId:%d, failed to propose optimized msg, index:%" PRId64 " type:%s", pSyncNode->vgId, retIndex,
             TMSG_INFO(pMsg->msgType));
      return -1;
    }
  } else {
    SRespStub stub = {.createTime = taosGetTimestampMs(), .rpcMsg = *pMsg};
    uint64_t  seqNum = syncRespMgrAdd(pSyncNode->pSyncRespMgr, &stub);
    SRpcMsg   rpcMsg = {0};
    int32_t   code = syncBuildClientRequest(&rpcMsg, pMsg, seqNum, isWeak, pSyncNode->vgId);
    if (code != 0) {
      sError("vgId:%d, failed to propose msg while serialize since %s", pSyncNode->vgId, terrstr());
      (void)syncRespMgrDel(pSyncNode->pSyncRespMgr, seqNum);
      return -1;
    }

    sNTrace(pSyncNode, "propose msg, type:%s", TMSG_INFO(pMsg->msgType));
    code = (*pSyncNode->syncEqMsg)(pSyncNode->msgcb, &rpcMsg);
    if (code != 0) {
      sError("vgId:%d, failed to propose msg while enqueue since %s", pSyncNode->vgId, terrstr());
      (void)syncRespMgrDel(pSyncNode->pSyncRespMgr, seqNum);
    }

    return code;
  }
}

static int32_t syncHbTimerInit(SSyncNode* pSyncNode, SSyncTimer* pSyncTimer, SRaftId destId) {
  pSyncTimer->pTimer = NULL;
  pSyncTimer->counter = 0;
  pSyncTimer->timerMS = pSyncNode->hbBaseLine;
  pSyncTimer->timerCb = syncNodeEqPeerHeartbeatTimer;
  pSyncTimer->destId = destId;
  atomic_store_64(&pSyncTimer->logicClock, 0);
  return 0;
}

static int32_t syncHbTimerStart(SSyncNode* pSyncNode, SSyncTimer* pSyncTimer) {
  int32_t ret = 0;
  if (syncIsInit()) {
    SSyncHbTimerData* pData = taosMemoryMalloc(sizeof(SSyncHbTimerData));
    pData->pSyncNode = pSyncNode;
    pData->pTimer = pSyncTimer;
    pData->destId = pSyncTimer->destId;
    pData->logicClock = pSyncTimer->logicClock;

    pSyncTimer->pData = pData;
    taosTmrReset(pSyncTimer->timerCb, pSyncTimer->timerMS, pData, syncEnv()->pTimerManager, &pSyncTimer->pTimer);
  } else {
    sError("vgId:%d, start ctrl hb timer error, sync env is stop", pSyncNode->vgId);
  }
  return ret;
}

static int32_t syncHbTimerStop(SSyncNode* pSyncNode, SSyncTimer* pSyncTimer) {
  int32_t ret = 0;
  atomic_add_fetch_64(&pSyncTimer->logicClock, 1);
  taosTmrStop(pSyncTimer->pTimer);
  pSyncTimer->pTimer = NULL;
  // taosMemoryFree(pSyncTimer->pData);
  return ret;
}

SSyncNode* syncNodeOpen(SSyncInfo* pSyncInfo) {
  SSyncNode* pSyncNode = taosMemoryCalloc(1, sizeof(SSyncNode));
  if (pSyncNode == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    goto _error;
  }

  if (!taosDirExist((char*)(pSyncInfo->path))) {
    if (taosMkDir(pSyncInfo->path) != 0) {
      terrno = TAOS_SYSTEM_ERROR(errno);
      sError("failed to create dir:%s since %s", pSyncInfo->path, terrstr());
      goto _error;
    }
  }

  snprintf(pSyncNode->configPath, sizeof(pSyncNode->configPath), "%s%sraft_config.json", pSyncInfo->path, TD_DIRSEP);
  if (!taosCheckExistFile(pSyncNode->configPath)) {
    // create a new raft config file
    SRaftCfgMeta meta = {0};
    meta.isStandBy = pSyncInfo->isStandBy;
    meta.snapshotStrategy = pSyncInfo->snapshotStrategy;
    meta.lastConfigIndex = SYNC_INDEX_INVALID;
    meta.batchSize = pSyncInfo->batchSize;
    if (raftCfgCreateFile(&pSyncInfo->syncCfg, meta, pSyncNode->configPath) != 0) {
      sError("vgId:%d, failed to create raft cfg file at %s", pSyncNode->vgId, pSyncNode->configPath);
      goto _error;
    }
    if (pSyncInfo->syncCfg.replicaNum == 0) {
      sInfo("vgId:%d, sync config not input", pSyncNode->vgId);
      pSyncInfo->syncCfg = pSyncNode->pRaftCfg->cfg;
    }
  } else {
    // update syncCfg by raft_config.json
    pSyncNode->pRaftCfg = raftCfgOpen(pSyncNode->configPath);
    if (pSyncNode->pRaftCfg == NULL) {
      sError("vgId:%d, failed to open raft cfg file at %s", pSyncNode->vgId, pSyncNode->configPath);
      goto _error;
    }

    if (pSyncInfo->syncCfg.replicaNum > 0 && syncIsConfigChanged(&pSyncNode->pRaftCfg->cfg, &pSyncInfo->syncCfg)) {
      sInfo("vgId:%d, use sync config from input options and write to cfg file", pSyncNode->vgId);
      pSyncNode->pRaftCfg->cfg = pSyncInfo->syncCfg;
      if (raftCfgPersist(pSyncNode->pRaftCfg) != 0) {
        sError("vgId:%d, failed to persist raft cfg file at %s", pSyncNode->vgId, pSyncNode->configPath);
        goto _error;
      }
    } else {
      sInfo("vgId:%d, use sync config from raft cfg file", pSyncNode->vgId);
      pSyncInfo->syncCfg = pSyncNode->pRaftCfg->cfg;
    }

    raftCfgClose(pSyncNode->pRaftCfg);
    pSyncNode->pRaftCfg = NULL;
  }

  // init by SSyncInfo
  pSyncNode->vgId = pSyncInfo->vgId;
  SSyncCfg* pCfg = &pSyncInfo->syncCfg;
  sDebug("vgId:%d, replica:%d selfIndex:%d", pSyncNode->vgId, pCfg->replicaNum, pCfg->myIndex);
  for (int32_t i = 0; i < pCfg->replicaNum; ++i) {
    SNodeInfo* pNode = &pCfg->nodeInfo[i];
    sDebug("vgId:%d, index:%d ep:%s:%u", pSyncNode->vgId, i, pNode->nodeFqdn, pNode->nodePort);
  }

  memcpy(pSyncNode->path, pSyncInfo->path, sizeof(pSyncNode->path));
  snprintf(pSyncNode->raftStorePath, sizeof(pSyncNode->raftStorePath), "%s%sraft_store.json", pSyncInfo->path,
           TD_DIRSEP);
  snprintf(pSyncNode->configPath, sizeof(pSyncNode->configPath), "%s%sraft_config.json", pSyncInfo->path, TD_DIRSEP);

  pSyncNode->pWal = pSyncInfo->pWal;
  pSyncNode->msgcb = pSyncInfo->msgcb;
  pSyncNode->syncSendMSg = pSyncInfo->syncSendMSg;
  pSyncNode->syncEqMsg = pSyncInfo->syncEqMsg;
  pSyncNode->syncEqCtrlMsg = pSyncInfo->syncEqCtrlMsg;

  // init raft config
  pSyncNode->pRaftCfg = raftCfgOpen(pSyncNode->configPath);
  if (pSyncNode->pRaftCfg == NULL) {
    sError("vgId:%d, failed to open raft cfg file at %s", pSyncNode->vgId, pSyncNode->configPath);
    goto _error;
  }

  // init internal
  pSyncNode->myNodeInfo = pSyncNode->pRaftCfg->cfg.nodeInfo[pSyncNode->pRaftCfg->cfg.myIndex];
  if (!syncUtilNodeInfo2RaftId(&pSyncNode->myNodeInfo, pSyncNode->vgId, &pSyncNode->myRaftId)) {
    sError("vgId:%d, failed to determine my raft member id", pSyncNode->vgId);
    goto _error;
  }

  // init peersNum, peers, peersId
  pSyncNode->peersNum = pSyncNode->pRaftCfg->cfg.replicaNum - 1;
  int32_t j = 0;
  for (int32_t i = 0; i < pSyncNode->pRaftCfg->cfg.replicaNum; ++i) {
    if (i != pSyncNode->pRaftCfg->cfg.myIndex) {
      pSyncNode->peersNodeInfo[j] = pSyncNode->pRaftCfg->cfg.nodeInfo[i];
      j++;
    }
  }
  for (int32_t i = 0; i < pSyncNode->peersNum; ++i) {
    if (!syncUtilNodeInfo2RaftId(&pSyncNode->peersNodeInfo[i], pSyncNode->vgId, &pSyncNode->peersId[i])) {
      sError("vgId:%d, failed to determine raft member id, peer:%d", pSyncNode->vgId, i);
      goto _error;
    }
  }

  // init replicaNum, replicasId
  pSyncNode->replicaNum = pSyncNode->pRaftCfg->cfg.replicaNum;
  for (int32_t i = 0; i < pSyncNode->pRaftCfg->cfg.replicaNum; ++i) {
    if (!syncUtilNodeInfo2RaftId(&pSyncNode->pRaftCfg->cfg.nodeInfo[i], pSyncNode->vgId, &pSyncNode->replicasId[i])) {
      sError("vgId:%d, failed to determine raft member id, replica:%d", pSyncNode->vgId, i);
      goto _error;
    }
  }

  // init raft algorithm
  pSyncNode->pFsm = pSyncInfo->pFsm;
  pSyncInfo->pFsm = NULL;
  pSyncNode->quorum = syncUtilQuorum(pSyncNode->pRaftCfg->cfg.replicaNum);
  pSyncNode->leaderCache = EMPTY_RAFT_ID;

  // init life cycle outside

  // TLA+ Spec
  // InitHistoryVars == /\ elections = {}
  //                    /\ allLogs   = {}
  //                    /\ voterLog  = [i \in Server |-> [j \in {} |-> <<>>]]
  // InitServerVars == /\ currentTerm = [i \in Server |-> 1]
  //                   /\ state       = [i \in Server |-> Follower]
  //                   /\ votedFor    = [i \in Server |-> Nil]
  // InitCandidateVars == /\ votesResponded = [i \in Server |-> {}]
  //                      /\ votesGranted   = [i \in Server |-> {}]
  // \* The values nextIndex[i][i] and matchIndex[i][i] are never read, since the
  // \* leader does not send itself messages. It's still easier to include these
  // \* in the functions.
  // InitLeaderVars == /\ nextIndex  = [i \in Server |-> [j \in Server |-> 1]]
  //                   /\ matchIndex = [i \in Server |-> [j \in Server |-> 0]]
  // InitLogVars == /\ log          = [i \in Server |-> << >>]
  //                /\ commitIndex  = [i \in Server |-> 0]
  // Init == /\ messages = [m \in {} |-> 0]
  //         /\ InitHistoryVars
  //         /\ InitServerVars
  //         /\ InitCandidateVars
  //         /\ InitLeaderVars
  //         /\ InitLogVars
  //

  // init TLA+ server vars
  pSyncNode->state = TAOS_SYNC_STATE_FOLLOWER;
  pSyncNode->pRaftStore = raftStoreOpen(pSyncNode->raftStorePath);
  if (pSyncNode->pRaftStore == NULL) {
    sError("vgId:%d, failed to open raft store at path %s", pSyncNode->vgId, pSyncNode->raftStorePath);
    goto _error;
  }

  // init TLA+ candidate vars
  pSyncNode->pVotesGranted = voteGrantedCreate(pSyncNode);
  if (pSyncNode->pVotesGranted == NULL) {
    sError("vgId:%d, failed to create VotesGranted", pSyncNode->vgId);
    goto _error;
  }
  pSyncNode->pVotesRespond = votesRespondCreate(pSyncNode);
  if (pSyncNode->pVotesRespond == NULL) {
    sError("vgId:%d, failed to create VotesRespond", pSyncNode->vgId);
    goto _error;
  }

  // init TLA+ leader vars
  pSyncNode->pNextIndex = syncIndexMgrCreate(pSyncNode);
  if (pSyncNode->pNextIndex == NULL) {
    sError("vgId:%d, failed to create SyncIndexMgr", pSyncNode->vgId);
    goto _error;
  }
  pSyncNode->pMatchIndex = syncIndexMgrCreate(pSyncNode);
  if (pSyncNode->pMatchIndex == NULL) {
    sError("vgId:%d, failed to create SyncIndexMgr", pSyncNode->vgId);
    goto _error;
  }

  // init TLA+ log vars
  pSyncNode->pLogStore = logStoreCreate(pSyncNode);
  if (pSyncNode->pLogStore == NULL) {
    sError("vgId:%d, failed to create SyncLogStore", pSyncNode->vgId);
    goto _error;
  }

  SyncIndex commitIndex = SYNC_INDEX_INVALID;
  if (pSyncNode->pFsm != NULL && pSyncNode->pFsm->FpGetSnapshotInfo != NULL) {
    SSnapshot snapshot = {0};
    int32_t   code = pSyncNode->pFsm->FpGetSnapshotInfo(pSyncNode->pFsm, &snapshot);
    if (code != 0) {
      sError("vgId:%d, failed to get snapshot info, code:%d", pSyncNode->vgId, code);
      goto _error;
    }
    if (snapshot.lastApplyIndex > commitIndex) {
      commitIndex = snapshot.lastApplyIndex;
      sNTrace(pSyncNode, "reset commit index by snapshot");
    }
  }
  pSyncNode->commitIndex = commitIndex;

  // timer ms init
  pSyncNode->pingBaseLine = PING_TIMER_MS;
  pSyncNode->electBaseLine = ELECT_TIMER_MS_MIN;
  pSyncNode->hbBaseLine = HEARTBEAT_TIMER_MS;

  // init ping timer
  pSyncNode->pPingTimer = NULL;
  pSyncNode->pingTimerMS = pSyncNode->pingBaseLine;
  atomic_store_64(&pSyncNode->pingTimerLogicClock, 0);
  atomic_store_64(&pSyncNode->pingTimerLogicClockUser, 0);
  pSyncNode->FpPingTimerCB = syncNodeEqPingTimer;
  pSyncNode->pingTimerCounter = 0;

  // init elect timer
  pSyncNode->pElectTimer = NULL;
  pSyncNode->electTimerMS = syncUtilElectRandomMS(pSyncNode->electBaseLine, 2 * pSyncNode->electBaseLine);
  atomic_store_64(&pSyncNode->electTimerLogicClock, 0);
  pSyncNode->FpElectTimerCB = syncNodeEqElectTimer;
  pSyncNode->electTimerCounter = 0;

  // init heartbeat timer
  pSyncNode->pHeartbeatTimer = NULL;
  pSyncNode->heartbeatTimerMS = pSyncNode->hbBaseLine;
  atomic_store_64(&pSyncNode->heartbeatTimerLogicClock, 0);
  atomic_store_64(&pSyncNode->heartbeatTimerLogicClockUser, 0);
  pSyncNode->FpHeartbeatTimerCB = syncNodeEqHeartbeatTimer;
  pSyncNode->heartbeatTimerCounter = 0;

  // init peer heartbeat timer
  for (int32_t i = 0; i < TSDB_MAX_REPLICA; ++i) {
    syncHbTimerInit(pSyncNode, &(pSyncNode->peerHeartbeatTimerArr[i]), (pSyncNode->replicasId)[i]);
  }

  // tools
  pSyncNode->pSyncRespMgr = syncRespMgrCreate(pSyncNode, SYNC_RESP_TTL_MS);
  if (pSyncNode->pSyncRespMgr == NULL) {
    sError("vgId:%d, failed to create SyncRespMgr", pSyncNode->vgId);
    goto _error;
  }

  // restore state
  pSyncNode->restoreFinish = false;

  // snapshot senders
  for (int32_t i = 0; i < TSDB_MAX_REPLICA; ++i) {
    SSyncSnapshotSender* pSender = snapshotSenderCreate(pSyncNode, i);
    // ASSERT(pSender != NULL);
    (pSyncNode->senders)[i] = pSender;
  }

  // snapshot receivers
  pSyncNode->pNewNodeReceiver = snapshotReceiverCreate(pSyncNode, EMPTY_RAFT_ID);

  // is config changing
  pSyncNode->changing = false;

  // peer state
  syncNodePeerStateInit(pSyncNode);

  // min match index
  pSyncNode->minMatchIndex = SYNC_INDEX_INVALID;

  // start in syncNodeStart
  // start raft
  // syncNodeBecomeFollower(pSyncNode);

  int64_t timeNow = taosGetTimestampMs();
  pSyncNode->startTime = timeNow;
  pSyncNode->leaderTime = timeNow;
  pSyncNode->lastReplicateTime = timeNow;

  // snapshotting
  atomic_store_64(&pSyncNode->snapshottingIndex, SYNC_INDEX_INVALID);

  sNTrace(pSyncNode, "sync open");

  return pSyncNode;

_error:
  if (pSyncInfo->pFsm) {
    taosMemoryFree(pSyncInfo->pFsm);
    pSyncInfo->pFsm = NULL;
  }
  syncNodeClose(pSyncNode);
  pSyncNode = NULL;
  return NULL;
}

void syncNodeMaybeUpdateCommitBySnapshot(SSyncNode* pSyncNode) {
  if (pSyncNode->pFsm != NULL && pSyncNode->pFsm->FpGetSnapshotInfo != NULL) {
    SSnapshot snapshot;
    int32_t   code = pSyncNode->pFsm->FpGetSnapshotInfo(pSyncNode->pFsm, &snapshot);
    ASSERT(code == 0);
    if (snapshot.lastApplyIndex > pSyncNode->commitIndex) {
      pSyncNode->commitIndex = snapshot.lastApplyIndex;
    }
  }
}

void syncNodeStart(SSyncNode* pSyncNode) {
  // start raft
  if (pSyncNode->replicaNum == 1) {
    raftStoreNextTerm(pSyncNode->pRaftStore);
    syncNodeBecomeLeader(pSyncNode, "one replica start");

    // Raft 3.6.2 Committing entries from previous terms
    syncNodeAppendNoop(pSyncNode);
    syncMaybeAdvanceCommitIndex(pSyncNode);

  } else {
    syncNodeBecomeFollower(pSyncNode, "first start");
  }

  int32_t ret = 0;
  ret = syncNodeStartPingTimer(pSyncNode);
  ASSERT(ret == 0);
}

void syncNodeStartStandBy(SSyncNode* pSyncNode) {
  // state change
  pSyncNode->state = TAOS_SYNC_STATE_FOLLOWER;
  syncNodeStopHeartbeatTimer(pSyncNode);

  // reset elect timer, long enough
  int32_t electMS = TIMER_MAX_MS;
  int32_t ret = syncNodeRestartElectTimer(pSyncNode, electMS);
  ASSERT(ret == 0);

  ret = 0;
  ret = syncNodeStartPingTimer(pSyncNode);
  ASSERT(ret == 0);
}

void syncNodePreClose(SSyncNode* pSyncNode) {
  // stop elect timer
  syncNodeStopElectTimer(pSyncNode);

  // stop heartbeat timer
  syncNodeStopHeartbeatTimer(pSyncNode);
}

void syncNodeClose(SSyncNode* pSyncNode) {
  if (pSyncNode == NULL) {
    return;
  }
  int32_t ret;

  sNTrace(pSyncNode, "sync close");

  ret = raftStoreClose(pSyncNode->pRaftStore);
  ASSERT(ret == 0);
  pSyncNode->pRaftStore = NULL;

  syncRespMgrDestroy(pSyncNode->pSyncRespMgr);
  pSyncNode->pSyncRespMgr = NULL;
  voteGrantedDestroy(pSyncNode->pVotesGranted);
  pSyncNode->pVotesGranted = NULL;
  votesRespondDestory(pSyncNode->pVotesRespond);
  pSyncNode->pVotesRespond = NULL;
  syncIndexMgrDestroy(pSyncNode->pNextIndex);
  pSyncNode->pNextIndex = NULL;
  syncIndexMgrDestroy(pSyncNode->pMatchIndex);
  pSyncNode->pMatchIndex = NULL;
  logStoreDestory(pSyncNode->pLogStore);
  pSyncNode->pLogStore = NULL;
  raftCfgClose(pSyncNode->pRaftCfg);
  pSyncNode->pRaftCfg = NULL;

  syncNodeStopPingTimer(pSyncNode);
  syncNodeStopElectTimer(pSyncNode);
  syncNodeStopHeartbeatTimer(pSyncNode);

  if (pSyncNode->pFsm != NULL) {
    taosMemoryFree(pSyncNode->pFsm);
  }

  for (int32_t i = 0; i < TSDB_MAX_REPLICA; ++i) {
    if ((pSyncNode->senders)[i] != NULL) {
      snapshotSenderDestroy((pSyncNode->senders)[i]);
      (pSyncNode->senders)[i] = NULL;
    }
  }

  if (pSyncNode->pNewNodeReceiver != NULL) {
    snapshotReceiverDestroy(pSyncNode->pNewNodeReceiver);
    pSyncNode->pNewNodeReceiver = NULL;
  }

  taosMemoryFree(pSyncNode);
}

ESyncStrategy syncNodeStrategy(SSyncNode* pSyncNode) { return pSyncNode->pRaftCfg->snapshotStrategy; }

// timer control --------------
int32_t syncNodeStartPingTimer(SSyncNode* pSyncNode) {
  int32_t ret = 0;
  if (syncIsInit()) {
    taosTmrReset(pSyncNode->FpPingTimerCB, pSyncNode->pingTimerMS, pSyncNode, syncEnv()->pTimerManager,
                 &pSyncNode->pPingTimer);
    atomic_store_64(&pSyncNode->pingTimerLogicClock, pSyncNode->pingTimerLogicClockUser);
  } else {
    sError("vgId:%d, start ping timer error, sync env is stop", pSyncNode->vgId);
  }
  return ret;
}

int32_t syncNodeStopPingTimer(SSyncNode* pSyncNode) {
  int32_t ret = 0;
  atomic_add_fetch_64(&pSyncNode->pingTimerLogicClockUser, 1);
  taosTmrStop(pSyncNode->pPingTimer);
  pSyncNode->pPingTimer = NULL;
  return ret;
}

int32_t syncNodeStartElectTimer(SSyncNode* pSyncNode, int32_t ms) {
  int32_t ret = 0;
  if (syncIsInit()) {
    pSyncNode->electTimerMS = ms;

    SElectTimer* pElectTimer = taosMemoryMalloc(sizeof(SElectTimer));
    pElectTimer->logicClock = pSyncNode->electTimerLogicClock;
    pElectTimer->pSyncNode = pSyncNode;
    pElectTimer->pData = NULL;

    taosTmrReset(pSyncNode->FpElectTimerCB, pSyncNode->electTimerMS, pElectTimer, syncEnv()->pTimerManager,
                 &pSyncNode->pElectTimer);

  } else {
    sError("vgId:%d, start elect timer error, sync env is stop", pSyncNode->vgId);
  }
  return ret;
}

int32_t syncNodeStopElectTimer(SSyncNode* pSyncNode) {
  int32_t ret = 0;
  atomic_add_fetch_64(&pSyncNode->electTimerLogicClock, 1);
  taosTmrStop(pSyncNode->pElectTimer);
  pSyncNode->pElectTimer = NULL;

  return ret;
}

int32_t syncNodeRestartElectTimer(SSyncNode* pSyncNode, int32_t ms) {
  int32_t ret = 0;
  syncNodeStopElectTimer(pSyncNode);
  syncNodeStartElectTimer(pSyncNode, ms);
  return ret;
}

int32_t syncNodeResetElectTimer(SSyncNode* pSyncNode) {
  int32_t ret = 0;
  int32_t electMS;

  if (pSyncNode->pRaftCfg->isStandBy) {
    electMS = TIMER_MAX_MS;
  } else {
    electMS = syncUtilElectRandomMS(pSyncNode->electBaseLine, 2 * pSyncNode->electBaseLine);
  }
  ret = syncNodeRestartElectTimer(pSyncNode, electMS);

  sNTrace(pSyncNode, "reset elect timer, min:%d, max:%d, ms:%d", pSyncNode->electBaseLine, 2 * pSyncNode->electBaseLine,
          electMS);
  return ret;
}

static int32_t syncNodeDoStartHeartbeatTimer(SSyncNode* pSyncNode) {
  int32_t ret = 0;
  if (syncIsInit()) {
    taosTmrReset(pSyncNode->FpHeartbeatTimerCB, pSyncNode->heartbeatTimerMS, pSyncNode, syncEnv()->pTimerManager,
                 &pSyncNode->pHeartbeatTimer);
    atomic_store_64(&pSyncNode->heartbeatTimerLogicClock, pSyncNode->heartbeatTimerLogicClockUser);
  } else {
    sError("vgId:%d, start heartbeat timer error, sync env is stop", pSyncNode->vgId);
  }

  sNTrace(pSyncNode, "start heartbeat timer, ms:%d", pSyncNode->heartbeatTimerMS);
  return ret;
}

int32_t syncNodeStartHeartbeatTimer(SSyncNode* pSyncNode) {
  int32_t ret = 0;

#if 0
  pSyncNode->heartbeatTimerMS = pSyncNode->hbBaseLine;
  ret = syncNodeDoStartHeartbeatTimer(pSyncNode);
#endif

  for (int32_t i = 0; i < pSyncNode->peersNum; ++i) {
    SSyncTimer* pSyncTimer = syncNodeGetHbTimer(pSyncNode, &(pSyncNode->peersId[i]));
    if (pSyncTimer != NULL) {
      syncHbTimerStart(pSyncNode, pSyncTimer);
    }
  }

  return ret;
}

int32_t syncNodeStopHeartbeatTimer(SSyncNode* pSyncNode) {
  int32_t ret = 0;

#if 0
  atomic_add_fetch_64(&pSyncNode->heartbeatTimerLogicClockUser, 1);
  taosTmrStop(pSyncNode->pHeartbeatTimer);
  pSyncNode->pHeartbeatTimer = NULL;
#endif

  for (int32_t i = 0; i < pSyncNode->peersNum; ++i) {
    SSyncTimer* pSyncTimer = syncNodeGetHbTimer(pSyncNode, &(pSyncNode->peersId[i]));
    if (pSyncTimer != NULL) {
      syncHbTimerStop(pSyncNode, pSyncTimer);
    }
  }

  return ret;
}

int32_t syncNodeRestartHeartbeatTimer(SSyncNode* pSyncNode) {
  syncNodeStopHeartbeatTimer(pSyncNode);
  syncNodeStartHeartbeatTimer(pSyncNode);
  return 0;
}

// utils --------------
int32_t syncNodeSendMsgById(const SRaftId* destRaftId, SSyncNode* pSyncNode, SRpcMsg* pMsg) {
  SEpSet epSet;
  syncUtilRaftId2EpSet(destRaftId, &epSet);
  if (pSyncNode->syncSendMSg != NULL) {
    // htonl
    syncUtilMsgHtoN(pMsg->pCont);

    pMsg->info.noResp = 1;
    pSyncNode->syncSendMSg(&epSet, pMsg);
  } else {
    sError("vgId:%d, sync send msg by id error, fp-send-msg is null", pSyncNode->vgId);
    rpcFreeCont(pMsg->pCont);
    return -1;
  }

  return 0;
}

int32_t syncNodeSendMsgByInfo(const SNodeInfo* nodeInfo, SSyncNode* pSyncNode, SRpcMsg* pMsg) {
  SEpSet epSet;
  syncUtilNodeInfo2EpSet(nodeInfo, &epSet);
  if (pSyncNode->syncSendMSg != NULL) {
    // htonl
    syncUtilMsgHtoN(pMsg->pCont);

    pMsg->info.noResp = 1;
    pSyncNode->syncSendMSg(&epSet, pMsg);
  } else {
    sError("vgId:%d, sync send msg by info error, fp-send-msg is null", pSyncNode->vgId);
  }
  return 0;
}

inline bool syncNodeInConfig(SSyncNode* pSyncNode, const SSyncCfg* config) {
  bool b1 = false;
  bool b2 = false;

  for (int32_t i = 0; i < config->replicaNum; ++i) {
    if (strcmp((config->nodeInfo)[i].nodeFqdn, pSyncNode->myNodeInfo.nodeFqdn) == 0 &&
        (config->nodeInfo)[i].nodePort == pSyncNode->myNodeInfo.nodePort) {
      b1 = true;
      break;
    }
  }

  for (int32_t i = 0; i < config->replicaNum; ++i) {
    SRaftId raftId;
    raftId.addr = syncUtilAddr2U64((config->nodeInfo)[i].nodeFqdn, (config->nodeInfo)[i].nodePort);
    raftId.vgId = pSyncNode->vgId;

    if (syncUtilSameId(&raftId, &(pSyncNode->myRaftId))) {
      b2 = true;
      break;
    }
  }

  ASSERT(b1 == b2);
  return b1;
}

static bool syncIsConfigChanged(const SSyncCfg* pOldCfg, const SSyncCfg* pNewCfg) {
  if (pOldCfg->replicaNum != pNewCfg->replicaNum) return true;
  if (pOldCfg->myIndex != pNewCfg->myIndex) return true;
  for (int32_t i = 0; i < pOldCfg->replicaNum; ++i) {
    const SNodeInfo* pOldInfo = &pOldCfg->nodeInfo[i];
    const SNodeInfo* pNewInfo = &pNewCfg->nodeInfo[i];
    if (strcmp(pOldInfo->nodeFqdn, pNewInfo->nodeFqdn) != 0) return true;
    if (pOldInfo->nodePort != pNewInfo->nodePort) return true;
  }

  return false;
}

void syncNodeDoConfigChange(SSyncNode* pSyncNode, SSyncCfg* pNewConfig, SyncIndex lastConfigChangeIndex) {
  SSyncCfg oldConfig = pSyncNode->pRaftCfg->cfg;
  if (!syncIsConfigChanged(&oldConfig, pNewConfig)) {
    sInfo("vgId:1, sync not reconfig since not changed");
    return;
  }

  pSyncNode->pRaftCfg->cfg = *pNewConfig;
  pSyncNode->pRaftCfg->lastConfigIndex = lastConfigChangeIndex;

  bool IamInOld = syncNodeInConfig(pSyncNode, &oldConfig);
  bool IamInNew = syncNodeInConfig(pSyncNode, pNewConfig);

  bool isDrop = false;
  bool isAdd = false;

  if (IamInOld && !IamInNew) {
    isDrop = true;
  } else {
    isDrop = false;
  }

  if (!IamInOld && IamInNew) {
    isAdd = true;
  } else {
    isAdd = false;
  }

  // log begin config change
  char oldCfgStr[1024] = {0};
  char newCfgStr[1024] = {0};
  syncCfg2SimpleStr(&oldConfig, oldCfgStr, sizeof(oldCfgStr));
  syncCfg2SimpleStr(pNewConfig, oldCfgStr, sizeof(oldCfgStr));
  sNTrace(pSyncNode, "begin do config change, from %s to %s", oldCfgStr, oldCfgStr);

  if (IamInNew) {
    pSyncNode->pRaftCfg->isStandBy = 0;  // change isStandBy to normal
  }
  if (isDrop) {
    pSyncNode->pRaftCfg->isStandBy = 1;  // set standby
  }

  // add last config index
  raftCfgAddConfigIndex(pSyncNode->pRaftCfg, lastConfigChangeIndex);

  if (IamInNew) {
    //-----------------------------------------
    int32_t ret = 0;

    // save snapshot senders
    int32_t oldReplicaNum = pSyncNode->replicaNum;
    SRaftId oldReplicasId[TSDB_MAX_REPLICA];
    memcpy(oldReplicasId, pSyncNode->replicasId, sizeof(oldReplicasId));
    SSyncSnapshotSender* oldSenders[TSDB_MAX_REPLICA];
    for (int32_t i = 0; i < TSDB_MAX_REPLICA; ++i) {
      oldSenders[i] = (pSyncNode->senders)[i];
      sSTrace(oldSenders[i], "snapshot sender save old");
    }

    // init internal
    pSyncNode->myNodeInfo = pSyncNode->pRaftCfg->cfg.nodeInfo[pSyncNode->pRaftCfg->cfg.myIndex];
    syncUtilNodeInfo2RaftId(&pSyncNode->myNodeInfo, pSyncNode->vgId, &pSyncNode->myRaftId);

    // init peersNum, peers, peersId
    pSyncNode->peersNum = pSyncNode->pRaftCfg->cfg.replicaNum - 1;
    int32_t j = 0;
    for (int32_t i = 0; i < pSyncNode->pRaftCfg->cfg.replicaNum; ++i) {
      if (i != pSyncNode->pRaftCfg->cfg.myIndex) {
        pSyncNode->peersNodeInfo[j] = pSyncNode->pRaftCfg->cfg.nodeInfo[i];
        j++;
      }
    }
    for (int32_t i = 0; i < pSyncNode->peersNum; ++i) {
      syncUtilNodeInfo2RaftId(&pSyncNode->peersNodeInfo[i], pSyncNode->vgId, &pSyncNode->peersId[i]);
    }

    // init replicaNum, replicasId
    pSyncNode->replicaNum = pSyncNode->pRaftCfg->cfg.replicaNum;
    for (int32_t i = 0; i < pSyncNode->pRaftCfg->cfg.replicaNum; ++i) {
      syncUtilNodeInfo2RaftId(&pSyncNode->pRaftCfg->cfg.nodeInfo[i], pSyncNode->vgId, &pSyncNode->replicasId[i]);
    }

    // update quorum first
    pSyncNode->quorum = syncUtilQuorum(pSyncNode->pRaftCfg->cfg.replicaNum);

    syncIndexMgrUpdate(pSyncNode->pNextIndex, pSyncNode);
    syncIndexMgrUpdate(pSyncNode->pMatchIndex, pSyncNode);
    voteGrantedUpdate(pSyncNode->pVotesGranted, pSyncNode);
    votesRespondUpdate(pSyncNode->pVotesRespond, pSyncNode);

    // reset snapshot senders

    // clear new
    for (int32_t i = 0; i < TSDB_MAX_REPLICA; ++i) {
      (pSyncNode->senders)[i] = NULL;
    }

    // reset new
    for (int32_t i = 0; i < pSyncNode->replicaNum; ++i) {
      // reset sender
      bool reset = false;
      for (int32_t j = 0; j < TSDB_MAX_REPLICA; ++j) {
        if (syncUtilSameId(&(pSyncNode->replicasId)[i], &oldReplicasId[j])) {
          char     host[128];
          uint16_t port;
          syncUtilU642Addr((pSyncNode->replicasId)[i].addr, host, sizeof(host), &port);
          sNTrace(pSyncNode, "snapshot sender reset for: %" PRId64 ", newIndex:%d, %s:%d, %p",
                  (pSyncNode->replicasId)[i].addr, i, host, port, oldSenders[j]);

          (pSyncNode->senders)[i] = oldSenders[j];
          oldSenders[j] = NULL;
          reset = true;

          // reset replicaIndex
          int32_t oldreplicaIndex = (pSyncNode->senders)[i]->replicaIndex;
          (pSyncNode->senders)[i]->replicaIndex = i;

          sNTrace(pSyncNode, "snapshot sender udpate replicaIndex from %d to %d, %s:%d, %p, reset:%d", oldreplicaIndex,
                  i, host, port, (pSyncNode->senders)[i], reset);
        }
      }
    }

    // create new
    for (int32_t i = 0; i < TSDB_MAX_REPLICA; ++i) {
      if ((pSyncNode->senders)[i] == NULL) {
        (pSyncNode->senders)[i] = snapshotSenderCreate(pSyncNode, i);
        sSTrace((pSyncNode->senders)[i], "snapshot sender create new");
      }
    }

    // free old
    for (int32_t i = 0; i < TSDB_MAX_REPLICA; ++i) {
      if (oldSenders[i] != NULL) {
        snapshotSenderDestroy(oldSenders[i]);
        sNTrace(pSyncNode, "snapshot sender delete old %p replica-index:%d", oldSenders[i], i);
        oldSenders[i] = NULL;
      }
    }

    // persist cfg
    raftCfgPersist(pSyncNode->pRaftCfg);

    char tmpbuf[1024] = {0};
    snprintf(tmpbuf, sizeof(tmpbuf), "config change from %d to %d, index:%" PRId64 ", %s  -->  %s",
             oldConfig.replicaNum, pNewConfig->replicaNum, lastConfigChangeIndex, oldCfgStr, newCfgStr);

    // change isStandBy to normal (election timeout)
    if (pSyncNode->state == TAOS_SYNC_STATE_LEADER) {
      syncNodeBecomeLeader(pSyncNode, tmpbuf);

      // Raft 3.6.2 Committing entries from previous terms
      syncNodeAppendNoop(pSyncNode);
      syncMaybeAdvanceCommitIndex(pSyncNode);

    } else {
      syncNodeBecomeFollower(pSyncNode, tmpbuf);
    }
  } else {
    // persist cfg
    raftCfgPersist(pSyncNode->pRaftCfg);
    sNTrace(pSyncNode, "do not config change from %d to %d, index:%" PRId64 ", %s  -->  %s", oldConfig.replicaNum,
            pNewConfig->replicaNum, lastConfigChangeIndex, oldCfgStr, newCfgStr);
  }

_END:
  // log end config change
  sNTrace(pSyncNode, "end do config change, from %s to %s", oldCfgStr, newCfgStr);
}

// raft state change --------------
void syncNodeUpdateTerm(SSyncNode* pSyncNode, SyncTerm term) {
  if (term > pSyncNode->pRaftStore->currentTerm) {
    raftStoreSetTerm(pSyncNode->pRaftStore, term);
    char tmpBuf[64];
    snprintf(tmpBuf, sizeof(tmpBuf), "update term to %" PRId64, term);
    syncNodeBecomeFollower(pSyncNode, tmpBuf);
    raftStoreClearVote(pSyncNode->pRaftStore);
  }
}

void syncNodeUpdateTermWithoutStepDown(SSyncNode* pSyncNode, SyncTerm term) {
  if (term > pSyncNode->pRaftStore->currentTerm) {
    raftStoreSetTerm(pSyncNode->pRaftStore, term);
  }
}

void syncNodeStepDown(SSyncNode* pSyncNode, SyncTerm newTerm) {
  if (pSyncNode->pRaftStore->currentTerm > newTerm) {
    sNTrace(pSyncNode, "step down, ignore, new-term:%" PRId64 ", current-term:%" PRId64, newTerm,
            pSyncNode->pRaftStore->currentTerm);
    return;
  }

  do {
    sNTrace(pSyncNode, "step down, new-term:%" PRId64 ", current-term:%" PRId64, newTerm,
            pSyncNode->pRaftStore->currentTerm);
  } while (0);

  if (pSyncNode->pRaftStore->currentTerm < newTerm) {
    raftStoreSetTerm(pSyncNode->pRaftStore, newTerm);
    char tmpBuf[64];
    snprintf(tmpBuf, sizeof(tmpBuf), "step down, update term to %" PRId64, newTerm);
    syncNodeBecomeFollower(pSyncNode, tmpBuf);
    raftStoreClearVote(pSyncNode->pRaftStore);

  } else {
    if (pSyncNode->state != TAOS_SYNC_STATE_FOLLOWER) {
      syncNodeBecomeFollower(pSyncNode, "step down");
    }
  }
}

void syncNodeLeaderChangeRsp(SSyncNode* pSyncNode) { syncRespCleanRsp(pSyncNode->pSyncRespMgr); }

void syncNodeBecomeFollower(SSyncNode* pSyncNode, const char* debugStr) {
  // maybe clear leader cache
  if (pSyncNode->state == TAOS_SYNC_STATE_LEADER) {
    pSyncNode->leaderCache = EMPTY_RAFT_ID;
  }

  // state change
  pSyncNode->state = TAOS_SYNC_STATE_FOLLOWER;
  syncNodeStopHeartbeatTimer(pSyncNode);

  // reset elect timer
  syncNodeResetElectTimer(pSyncNode);

  // send rsp to client
  syncNodeLeaderChangeRsp(pSyncNode);

  // call back
  if (pSyncNode->pFsm != NULL && pSyncNode->pFsm->FpBecomeFollowerCb != NULL) {
    pSyncNode->pFsm->FpBecomeFollowerCb(pSyncNode->pFsm);
  }

  // min match index
  pSyncNode->minMatchIndex = SYNC_INDEX_INVALID;

  // trace log
  sNTrace(pSyncNode, "become follower %s", debugStr);
}

// TLA+ Spec
// \* Candidate i transitions to leader.
// BecomeLeader(i) ==
//     /\ state[i] = Candidate
//     /\ votesGranted[i] \in Quorum
//     /\ state'      = [state EXCEPT ![i] = Leader]
//     /\ nextIndex'  = [nextIndex EXCEPT ![i] =
//                          [j \in Server |-> Len(log[i]) + 1]]
//     /\ matchIndex' = [matchIndex EXCEPT ![i] =
//                          [j \in Server |-> 0]]
//     /\ elections'  = elections \cup
//                          {[eterm     |-> currentTerm[i],
//                            eleader   |-> i,
//                            elog      |-> log[i],
//                            evotes    |-> votesGranted[i],
//                            evoterLog |-> voterLog[i]]}
//     /\ UNCHANGED <<messages, currentTerm, votedFor, candidateVars, logVars>>
//
void syncNodeBecomeLeader(SSyncNode* pSyncNode, const char* debugStr) {
  pSyncNode->leaderTime = taosGetTimestampMs();

  // reset restoreFinish
  pSyncNode->restoreFinish = false;

  // state change
  pSyncNode->state = TAOS_SYNC_STATE_LEADER;

  // set leader cache
  pSyncNode->leaderCache = pSyncNode->myRaftId;

  for (int32_t i = 0; i < pSyncNode->pNextIndex->replicaNum; ++i) {
    // maybe overwrite myself, no harm
    // just do it!

    // pSyncNode->pNextIndex->index[i] = pSyncNode->pLogStore->getLastIndex(pSyncNode->pLogStore) + 1;

    // maybe wal is deleted
    SyncIndex lastIndex;
    SyncTerm  lastTerm;
    int32_t   code = syncNodeGetLastIndexTerm(pSyncNode, &lastIndex, &lastTerm);
    ASSERT(code == 0);
    pSyncNode->pNextIndex->index[i] = lastIndex + 1;
  }

  for (int32_t i = 0; i < pSyncNode->pMatchIndex->replicaNum; ++i) {
    // maybe overwrite myself, no harm
    // just do it!
    pSyncNode->pMatchIndex->index[i] = SYNC_INDEX_INVALID;
  }

  // init peer mgr
  syncNodePeerStateInit(pSyncNode);

#if 0
  // update sender private term
  SSyncSnapshotSender* pMySender = syncNodeGetSnapshotSender(pSyncNode, &(pSyncNode->myRaftId));
  if (pMySender != NULL) {
    for (int32_t i = 0; i < pSyncNode->pMatchIndex->replicaNum; ++i) {
      if ((pSyncNode->senders)[i]->privateTerm > pMySender->privateTerm) {
        pMySender->privateTerm = (pSyncNode->senders)[i]->privateTerm;
      }
    }
    (pMySender->privateTerm) += 100;
  }
#endif

  // close receiver
  if (snapshotReceiverIsStart(pSyncNode->pNewNodeReceiver)) {
    snapshotReceiverForceStop(pSyncNode->pNewNodeReceiver);
  }

  // stop elect timer
  syncNodeStopElectTimer(pSyncNode);

  // start heartbeat timer
  syncNodeStartHeartbeatTimer(pSyncNode);

  // send heartbeat right now
  syncNodeHeartbeatPeers(pSyncNode);

  // call back
  if (pSyncNode->pFsm != NULL && pSyncNode->pFsm->FpBecomeLeaderCb != NULL) {
    pSyncNode->pFsm->FpBecomeLeaderCb(pSyncNode->pFsm);
  }

  // min match index
  pSyncNode->minMatchIndex = SYNC_INDEX_INVALID;

  // trace log
  sNTrace(pSyncNode, "become leader %s", debugStr);
}

void syncNodeCandidate2Leader(SSyncNode* pSyncNode) {
  ASSERT(pSyncNode->state == TAOS_SYNC_STATE_CANDIDATE);
  ASSERT(voteGrantedMajority(pSyncNode->pVotesGranted));
  syncNodeBecomeLeader(pSyncNode, "candidate to leader");

  sNTrace(pSyncNode, "state change syncNodeCandidate2Leader");

  // Raft 3.6.2 Committing entries from previous terms
  syncNodeAppendNoop(pSyncNode);
  syncMaybeAdvanceCommitIndex(pSyncNode);

  if (pSyncNode->replicaNum > 1) {
    syncNodeReplicate(pSyncNode);
  }
}

bool syncNodeIsMnode(SSyncNode* pSyncNode) { return (pSyncNode->vgId == 1); }

int32_t syncNodePeerStateInit(SSyncNode* pSyncNode) {
  for (int32_t i = 0; i < TSDB_MAX_REPLICA; ++i) {
    pSyncNode->peerStates[i].lastSendIndex = SYNC_INDEX_INVALID;
    pSyncNode->peerStates[i].lastSendTime = 0;
  }

  return 0;
}

void syncNodeFollower2Candidate(SSyncNode* pSyncNode) {
  ASSERT(pSyncNode->state == TAOS_SYNC_STATE_FOLLOWER);
  pSyncNode->state = TAOS_SYNC_STATE_CANDIDATE;
  sNTrace(pSyncNode, "follower to candidate");
}

void syncNodeLeader2Follower(SSyncNode* pSyncNode) {
  ASSERT(pSyncNode->state == TAOS_SYNC_STATE_LEADER);
  syncNodeBecomeFollower(pSyncNode, "leader to follower");
  sNTrace(pSyncNode, "leader to follower");
}

void syncNodeCandidate2Follower(SSyncNode* pSyncNode) {
  ASSERT(pSyncNode->state == TAOS_SYNC_STATE_CANDIDATE);
  syncNodeBecomeFollower(pSyncNode, "candidate to follower");
  sNTrace(pSyncNode, "candidate to follower");
}

// just called by syncNodeVoteForSelf
// need assert
void syncNodeVoteForTerm(SSyncNode* pSyncNode, SyncTerm term, SRaftId* pRaftId) {
  ASSERT(term == pSyncNode->pRaftStore->currentTerm);
  ASSERT(!raftStoreHasVoted(pSyncNode->pRaftStore));

  raftStoreVote(pSyncNode->pRaftStore, pRaftId);
}

// simulate get vote from outside
void syncNodeVoteForSelf(SSyncNode* pSyncNode) {
  syncNodeVoteForTerm(pSyncNode, pSyncNode->pRaftStore->currentTerm, &pSyncNode->myRaftId);

  SRpcMsg rpcMsg = {0};
  int32_t ret = syncBuildRequestVoteReply(&rpcMsg, pSyncNode->vgId);
  if (ret != 0) return;

  SyncRequestVoteReply* pMsg = rpcMsg.pCont;
  pMsg->srcId = pSyncNode->myRaftId;
  pMsg->destId = pSyncNode->myRaftId;
  pMsg->term = pSyncNode->pRaftStore->currentTerm;
  pMsg->voteGranted = true;

  voteGrantedVote(pSyncNode->pVotesGranted, pMsg);
  votesRespondAdd(pSyncNode->pVotesRespond, pMsg);
  rpcFreeCont(rpcMsg.pCont);
}

// return if has a snapshot
bool syncNodeHasSnapshot(SSyncNode* pSyncNode) {
  bool      ret = false;
  SSnapshot snapshot = {.data = NULL, .lastApplyIndex = -1, .lastApplyTerm = 0, .lastConfigIndex = -1};
  if (pSyncNode->pFsm->FpGetSnapshotInfo != NULL) {
    pSyncNode->pFsm->FpGetSnapshotInfo(pSyncNode->pFsm, &snapshot);
    if (snapshot.lastApplyIndex >= SYNC_INDEX_BEGIN) {
      ret = true;
    }
  }
  return ret;
}

// return max(logLastIndex, snapshotLastIndex)
// if no snapshot and log, return -1
SyncIndex syncNodeGetLastIndex(const SSyncNode* pSyncNode) {
  SSnapshot snapshot = {.data = NULL, .lastApplyIndex = -1, .lastApplyTerm = 0, .lastConfigIndex = -1};
  if (pSyncNode->pFsm->FpGetSnapshotInfo != NULL) {
    pSyncNode->pFsm->FpGetSnapshotInfo(pSyncNode->pFsm, &snapshot);
  }
  SyncIndex logLastIndex = pSyncNode->pLogStore->syncLogLastIndex(pSyncNode->pLogStore);

  SyncIndex lastIndex = logLastIndex > snapshot.lastApplyIndex ? logLastIndex : snapshot.lastApplyIndex;
  return lastIndex;
}

// return the last term of snapshot and log
// if error, return SYNC_TERM_INVALID (by syncLogLastTerm)
SyncTerm syncNodeGetLastTerm(SSyncNode* pSyncNode) {
  SyncTerm lastTerm = 0;
  if (syncNodeHasSnapshot(pSyncNode)) {
    // has snapshot
    SSnapshot snapshot = {.data = NULL, .lastApplyIndex = -1, .lastApplyTerm = 0, .lastConfigIndex = -1};
    if (pSyncNode->pFsm->FpGetSnapshotInfo != NULL) {
      pSyncNode->pFsm->FpGetSnapshotInfo(pSyncNode->pFsm, &snapshot);
    }

    SyncIndex logLastIndex = pSyncNode->pLogStore->syncLogLastIndex(pSyncNode->pLogStore);
    if (logLastIndex > snapshot.lastApplyIndex) {
      lastTerm = pSyncNode->pLogStore->syncLogLastTerm(pSyncNode->pLogStore);
    } else {
      lastTerm = snapshot.lastApplyTerm;
    }

  } else {
    // no snapshot
    lastTerm = pSyncNode->pLogStore->syncLogLastTerm(pSyncNode->pLogStore);
  }

  return lastTerm;
}

// get last index and term along with snapshot
int32_t syncNodeGetLastIndexTerm(SSyncNode* pSyncNode, SyncIndex* pLastIndex, SyncTerm* pLastTerm) {
  *pLastIndex = syncNodeGetLastIndex(pSyncNode);
  *pLastTerm = syncNodeGetLastTerm(pSyncNode);
  return 0;
}

// return append-entries first try index
SyncIndex syncNodeSyncStartIndex(SSyncNode* pSyncNode) {
  SyncIndex syncStartIndex = syncNodeGetLastIndex(pSyncNode) + 1;
  return syncStartIndex;
}

// if index > 0, return index - 1
// else, return -1
SyncIndex syncNodeGetPreIndex(SSyncNode* pSyncNode, SyncIndex index) {
  SyncIndex preIndex = index - 1;
  if (preIndex < SYNC_INDEX_INVALID) {
    preIndex = SYNC_INDEX_INVALID;
  }

  return preIndex;
}

// if index < 0, return SYNC_TERM_INVALID
// if index == 0, return 0
// if index > 0, return preTerm
// if error, return SYNC_TERM_INVALID
SyncTerm syncNodeGetPreTerm(SSyncNode* pSyncNode, SyncIndex index) {
  if (index < SYNC_INDEX_BEGIN) {
    return SYNC_TERM_INVALID;
  }

  if (index == SYNC_INDEX_BEGIN) {
    return 0;
  }

  SyncTerm        preTerm = 0;
  SyncIndex       preIndex = index - 1;
  SSyncRaftEntry* pPreEntry = NULL;
  int32_t         code = pSyncNode->pLogStore->syncLogGetEntry(pSyncNode->pLogStore, preIndex, &pPreEntry);

  SSnapshot snapshot = {.data = NULL,
                        .lastApplyIndex = SYNC_INDEX_INVALID,
                        .lastApplyTerm = SYNC_TERM_INVALID,
                        .lastConfigIndex = SYNC_INDEX_INVALID};

  if (code == 0) {
    ASSERT(pPreEntry != NULL);
    preTerm = pPreEntry->term;
    taosMemoryFree(pPreEntry);
    return preTerm;
  } else {
    if (pSyncNode->pFsm->FpGetSnapshotInfo != NULL) {
      pSyncNode->pFsm->FpGetSnapshotInfo(pSyncNode->pFsm, &snapshot);
      if (snapshot.lastApplyIndex == preIndex) {
        return snapshot.lastApplyTerm;
      }
    }
  }

  sNError(pSyncNode, "sync node get pre term error, index:%" PRId64 ", snap-index:%" PRId64 ", snap-term:%" PRId64,
          index, snapshot.lastApplyIndex, snapshot.lastApplyTerm);
  return SYNC_TERM_INVALID;
}

// get pre index and term of "index"
int32_t syncNodeGetPreIndexTerm(SSyncNode* pSyncNode, SyncIndex index, SyncIndex* pPreIndex, SyncTerm* pPreTerm) {
  *pPreIndex = syncNodeGetPreIndex(pSyncNode, index);
  *pPreTerm = syncNodeGetPreTerm(pSyncNode, index);
  return 0;
}

static void syncNodeEqPingTimer(void* param, void* tmrId) {
  if (!syncIsInit()) return;

  SSyncNode* pNode = param;
  if (atomic_load_64(&pNode->pingTimerLogicClockUser) <= atomic_load_64(&pNode->pingTimerLogicClock)) {
    SRpcMsg rpcMsg = {0};
    int32_t code = syncBuildTimeout(&rpcMsg, SYNC_TIMEOUT_PING, atomic_load_64(&pNode->pingTimerLogicClock),
                                    pNode->pingTimerMS, pNode);
    if (code != 0) {
      sNError(pNode, "failed to build ping msg");
      rpcFreeCont(rpcMsg.pCont);
      return;
    }

    sNTrace(pNode, "enqueue ping msg");
    code = pNode->syncEqMsg(pNode->msgcb, &rpcMsg);
    if (code != 0) {
      sNError(pNode, "failed to sync enqueue ping msg since %s", terrstr());
      rpcFreeCont(rpcMsg.pCont);
      return;
    }

    taosTmrReset(syncNodeEqPingTimer, pNode->pingTimerMS, pNode, syncEnv()->pTimerManager, &pNode->pPingTimer);
  } else {
    sTrace("==syncNodeEqPingTimer== pingTimerLogicClock:%" PRId64 ", pingTimerLogicClockUser:%" PRId64,
           pNode->pingTimerLogicClock, pNode->pingTimerLogicClockUser);
  }
}

static void syncNodeEqElectTimer(void* param, void* tmrId) {
  if (!syncIsInit()) return;

  SElectTimer* pElectTimer = param;
  SSyncNode*   pNode = pElectTimer->pSyncNode;

  if (pNode == NULL) return;
  if (pNode->syncEqMsg == NULL) return;

  SRpcMsg rpcMsg = {0};
  int32_t code = syncBuildTimeout(&rpcMsg, SYNC_TIMEOUT_ELECTION, pElectTimer->logicClock, pNode->electTimerMS, pNode);

  if (code != 0) {
    sNError(pNode, "failed to build elect msg");
    taosMemoryFree(pElectTimer);
    return;
  }

  SyncTimeout* pTimeout = rpcMsg.pCont;
  sNTrace(pNode, "enqueue elect msg lc:%" PRId64, pTimeout->logicClock);

  code = pNode->syncEqMsg(pNode->msgcb, &rpcMsg);
  if (code != 0) {
    sNError(pNode, "failed to sync enqueue elect msg since %s", terrstr());
    rpcFreeCont(rpcMsg.pCont);
    taosMemoryFree(pElectTimer);
    return;
  }

  taosMemoryFree(pElectTimer);

#if 0
  // reset timer ms
  if (syncIsInit() && pNode->electBaseLine > 0) {
    pNode->electTimerMS = syncUtilElectRandomMS(pNode->electBaseLine, 2 * pNode->electBaseLine);
    taosTmrReset(syncNodeEqElectTimer, pNode->electTimerMS, pNode, syncEnv()->pTimerManager, &pNode->pElectTimer);
  } else {
    sError("sync env is stop, syncNodeEqElectTimer");
  }
#endif
}

static void syncNodeEqHeartbeatTimer(void* param, void* tmrId) {
  if (!syncIsInit()) return;

  SSyncNode* pNode = param;
  if (pNode->replicaNum > 1) {
    if (atomic_load_64(&pNode->heartbeatTimerLogicClockUser) <= atomic_load_64(&pNode->heartbeatTimerLogicClock)) {
      SRpcMsg rpcMsg = {0};
      int32_t code = syncBuildTimeout(&rpcMsg, SYNC_TIMEOUT_HEARTBEAT, atomic_load_64(&pNode->heartbeatTimerLogicClock),
                                      pNode->heartbeatTimerMS, pNode);

      if (code != 0) {
        sNError(pNode, "failed to build heartbeat msg");
        return;
      }

      sNTrace(pNode, "enqueue heartbeat timer");
      code = pNode->syncEqMsg(pNode->msgcb, &rpcMsg);
      if (code != 0) {
        sNError(pNode, "failed to enqueue heartbeat msg since %s", terrstr());
        rpcFreeCont(rpcMsg.pCont);
        return;
      }

      taosTmrReset(syncNodeEqHeartbeatTimer, pNode->heartbeatTimerMS, pNode, syncEnv()->pTimerManager,
                   &pNode->pHeartbeatTimer);

    } else {
      sTrace("==syncNodeEqHeartbeatTimer== heartbeatTimerLogicClock:%" PRId64 ", heartbeatTimerLogicClockUser:%" PRId64,
             pNode->heartbeatTimerLogicClock, pNode->heartbeatTimerLogicClockUser);
    }
  }
}

static void syncNodeEqPeerHeartbeatTimer(void* param, void* tmrId) {
  SSyncHbTimerData* pData = (SSyncHbTimerData*)param;
  SSyncNode*        pSyncNode = pData->pSyncNode;
  SSyncTimer*       pSyncTimer = pData->pTimer;

  if (pSyncNode == NULL) {
    return;
  }

  if (pSyncNode->state != TAOS_SYNC_STATE_LEADER) {
    return;
  }

  if (pSyncNode->pRaftStore == NULL) {
    return;
  }

  // sNTrace(pSyncNode, "eq peer hb timer");

  int64_t timerLogicClock = atomic_load_64(&pSyncTimer->logicClock);
  int64_t msgLogicClock = atomic_load_64(&pData->logicClock);

  if (pSyncNode->replicaNum > 1) {
    if (timerLogicClock == msgLogicClock) {
      SRpcMsg rpcMsg = {0};
      (void)syncBuildHeartbeat(&rpcMsg, pSyncNode->vgId);

      SyncHeartbeat* pSyncMsg = rpcMsg.pCont;
      pSyncMsg->srcId = pSyncNode->myRaftId;
      pSyncMsg->destId = pData->destId;
      pSyncMsg->term = pSyncNode->pRaftStore->currentTerm;
      pSyncMsg->commitIndex = pSyncNode->commitIndex;
      pSyncMsg->minMatchIndex = syncMinMatchIndex(pSyncNode);
      pSyncMsg->privateTerm = 0;

      // send msg
      syncNodeSendHeartbeat(pSyncNode, &pSyncMsg->destId, &rpcMsg);

      if (syncIsInit()) {
        taosTmrReset(syncNodeEqPeerHeartbeatTimer, pSyncTimer->timerMS, pData, syncEnv()->pTimerManager,
                     &pSyncTimer->pTimer);
      } else {
        sError("sync env is stop, syncNodeEqHeartbeatTimer");
      }

    } else {
      sTrace("==syncNodeEqPeerHeartbeatTimer== timerLogicClock:%" PRId64 ", msgLogicClock:%" PRId64 "", timerLogicClock,
             msgLogicClock);
    }
  }
}

static int32_t syncNodeEqNoop(SSyncNode* pNode) {
  if (pNode->state == TAOS_SYNC_STATE_LEADER) {
    terrno = TSDB_CODE_SYN_NOT_LEADER;
    return -1;
  }

  SyncIndex       index = pNode->pLogStore->syncLogWriteIndex(pNode->pLogStore);
  SyncTerm        term = pNode->pRaftStore->currentTerm;
  SSyncRaftEntry* pEntry = syncEntryBuildNoop(term, index, pNode->vgId);
  if (pEntry == NULL) return -1;

  SRpcMsg rpcMsg = {0};
  int32_t code = syncBuildClientRequestFromNoopEntry(&rpcMsg, pEntry, pNode->vgId);
  syncEntryDestory(pEntry);

  sNTrace(pNode, "propose msg, type:noop");
  code = (*pNode->syncEqMsg)(pNode->msgcb, &rpcMsg);
  if (code != 0) {
    sNError(pNode, "failed to propose noop msg while enqueue since %s", terrstr());
  }

  return code;
}

static void deleteCacheEntry(const void* key, size_t keyLen, void* value) { taosMemoryFree(value); }

static int32_t syncCacheEntry(SSyncLogStore* pLogStore, SSyncRaftEntry* pEntry, LRUHandle** h) {
  int32_t   code = 0;
  int32_t   entryLen = sizeof(*pEntry) + pEntry->dataLen;
  LRUStatus status = taosLRUCacheInsert(pLogStore->pCache, &pEntry->index, sizeof(pEntry->index), pEntry, entryLen,
                                        deleteCacheEntry, h, TAOS_LRU_PRIORITY_LOW);
  if (status != TAOS_LRU_STATUS_OK) {
    code = -1;
  }

  return code;
}

static int32_t syncNodeAppendNoop(SSyncNode* ths) {
  int32_t ret = 0;

  SyncIndex       index = ths->pLogStore->syncLogWriteIndex(ths->pLogStore);
  SyncTerm        term = ths->pRaftStore->currentTerm;
  SSyncRaftEntry* pEntry = syncEntryBuildNoop(term, index, ths->vgId);
  ASSERT(pEntry != NULL);

  LRUHandle* h = NULL;
  syncCacheEntry(ths->pLogStore, pEntry, &h);

  if (ths->state == TAOS_SYNC_STATE_LEADER) {
    int32_t code = ths->pLogStore->syncLogAppendEntry(ths->pLogStore, pEntry);
    if (code != 0) {
      sNError(ths, "append noop error");
      return -1;
    }
  }

  if (h) {
    taosLRUCacheRelease(ths->pLogStore->pCache, h, false);
  } else {
    syncEntryDestory(pEntry);
  }

  return ret;
}

int32_t syncNodeOnHeartbeat(SSyncNode* ths, const SRpcMsg* pRpcMsg) {
  SyncHeartbeat* pMsg = pRpcMsg->pCont;
  syncLogRecvHeartbeat(ths, pMsg, "");

  SRpcMsg rpcMsg = {0};
  (void)syncBuildHeartbeatReply(&rpcMsg, ths->vgId);

  SyncHeartbeatReply* pMsgReply = rpcMsg.pCont;
  pMsgReply->destId = pMsg->srcId;
  pMsgReply->srcId = ths->myRaftId;
  pMsgReply->term = ths->pRaftStore->currentTerm;
  pMsgReply->privateTerm = 8864;  // magic number

  if (pMsg->term == ths->pRaftStore->currentTerm && ths->state != TAOS_SYNC_STATE_LEADER) {
    syncNodeResetElectTimer(ths);
    ths->minMatchIndex = pMsg->minMatchIndex;

    if (ths->state == TAOS_SYNC_STATE_FOLLOWER) {
      // syncNodeFollowerCommit(ths, pMsg->commitIndex);
      SRpcMsg rpcMsgLocalCmd = {0};
      (void)syncBuildLocalCmd(&rpcMsgLocalCmd, ths->vgId);

      SyncLocalCmd* pSyncMsg = rpcMsgLocalCmd.pCont;
      pSyncMsg->cmd = SYNC_LOCAL_CMD_FOLLOWER_CMT;
      pSyncMsg->fcIndex = pMsg->commitIndex;

      if (ths->syncEqMsg != NULL && ths->msgcb != NULL) {
        int32_t code = ths->syncEqMsg(ths->msgcb, &rpcMsgLocalCmd);
        if (code != 0) {
          sError("vgId:%d, sync enqueue fc-commit msg error, code:%d", ths->vgId, code);
          rpcFreeCont(rpcMsgLocalCmd.pCont);
        } else {
          sTrace("vgId:%d, sync enqueue fc-commit msg, fc-index:%" PRId64, ths->vgId, pSyncMsg->fcIndex);
        }
      }
    }
  }

  if (pMsg->term >= ths->pRaftStore->currentTerm && ths->state != TAOS_SYNC_STATE_FOLLOWER) {
    // syncNodeStepDown(ths, pMsg->term);
    SRpcMsg rpcMsgLocalCmd = {0};
    (void)syncBuildLocalCmd(&rpcMsgLocalCmd, ths->vgId);

    SyncLocalCmd* pSyncMsg = rpcMsgLocalCmd.pCont;
    pSyncMsg->cmd = SYNC_LOCAL_CMD_STEP_DOWN;
    pSyncMsg->sdNewTerm = pMsg->term;

    if (ths->syncEqMsg != NULL && ths->msgcb != NULL) {
      int32_t code = ths->syncEqMsg(ths->msgcb, &rpcMsgLocalCmd);
      if (code != 0) {
        sError("vgId:%d, sync enqueue step-down msg error, code:%d", ths->vgId, code);
        rpcFreeCont(rpcMsgLocalCmd.pCont);
      } else {
        sTrace("vgId:%d, sync enqueue step-down msg, new-term: %" PRId64, ths->vgId, pSyncMsg->sdNewTerm);
      }
    }
  }

  /*
    // htonl
    SMsgHead* pHead = rpcMsg.pCont;
    pHead->contLen = htonl(pHead->contLen);
    pHead->vgId = htonl(pHead->vgId);
  */

  // reply
  syncNodeSendMsgById(&pMsgReply->destId, ths, &rpcMsg);
  return 0;
}

int32_t syncNodeOnHeartbeatReply(SSyncNode* ths, const SRpcMsg* pRpcMsg) {
  SyncHeartbeatReply* pMsg = pRpcMsg->pCont;
  syncLogRecvHeartbeatReply(ths, pMsg, "");

  // update last reply time, make decision whether the other node is alive or not
  syncIndexMgrSetRecvTime(ths->pMatchIndex, &pMsg->destId, pMsg->startTime);
  return 0;
}

int32_t syncNodeOnLocalCmd(SSyncNode* ths, const SRpcMsg* pRpcMsg) {
  SyncLocalCmd* pMsg = pRpcMsg->pCont;
  syncLogRecvLocalCmd(ths, pMsg, "");

  if (pMsg->cmd == SYNC_LOCAL_CMD_STEP_DOWN) {
    syncNodeStepDown(ths, pMsg->sdNewTerm);

  } else if (pMsg->cmd == SYNC_LOCAL_CMD_FOLLOWER_CMT) {
    syncNodeFollowerCommit(ths, pMsg->fcIndex);

  } else {
    sNError(ths, "error local cmd");
  }

  return 0;
}

// TLA+ Spec
// ClientRequest(i, v) ==
//     /\ state[i] = Leader
//     /\ LET entry == [term  |-> currentTerm[i],
//                      value |-> v]
//            newLog == Append(log[i], entry)
//        IN  log' = [log EXCEPT ![i] = newLog]
//     /\ UNCHANGED <<messages, serverVars, candidateVars,
//                    leaderVars, commitIndex>>
//

int32_t syncNodeOnClientRequest(SSyncNode* ths, SRpcMsg* pMsg, SyncIndex* pRetIndex) {
  sNTrace(ths, "on client request");

  int32_t ret = 0;
  int32_t code = 0;

  SyncIndex       index = ths->pLogStore->syncLogWriteIndex(ths->pLogStore);
  SyncTerm        term = ths->pRaftStore->currentTerm;
  SSyncRaftEntry* pEntry;

  if (pMsg->msgType == TDMT_SYNC_CLIENT_REQUEST) {
    pEntry = syncEntryBuildFromClientRequest(pMsg->pCont, term, index);
  } else {
    pEntry = syncEntryBuildFromRpcMsg(pMsg, term, index);
  }

  LRUHandle* h = NULL;
  syncCacheEntry(ths->pLogStore, pEntry, &h);

  if (ths->state == TAOS_SYNC_STATE_LEADER) {
    // append entry
    code = ths->pLogStore->syncLogAppendEntry(ths->pLogStore, pEntry);
    if (code != 0) {
      if (ths->replicaNum == 1) {
        if (h) {
          taosLRUCacheRelease(ths->pLogStore->pCache, h, false);
        } else {
          syncEntryDestory(pEntry);
        }

        return -1;

      } else {
        // del resp mgr, call FpCommitCb
        SFsmCbMeta cbMeta = {
            .index = pEntry->index,
            .lastConfigIndex = SYNC_INDEX_INVALID,
            .isWeak = pEntry->isWeak,
            .code = -1,
            .state = ths->state,
            .seqNum = pEntry->seqNum,
            .term = pEntry->term,
            .currentTerm = ths->pRaftStore->currentTerm,
            .flag = 0,
        };
        ths->pFsm->FpCommitCb(ths->pFsm, pMsg, &cbMeta);

        if (h) {
          taosLRUCacheRelease(ths->pLogStore->pCache, h, false);
        } else {
          syncEntryDestory(pEntry);
        }

        return -1;
      }
    }

    // if mulit replica, start replicate right now
    if (ths->replicaNum > 1) {
      syncNodeReplicate(ths);
    }

    // if only myself, maybe commit right now
    if (ths->replicaNum == 1) {
      if (syncNodeIsMnode(ths)) {
        syncMaybeAdvanceCommitIndex(ths);
      } else {
        syncOneReplicaAdvance(ths);
      }
    }
  }

  if (pRetIndex != NULL) {
    if (ret == 0 && pEntry != NULL) {
      *pRetIndex = pEntry->index;
    } else {
      *pRetIndex = SYNC_INDEX_INVALID;
    }
  }

  if (h) {
    taosLRUCacheRelease(ths->pLogStore->pCache, h, false);
  } else {
    syncEntryDestory(pEntry);
  }

  return ret;
}

const char* syncStr(ESyncState state) {
  switch (state) {
    case TAOS_SYNC_STATE_FOLLOWER:
      return "follower";
    case TAOS_SYNC_STATE_CANDIDATE:
      return "candidate";
    case TAOS_SYNC_STATE_LEADER:
      return "leader";
    default:
      return "error";
  }
}

#if 0
int32_t syncDoLeaderTransfer(SSyncNode* ths, SRpcMsg* pRpcMsg, SSyncRaftEntry* pEntry) {
  if (ths->state != TAOS_SYNC_STATE_FOLLOWER) {
    sNTrace(ths, "I am not follower, can not do leader transfer");
    return 0;
  }

  if (!ths->restoreFinish) {
    sNTrace(ths, "restore not finish, can not do leader transfer");
    return 0;
  }

  if (pEntry->term < ths->pRaftStore->currentTerm) {
    sNTrace(ths, "little term:%" PRId64 ", can not do leader transfer", pEntry->term);
    return 0;
  }

  if (pEntry->index < syncNodeGetLastIndex(ths)) {
    sNTrace(ths, "little index:%" PRId64 ", can not do leader transfer", pEntry->index);
    return 0;
  }

  /*
    if (ths->vgId > 1) {
      sNTrace(ths, "I am vnode, can not do leader transfer");
      return 0;
    }
  */

  SyncLeaderTransfer* pSyncLeaderTransfer = pRpcMsg->pCont;
  sNTrace(ths, "do leader transfer, index:%" PRId64, pEntry->index);

  bool sameId = syncUtilSameId(&(pSyncLeaderTransfer->newLeaderId), &(ths->myRaftId));
  bool sameNodeInfo = strcmp(pSyncLeaderTransfer->newNodeInfo.nodeFqdn, ths->myNodeInfo.nodeFqdn) == 0 &&
                      pSyncLeaderTransfer->newNodeInfo.nodePort == ths->myNodeInfo.nodePort;

  bool same = sameId || sameNodeInfo;
  if (same) {
    // reset elect timer now!
    int32_t electMS = 1;
    int32_t ret = syncNodeRestartElectTimer(ths, electMS);
    ASSERT(ret == 0);

    sNTrace(ths, "maybe leader transfer to %s:%d %" PRId64, pSyncLeaderTransfer->newNodeInfo.nodeFqdn,
            pSyncLeaderTransfer->newNodeInfo.nodePort, pSyncLeaderTransfer->newLeaderId.addr);
  }

  if (ths->pFsm->FpLeaderTransferCb != NULL) {
    SFsmCbMeta cbMeta = {
        .code = 0,
        .currentTerm = ths->pRaftStore->currentTerm,
        .flag = 0,
        .index = pEntry->index,
        .lastConfigIndex = syncNodeGetSnapshotConfigIndex(ths, pEntry->index),
        .isWeak = pEntry->isWeak,
        .seqNum = pEntry->seqNum,
        .state = ths->state,
        .term = pEntry->term,
    };
    ths->pFsm->FpLeaderTransferCb(ths->pFsm, pRpcMsg, &cbMeta);
  }

  return 0;
}

#endif

int32_t syncNodeUpdateNewConfigIndex(SSyncNode* ths, SSyncCfg* pNewCfg) {
  for (int32_t i = 0; i < pNewCfg->replicaNum; ++i) {
    SRaftId raftId;
    raftId.addr = syncUtilAddr2U64((pNewCfg->nodeInfo)[i].nodeFqdn, (pNewCfg->nodeInfo)[i].nodePort);
    raftId.vgId = ths->vgId;

    if (syncUtilSameId(&(ths->myRaftId), &raftId)) {
      pNewCfg->myIndex = i;
      return 0;
    }
  }

  return -1;
}

bool syncNodeIsOptimizedOneReplica(SSyncNode* ths, SRpcMsg* pMsg) {
  return (ths->replicaNum == 1 && syncUtilUserCommit(pMsg->msgType) && ths->vgId != 1);
}

int32_t syncNodeDoCommit(SSyncNode* ths, SyncIndex beginIndex, SyncIndex endIndex, uint64_t flag) {
  if (beginIndex > endIndex) {
    return 0;
  }

  if (ths == NULL) {
    return -1;
  }

  if (ths->pFsm != NULL && ths->pFsm->FpGetSnapshotInfo != NULL) {
    // advance commit index to sanpshot first
    SSnapshot snapshot = {0};
    ths->pFsm->FpGetSnapshotInfo(ths->pFsm, &snapshot);
    if (snapshot.lastApplyIndex >= 0 && snapshot.lastApplyIndex >= beginIndex) {
      sNTrace(ths, "commit by snapshot from index:%" PRId64 " to index:%" PRId64, beginIndex, snapshot.lastApplyIndex);

      // update begin index
      beginIndex = snapshot.lastApplyIndex + 1;
    }
  }

  int32_t    code = 0;
  ESyncState state = flag;

  sNTrace(ths, "commit by wal from index:%" PRId64 " to index:%" PRId64, beginIndex, endIndex);

  // execute fsm
  if (ths->pFsm != NULL) {
    for (SyncIndex i = beginIndex; i <= endIndex; ++i) {
      if (i != SYNC_INDEX_INVALID) {
        SSyncRaftEntry* pEntry;
        SLRUCache*      pCache = ths->pLogStore->pCache;
        LRUHandle*      h = taosLRUCacheLookup(pCache, &i, sizeof(i));
        if (h) {
          pEntry = (SSyncRaftEntry*)taosLRUCacheValue(pCache, h);
        } else {
          code = ths->pLogStore->syncLogGetEntry(ths->pLogStore, i, &pEntry);
          // ASSERT(code == 0);
          // ASSERT(pEntry != NULL);
          if (code != 0 || pEntry == NULL) {
            sNError(ths, "get log entry error");
            sFatal("vgId:%d, get log entry %" PRId64 " error when commit since %s", ths->vgId, i, terrstr());
            continue;
          }
        }

        SRpcMsg rpcMsg = {0};
        syncEntry2OriginalRpc(pEntry, &rpcMsg);

        // user commit
        if ((ths->pFsm->FpCommitCb != NULL) && syncUtilUserCommit(pEntry->originalRpcType)) {
          bool internalExecute = true;
          if ((ths->replicaNum == 1) && ths->restoreFinish && ths->vgId != 1) {
            internalExecute = false;
          }

          sNTrace(ths, "commit index:%" PRId64 ", internal:%d", i, internalExecute);

          // execute fsm in apply thread, or execute outside syncPropose
          if (internalExecute) {
            SFsmCbMeta cbMeta = {
                .index = pEntry->index,
                .lastConfigIndex = syncNodeGetSnapshotConfigIndex(ths, pEntry->index),
                .isWeak = pEntry->isWeak,
                .code = 0,
                .state = ths->state,
                .seqNum = pEntry->seqNum,
                .term = pEntry->term,
                .currentTerm = ths->pRaftStore->currentTerm,
                .flag = flag,
            };

            syncRespMgrGetAndDel(ths->pSyncRespMgr, cbMeta.seqNum, &rpcMsg.info);
            ths->pFsm->FpCommitCb(ths->pFsm, &rpcMsg, &cbMeta);
          }
        }

#if 0
        // execute in pre-commit
        // leader transfer
        if (pEntry->originalRpcType == TDMT_SYNC_LEADER_TRANSFER) {
          code = syncDoLeaderTransfer(ths, &rpcMsg, pEntry);
          ASSERT(code == 0);
        }
#endif

        // restore finish
        // if only snapshot, a noop entry will be append, so syncLogLastIndex is always ok
        if (pEntry->index == ths->pLogStore->syncLogLastIndex(ths->pLogStore)) {
          if (ths->restoreFinish == false) {
            if (ths->pFsm->FpRestoreFinishCb != NULL) {
              ths->pFsm->FpRestoreFinishCb(ths->pFsm);
            }
            ths->restoreFinish = true;

            int64_t restoreDelay = taosGetTimestampMs() - ths->leaderTime;
            sNTrace(ths, "restore finish, index:%" PRId64 ", elapsed:%" PRId64 " ms", pEntry->index, restoreDelay);
          }
        }

        rpcFreeCont(rpcMsg.pCont);
        if (h) {
          taosLRUCacheRelease(pCache, h, false);
        } else {
          syncEntryDestory(pEntry);
        }
      }
    }
  }
  return 0;
}

bool syncNodeInRaftGroup(SSyncNode* ths, SRaftId* pRaftId) {
  for (int32_t i = 0; i < ths->replicaNum; ++i) {
    if (syncUtilSameId(&((ths->replicasId)[i]), pRaftId)) {
      return true;
    }
  }
  return false;
}

SSyncSnapshotSender* syncNodeGetSnapshotSender(SSyncNode* ths, SRaftId* pDestId) {
  SSyncSnapshotSender* pSender = NULL;
  for (int32_t i = 0; i < ths->replicaNum; ++i) {
    if (syncUtilSameId(pDestId, &((ths->replicasId)[i]))) {
      pSender = (ths->senders)[i];
    }
  }
  return pSender;
}

SSyncTimer* syncNodeGetHbTimer(SSyncNode* ths, SRaftId* pDestId) {
  SSyncTimer* pTimer = NULL;
  for (int32_t i = 0; i < ths->replicaNum; ++i) {
    if (syncUtilSameId(pDestId, &((ths->replicasId)[i]))) {
      pTimer = &((ths->peerHeartbeatTimerArr)[i]);
    }
  }
  return pTimer;
}

SPeerState* syncNodeGetPeerState(SSyncNode* ths, const SRaftId* pDestId) {
  SPeerState* pState = NULL;
  for (int32_t i = 0; i < ths->replicaNum; ++i) {
    if (syncUtilSameId(pDestId, &((ths->replicasId)[i]))) {
      pState = &((ths->peerStates)[i]);
    }
  }
  return pState;
}

bool syncNodeNeedSendAppendEntries(SSyncNode* ths, const SRaftId* pDestId, const SyncAppendEntries* pMsg) {
  SPeerState* pState = syncNodeGetPeerState(ths, pDestId);
  if (pState == NULL) {
    sError("vgId:%d, replica maybe dropped", ths->vgId);
    return false;
  }

  SyncIndex sendIndex = pMsg->prevLogIndex + 1;
  int64_t   tsNow = taosGetTimestampMs();

  if (pState->lastSendIndex == sendIndex && tsNow - pState->lastSendTime < SYNC_APPEND_ENTRIES_TIMEOUT_MS) {
    return false;
  }

  return true;
}

bool syncNodeCanChange(SSyncNode* pSyncNode) {
  if (pSyncNode->changing) {
    sError("sync cannot change");
    return false;
  }

  if ((pSyncNode->commitIndex >= SYNC_INDEX_BEGIN)) {
    SyncIndex lastIndex = syncNodeGetLastIndex(pSyncNode);
    if (pSyncNode->commitIndex != lastIndex) {
      sError("sync cannot change2");
      return false;
    }
  }

  for (int32_t i = 0; i < pSyncNode->peersNum; ++i) {
    SSyncSnapshotSender* pSender = syncNodeGetSnapshotSender(pSyncNode, &(pSyncNode->peersId)[i]);
    if (pSender != NULL && pSender->start) {
      sError("sync cannot change3");
      return false;
    }
  }

  return true;
}