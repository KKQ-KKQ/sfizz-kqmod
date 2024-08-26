// SPDX-License-Identifier: BSD-2-Clause

// Copyright (c) 2019-2020, Paul Ferrand, Andrea Zanellato
// All rights reserved.

// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:

// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.

// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "FilePool.h"
#include "AudioReader.h"
#include "Buffer.h"
#include "AudioBuffer.h"
#include "AudioSpan.h"
#include "Config.h"
#include "SynthConfig.h"
#include "utility/SwapAndPop.h"
#include "utility/Debug.h"
#include <ThreadPool.h>
#include <absl/types/span.h>
#include <absl/strings/match.h>
#include <absl/memory/memory.h>
#include <algorithm>
#include <memory>
#include <thread>
#include <system_error>
#include <atomic_queue/defs.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif
#if defined(SFIZZ_FILEOPENPREEXEC)
#include "FileOpenPreexec.h"
#endif
using namespace std::placeholders;

sfz::FilePool::GlobalObject::GlobalObject(size_t num_threads)
: lastGarbageCollection_ { highResNow() },
  threadPool { new ThreadPool(num_threads) }
{
    garbageThread.reset(new std::thread(&GlobalObject::garbageJob, this));
}

sfz::FilePool::GlobalObject::~GlobalObject()
{
    garbageFlag = false;
    std::error_code ec;
    semGarbageBarrier.post(ec);
    ASSERT(!ec);
    garbageThread->join();
    // clear semaphore
    while (semGarbageBarrier.try_wait());
}

RTSemaphore sfz::FilePool::GlobalObject::semGarbageBarrier { 0 };
std::weak_ptr<sfz::FilePool::GlobalObject> sfz::FilePool::globalObjectWeakPtr;
std::mutex sfz::FilePool::globalObjectMutex;

std::shared_ptr<sfz::FilePool::GlobalObject> sfz::FilePool::getGlobalObject()
{
    std::lock_guard<std::mutex> lock { globalObjectMutex };

    std::shared_ptr<GlobalObject> globalObject = globalObjectWeakPtr.lock();
    if (globalObject)
        return globalObject;

#if 1
    constexpr size_t numThreads = 1;
#else
    size_t numThreads = std::thread::hardware_concurrency();
    numThreads = (numThreads > 2) ? (numThreads - 2) : 1;
#endif
    globalObject.reset(new GlobalObject(numThreads));
    globalObjectWeakPtr = globalObject;
    return globalObject;
}

sfz::FileData::FileData(const FilePool* owner, uint32_t preloadSize)
{
    ownerPreloadSizeMap[owner] = { preloadSize, true };
    preloadCallCount += 2;
}

void sfz::FileData::prepareForRemovingOwner(const FilePool* owner)
{
    std::lock_guard<std::mutex> guard { ownerMutex };
    auto it = ownerPreloadSizeMap.find(owner);
    if (it != ownerPreloadSizeMap.end()) {
        if (it->second.checkReleaseFlag) {
            it->second.checkReleaseFlag = false;
            preloadCallCount--;
        }
    }
}

bool sfz::FileData::checkAndRemoveOwner(const FilePool* owner)
{
    std::lock_guard<std::mutex> lock { ownerMutex };
    auto it = ownerPreloadSizeMap.find(owner);
    if (it != ownerPreloadSizeMap.end()) {
        if (!it->second.checkReleaseFlag) {
            ownerPreloadSizeMap.erase(it);
            --preloadCallCount;
            return true;
        }
    }
    return false;
}

/**
 * @brief check existence of owner and maximize preload size
 */
