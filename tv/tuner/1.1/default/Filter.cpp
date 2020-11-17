/*
 * Copyright 2020 The Android Open Source Project
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

#define LOG_TAG "android.hardware.tv.tuner@1.1-Filter"

#include "Filter.h"
#include <utils/Log.h>

namespace android {
namespace hardware {
namespace tv {
namespace tuner {
namespace V1_0 {
namespace implementation {

#define WAIT_TIMEOUT 3000000000

Filter::Filter() {}

Filter::Filter(DemuxFilterType type, uint64_t filterId, uint32_t bufferSize,
               const sp<IFilterCallback>& cb, sp<Demux> demux) {
    mType = type;
    mFilterId = filterId;
    mBufferSize = bufferSize;
    mDemux = demux;

    switch (mType.mainType) {
        case DemuxFilterMainType::TS:
            if (mType.subType.tsFilterType() == DemuxTsFilterType::AUDIO ||
                mType.subType.tsFilterType() == DemuxTsFilterType::VIDEO) {
                mIsMediaFilter = true;
            }
            if (mType.subType.tsFilterType() == DemuxTsFilterType::PCR) {
                mIsPcrFilter = true;
            }
            if (mType.subType.tsFilterType() == DemuxTsFilterType::RECORD) {
                mIsRecordFilter = true;
            }
            break;
        case DemuxFilterMainType::MMTP:
            if (mType.subType.mmtpFilterType() == DemuxMmtpFilterType::AUDIO ||
                mType.subType.mmtpFilterType() == DemuxMmtpFilterType::VIDEO) {
                mIsMediaFilter = true;
            }
            if (mType.subType.mmtpFilterType() == DemuxMmtpFilterType::RECORD) {
                mIsRecordFilter = true;
            }
            break;
        case DemuxFilterMainType::IP:
            break;
        case DemuxFilterMainType::TLV:
            break;
        case DemuxFilterMainType::ALP:
            break;
        default:
            break;
    }

    sp<V1_1::IFilterCallback> filterCallback_v1_1 = V1_1::IFilterCallback::castFrom(cb);
    if (filterCallback_v1_1 != NULL) {
        mCallback_1_1 = filterCallback_v1_1;
    } else {
        mCallback = cb;
    }
}

Filter::~Filter() {}

Return<void> Filter::getId64Bit(getId64Bit_cb _hidl_cb) {
    ALOGV("%s", __FUNCTION__);

    _hidl_cb(Result::SUCCESS, mFilterId);
    return Void();
}

Return<void> Filter::getId(getId_cb _hidl_cb) {
    ALOGV("%s", __FUNCTION__);

    _hidl_cb(Result::SUCCESS, static_cast<uint32_t>(mFilterId));
    return Void();
}

Return<Result> Filter::setDataSource(const sp<V1_0::IFilter>& filter) {
    ALOGV("%s", __FUNCTION__);

    mDataSource = filter;
    mIsDataSourceDemux = false;

    return Result::SUCCESS;
}

Return<void> Filter::getQueueDesc(getQueueDesc_cb _hidl_cb) {
    ALOGV("%s", __FUNCTION__);

    mIsUsingFMQ = mIsRecordFilter ? false : true;

    _hidl_cb(Result::SUCCESS, *mFilterMQ->getDesc());
    return Void();
}

Return<Result> Filter::configure(const DemuxFilterSettings& settings) {
    ALOGV("%s", __FUNCTION__);

    mFilterSettings = settings;
    switch (mType.mainType) {
        case DemuxFilterMainType::TS:
            mTpid = settings.ts().tpid;
            break;
        case DemuxFilterMainType::MMTP:
            break;
        case DemuxFilterMainType::IP:
            break;
        case DemuxFilterMainType::TLV:
            break;
        case DemuxFilterMainType::ALP:
            break;
        default:
            break;
    }

    mConfigured = true;
    return Result::SUCCESS;
}

Return<Result> Filter::start() {
    ALOGV("%s", __FUNCTION__);

    return startFilterLoop();
}

Return<Result> Filter::stop() {
    ALOGV("%s", __FUNCTION__);

    mFilterThreadRunning = false;

    return Result::SUCCESS;
}

Return<Result> Filter::flush() {
    ALOGV("%s", __FUNCTION__);

    // temp implementation to flush the FMQ
    int size = mFilterMQ->availableToRead();
    char* buffer = new char[size];
    mFilterMQ->read((unsigned char*)&buffer[0], size);
    delete[] buffer;
    mFilterStatus = DemuxFilterStatus::DATA_READY;

    return Result::SUCCESS;
}

Return<Result> Filter::releaseAvHandle(const hidl_handle& avMemory, uint64_t avDataId) {
    ALOGV("%s", __FUNCTION__);

    if ((avMemory.getNativeHandle()->numFds > 0) &&
        (mSharedAvMemHandle.getNativeHandle()->numFds > 0) &&
        (sameFile(avMemory.getNativeHandle()->data[0],
                  mSharedAvMemHandle.getNativeHandle()->data[0]))) {
        freeSharedAvHandle();
        return Result::SUCCESS;
    }

    if (mDataId2Avfd.find(avDataId) == mDataId2Avfd.end()) {
        return Result::INVALID_ARGUMENT;
    }

    ::close(mDataId2Avfd[avDataId]);
    return Result::SUCCESS;
}

Return<Result> Filter::close() {
    ALOGV("%s", __FUNCTION__);

    return mDemux->removeFilter(mFilterId);
}

Return<Result> Filter::configureIpCid(uint32_t ipCid) {
    ALOGV("%s", __FUNCTION__);

    if (mType.mainType != DemuxFilterMainType::IP) {
        return Result::INVALID_STATE;
    }

    mCid = ipCid;
    return Result::SUCCESS;
}

Return<void> Filter::getAvSharedHandle(getAvSharedHandle_cb _hidl_cb) {
    ALOGV("%s", __FUNCTION__);

    if (!mIsMediaFilter) {
        _hidl_cb(Result::INVALID_STATE, NULL, BUFFER_SIZE_16M);
        return Void();
    }

    if (mSharedAvMemHandle.getNativeHandle() != nullptr) {
        _hidl_cb(Result::SUCCESS, mSharedAvMemHandle, BUFFER_SIZE_16M);
        mUsingSharedAvMem = true;
        return Void();
    }

    int av_fd = createAvIonFd(BUFFER_SIZE_16M);
    if (av_fd == -1) {
        _hidl_cb(Result::UNKNOWN_ERROR, NULL, 0);
    }

    native_handle_t* nativeHandle = createNativeHandle(av_fd);
    if (nativeHandle == NULL) {
        _hidl_cb(Result::UNKNOWN_ERROR, NULL, 0);
    }
    mSharedAvMemHandle.setTo(nativeHandle, /*shouldOwn=*/true);
    ::close(av_fd);

    _hidl_cb(Result::SUCCESS, mSharedAvMemHandle, BUFFER_SIZE_16M);
    mUsingSharedAvMem = true;
    return Void();
}

