/*************************************************************************
 * SPDX-FileCopyrightText: Copyright (c) 2022-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * See LICENSE.txt for more license information
 *
 * Lightweight NCCL plugin header for net_observ daemon client.
 * No gRPC / protobuf / heavy class dependencies.
 * Communicates with the standalone net_observ_daemon via:
 *   - Shared memory (read alerts from daemon)
 *   - Unix Domain Socket (send IB errors to daemon)
 *************************************************************************/

#ifndef NCCL_NET_OBSERV_H_
#define NCCL_NET_OBSERV_H_

#include <cstdint>
#include <cstring>
#include <sys/un.h>

namespace net_observ {

// =========================================================================
// Color constants for WARN output formatting
// =========================================================================
extern const char* const COLOR_RED;
extern const char* const COLOR_YELLOW;
extern const char* const COLOR_GREEN;
extern const char* const COLOR_CYAN;
extern const char* const COLOR_MAGENTA;
extern const char* const COLOR_BOLD;
extern const char* const COLOR_DIM;
extern const char* const COLOR_RESET;

// =========================================================================
// IPC constants and types (mirrors net_observ_daemon/include/net_observ_ipc.h)
// =========================================================================

// Unix Domain Socket path (daemon listens here for IB error reports)
constexpr const char* NET_OBSERV_UDS_PATH = "/tmp/nccl_net_observ.sock";

// Shared memory segment name (daemon writes alerts here)
constexpr const char* NET_OBSERV_SHM_NAME = "/nccl_net_observ_shm";

// UDS message types
enum class IpcMessageType : uint32_t {
  IB_ERROR_REPORT = 1,
  HEARTBEAT = 2,
  SHUTDOWN = 3,
  RESET_BASELINE = 4,
};

// IB error report message (fixed-size for non-blocking datagram send)
struct IbErrorReport {
  IpcMessageType type;
  uint32_t seqNum;
  int64_t timestamp;  // microseconds since epoch

  char rdmaNic[64];
  char peerIp[64];
  int wcStatus;
  int tpRank;
  int tpRemoteRank;
  int coll;
  int isSend;  // bool

  IbErrorReport() {
    memset(this, 0, sizeof(*this));
    type = IpcMessageType::IB_ERROR_REPORT;
  }
};

static_assert(sizeof(IbErrorReport) <= 4096,
              "IbErrorReport must fit within typical datagram size");

// Baseline reset request message (NCCL init ¡ú daemon)
// This is a global operation, no rank-specific info needed
struct BaselineResetRequest {
  IpcMessageType type;
  uint32_t seqNum;
  int64_t timestamp;

  BaselineResetRequest() {
    memset(this, 0, sizeof(*this));
    type = IpcMessageType::RESET_BASELINE;
  }
};

static_assert(sizeof(BaselineResetRequest) <= 4096,
              "BaselineResetRequest must fit within typical datagram size");

// Topology table entry (rdmaNic ¡ú portName mapping from LLDP)
struct TopologyTableEntry {
  char rdmaNic[32];    // e.g. "mlx5_0"
  char rdmaNicIp[64];  // e.g. "192.168.1.10"
  char portName[64];   // e.g. "TFGigabitEthernet 0/31"
  char nodeIp[64];     // e.g. "10.110.181.242"
};

// Per-rank alert entry (rank-specific data for CONFIRMED alerts)
struct PerRankAlertEntry {
  int32_t tpRank;            // this rank
  int32_t tpRemoteRank;      // remote rank
  int32_t coll;              // NCCL collective op code
  int32_t isSend;            // 1=Send, 0=Recv
  char ncclError[256];       // per-rank error message
  char faultPath[512];       // per-rank fault path
  int64_t confirmTime;       // epoch seconds when this entry was confirmed
  // Per-rank switch drop & NIC deltas (captured at UDS arrival time)
  int64_t dropCount;         // per-rank switch drop count
  int32_t nicDeltaCount;     // number of valid nic name/value pairs below
  int32_t padNic_;           // explicit padding
  char nicDeltaNames[2][32]; // NIC counter names (e.g. "roce_adp_retrans")
  int64_t nicDeltaValues[2]; // NIC counter delta values
};

// Shared memory layout (daemon writes, NCCL plugin reads)
struct SharedMemoryLayout {
  volatile uint64_t version;   // incremented on each update (reader polls this)

  // ---- Common alert fields (shared by all ranks) ----
  char alertTag[32];           // "NET-10006"
  char timestamp[64];          // "2026-06-01 06:37:30 UTC"
  char portName[64];           // "TFGigabitEthernet 0/31"
  char rdmaNic[32];            // "mlx5_0"
  char deviceModel[64];        // switch model
  char switchName[64];         // switch name

  char status[16];             // "WATCH" / "PREDICTIVE" / "CONFIRMED"

  int64_t createTime;          // epoch seconds of incident creation

  // ---- Per-rank alert entries (each rank sees its own) ----
  int32_t perRankAlertCount;   // number of valid entries in perRankAlerts[]
  int32_t pad2_;               // explicit padding
  PerRankAlertEntry perRankAlerts[8];   // up to 8 per-rank alerts

  // ---- Topology table ----
  int32_t topologyEntryCount;  // number of valid entries in topologyTable[]
  int32_t pad3_;               // explicit padding
  TopologyTableEntry topologyTable[8];  // rdmaNic ¡ú portName mapping

  char reserved[128];          // future use
};

static_assert(sizeof(SharedMemoryLayout) <= 16384,
              "SharedMemoryLayout should fit within 16KB");

// =========================================================================
// C interface functions (called from init.cc and p2p.cc)
// =========================================================================

int ncclNetObservInit(void);
void ncclNetObservFinalize(void);
void ncclNetObservSetTpRank(int tpRank);
void ncclNetObservHandleIbError(const char* rdmaNic, const char* peerIp,
                                 int wcStatus, int tpRank, int tpRemoteRank,
                                 int coll, bool isSend);

}  // namespace net_observ

#endif  // NCCL_NET_OBSERV_H_