bool sfz::FileData::addSecondaryOwner(const FilePool* owner, uint32_t preloadSize, bool& needsReloading)
{
    {
        std::unique_lock<std::mutex> guard { readyMutex };
        if (!readyCond.wait_for(guard, std::chrono::seconds(10), [this]{ return ready; })) {
            return false;
        }
    }
    std::lock_guard<std::mutex> guard2 { ownerMutex };
    if (preloadCallCount != 0) {
        bool found = false;
        uint32_t maxPreloadSize = 0;
        uint32_t oldPreloadSize = 0;
        for (auto &itPair : ownerPreloadSizeMap) {
            if (itPair.first == owner) {
                found = true;
                if (!itPair.second.checkReleaseFlag) {
                    preloadCallCount++;
                }
                oldPreloadSize = itPair.second.size;
                if (oldPreloadSize != FileData::maxPreloadSize) {
                    itPair.second.size = preloadSize;
                }
                itPair.second.checkReleaseFlag = true;
            }
            else if (maxPreloadSize < itPair.second.size) {
                maxPreloadSize = itPair.second.size;
            }
        }
        if (!found) {
            preloadCallCount += 2;
            ownerPreloadSizeMap[owner] = { preloadSize, true };
        }
        if (maxPreloadSize == 0) {
            needsReloading = (oldPreloadSize != preloadSize) && (oldPreloadSize != FileData::maxPreloadSize);
        }
        else {
            needsReloading = maxPreloadSize < preloadSize || maxPreloadSize < oldPreloadSize;
        }
        return true;
    }
    return false;
}

bool sfz::FileData::replaceAndGetOldMaxPreloadSize(const sfz::FilePool* owner, uint32_t& preloadSize)
{
    uint32_t size = preloadSize;
    uint32_t maxPreloadSize = 0;
    uint32_t oldPreloadSize = 0;
    {
        std::lock_guard<std::mutex> guard2 { ownerMutex };
        for (auto &itPair : ownerPreloadSizeMap) {
            if (itPair.first == owner) {
                oldPreloadSize = itPair.second.size;
                itPair.second = { size, true };
            }
            else if (maxPreloadSize < itPair.second.size) {
                maxPreloadSize = itPair.second.size;
            }
        }
    }
    bool ret = maxPreloadSize < size || maxPreloadSize < oldPreloadSize;
    if (maxPreloadSize > size && maxPreloadSize < oldPreloadSize) {
        preloadSize = maxPreloadSize;
    }
    return ret;
}

sfz::FileAudioBuffer& sfz::FileData::getPreloadedData()
{
    // SpinSharedLock
    auto readerCount = preloadedDataReaderCount.load();
    while (readerCount == lockedReaderCount && !preloadedDataReaderCount.compare_exchange_weak(readerCount, readerCount + 1)) {
        if (readerCount == lockedReaderCount) {
            atomic_queue::spin_loop_pause();
            atomic_queue::spin_loop_pause();
        }
        readerCount = preloadedDataReaderCount.load();
    }

    FileAudioBuffer& buffer = *preloadedData;

    preloadedDataReaderCount--;

    return buffer;
}

void sfz::FileData::replacePreloadedData(sfz::FileAudioBuffer&& audioBuffer, bool fullyLoaded)
{
    FileAudioBufferPtr newPointer { new FileAudioBuffer(std::move(audioBuffer)) };
    // SpinUniqueLock
    auto readerCount = preloadedDataReaderCount.load();
    while (readerCount != 0 && !preloadedDataReaderCount.compare_exchange_weak(readerCount, lockedReaderCount)) {
        if (readerCount != 0) {
            atomic_queue::spin_loop_pause();
            atomic_queue::spin_loop_pause();
        }
        readerCount = preloadedDataReaderCount.load();
    }

    preloadedDataToClear.push_back(std::move(preloadedData));
    preloadedData = std::move(newPointer);
    this->fullyLoaded = fullyLoaded;

    preloadedDataReaderCount = 0;
}

void readBaseFile(sfz::AudioReader& reader, sfz::FileAudioBuffer& output, uint32_t numFrames)
{
    output.reset();
    output.resize(numFrames);

    const unsigned channels = reader.channels();

    if (channels == 1) {
        output.addChannel();
        output.clear();
        reader.readNextBlock(output.channelWriter(0), numFrames);
    } else if (channels == 2) {
        output.addChannel();
        output.addChannel();
        output.clear();
        sfz::Buffer<float> tempReadBuffer { 2 * numFrames };
        reader.readNextBlock(tempReadBuffer.data(), numFrames);
        sfz::readInterleaved(tempReadBuffer, output.getSpan(0), output.getSpan(1));
    }
}