Return<Result> Filter::configureAvStreamType(const V1_1::AvStreamType& avStreamType) {
    ALOGV("%s", __FUNCTION__);

    if (!mIsMediaFilter) {
        return Result::UNAVAILABLE;
    }

    switch (avStreamType.getDiscriminator()) {
        case V1_1::AvStreamType::hidl_discriminator::audio:
            mAudioStreamType = static_cast<uint32_t>(avStreamType.audio());
            break;
        case V1_1::AvStreamType::hidl_discriminator::video:
            mVideoStreamType = static_cast<uint32_t>(avStreamType.video());
            break;
        default:
            break;
    }

    return Result::SUCCESS;
}

Return<Result> Filter::configureScramblingEvent(uint32_t statuses) {
    ALOGV("%s", __FUNCTION__);

    mStatuses = statuses;
    if (mCallback_1_1 != nullptr) {
        // Assuming current status is always NOT_SCRAMBLED
        V1_1::DemuxFilterEventExt filterEventExt;
        V1_1::DemuxFilterEventExt::Event event;
        event.scramblingStatus(V1_1::ScramblingStatus::NOT_SCRAMBLED);
        int size = filterEventExt.events.size();
        filterEventExt.events.resize(size + 1);
        filterEventExt.events[size] = event;
        DemuxFilterEvent emptyFilterEvent;

        mCallback_1_1->onFilterEvent_1_1(emptyFilterEvent, filterEventExt);
        mFilterEventExt.events.resize(0);
    } else {
        return Result::INVALID_STATE;
    }
    return Result::SUCCESS;
}

