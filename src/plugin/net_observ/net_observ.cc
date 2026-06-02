/*************************************************************************
 * SPDX-FileCopyrightText: Copyright (c) 2022-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * See LICENSE.txt for more license information
 *
 * Lightweight NCCL plugin that acts as a client to the net_observ_daemon.
 *
 * Architecture:
 *   net_observ_daemon (standalone process)
 *     ©À©¤©¤ gRPC server ¡û receives switch events
 *     ©À©¤©¤ NIC counter monitoring
 *     ©À©¤©¤ Alert generation (PREDICTIVE / CONFIRMED)
 *     ©À©¤©¤ Shared memory ¡û writes structured alert fields
 *     ©¸©¤©¤ UDS server   ¡û receives IB error reports
 *
 *   net_observ.cc (NCCL plugin, one per NCCL process)
 *     ©À©¤©¤ Shared memory reader  ¡û polls alerts from daemon
 *     ©À©¤©¤ UDS client            ¡û forwards IB errors to daemon
 *     ©¸©¤©¤ WARN emitter          ¡û prints alerts via NCCL logging
 *
 * No gRPC / protobuf / heavy class dependencies.
 *************************************************************************/

#include "net_observ/net_observ.h"

#include <thread>
#include <chrono>
#include <atomic>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <ctime>

#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include "checks.h"
#include "param.h"

namespace net_observ {

// =========================================================================
// Color constant definitions
// =========================================================================
const char* const COLOR_RED     = "\033[91m";
const char* const COLOR_YELLOW  = "\033[93m";
const char* const COLOR_GREEN   = "\033[92m";
const char* const COLOR_CYAN    = "\033[96m";
const char* const COLOR_MAGENTA = "\033[95m";
const char* const COLOR_BOLD    = "\033[1m";
const char* const COLOR_DIM     = "\033[2m";
const char* const COLOR_RESET   = "\033[0m";

// =========================================================================
// Global state
// =========================================================================

static SharedMemoryLayout* g_shmPtr = nullptr;
static int g_udsFd = -1;
static struct sockaddr_un g_udsAddr;
static std::thread* g_pollThread = nullptr;
static volatile bool g_stopRequested = false;
static uint64_t g_lastShmVersion = 0;
static uint32_t g_seqNum = 0;
static int g_myTpRank = -1;
static int g_lastProcessedPerRankCount = 0;

// =========================================================================
// Helper: map NCCL collective op code to string
// =========================================================================
static const char* ncclFuncStr(int coll) {
  switch (coll) {
    case 0: return "Broadcast";
    case 1: return "Reduce";
    case 2: return "AllGather";
    case 3: return "ReduceScatter";
    case 4: return "AllReduce";
    case 5: return "SendRecv";
    case 6: return "Send";
    case 7: return "Recv";
    case 8: return "AlltoAll";
    case 9: return "Scatter";
    case 10: return "Gather";
    case 11: return "AllGatherV";
    case 12: return "PutSignal";
    case 13: return "Signal";
    case 14: return "WaitSignal";
    default: return "Unknown";
  }
}

// =========================================================================
// Helper: format epoch seconds to "YYYY-MM-DD HH:MM:SS UTC"
// =========================================================================
static std::string formatEpochSeconds(int64_t epochSec) {
  if (epochSec <= 0) return "N/A";
  time_t t = static_cast<time_t>(epochSec);
  struct tm utcTm;
  gmtime_r(&t, &utcTm);
  char buf[64];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &utcTm);
  return std::string(buf);
}