sfz::FileAudioBuffer readFromFile(sfz::AudioReader& reader, uint32_t numFrames)
{
    sfz::FileAudioBuffer baseBuffer;
    readBaseFile(reader, baseBuffer, numFrames);
    return baseBuffer;
}

/*
 * this function returns false when the loading is not completed.
 */
bool streamFromFile(sfz::AudioReader& reader, sfz::FileAudioBuffer& output, std::atomic<size_t>& filledFrames, bool freeWheeling)
{
    const auto numFrames = static_cast<size_t>(reader.frames());
    const auto numChannels = reader.channels();
    const auto chunkSize = static_cast<size_t>(sfz::config::fileChunkSize);

    if (filledFrames == 0) {
        output.reset();
        output.addChannels(reader.channels());
        output.resize(numFrames);
        output.clear();
    }

    sfz::Buffer<float> fileBlock { chunkSize * numChannels };
    size_t inputFrameCounter { filledFrames };
    size_t outputFrameCounter { inputFrameCounter };
    bool inputEof = false;
    bool seekable = reader.seekable();
    if (seekable)
        reader.seek(inputFrameCounter);

    int chunkCounter = (freeWheeling || !seekable) ? INT_MAX : static_cast<int>(sfz::config::numChunkForLoadingAtOnce);

    while (!inputEof && inputFrameCounter < numFrames)
    {
        if (chunkCounter-- == 0)
            return false;

        auto thisChunkSize = std::min(chunkSize, numFrames - inputFrameCounter);
        const auto numFramesRead = static_cast<size_t>(
            reader.readNextBlock(fileBlock.data(), thisChunkSize));
        if (numFramesRead == 0)
            break;

        if (numFramesRead < thisChunkSize) {
            inputEof = true;
            thisChunkSize = numFramesRead;
        }
        const auto outputChunkSize = thisChunkSize;

        for (size_t chanIdx = 0; chanIdx < numChannels; chanIdx++) {
            const auto outputChunk = output.getSpan(chanIdx).subspan(outputFrameCounter, outputChunkSize);
            for (size_t i = 0; i < thisChunkSize; ++i)
                outputChunk[i] = fileBlock[i * numChannels + chanIdx];
        }
        inputFrameCounter += thisChunkSize;
        outputFrameCounter += outputChunkSize;

        filledFrames.fetch_add(outputChunkSize);
    }
    return true;
}

#if defined(SFIZZ_FILEOPENPREEXEC)
sfz::FilePool::FilePool(const SynthConfig& synthConfig, FileOpenPreexec& preexec_in)
#else
sfz::FilePool::FilePool(const SynthConfig& synthConfig)
#endif
    : filesToLoad(alignedNew<FileQueue>()),
      globalObject(getGlobalObject()),
      synthConfig(synthConfig)
#if defined(SFIZZ_FILEOPENPREEXEC)
    , preexec(preexec_in)
#endif
{
    loadingJobs.reserve(config::maxVoices * 16);
}

sfz::FilePool::~FilePool()
{
    clear();

    std::error_code ec;

    dispatchFlag = false;
    dispatchBarrier.post(ec);
    dispatchThread.join();

    for (auto& job : loadingJobs)
        job.wait();
}