bool Filter::createFilterMQ() {
    ALOGV("%s", __FUNCTION__);

    // Create a synchronized FMQ that supports blocking read/write
    std::unique_ptr<FilterMQ> tmpFilterMQ =
            std::unique_ptr<FilterMQ>(new (std::nothrow) FilterMQ(mBufferSize, true));
    if (!tmpFilterMQ->isValid()) {
        ALOGW("[Filter] Failed to create FMQ of filter with id: %" PRIu64, mFilterId);
        return false;
    }

    mFilterMQ = std::move(tmpFilterMQ);

    if (EventFlag::createEventFlag(mFilterMQ->getEventFlagWord(), &mFilterEventFlag) != OK) {
        return false;
    }

    return true;
}

Result Filter::startFilterLoop() {
    pthread_create(&mFilterThread, NULL, __threadLoopFilter, this);
    pthread_setname_np(mFilterThread, "filter_waiting_loop");

    return Result::SUCCESS;
}

void* Filter::__threadLoopFilter(void* user) {
    Filter* const self = static_cast<Filter*>(user);
    self->filterThreadLoop();
    return 0;
}

void Filter::filterThreadLoop() {
    ALOGD("[Filter] filter %" PRIu64 " threadLoop start.", mFilterId);
    std::lock_guard<std::mutex> lock(mFilterThreadLock);
    mFilterThreadRunning = true;

    // For the first time of filter output, implementation needs to send the filter
    // Event Callback without waiting for the DATA_CONSUMED to init the process.
    while (mFilterThreadRunning) {
        if (mFilterEvent.events.size() == 0 && mFilterEventExt.events.size() == 0) {
            if (DEBUG_FILTER) {
                ALOGD("[Filter] wait for filter data output.");
            }
            usleep(1000 * 1000);
            continue;
        }

        // After successfully write, send a callback and wait for the read to be done
        if (mCallback_1_1 != nullptr) {
            if (mConfigured) {
                DemuxFilterEvent emptyEvent;
                V1_1::DemuxFilterEventExt startEvent;
                startEvent.events.resize(1);
                startEvent.events[0].startId(mStartId++);
                mCallback_1_1->onFilterEvent_1_1(emptyEvent, startEvent);
                mConfigured = false;
            }
            mCallback_1_1->onFilterEvent_1_1(mFilterEvent, mFilterEventExt);
            mFilterEventExt.events.resize(0);
        } else if (mCallback != nullptr) {
            mCallback->onFilterEvent(mFilterEvent);
        } else {
            ALOGD("[Filter] filter callback is not configured yet.");
            mFilterThreadRunning = false;
            return;
        }
        mFilterEvent.events.resize(0);

        freeAvHandle();
        mFilterStatus = DemuxFilterStatus::DATA_READY;
        if (mCallback != nullptr) {
            mCallback->onFilterStatus(mFilterStatus);
        } else if (mCallback_1_1 != nullptr) {
            mCallback_1_1->onFilterStatus(mFilterStatus);
        }
        break;
    }

    while (mFilterThreadRunning) {
        uint32_t efState = 0;
        // We do not wait for the last round of written data to be read to finish the thread
        // because the VTS can verify the reading itself.
        for (int i = 0; i < SECTION_WRITE_COUNT; i++) {
            while (mFilterThreadRunning && mIsUsingFMQ) {
                status_t status = mFilterEventFlag->wait(
                        static_cast<uint32_t>(DemuxQueueNotifyBits::DATA_CONSUMED), &efState,
                        WAIT_TIMEOUT, true /* retry on spurious wake */);
                if (status != OK) {
                    ALOGD("[Filter] wait for data consumed");
                    continue;
                }
                break;
            }

            maySendFilterStatusCallback();

            while (mFilterThreadRunning) {
                std::lock_guard<std::mutex> lock(mFilterEventLock);
                if (mFilterEvent.events.size() == 0 && mFilterEventExt.events.size() == 0) {
                    continue;
                }
                // After successfully write, send a callback and wait for the read to be done
                if (mCallback_1_1 != nullptr) {
                    mCallback_1_1->onFilterEvent_1_1(mFilterEvent, mFilterEventExt);
                    mFilterEventExt.events.resize(0);
                } else if (mCallback != nullptr) {
                    mCallback->onFilterEvent(mFilterEvent);
                }
                mFilterEvent.events.resize(0);
                break;
            }
            // We do not wait for the last read to be done
            // VTS can verify the read result itself.
            if (i == SECTION_WRITE_COUNT - 1) {
                ALOGD("[Filter] filter %" PRIu64 " writing done. Ending thread", mFilterId);
                break;
            }
        }
        mFilterThreadRunning = false;
    }

    ALOGD("[Filter] filter thread ended.");
}