// =========================================================================
// Helper: get local RDMA NIC IP from mlx5 device name
// =========================================================================
static std::string getLocalRdmaNicIp(const std::string& mlx5Dev) {
  std::string netDir = "/sys/class/infiniband/" + mlx5Dev + "/device/net/";
  DIR* dir = opendir(netDir.c_str());
  if (!dir) return "";
  struct dirent* entry;
  std::string netIface;
  while ((entry = readdir(dir)) != nullptr) {
    if (entry->d_name[0] == '.') continue;
    netIface = entry->d_name;
    break;
  }
  closedir(dir);
  if (netIface.empty()) return "";

  std::string ipCmd = "ip -o -4 addr show " + netIface + " 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -1";
  FILE* fp = popen(ipCmd.c_str(), "r");
  if (!fp) return "";
  char buf[64];
  std::string ip;
  if (fgets(buf, sizeof(buf), fp)) {
    ip = std::string(buf);
    // trim whitespace
    while (!ip.empty() && (ip.back() == ' ' || ip.back() == '\t' || ip.back() == '\n' || ip.back() == '\r'))
      ip.pop_back();
  }
  pclose(fp);
  return ip;
}

// =========================================================================
// Shared memory poll thread
// =========================================================================
static void shmPollLoop() {
  while (!g_stopRequested) {
    if (g_shmPtr) {
      uint64_t version = g_shmPtr->version;
      if (version != g_lastShmVersion && version > 0) {
        g_lastShmVersion = version;

        std::string status(g_shmPtr->status);

        if (status == "CONFIRMED") {
          std::string tag(g_shmPtr->alertTag);
          std::string ts(g_shmPtr->timestamp);
          std::string port(g_shmPtr->portName);
          std::string nic(g_shmPtr->rdmaNic);
          int64_t createTime = g_shmPtr->createTime;

          int currentCount = g_shmPtr->perRankAlertCount;
          if (currentCount > 8) currentCount = 8;

          for (int i = g_lastProcessedPerRankCount; i < currentCount; i++) {
            const PerRankAlertEntry& entry = g_shmPtr->perRankAlerts[i];

            if (g_myTpRank >= 0 && entry.tpRank != g_myTpRank) {
              continue;
            }

            std::string rankStr = "Rank-" + std::to_string(entry.tpRank)
                + ", Rank-" + std::to_string(entry.tpRemoteRank);

            std::string collStr = "N/A";
            if (entry.coll >= 0) {
              collStr = std::string(ncclFuncStr(entry.coll)) + " (" + std::to_string(entry.coll) + ")";
            }

            std::string timeToConfirm = "N/A";
            if (entry.confirmTime > 0 && createTime > 0) {
              double elapsed = static_cast<double>(entry.confirmTime - createTime);
              char elapsedBuf[32];
              snprintf(elapsedBuf, sizeof(elapsedBuf), "%.1fs", elapsed);
              timeToConfirm = elapsedBuf;
            }

            fprintf(stderr, "%s%s========================================================================%s\n",
                    COLOR_RED, COLOR_BOLD, COLOR_RESET);
            fprintf(stderr, "%s%s[NETWORK_ADVISOR][CONFIRMED] RDMA Timeout - Fault Path Confirmed!%s\n",
                    COLOR_RED, COLOR_BOLD, COLOR_RESET);
            fprintf(stderr, "%s%s========================================================================%s\n",
                    COLOR_RED, COLOR_BOLD, COLOR_RESET);
            fprintf(stderr, "  %sIncident ID:%s    %s\n", COLOR_CYAN, COLOR_RESET, tag.c_str());
            fprintf(stderr, "  %sOriginal Time:%s  %s\n", COLOR_CYAN, COLOR_RESET, ts.c_str());
            fprintf(stderr, "  %sConfirmed At:%s   %s\n", COLOR_CYAN, COLOR_RESET,
                    formatEpochSeconds(entry.confirmTime).c_str());
            fprintf(stderr, "  %sTime to Confirm:%s %s\n", COLOR_CYAN, COLOR_RESET, timeToConfirm.c_str());
            fprintf(stderr, "  %sAffected Ranks:%s %s\n", COLOR_CYAN, COLOR_RESET, rankStr.c_str());
            fprintf(stderr, "  %sAffected Port:%s  %s\n", COLOR_CYAN, COLOR_RESET, port.c_str());
            fprintf(stderr, "  %sSwitch Drop:%s    %ld packets\n", COLOR_CYAN, COLOR_RESET, (long)entry.dropCount);
            fprintf(stderr, "  %sRDMA NIC:%s       %s\n", COLOR_CYAN, COLOR_RESET, nic.c_str());

            int perRankNicCount = entry.nicDeltaCount;
            if (perRankNicCount > 2) perRankNicCount = 2;
            if (perRankNicCount > 0) {
              fprintf(stderr, "  %sNIC Counter Delta (cumulative):%s\n", COLOR_CYAN, COLOR_RESET);
              for (int j = 0; j < perRankNicCount; j++) {
                std::string marker;
                if (strcmp(entry.nicDeltaNames[j], "roce_adp_retrans") == 0 ||
                    strcmp(entry.nicDeltaNames[j], "roce_adp_retrans_to") == 0)
                  marker = " <<<";
                fprintf(stderr, "    %s%s:%s +%ld%s\n", COLOR_DIM,
                        entry.nicDeltaNames[j], COLOR_RESET,
                        (long)entry.nicDeltaValues[j], marker.c_str());
              }
            }

            if (strlen(entry.faultPath) > 0) {
              fprintf(stderr, "  %sConfirmed Fault Path:%s %s\n",
                      COLOR_CYAN, COLOR_RESET, entry.faultPath);
            }
            if (strlen(entry.ncclError) > 0) {
              fprintf(stderr, "  %sNCCL Error:%s    %s\n", COLOR_CYAN, COLOR_RESET,
                      entry.ncclError);
            }
            if (entry.coll >= 0) {
              fprintf(stderr, "  %sNCCL Op:%s       %s\n", COLOR_CYAN, COLOR_RESET, collStr.c_str());
            }
            fprintf(stderr, "  %sRoot Cause:%s     [Confirmed] Switch drop caused RDMA timeout on %s\n",
                    COLOR_CYAN, COLOR_RESET, nic.c_str());
            fprintf(stderr, "  %sImpact:%s         Training stalled, NCCL timeout likely\n",
                    COLOR_CYAN, COLOR_RESET);
            fprintf(stderr, "  %sStatus:%s        %s%sCONFIRMED%s - RDMA timeout reached\n",
                    COLOR_CYAN, COLOR_RESET, COLOR_RED, COLOR_BOLD, COLOR_RESET);
            fprintf(stderr, "  %sSuggestion:%s     Immediate action required: check %s and %s\n",
                    COLOR_CYAN, COLOR_RESET, port.c_str(), nic.c_str());
            fprintf(stderr, "%s%s========================================================================%s\n",
                    COLOR_RED, COLOR_BOLD, COLOR_RESET);
          }

          g_lastProcessedPerRankCount = currentCount;
        }
      }
    }

    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 100000000;
    nanosleep(&ts, nullptr);
  }
}