bool sfz::FilePool::checkSample(std::string& filename) const noexcept
{
    fs::path path { rootDirectory / filename };
    std::error_code ec;
#if defined(SFIZZ_FILEOPENPREEXEC)
    bool ret = false;
    preexec.executeFileOpen(path, [&ec, &ret, &path](const fs::path &path2) {
        if (fs::exists(path2, ec)) {
            ret = true;
            path = path2;
        }
    });
    if (ret) {
        filename = path.string();
        return true;
    }
    return false;
#else
    if (fs::exists(path, ec)) {
        filename = path;
        return true;
    }

#if defined(_WIN32)
    return false;
#else
    fs::path oldPath = std::move(path);
    path = oldPath.root_path();

    static const fs::path dot { "." };
    static const fs::path dotdot { ".." };

    for (const fs::path& part : oldPath.relative_path()) {
        if (part == dot || part == dotdot) {
            path /= part;
            continue;
        }

        if (fs::exists(path / part, ec)) {
            path /= part;
            continue;
        }

        auto it = path.empty() ? fs::directory_iterator { dot, ec } : fs::directory_iterator { path, ec };
        if (ec) {
            DBG("Error creating a directory iterator for " << filename << " (Error code: " << ec.message() << ")");
            return false;
        }

        auto searchPredicate = [&part](const fs::directory_entry &ent) -> bool {
#if !defined(GHC_USE_WCHAR_T)
            return absl::EqualsIgnoreCase(
                ent.path().filename().native(), part.native());
#else
            return absl::EqualsIgnoreCase(
                ent.path().filename().u8string(), part.u8string());
#endif
        };

        while (it != fs::directory_iterator {} && !searchPredicate(*it))
            it.increment(ec);

        if (it == fs::directory_iterator {}) {
            DBG("File not found, could not resolve " << filename);
            return false;
        }

        path /= it->path().filename();
    }

    const auto newPath = fs::relative(path, rootDirectory, ec);
    if (ec) {
        DBG("Error extracting the new relative path for " << filename << " (Error code: " << ec.message() << ")");
        return false;
    }
    DBG("Updating " << filename << " to " << newPath);
    filename = newPath.string();
    return true;
#endif
#endif
}

bool sfz::FilePool::checkSampleId(FileId& fileId) const noexcept
{
    if (loadedFiles.contains(fileId))
        return true;

    std::string filename = fileId.filename();

    FileId fileId2 = FileId(rootDirectory / filename, fileId.isReverse());
    if (loadedFiles.contains(fileId2)) {
        fileId = fileId2;
        return true;
    }

    bool result = checkSample(filename);
    if (result)
        fileId = FileId(std::move(filename), fileId.isReverse());
    return result;
}

absl::optional<sfz::FileInformation> getReaderInformation(sfz::AudioReader* reader) noexcept
{
    const unsigned channels = reader->channels();
    if (channels != 1 && channels != 2)
        return {};

    sfz::FileInformation returnedValue;
    returnedValue.end = static_cast<uint32_t>(reader->frames()) - 1;
    returnedValue.sampleRate = static_cast<double>(reader->sampleRate());
    returnedValue.numChannels = static_cast<int>(channels);

    // Check for instrument info
    sfz::InstrumentInfo instrumentInfo {};
    if (reader->getInstrumentInfo(instrumentInfo)) {
        returnedValue.rootKey = clamp<uint8_t>(instrumentInfo.basenote, 0, 127);
        if (reader->type() == sfz::AudioReaderType::Forward) {
            if (instrumentInfo.loop_count > 0) {
                returnedValue.hasLoop = true;
                returnedValue.loopStart = instrumentInfo.loops[0].start;
                returnedValue.loopEnd =
                    min(returnedValue.end, static_cast<int64_t>(instrumentInfo.loops[0].end - 1));
            }
        } else {
            // TODO loops ignored when reversed
            //   prehaps it can make use of SF_LOOP_BACKWARD?
        }
    }

    // Check for wavetable info
    sfz::WavetableInfo wt {};
    if (reader->getWavetableInfo(wt))
        returnedValue.wavetable = wt;

    return returnedValue;
}

absl::optional<sfz::FileInformation> sfz::FilePool::checkExistingFileInformation(const FileId& fileId) noexcept
{
    const auto loadedFile = loadedFiles.find(fileId);
    if (loadedFile != loadedFiles.end())
        return loadedFile->second->information;

    return {};
}

absl::optional<sfz::FileInformation> sfz::FilePool::getFileInformation(const FileId& fileId) noexcept
{
    auto existingInformation = checkExistingFileInformation(fileId);
    if (existingInformation)
        return existingInformation;

    const fs::path file { rootDirectory / fileId.filename() };

#if defined(SFIZZ_FILEOPENPREEXEC)
    AudioReaderPtr reader = createAudioReader(file, fileId.isReverse(), preexec);
#else
    if (!fs::exists(file))
        return {};

    AudioReaderPtr reader = createAudioReader(file, fileId.isReverse());
#endif
    return getReaderInformation(reader.get());
}