void Filter::freeAvHandle() {
    if (!mIsMediaFilter) {
        return;
    }
    for (int i = 0; i < mFilterEvent.events.size(); i++) {
        ::close(mFilterEvent.events[i].media().avMemory.getNativeHandle()->data[0]);
        native_handle_delete(const_cast<native_handle_t*>(
                mFilterEvent.events[i].media().avMemory.getNativeHandle()));
    }
}

void Filter::freeSharedAvHandle() {
    if (!mIsMediaFilter) {
        return;
    }
    ::close(mSharedAvMemHandle.getNativeHandle()->data[0]);
    native_handle_delete(const_cast<native_handle_t*>(mSharedAvMemHandle.getNativeHandle()));
}

void Filter::maySendFilterStatusCallback() {
    if (!mIsUsingFMQ) {
        return;
    }
    std::lock_guard<std::mutex> lock(mFilterStatusLock);
    int availableToRead = mFilterMQ->availableToRead();
    int availableToWrite = mFilterMQ->availableToWrite();
    int fmqSize = mFilterMQ->getQuantumCount();

    DemuxFilterStatus newStatus = checkFilterStatusChange(
            availableToWrite, availableToRead, ceil(fmqSize * 0.75), ceil(fmqSize * 0.25));
    if (mFilterStatus != newStatus) {
        if (mCallback != nullptr) {
            mCallback->onFilterStatus(newStatus);
        } else if (mCallback_1_1 != nullptr) {
            mCallback_1_1->onFilterStatus(newStatus);
        }
        mFilterStatus = newStatus;
    }
}

DemuxFilterStatus Filter::checkFilterStatusChange(uint32_t availableToWrite,
                                                  uint32_t availableToRead, uint32_t highThreshold,
                                                  uint32_t lowThreshold) {
    if (availableToWrite == 0) {
        return DemuxFilterStatus::OVERFLOW;
    } else if (availableToRead > highThreshold) {
        return DemuxFilterStatus::HIGH_WATER;
    } else if (availableToRead < lowThreshold) {
        return DemuxFilterStatus::LOW_WATER;
    }
    return mFilterStatus;
}

