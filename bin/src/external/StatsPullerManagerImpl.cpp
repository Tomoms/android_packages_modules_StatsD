/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define DEBUG true
#include "Log.h"

#include <android/os/IStatsCompanionService.h>
#include <cutils/log.h>
#include <algorithm>
#include <climits>
#include "CpuTimePerUidFreqPuller.h"
#include "CpuTimePerUidPuller.h"
#include "ResourcePowerManagerPuller.h"
#include "StatsCompanionServicePuller.h"
#include "StatsPullerManagerImpl.h"
#include "StatsService.h"
#include "logd/LogEvent.h"
#include "statslog.h"

#include <iostream>

using std::make_shared;
using std::map;
using std::shared_ptr;
using std::string;
using std::vector;
using std::list;

namespace android {
namespace os {
namespace statsd {

StatsPullerManagerImpl::StatsPullerManagerImpl()
    : mCurrentPullingInterval(LONG_MAX) {
    shared_ptr<StatsPuller> statsCompanionServicePuller = make_shared<StatsCompanionServicePuller>();
    shared_ptr<StatsPuller> resourcePowerManagerPuller = make_shared<ResourcePowerManagerPuller>();
    shared_ptr<StatsPuller> cpuTimePerUidPuller = make_shared<CpuTimePerUidPuller>();
    shared_ptr<StatsPuller> cpuTimePerUidFreqPuller = make_shared<CpuTimePerUidFreqPuller>();

    mPullers.insert({android::util::KERNEL_WAKELOCK_PULLED,
                     statsCompanionServicePuller});
    mPullers.insert({android::util::WIFI_BYTES_TRANSFERRED,
                     statsCompanionServicePuller});
    mPullers.insert({android::util::MOBILE_BYTES_TRANSFERRED,
                     statsCompanionServicePuller});
    mPullers.insert({android::util::WIFI_BYTES_TRANSFERRED_BY_FG_BG,
                     statsCompanionServicePuller});
    mPullers.insert({android::util::MOBILE_BYTES_TRANSFERRED_BY_FG_BG,
                     statsCompanionServicePuller});
    mPullers.insert({android::util::POWER_STATE_PLATFORM_SLEEP_STATE_PULLED,
                     resourcePowerManagerPuller});
    mPullers.insert({android::util::POWER_STATE_VOTER_PULLED,
                     resourcePowerManagerPuller});
    mPullers.insert({android::util::POWER_STATE_SUBSYSTEM_SLEEP_STATE_PULLED,
                     resourcePowerManagerPuller});
    mPullers.insert({android::util::CPU_TIME_PER_UID_PULLED, cpuTimePerUidPuller});
    mPullers.insert({android::util::CPU_TIME_PER_UID_FREQ_PULLED, cpuTimePerUidFreqPuller});

    mStatsCompanionService = StatsService::getStatsCompanionService();
}

bool StatsPullerManagerImpl::Pull(int tagId, vector<shared_ptr<LogEvent>>* data) {
    if (DEBUG) ALOGD("Initiating pulling %d", tagId);

    if (mPullers.find(tagId) != mPullers.end()) {
        bool ret = mPullers.find(tagId)->second->Pull(tagId, data);
        ALOGD("pulled %d items", (int)data->size());
        return ret;
    } else {
        ALOGD("Unknown tagId %d", tagId);
        return false;  // Return early since we don't know what to pull.
    }
}

StatsPullerManagerImpl& StatsPullerManagerImpl::GetInstance() {
    static StatsPullerManagerImpl instance;
    return instance;
}

bool StatsPullerManagerImpl::PullerForMatcherExists(int tagId) const {
    return mPullers.find(tagId) != mPullers.end();
}

void StatsPullerManagerImpl::RegisterReceiver(int tagId, wp<PullDataReceiver> receiver,
                                              long intervalMs) {
    AutoMutex _l(mReceiversLock);
    auto& receivers = mReceivers[tagId];
    for (auto it = receivers.begin(); it != receivers.end(); it++) {
        if (it->receiver == receiver) {
            VLOG("Receiver already registered of %d", (int)receivers.size());
            return;
        }
    }
    ReceiverInfo receiverInfo;
    receiverInfo.receiver = receiver;
    receiverInfo.timeInfo.first = intervalMs;
    receivers.push_back(receiverInfo);

    // Round it to the nearest minutes. This is the limit of alarm manager.
    // In practice, we should limit it higher.
    long roundedIntervalMs = intervalMs/1000/60 * 1000 * 60;
    // There is only one alarm for all pulled events. So only set it to the smallest denom.
    if (roundedIntervalMs < mCurrentPullingInterval) {
        VLOG("Updating pulling interval %ld", intervalMs);
        mCurrentPullingInterval = roundedIntervalMs;
        long currentTimeMs = time(nullptr) * 1000;
        long nextAlarmTimeMs = currentTimeMs + mCurrentPullingInterval - (currentTimeMs - mTimeBaseSec * 1000) % mCurrentPullingInterval;
        if (mStatsCompanionService != nullptr) {
            mStatsCompanionService->setPullingAlarms(nextAlarmTimeMs, mCurrentPullingInterval);
        } else {
            VLOG("Failed to update pulling interval");
        }
    }
    VLOG("Puller for tagId %d registered of %d", tagId, (int)receivers.size());
}

void StatsPullerManagerImpl::UnRegisterReceiver(int tagId, wp<PullDataReceiver> receiver) {
    AutoMutex _l(mReceiversLock);
    if (mReceivers.find(tagId) == mReceivers.end()) {
        VLOG("Unknown pull code or no receivers: %d", tagId);
        return;
    }
    auto& receivers = mReceivers.find(tagId)->second;
    for (auto it = receivers.begin(); it != receivers.end(); it++) {
        if (receiver == it->receiver) {
            receivers.erase(it);
            VLOG("Puller for tagId %d unregistered of %d", tagId, (int)receivers.size());
            return;
        }
    }
}

void StatsPullerManagerImpl::OnAlarmFired() {
    AutoMutex _l(mReceiversLock);

    uint64_t currentTimeMs = time(nullptr) /60 * 60 * 1000;

    vector<pair<int, vector<ReceiverInfo*>>> needToPull =
            vector<pair<int, vector<ReceiverInfo*>>>();
    for (auto& pair : mReceivers) {
        vector<ReceiverInfo*> receivers = vector<ReceiverInfo*>();
        if (pair.second.size() != 0) {
            for (auto& receiverInfo : pair.second) {
                if (receiverInfo.timeInfo.first + receiverInfo.timeInfo.second > currentTimeMs) {
                    receivers.push_back(&receiverInfo);
                }
            }
            if (receivers.size() > 0) {
                needToPull.push_back(make_pair(pair.first, receivers));
            }
        }
    }

    for (const auto& pullInfo : needToPull) {
        vector<shared_ptr<LogEvent>> data;
        if (Pull(pullInfo.first, &data)) {
            for (const auto& receiverInfo : pullInfo.second) {
                sp<PullDataReceiver> receiverPtr = receiverInfo->receiver.promote();
                if (receiverPtr != nullptr) {
                    receiverPtr->onDataPulled(data);
                    receiverInfo->timeInfo.second = currentTimeMs;
                } else {
                    VLOG("receiver already gone.");
                }
            }
        }
    }
}

}  // namespace statsd
}  // namespace os
}  // namespace android