bool sfz::FilePool::preloadFile(const FileId& fileId, uint32_t maxOffset) noexcept
{
    maxOffsetMap[fileId] = maxOffset;
    uint32_t preloadSizeWithOffset = loadInRam ? FileData::maxPreloadSize : preloadSize + maxOffset;
    return static_cast<bool>(loadFile(fileId, preloadSizeWithOffset));
}

std::shared_ptr<sfz::FileData> sfz::FilePool::loadFile(const FileId& fileId, uint32_t preloadSizeWithOffset) noexcept
{
    absl::optional<sfz::FileInformation> fileInformation;
    AudioReaderPtr reader;
    uint32_t frames;

    auto createReaderLocal = [&]() {
        fileInformation = getFileInformation(fileId);
        if (!fileInformation)
            return false;

        const fs::path file { rootDirectory / fileId.filename() };
#if defined(SFIZZ_FILEOPENPREEXEC)
        reader = createAudioReader(file, fileId.isReverse(), preexec);
#else
        reader = createAudioReader(file, fileId.isReverse());
#endif
        frames = static_cast<uint32_t>(reader->frames());
        return true;
    };


    const auto existingFile = loadedFiles.find(fileId);
    if (existingFile != loadedFiles.end()) {
        auto& fileData = existingFile->second;
        bool needsReloading;
        if (fileData->addSecondaryOwner(this, preloadSizeWithOffset, needsReloading)) {
            if (needsReloading) {
                if (!createReaderLocal()) {
                    return {};
                }
                uint32_t framesToLoad = min(frames, preloadSizeWithOffset);
                fileData->replacePreloadedData(readFromFile(*reader, framesToLoad), frames == framesToLoad);
            }
            return fileData;
        }
    }
    {
        auto& files = globalObject->loadedFiles;
        std::unique_lock<std::mutex> guard { globalObject->mutex };
        const auto existingFile = files.find(fileId);
        if (existingFile != files.end()) {
            auto fileData = existingFile->second;
            bool needsReloading;
            if (fileData->addSecondaryOwner(this, preloadSizeWithOffset, needsReloading)) {
                loadedFiles[fileId] = fileData;
                guard.unlock();
                if (needsReloading) {
                    if (!createReaderLocal()) {
                        return {};
                    }
                    uint32_t framesToLoad = min(frames, preloadSizeWithOffset);
                    fileData->replacePreloadedData(readFromFile(*reader, framesToLoad), frames == framesToLoad);
                }
                return fileData;
            }
        }
        if (!createReaderLocal()) {
            return {};
        }
        fileInformation->sampleRate = static_cast<double>(reader->sampleRate());

        auto insertedPair = loadedFiles.insert_or_assign(fileId, std::shared_ptr<FileData>(new FileData(this, preloadSizeWithOffset)));
        auto& fileData = insertedPair.first->second;
        files[fileId] = fileData;
        guard.unlock();
        uint32_t framesToLoad = min(frames, preloadSizeWithOffset);
        fileData->initWith(
            FileData::Status::Preloaded,
            FileAudioBufferPtr(new FileAudioBuffer(readFromFile(*reader, framesToLoad))),
            *fileInformation,
            frames == framesToLoad
        );
        return fileData;
    }
}

void sfz::FilePool::resetPreloadCallCounts() noexcept
{
    for (auto& loadedFile: loadedFiles)
        loadedFile.second->prepareForRemovingOwner(this);
}

void sfz::FilePool::removeUnusedPreloadedData() noexcept
{
    for (auto it = loadedFiles.begin(), end = loadedFiles.end(); it != end; ) {
        auto copyIt = it++;
        if (copyIt->second->checkAndRemoveOwner(this)) {
            DBG("[sfizz] Removing unused preloaded data: " << copyIt->first.filename());
            maxOffsetMap.erase(copyIt->first);
            loadedFiles.erase(copyIt);
        }
    }
}