uint16_t Filter::getTpid() {
    return mTpid;
}

void Filter::updateFilterOutput(vector<uint8_t> data) {
    std::lock_guard<std::mutex> lock(mFilterOutputLock);
    mFilterOutput.insert(mFilterOutput.end(), data.begin(), data.end());
}

void Filter::updatePts(uint64_t pts) {
    std::lock_guard<std::mutex> lock(mFilterOutputLock);
    mPts = pts;
}

void Filter::updateRecordOutput(vector<uint8_t> data) {
    std::lock_guard<std::mutex> lock(mRecordFilterOutputLock);
    mRecordFilterOutput.insert(mRecordFilterOutput.end(), data.begin(), data.end());
}

Result Filter::startFilterHandler() {
    std::lock_guard<std::mutex> lock(mFilterOutputLock);
    switch (mType.mainType) {
        case DemuxFilterMainType::TS:
            switch (mType.subType.tsFilterType()) {
                case DemuxTsFilterType::UNDEFINED:
                    break;
                case DemuxTsFilterType::SECTION:
                    startSectionFilterHandler();
                    break;
                case DemuxTsFilterType::PES:
                    startPesFilterHandler();
                    break;
                case DemuxTsFilterType::TS:
                    startTsFilterHandler();
                    break;
                case DemuxTsFilterType::AUDIO:
                case DemuxTsFilterType::VIDEO:
                    startMediaFilterHandler();
                    break;
                case DemuxTsFilterType::PCR:
                    startPcrFilterHandler();
                    break;
                case DemuxTsFilterType::TEMI:
                    startTemiFilterHandler();
                    break;
                default:
                    break;
            }
            break;
        case DemuxFilterMainType::MMTP:
            /*mmtpSettings*/
            break;
        case DemuxFilterMainType::IP:
            /*ipSettings*/
            break;
        case DemuxFilterMainType::TLV:
            /*tlvSettings*/
            break;
        case DemuxFilterMainType::ALP:
            /*alpSettings*/
            break;
        default:
            break;
    }
    return Result::SUCCESS;
}

Result Filter::startSectionFilterHandler() {
    if (mFilterOutput.empty()) {
        return Result::SUCCESS;
    }
    if (!writeSectionsAndCreateEvent(mFilterOutput)) {
        ALOGD("[Filter] filter %" PRIu64 " fails to write into FMQ. Ending thread", mFilterId);
        return Result::UNKNOWN_ERROR;
    }

    mFilterOutput.clear();

    return Result::SUCCESS;
}

Result Filter::startPesFilterHandler() {
    std::lock_guard<std::mutex> lock(mFilterEventLock);
    if (mFilterOutput.empty()) {
        return Result::SUCCESS;
    }

    for (int i = 0; i < mFilterOutput.size(); i += 188) {
        if (mPesSizeLeft == 0) {
            uint32_t prefix = (mFilterOutput[i + 4] << 16) | (mFilterOutput[i + 5] << 8) |
                              mFilterOutput[i + 6];
            if (DEBUG_FILTER) {
                ALOGD("[Filter] prefix %d", prefix);
            }
            if (prefix == 0x000001) {
                // TODO handle mulptiple Pes filters
                mPesSizeLeft = (mFilterOutput[i + 8] << 8) | mFilterOutput[i + 9];
                mPesSizeLeft += 6;
                if (DEBUG_FILTER) {
                    ALOGD("[Filter] pes data length %d", mPesSizeLeft);
                }
            } else {
                continue;
            }
        }

        int endPoint = min(184, mPesSizeLeft);
        // append data and check size
        vector<uint8_t>::const_iterator first = mFilterOutput.begin() + i + 4;
        vector<uint8_t>::const_iterator last = mFilterOutput.begin() + i + 4 + endPoint;
        mPesOutput.insert(mPesOutput.end(), first, last);
        // size does not match then continue
        mPesSizeLeft -= endPoint;
        if (DEBUG_FILTER) {
            ALOGD("[Filter] pes data left %d", mPesSizeLeft);
        }
        if (mPesSizeLeft > 0) {
            continue;
        }
        // size match then create event
        if (!writeDataToFilterMQ(mPesOutput)) {
            ALOGD("[Filter] pes data write failed");
            mFilterOutput.clear();
            return Result::INVALID_STATE;
        }
        maySendFilterStatusCallback();
        DemuxFilterPesEvent pesEvent;
        pesEvent = {
                // temp dump meta data
                .streamId = mPesOutput[3],
                .dataLength = static_cast<uint16_t>(mPesOutput.size()),
        };
        if (DEBUG_FILTER) {
            ALOGD("[Filter] assembled pes data length %d", pesEvent.dataLength);
        }

        int size = mFilterEvent.events.size();
        mFilterEvent.events.resize(size + 1);
        mFilterEvent.events[size].pes(pesEvent);
        mPesOutput.clear();
    }

    mFilterOutput.clear();

    return Result::SUCCESS;
}