// =========================================================================
// ncclNetObservInit ¡ª connect to daemon via shared memory + UDS
// =========================================================================
int ncclNetObservInit(void) {
  const char* enable = ncclGetEnv("NCCL_NET_OBSERV_ENABLE");
  if (!enable || strcmp(enable, "1") != 0) {
    return 0;
  }

  // 1. Open shared memory (read-only) ¡ª daemon writes alerts here
  int shmFd = shm_open(NET_OBSERV_SHM_NAME, O_RDONLY, 0666);
  if (shmFd < 0) {
    INFO(NCCL_NET, "NET/OBSERV: Daemon shared memory not found (daemon not running?), skipping shared memory connection");
    // Don't fail ¡ª UDS may still work for sending IB errors; poll thread just won't emit
  } else {
    g_shmPtr = (SharedMemoryLayout*)mmap(nullptr, sizeof(SharedMemoryLayout),
                                         PROT_READ, MAP_SHARED, shmFd, 0);
    close(shmFd);
    if (g_shmPtr == MAP_FAILED) {
      g_shmPtr = nullptr;
      INFO(NCCL_NET, "NET/OBSERV: Failed to mmap shared memory");
    }
  }

  // 2. Connect UDS datagram socket for sending IB errors to daemon
  g_udsFd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (g_udsFd < 0) {
    INFO(NCCL_NET, "NET/OBSERV: Failed to create UDS socket");
  } else {
    memset(&g_udsAddr, 0, sizeof(g_udsAddr));
    g_udsAddr.sun_family = AF_UNIX;
    strncpy(g_udsAddr.sun_path, NET_OBSERV_UDS_PATH, sizeof(g_udsAddr.sun_path) - 1);
  }

  // 3. Start shared memory poll thread
  if (g_shmPtr) {
    g_lastShmVersion = g_shmPtr->version;
  }
  g_stopRequested = false;
  g_pollThread = new std::thread(shmPollLoop);

  INFO(NCCL_INIT|NCCL_NET, "NET/OBSERV: Connected to daemon (shm=%s, uds=%s)",
       g_shmPtr ? "ok" : "no",
       g_udsFd >= 0 ? "ok" : "no");
  return 0;
}