sfz::FileDataHolder sfz::FilePool::loadFromRam(const FileId& fileId, const std::vector<char>& data) noexcept
{
    uint32_t preloadSizeWithOffset = FileData::maxPreloadSize;

    auto reader = createAudioReaderFromMemory(data.data(), data.size(), fileId.isReverse());
    auto fileInformation = getReaderInformation(reader.get());
    auto frames = static_cast<uint32_t>(reader->frames());

    const auto loaded = loadedFiles.find(fileId);
    if (loaded != loadedFiles.end()) {
        auto& fileData = loaded->second;
        bool needsReloading;
        if (fileData->addSecondaryOwner(this, preloadSizeWithOffset, needsReloading)) {
            //if (needsReloading) // Reload always
            {
                fileData->replacePreloadedData(FileAudioBuffer(readFromFile(*reader, frames)), true);
            }
            return { fileData };
        }
    }

    auto insertedPair = loadedFiles.insert_or_assign(fileId, std::shared_ptr<FileData>(new FileData(this, preloadSizeWithOffset)));
    auto& fileData = insertedPair.first->second;
    fileData->initWith(FileData::Status::Preloaded,
        FileAudioBufferPtr(new FileAudioBuffer(readFromFile(*reader, frames))),
        *fileInformation,
        true
    );
    DBG("Added a file " << fileId.filename() << " frames: " << fileData->fileData.getNumFrames());
    return { fileData };
}

sfz::FileDataHolder sfz::FilePool::getFilePromise(const std::shared_ptr<FileId>& fileId) noexcept
{
    const auto preloaded = loadedFiles.find(*fileId);
    if (preloaded == loadedFiles.end()) {
        DBG("[sfizz] File not found in the preloaded files: " << fileId->filename());
        return {};
    }

    auto& fileData = preloaded->second;
    auto status = fileData->status.load();
    if (status == FileData::Status::Preloaded && !fileData->fullyLoaded) {
        QueuedFileData queuedData { fileId, fileData };
        if (!filesToLoad->try_push(queuedData)) {
            DBG("[sfizz] Could not enqueue the file to load for " << fileId << " (queue capacity " << filesToLoad->capacity() << ")");
            return {};
        }
        // status should not change when it is changed at this moment
        fileData->status.compare_exchange_strong(status, FileData::Status::PendingStreaming);

        // this is needed even when status is not PendingStreaming
        std::error_code ec;
        dispatchBarrier.post(ec);
        ASSERT(!ec);
    }

    return { fileData };
}

void sfz::FilePool::setPreloadSize(uint32_t preloadSize) noexcept
{
    this->preloadSize = preloadSize;
    if (loadInRam)
        return;

    // Update all the preloaded sizes
    for (auto& preloadedFile : loadedFiles) {
        auto& fileId = preloadedFile.first;
        auto it = maxOffsetMap.find(fileId);
        if (it == maxOffsetMap.end()) {
            continue;
        }
        int64_t maxOffset = it->second;
        auto& fileData = preloadedFile.second;
        fs::path file { rootDirectory / fileId.filename() };
#if defined(SFIZZ_FILEOPENPREEXEC)
        AudioReaderPtr reader = createAudioReader(file, fileId.isReverse(), preexec);
#else
        AudioReaderPtr reader = createAudioReader(file, fileId.isReverse());
#endif
        const auto frames = reader->frames();
        uint32_t preloadSizeWithOffset = max(int64_t(0), min(int64_t(FileData::maxPreloadSize), maxOffset + preloadSize));
        bool needsReloading = fileData->replaceAndGetOldMaxPreloadSize(this, preloadSizeWithOffset);
        if (needsReloading) {
            const auto framesToLoad = min(frames, int64_t(preloadSizeWithOffset));
            fileData->replacePreloadedData(readFromFile(*reader, static_cast<uint32_t>(framesToLoad)), frames == framesToLoad);
        }
    }
    std::error_code ec;
    globalObject->semGarbageBarrier.post(ec);
    ASSERT(!ec);
}