Result Filter::startTsFilterHandler() {
    // TODO handle starting TS filter
    return Result::SUCCESS;
}

Result Filter::startMediaFilterHandler() {
    std::lock_guard<std::mutex> lock(mFilterEventLock);
    if (mFilterOutput.empty()) {
        return Result::SUCCESS;
    }

    Result result;
    if (mPts) {
        result = createMediaFilterEventWithIon(mFilterOutput);
        if (result == Result::SUCCESS) {
            mFilterOutput.clear();
        }
        return result;
    }

    for (int i = 0; i < mFilterOutput.size(); i += 188) {
        if (mPesSizeLeft == 0) {
            uint32_t prefix = (mFilterOutput[i + 4] << 16) | (mFilterOutput[i + 5] << 8) |
                              mFilterOutput[i + 6];
            if (DEBUG_FILTER) {
                ALOGD("[Filter] prefix %d", prefix);
            }
            if (prefix == 0x000001) {
                // TODO handle mulptiple Pes filters
                mPesSizeLeft = (mFilterOutput[i + 8] << 8) | mFilterOutput[i + 9];
                mPesSizeLeft += 6;
                if (DEBUG_FILTER) {
                    ALOGD("[Filter] pes data length %d", mPesSizeLeft);
                }
            } else {
                continue;
            }
        }

        int endPoint = min(184, mPesSizeLeft);
        // append data and check size
        vector<uint8_t>::const_iterator first = mFilterOutput.begin() + i + 4;
        vector<uint8_t>::const_iterator last = mFilterOutput.begin() + i + 4 + endPoint;
        mPesOutput.insert(mPesOutput.end(), first, last);
        // size does not match then continue
        mPesSizeLeft -= endPoint;
        if (DEBUG_FILTER) {
            ALOGD("[Filter] pes data left %d", mPesSizeLeft);
        }
        if (mPesSizeLeft > 0 || mAvBufferCopyCount++ < 10) {
            continue;
        }

        result = createMediaFilterEventWithIon(mPesOutput);
        if (result != Result::SUCCESS) {
            return result;
        }
    }

    mFilterOutput.clear();

    return Result::SUCCESS;
}

Result Filter::createMediaFilterEventWithIon(vector<uint8_t> output) {
    if (mUsingSharedAvMem) {
        if (mSharedAvMemHandle.getNativeHandle() == nullptr) {
            return Result::UNKNOWN_ERROR;
        }
        return createShareMemMediaEvents(output);
    }

    return createIndependentMediaEvents(output);
}