// =========================================================================
// ncclNetObservFinalize ¡ª disconnect from daemon and clean up
// =========================================================================
void ncclNetObservFinalize(void) {
  g_stopRequested = true;
  if (g_pollThread && g_pollThread->joinable()) {
    g_pollThread->join();
    delete g_pollThread;
    g_pollThread = nullptr;
  }
  if (g_shmPtr) {
    munmap(g_shmPtr, sizeof(SharedMemoryLayout));
    g_shmPtr = nullptr;
  }
  if (g_udsFd >= 0) {
    close(g_udsFd);
    g_udsFd = -1;
  }
  INFO(NCCL_INIT|NCCL_NET, "NET/OBSERV: Disconnected from daemon");
}

// =========================================================================
// ncclNetObservHandleIbError ¡ª forward IB error to daemon via UDS
// =========================================================================
void ncclNetObservHandleIbError(const char* rdmaNic, const char* peerIp,
                                 int wcStatus, int tpRank, int tpRemoteRank,
                                 int coll, bool isSend) {
  if (!rdmaNic || g_udsFd < 0) {
    INFO(NCCL_NET, "NET/OBSERV: handleIbError skipped (rdmaNic=%p, udsFd=%d)",
         (void*)rdmaNic, g_udsFd);
    return;
  }

  g_myTpRank = tpRank;

  IbErrorReport report;
  report.seqNum = ++g_seqNum;
  report.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();

  strncpy(report.rdmaNic, rdmaNic, sizeof(report.rdmaNic) - 1);
  if (peerIp) {
    strncpy(report.peerIp, peerIp, sizeof(report.peerIp) - 1);
  }
  report.wcStatus = wcStatus;
  report.tpRank = tpRank;
  report.tpRemoteRank = tpRemoteRank;
  report.coll = coll;
  report.isSend = isSend ? 1 : 0;

  INFO(NCCL_NET, "NET/OBSERV: handleIbError sending to daemon (rdmaNic=%s, peerIp=%s, wcStatus=%d, tpRank=%d, tpRemoteRank=%d, coll=%d, isSend=%d, seq=%u)",
       rdmaNic, peerIp ? peerIp : "", wcStatus, tpRank, tpRemoteRank, coll, isSend, report.seqNum);

  ssize_t sent = sendto(g_udsFd, &report, sizeof(report), 0,
                        (struct sockaddr*)&g_udsAddr, sizeof(g_udsAddr));
  if (sent < 0) {
    INFO(NCCL_NET, "NET/OBSERV: handleIbError sendto failed (seq=%u, errno=%d: %s)",
         report.seqNum, errno, strerror(errno));
  } else {
    INFO(NCCL_NET, "NET/OBSERV: handleIbError sendto succeeded (seq=%u, sent=%zd bytes)",
         report.seqNum, sent);
  }
}

}  // namespace net_observ