void sfz::FilePool::loadingJob(const QueuedFileData& data) noexcept
{
    raiseCurrentThreadPriority();

    std::shared_ptr<FileId> id = data.id.lock();
    if (!id) {
        // file ID was nulled, it means the region was deleted, ignore
        return;
    }

    const fs::path file { rootDirectory / id->filename() };
    std::error_code readError;
#if defined(SFIZZ_FILEOPENPREEXEC)
    AudioReaderPtr reader = createAudioReader(file, id->isReverse(), preexec, &readError);
#else
    AudioReaderPtr reader = createAudioReader(file, id->isReverse(), &readError);
#endif

    if (readError) {
        DBG("[sfizz] reading the file errored for " << *id << " with code " << readError << ": " << readError.message());
        return;
    }

    FileData::Status currentStatus;

    unsigned spinCounter { 0 };

    while (1) {
        currentStatus = data.data->status.load();
        while (currentStatus == FileData::Status::Invalid) {
            // Spin until the state changes
            if (spinCounter > 1024) {
                DBG("[sfizz] " << *id << " is stuck on Invalid? Leaving the load");
                return;
            }

            std::this_thread::sleep_for(std::chrono::microseconds(100));
            currentStatus = data.data->status.load();
            spinCounter += 1;
        }
        // wait for garbage collection
        if (currentStatus == FileData::Status::GarbageCollecting) {
            atomic_queue::spin_loop_pause();
            atomic_queue::spin_loop_pause();
            atomic_queue::spin_loop_pause();
            atomic_queue::spin_loop_pause();
            continue;
        }
        // Already loading, loaded, or released
        if (currentStatus != FileData::Status::PendingStreaming)
            return;

        // go outside loop if this gets token
        if (data.data->status.compare_exchange_strong(currentStatus, FileData::Status::Streaming))
            break;
    }

    bool completed = streamFromFile(*reader, data.data->fileData, data.data->availableFrames, synthConfig.freeWheeling);

    if (completed) {
        data.data->status = FileData::Status::Done;
    }
    else {
        data.data->status = FileData::Status::PendingStreaming;
        if (filesToLoad->try_push(data)) {
            std::error_code ec;
            dispatchBarrier.post(ec);
            ASSERT(!ec);
        }
    }
}

void sfz::FilePool::clear()
{
    resetPreloadCallCounts();
    removeUnusedPreloadedData();
    ASSERT(loadedFiles.size() == 0);
    emptyFileLoadingQueues();
    std::error_code ec;
    ASSERT(!ec);
}

uint32_t sfz::FilePool::getPreloadSize() const noexcept
{
    return preloadSize;
}