Result Filter::startRecordFilterHandler() {
    std::lock_guard<std::mutex> lock(mRecordFilterOutputLock);
    if (mRecordFilterOutput.empty()) {
        return Result::SUCCESS;
    }

    if (mDvr == nullptr || !mDvr->writeRecordFMQ(mRecordFilterOutput)) {
        ALOGD("[Filter] dvr fails to write into record FMQ.");
        return Result::UNKNOWN_ERROR;
    }

    V1_0::DemuxFilterTsRecordEvent recordEvent;
    recordEvent = {
            .byteNumber = mRecordFilterOutput.size(),
    };
    V1_1::DemuxFilterTsRecordEventExt recordEventExt;
    recordEventExt = {
            .pts = (mPts == 0) ? time(NULL) * 900000 : mPts,
            .firstMbInSlice = 0,     // random address
    };

    int size;
    size = mFilterEventExt.events.size();
    mFilterEventExt.events.resize(size + 1);
    mFilterEventExt.events[size].tsRecord(recordEventExt);
    size = mFilterEvent.events.size();
    mFilterEvent.events.resize(size + 1);
    mFilterEvent.events[size].tsRecord(recordEvent);

    mRecordFilterOutput.clear();
    return Result::SUCCESS;
}

Result Filter::startPcrFilterHandler() {
    // TODO handle starting PCR filter
    return Result::SUCCESS;
}

Result Filter::startTemiFilterHandler() {
    // TODO handle starting TEMI filter
    return Result::SUCCESS;
}

bool Filter::writeSectionsAndCreateEvent(vector<uint8_t> data) {
    // TODO check how many sections has been read
    ALOGD("[Filter] section handler");
    std::lock_guard<std::mutex> lock(mFilterEventLock);
    if (!writeDataToFilterMQ(data)) {
        return false;
    }
    int size = mFilterEvent.events.size();
    mFilterEvent.events.resize(size + 1);
    DemuxFilterSectionEvent secEvent;
    secEvent = {
            // temp dump meta data
            .tableId = 0,
            .version = 1,
            .sectionNum = 1,
            .dataLength = static_cast<uint16_t>(data.size()),
    };
    mFilterEvent.events[size].section(secEvent);
    return true;
}

bool Filter::writeDataToFilterMQ(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(mWriteLock);
    if (mFilterMQ->write(data.data(), data.size())) {
        return true;
    }
    return false;
}

void Filter::attachFilterToRecord(const sp<Dvr> dvr) {
    mDvr = dvr;
}

void Filter::detachFilterFromRecord() {
    mDvr = nullptr;
}

int Filter::createAvIonFd(int size) {
    // Create an ion fd and allocate an av fd mapped to a buffer to it.
    int ion_fd = ion_open();
    if (ion_fd == -1) {
        ALOGE("[Filter] Failed to open ion fd %d", errno);
        return -1;
    }
    int av_fd = -1;
    ion_alloc_fd(dup(ion_fd), size, 0 /*align*/, ION_HEAP_SYSTEM_MASK, 0 /*flags*/, &av_fd);
    if (av_fd == -1) {
        ALOGE("[Filter] Failed to create av fd %d", errno);
        return -1;
    }
    return av_fd;
}

uint8_t* Filter::getIonBuffer(int fd, int size) {
    uint8_t* avBuf = static_cast<uint8_t*>(
            mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 /*offset*/));
    if (avBuf == MAP_FAILED) {
        ALOGE("[Filter] fail to allocate buffer %d", errno);
        return NULL;
    }
    return avBuf;
}

native_handle_t* Filter::createNativeHandle(int fd) {
    native_handle_t* nativeHandle;
    if (fd < 0) {
        nativeHandle = native_handle_create(/*numFd*/ 0, 0);
    } else {
        // Create a native handle to pass the av fd via the callback event.
        nativeHandle = native_handle_create(/*numFd*/ 1, 0);
    }
    if (nativeHandle == NULL) {
        ALOGE("[Filter] Failed to create native_handle %d", errno);
        return NULL;
    }
    if (nativeHandle->numFds > 0) {
        nativeHandle->data[0] = dup(fd);
    }
    return nativeHandle;
}

Result Filter::createIndependentMediaEvents(vector<uint8_t> output) {
    int av_fd = createAvIonFd(output.size());
    if (av_fd == -1) {
        return Result::UNKNOWN_ERROR;
    }
    // copy the filtered data to the buffer
    uint8_t* avBuffer = getIonBuffer(av_fd, output.size());
    if (avBuffer == NULL) {
        return Result::UNKNOWN_ERROR;
    }
    memcpy(avBuffer, output.data(), output.size() * sizeof(uint8_t));

    native_handle_t* nativeHandle = createNativeHandle(av_fd);
    if (nativeHandle == NULL) {
        return Result::UNKNOWN_ERROR;
    }
    hidl_handle handle;
    handle.setTo(nativeHandle, /*shouldOwn=*/true);

    // Create a dataId and add a <dataId, av_fd> pair into the dataId2Avfd map
    uint64_t dataId = mLastUsedDataId++ /*createdUID*/;
    mDataId2Avfd[dataId] = dup(av_fd);

    // Create mediaEvent and send callback
    DemuxFilterMediaEvent mediaEvent;
    mediaEvent = {
            .avMemory = std::move(handle),
            .dataLength = static_cast<uint32_t>(output.size()),
            .avDataId = dataId,
    };
    if (mPts) {
        mediaEvent.pts = mPts;
        mPts = 0;
    }
    int size = mFilterEvent.events.size();
    mFilterEvent.events.resize(size + 1);
    mFilterEvent.events[size].media(mediaEvent);

    // Clear and log
    output.clear();
    mAvBufferCopyCount = 0;
    ::close(av_fd);
    if (DEBUG_FILTER) {
        ALOGD("[Filter] av data length %d", mediaEvent.dataLength);
    }
    return Result::SUCCESS;
}

Result Filter::createShareMemMediaEvents(vector<uint8_t> output) {
    // copy the filtered data to the shared buffer
    uint8_t* sharedAvBuffer = getIonBuffer(mSharedAvMemHandle.getNativeHandle()->data[0],
                                           output.size() + mSharedAvMemOffset);
    if (sharedAvBuffer == NULL) {
        return Result::UNKNOWN_ERROR;
    }
    memcpy(sharedAvBuffer + mSharedAvMemOffset, output.data(), output.size() * sizeof(uint8_t));

    // Create a memory handle with numFds == 0
    native_handle_t* nativeHandle = createNativeHandle(-1);
    if (nativeHandle == NULL) {
        return Result::UNKNOWN_ERROR;
    }
    hidl_handle handle;
    handle.setTo(nativeHandle, /*shouldOwn=*/true);

    // Create mediaEvent and send callback
    DemuxFilterMediaEvent mediaEvent;
    mediaEvent = {
            .offset = static_cast<uint32_t>(mSharedAvMemOffset),
            .dataLength = static_cast<uint32_t>(output.size()),
            .avMemory = handle,
    };
    mSharedAvMemOffset += output.size();
    if (mPts) {
        mediaEvent.pts = mPts;
        mPts = 0;
    }
    int size = mFilterEvent.events.size();
    mFilterEvent.events.resize(size + 1);
    mFilterEvent.events[size].media(mediaEvent);

    // Clear and log
    output.clear();
    if (DEBUG_FILTER) {
        ALOGD("[Filter] shared av data length %d", mediaEvent.dataLength);
    }
    return Result::SUCCESS;
}

bool Filter::sameFile(int fd1, int fd2) {
    struct stat stat1, stat2;
    if (fstat(fd1, &stat1) < 0 || fstat(fd2, &stat2) < 0) {
        return false;
    }
    return (stat1.st_dev == stat2.st_dev) && (stat1.st_ino == stat2.st_ino);
}
}  // namespace implementation
}  // namespace V1_0
}  // namespace tuner
}  // namespace tv
}  // namespace hardware
}  // namespace android