template <typename R>
bool is_ready(std::future<R> const& f)
{
    return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

void sfz::FilePool::dispatchingJob() noexcept
{
    while (dispatchBarrier.wait(), dispatchFlag) {
        std::lock_guard<std::mutex> guard { loadingJobsMutex };

        QueuedFileData queuedData;
        if (filesToLoad->try_pop(queuedData)) {
            if (queuedData.id.expired() || queuedData.data->status != FileData::Status::PendingStreaming) {
                // file ID was nulled, it means the region was deleted, ignore
            }
            else
                loadingJobs.push_back(
                    globalObject->threadPool->enqueue([this](const QueuedFileData& data) { loadingJob(data); }, std::move(queuedData)));
        }

        // Clear finished jobs
        swapAndPopAll(loadingJobs, [](std::future<void>& future) {
            return is_ready(future);
        });
    }
}

void sfz::FilePool::waitForBackgroundLoading() noexcept
{
    const auto now = highResNow();
    const auto timeSinceLastCollection = std::chrono::duration_cast<std::chrono::milliseconds>(now - globalObject->lastGarbageCollection_);
    // 10 times frequent garbage collection
    if (timeSinceLastCollection.count() >= config::fileClearingPeriod * 100) {
        globalObject->lastGarbageCollection_ = now;
        std::error_code ec;
        globalObject->semGarbageBarrier.post(ec);
        ASSERT(!ec);
    }

    std::lock_guard<std::mutex> guard { loadingJobsMutex };

    for (auto& job : loadingJobs)
        job.wait();

    loadingJobs.clear();
}

void sfz::FilePool::raiseCurrentThreadPriority() noexcept
{
#if defined(_WIN32)
    HANDLE thread = GetCurrentThread();
    const int priority = THREAD_PRIORITY_ABOVE_NORMAL; /*THREAD_PRIORITY_HIGHEST*/
    if (!SetThreadPriority(thread, priority)) {
        std::system_error error(GetLastError(), std::system_category());
        DBG("[sfizz] Cannot set current thread priority: " << error.what());
    }
#else
    pthread_t thread = pthread_self();
    int policy;
    sched_param param;

    if (pthread_getschedparam(thread, &policy, &param) != 0) {
        DBG("[sfizz] Cannot get current thread scheduling parameters");
        return;
    }

    policy = SCHED_RR;
    const int minprio = sched_get_priority_min(policy);
    const int maxprio = sched_get_priority_max(policy);
    param.sched_priority = minprio + config::backgroundLoaderPthreadPriority * (maxprio - minprio) / 100;

    if (pthread_setschedparam(thread, policy, &param) != 0) {
        DBG("[sfizz] Cannot set current thread scheduling parameters");
        return;
    }
#endif
}

void sfz::FilePool::setRamLoading(bool loadInRam) noexcept
{
    if (loadInRam == this->loadInRam)
        return;

    this->loadInRam = loadInRam;

    if (loadInRam) {
        for (auto& preloadedFile : loadedFiles) {
            auto& fileId = preloadedFile.first;
            if (!maxOffsetMap.contains(fileId))
                continue;
            auto& fileData = preloadedFile.second;
            if (fileData->fullyLoaded)
                continue;
            fs::path file { rootDirectory / fileId.filename() };
#if defined(SFIZZ_FILEOPENPREEXEC)
            AudioReaderPtr reader = createAudioReader(file, fileId.isReverse(), preexec);
#else
            AudioReaderPtr reader = createAudioReader(file, fileId.isReverse());
#endif
            fileData->replacePreloadedData(readFromFile(
                *reader,
                fileData->information.end
            ), true);
        }
    } else {
        setPreloadSize(preloadSize);
    }
}

void sfz::FilePool::GlobalObject::garbageJob()
{
    while (semGarbageBarrier.timed_wait(sfz::config::fileClearingPeriod * 1000), garbageFlag) {
        const auto now = highResNow();
        lastGarbageCollection_ = now;

        {
            std::unique_lock<std::mutex> guard { mutex, std::try_to_lock };
            if (guard.owns_lock()) {
                for (auto it = loadedFiles.begin(); it != loadedFiles.end();) {
                    auto copyIt = it++;
                    auto& data = copyIt->second;

                    if (data->canRemove()) {
                        loadedFiles.erase(copyIt);
                        continue;
                    }

                    if ((data->availableFrames == 0 && data->preloadedDataToClear.empty()) || data->readerCount != 0)
                        continue;

                    if (data->fullyLoaded)
                        continue;

                    const auto secondsIdle = std::chrono::duration_cast<std::chrono::seconds>(now - data->lastViewerLeftAt).count();
                    if (secondsIdle < config::fileClearingPeriod)
                        continue;

                    auto status = data->status.load();
                    if (status != FileData::Status::Preloaded && status != FileData::Status::Done && status != FileData::Status::PendingStreaming) {
                        continue;
                    }

                    // construct/destruct outside the lock
                    FileAudioBuffer garbage;
                    std::vector<FileAudioBufferPtr> garbage2;
                    garbage2.reserve(config::maxPreloadDataClearBufferSize);

                    // do garbage collection when changing the status is success
                    if (data->status.compare_exchange_strong(status, FileData::Status::GarbageCollecting)) {
                        // recheck readerCount
                        if (data->readerCount == 0) {
                            data->availableFrames = 0;
                            // garbage collection
                            garbage = std::move(data->fileData);
                            garbage2.swap(data->preloadedDataToClear);
                            data->status = FileData::Status::Preloaded;
                        }
                        else {
                            data->status = status;
                        }
                    }
                }
            }
        }
    }
}
