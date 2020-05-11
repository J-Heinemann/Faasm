#include <catch/catch.hpp>

#include "utils.h"

#include <redis/Redis.h>
#include <util/memory.h>
#include <util/config.h>
#include <state/State.h>
#include <sys/mman.h>
#include <emulator/emulator.h>
#include <faasm/state.h>
#include <util/state.h>

using namespace state;

namespace tests {
    static int staticCount = 0;
    static std::string originalStateMode;

    static void setUpStateMode(const std::string &newMode) {
        cleanSystem();

        util::SystemConfig &conf = util::getSystemConfig();
        originalStateMode = conf.stateMode;
        conf.stateMode = newMode;
    }

    static void resetStateMode() {
        util::getSystemConfig().stateMode = originalStateMode;
    }

    std::shared_ptr<StateKeyValue> setupKV(size_t size) {
        // We have to make sure emulator is using the right user
        const std::string user = getEmulatorUser();

        staticCount++;

        const std::string stateKey = "state_key_" + std::to_string(staticCount);

        // Get state and remove key if already exists
        State &s = getGlobalState();
        auto kv = s.getKV(user, stateKey, size);

        return kv;
    }

    static void doStateSizeCheck(const std::string &stateMode) {
        setUpStateMode(stateMode);

        State &s = getGlobalState();
        std::string user = "alpha";
        std::string key = "beta";

        // Empty should be none
        size_t initialSize = s.getStateSize(user, key);
        REQUIRE(initialSize == 0);

        // Set a value
        std::vector<uint8_t> bytes = {0, 1, 2, 3, 4};
        auto kv = s.getKV(user, key, bytes.size());
        kv->set(bytes.data());
        kv->pushFull();

        // Get size
        REQUIRE(s.getStateSize(user, key) == bytes.size());

        resetStateMode();
    }

    TEST_CASE("Test Redis state sizes", "[state]") {
        doStateSizeCheck("redis");
    }

    TEST_CASE("Test in-memory state sizes", "[state]") {
        doStateSizeCheck("inmemory");
    }

    static void doGetSetCheck(const std::string &stateMode) {
        setUpStateMode(stateMode);

        redis::Redis &redisState = redis::Redis::getState();
        auto kv = setupKV(5);

        std::vector<uint8_t> actual(5);
        std::vector<uint8_t> expected = {0, 0, 0, 0, 0};

        kv->get(actual.data());
        REQUIRE(actual == expected);

        // Update
        std::vector<uint8_t> values = {0, 1, 2, 3, 4};
        kv->set(values.data());

        // Check that getting returns the update
        kv->get(actual.data());
        REQUIRE(actual == values);

        // Check that the underlying key in Redis isn't changed
        std::string actualKey = util::keyForUser(kv->user, kv->key);
        if(stateMode == "redis") {
            REQUIRE(redisState.get(actualKey).empty());
        }

        kv->pushFull();
        kv->get(actual.data());
        REQUIRE(actual == values);

        // Check that when pushed, the update is pushed to redis
        if(stateMode == "redis") {
            REQUIRE(redisState.get(actualKey) == values);
        }

        resetStateMode();
    }

    TEST_CASE("Test simple redis state get/set", "[state]") {
        doGetSetCheck("redis");
    }

    TEST_CASE("Test simple in memory state get/set", "[state]") {
        doGetSetCheck("inmemory");
    }

    static void doGetSetSegmentChecks(const std::string &stateMode) {
        setUpStateMode(stateMode);

        redis::Redis &redisState = redis::Redis::getState();
        auto kv = setupKV(10);

        // Set up and push
        std::vector<uint8_t> values = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4};
        kv->set(values.data());
        kv->pushFull();

        // Get and check
        std::vector<uint8_t> actual(10);
        kv->get(actual.data());
        std::string actualKey = util::keyForUser(kv->user, kv->key);
        REQUIRE(actual == values);

        if(stateMode == "redis") {
            REQUIRE(redisState.get(actualKey) == values);
        }

        // Update a subsection
        std::vector<uint8_t> update = {8, 8, 8};
        kv->setSegment(3, update.data(), 3);

        std::vector<uint8_t> expected = {0, 0, 1, 8, 8, 8, 3, 3, 4, 4};
        kv->get(actual.data());
        REQUIRE(actual == expected);

        // Check changed locally but not in redis
        if(stateMode == "redis") {
            REQUIRE(redisState.get(actualKey) == values);
        }

        // Try getting a segment
        std::vector<uint8_t> actualSegment(3);
        kv->getSegment(3, actualSegment.data(), 3);
        REQUIRE(actualSegment == update);

        // Run push and check redis updated
        kv->pushPartial();
        if(stateMode == "redis") {
            REQUIRE(redisState.get(actualKey) == expected);
        }

        resetStateMode();
    }

    TEST_CASE("Test redis get/ set segment", "[state]") {
        doGetSetSegmentChecks("redis");
    }

    TEST_CASE("Test in memory get/ set segment", "[state]") {
        doGetSetSegmentChecks("inmemory");
    }

    static void doMarkDirtyChecks(const std::string &stateMode) {
        setUpStateMode(stateMode);

        redis::Redis &redisState = redis::Redis::getState();
        auto kv = setupKV(10);

        // Set up and push
        std::vector<uint8_t> values = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
        kv->set(values.data());
        kv->pushFull();

        // Get pointer and update in memory only
        uint8_t *ptr = kv->get();
        ptr[0] = 8;
        ptr[5] = 7;

        // Mark one region as dirty and do push partial
        kv->flagSegmentDirty(0, 2);
        kv->pushPartial();

        // Update expectation
        values.at(0) = 8;
        std::string actualKey = util::keyForUser(kv->user, kv->key);

        // Check in redis
        if(stateMode == "redis") {
            REQUIRE(redisState.get(actualKey) == values);
        }

        // With in-memory state we expect the unmarked update
        // *not* to be overwritten, as this is the master
        // With the redis state, we expect to lose the unmarked
        // local change.
        if(stateMode == "inmemory") {
            values.at(5) = 7;
        }

        // Check expectation
        std::vector<uint8_t> actualMemory(ptr, ptr + values.size());
        REQUIRE(actualMemory == values);

        resetStateMode();
    }

    TEST_CASE("Test redis marking segments dirty", "[state]") {
        doMarkDirtyChecks("redis");
    }

    TEST_CASE("Test in memory marking segments dirty", "[state]") {
        doMarkDirtyChecks("inmemory");
    }

    TEST_CASE("Test redis overlaps with multiple segments dirty", "[state]") {
        cleanSystem();

        redis::Redis &redisState = redis::Redis::getState();
        auto kv = setupKV(20);
        std::string actualKey = util::keyForUser(kv->user, kv->key);
        const char *key = actualKey.c_str();

        // Set up and push
        std::vector<uint8_t> values = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        kv->set(values.data());
        kv->pushFull();

        // Get pointer
        uint8_t *statePtr = kv->get();

        // Update a couple of areas
        statePtr[1] = 1;
        statePtr[2] = 2;
        statePtr[3] = 3;

        statePtr[10] = 4;
        statePtr[11] = 5;

        statePtr[14] = 7;
        statePtr[15] = 7;
        statePtr[16] = 7;
        statePtr[17] = 7;

        // Mark regions as dirty
        kv->flagSegmentDirty(1, 3);
        kv->flagSegmentDirty(10, 2);
        kv->flagSegmentDirty(14, 4);

        // Update one non-overlapping value in state
        std::vector<uint8_t> directA = {2, 2};
        redisState.setRange(key, 6, directA.data(), 2);

        // Update one overlapping value in state
        std::vector<uint8_t> directB = {6, 6, 6, 6, 6};
        redisState.setRange(key, 0, directB.data(), 5);

        // Check all updates are taken and the state ones take precedence
        std::vector<uint8_t> expected = {6, 1, 2, 3, 6, 0, 2, 2, 0, 0, 4, 5, 0, 0, 7, 7, 7, 7, 0, 0};

        // Push and check that with no pull we're up to date
        kv->pushPartial();
        REQUIRE(redisState.get(key) == expected);
    }

    static void doPartialDoubleUpdatesCheck(const std::string &stateMode) {
        setUpStateMode(stateMode);

        redis::Redis &redisState = redis::Redis::getState();

        long nDoubles = 20;
        long nBytes = nDoubles * sizeof(double);
        auto kv = setupKV(nBytes);
        std::string actualKey = util::keyForUser(kv->user, kv->key);
        const char *key = actualKey.c_str();

        // Set up both with zeroes initiall
        std::vector<double> expected(nDoubles);
        std::vector<uint8_t> actualBytes(nBytes);
        memset(expected.data(), 0, nBytes);
        memset(actualBytes.data(), 0, nBytes);

        // Push zeroes to state
        kv->set(actualBytes.data());
        kv->pushFull();

        // Update some elements in both and flag dirty
        auto actualPtr = reinterpret_cast<double *>(kv->get());
        auto expectedPtr = expected.data();
        actualPtr[0] = 123.456;
        expectedPtr[0] = 123.456;
        kv->flagSegmentDirty(0, sizeof(double));

        actualPtr[1] = -100304.223;
        expectedPtr[1] = -100304.223;
        kv->flagSegmentDirty(1 * sizeof(double), sizeof(double));

        actualPtr[9] = 6090293.222;
        expectedPtr[9] = 6090293.222;
        kv->flagSegmentDirty(9 * sizeof(double), sizeof(double));

        actualPtr[13] = -123.444;
        expectedPtr[13] = -123.444;
        kv->flagSegmentDirty(13 * sizeof(double), sizeof(double));

        // Push and check that with no pull we're up to date
        kv->pushPartial();
        auto postPushDoublePtr = reinterpret_cast<double *>(kv->get());
        std::vector<double> actualPostPush(postPushDoublePtr, postPushDoublePtr + nDoubles);
        REQUIRE(expected == actualPostPush);

        // Also check redis
        if(stateMode == "redis") {
            std::vector<double> actualFromRedis(nDoubles);
            redisState.get(key, BYTES(actualFromRedis.data()), nBytes);
            REQUIRE(expected == actualFromRedis);
        }

        resetStateMode();
    }

    TEST_CASE("Test redis partial update of doubles in state", "[state]") {
        doPartialDoubleUpdatesCheck("redis");
    }

    TEST_CASE("Test in memory partial update of doubles in state", "[state]") {
        doPartialDoubleUpdatesCheck("inmemory");
    }

    TEST_CASE("Test set segment cannot be out of bounds", "[state]") {
        auto kv = setupKV(2);

        // Set a segment offset
        std::vector<uint8_t> update = {8, 8, 8};

        REQUIRE_THROWS(kv->setSegment(5, update.data(), 3));
    }

    TEST_CASE("Test redis partially setting just first/ last element", "[state]") {
        redis::Redis &redisState = redis::Redis::getState();
        auto kv = setupKV(5);
        std::string actualKey = util::keyForUser(kv->user, kv->key);

        // Set up and push
        std::vector<uint8_t> values = {0, 1, 2, 3, 4};
        kv->set(values.data());
        kv->pushFull();

        // Update just the last
        std::vector<uint8_t> update = {8};
        kv->setSegment(4, update.data(), 1);

        kv->pushPartial();
        std::vector<uint8_t> expected = {0, 1, 2, 3, 8};
        REQUIRE(redisState.get(actualKey) == expected);

        // Update the first
        kv->setSegment(0, update.data(), 1);

        kv->pushPartial();
        expected = {8, 1, 2, 3, 8};
        REQUIRE(redisState.get(actualKey) == expected);

        // Update both
        update = {6};
        kv->setSegment(0, update.data(), 1);
        kv->setSegment(4, update.data(), 1);

        kv->pushPartial();
        expected = {6, 1, 2, 3, 6};
        REQUIRE(redisState.get(actualKey) == expected);
    }

    TEST_CASE("Test redis push partial with mask", "[state]") {
        cleanSystem();

        redis::Redis &redisState = redis::Redis::getState();

        // Create two key-values of same size
        size_t stateSize = 4 * sizeof(double);
        std::shared_ptr<StateKeyValue> kvData = setupKV(stateSize);
        std::shared_ptr<StateKeyValue> kvMask = setupKV(stateSize);

        // Set up value in memory
        uint8_t *dataBytePtr = kvData->get();
        auto dataDoublePtr = reinterpret_cast<double *>(dataBytePtr);
        std::vector<double> initial = {
                1.2345,
                12.345,
                987.6543,
                10987654.3
        };

        std::copy(initial.begin(), initial.end(), dataDoublePtr);

        // Push 
        kvData->flagDirty();
        kvData->pushFull();

        // Check round trip
        std::string actualKey = util::keyForUser(kvData->user, kvData->key);
        std::vector<uint8_t> actualBytes = redisState.get(actualKey);
        auto actualDoublePtr = reinterpret_cast<double*>(actualBytes.data());
        std::vector<double> actualDoubles(actualDoublePtr, actualDoublePtr + 4);

        REQUIRE(actualDoubles == initial);

        // Now update a couple of elements in memory
        dataDoublePtr[1] = 11.11;
        dataDoublePtr[2] = 222.222;
        dataDoublePtr[3] = 3333.3333;

        // Mask two as having changed and push with mask
        uint8_t *maskBytePtr = kvMask->get();
        auto maskIntPtr = reinterpret_cast<unsigned int *>(maskBytePtr);
        faasm::maskDouble(maskIntPtr, 1);
        faasm::maskDouble(maskIntPtr, 3);

        kvData->flagDirty();
        kvData->pushPartialMask(kvMask);

        // Expected value will be a mix of new and old
        std::vector<double> expected = {
                1.2345,    // Old
                11.11,     // New (updated in memory and masked)
                987.6543,  // Old (updated in memory but not masked)
                3333.3333  // New (updated in memory and masked)
        };

        // Check in redis
        std::vector<uint8_t> actualValue2 = redisState.get(actualKey);
        auto actualDoublesPtr = reinterpret_cast<double *>(actualValue2.data());
        std::vector<double> actualDoubles2(actualDoublesPtr, actualDoublesPtr + 4);
        REQUIRE(actualDoubles2 == expected);
    }

    void checkPulling(bool async) {
        auto kv = setupKV(4);
        std::string actualKey = util::keyForUser(kv->user, kv->key);

        std::vector<uint8_t> values = {0, 1, 2, 3};

        // Push and make sure reflected in redis
        kv->set(values.data());
        kv->pushFull();

        redis::Redis &redisState = redis::Redis::getState();
        REQUIRE(redisState.get(actualKey) == values);

        // Now update in Redis directly
        std::vector<uint8_t> newValues = {5, 5, 5, 5};
        redisState.set(actualKey, newValues);

        // Get and check whether the remote is pulled
        if (!async) {
            kv->pull();
        }

        std::vector<uint8_t> actual(4);
        kv->get(actual.data());

        if (async) {
            REQUIRE(actual == values);
        } else {
            REQUIRE(actual == newValues);
        }
    }

    TEST_CASE("Test redis async pulling", "[state]") {
        checkPulling(true);
    }

    TEST_CASE("Test redis sync pulling", "[state]") {
        checkPulling(false);
    }

    TEST_CASE("Test pushing only happens when dirty", "[state]") {
        redis::Redis &redisState = redis::Redis::getState();

        auto kv = setupKV(4);
        std::string actualKey = util::keyForUser(kv->user, kv->key);

        std::vector<uint8_t> values = {0, 1, 2, 3};
        kv->set(values.data());
        kv->pushFull();

        // Change in redis directly
        std::vector<uint8_t> newValues = {3, 4, 5, 6};
        redisState.set(actualKey, newValues);

        // Push and make sure redis not changed as it's not dirty
        kv->pushFull();
        REQUIRE(redisState.get(actualKey) == newValues);

        // Now change locally and check push happens
        std::vector<uint8_t> newValues2 = {7, 7, 7, 7};
        kv->set(newValues2.data());
        kv->pushFull();
        REQUIRE(redisState.get(actualKey) == newValues2);
    }

    TEST_CASE("Test mapping shared memory", "[state]") {
        // Set up the KV
        auto kv = setupKV(5);
        std::vector<uint8_t> value = {0, 1, 2, 3, 4};
        kv->set(value.data());

        // Get a pointer to the shared memory and update
        uint8_t *sharedRegion = kv->get();
        sharedRegion[0] = 5;
        sharedRegion[2] = 5;

        // Map some shared memory areas
        int memSize = 2 * util::HOST_PAGE_SIZE;
        void *mappedRegionA = mmap(nullptr, memSize, PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        auto byteRegionA = static_cast<uint8_t *>(mappedRegionA);
        kv->mapSharedMemory(mappedRegionA, 0, 2);

        void *mappedRegionB = mmap(nullptr, memSize, PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        kv->mapSharedMemory(mappedRegionB, 0, 2);
        auto byteRegionB = static_cast<uint8_t *>(mappedRegionB);

        // Check shared memory regions reflect state
        std::vector<uint8_t> expected = {5, 1, 5, 3, 4};
        for (int i = 0; i < 5; i++) {
            REQUIRE(byteRegionA[i] == expected.at(i));
            REQUIRE(byteRegionB[i] == expected.at(i));
        }

        // Now update the pointer directly from both
        byteRegionA[2] = 6;
        byteRegionB[3] = 7;
        sharedRegion[4] = 8;
        std::vector<uint8_t> expected2 = {5, 1, 6, 7, 8};
        for (int i = 0; i < 5; i++) {
            REQUIRE(sharedRegion[i] == expected2.at(i));
            REQUIRE(byteRegionA[i] == expected2.at(i));
            REQUIRE(byteRegionB[i] == expected2.at(i));
        }

        // Now unmap the memory of one shared region
        kv->unmapSharedMemory(mappedRegionA);

        // Check original and shared regions still intact
        for (int i = 0; i < 5; i++) {
            REQUIRE(sharedRegion[i] == expected2.at(i));
            REQUIRE(byteRegionB[i] == expected2.at(i));
        }
    }

    TEST_CASE("Test mapping shared memory pulls if not initialised", "[state]") {
        // Set up the KV
        int length = 5;
        auto kv = setupKV(length);
        std::string actualKey = util::keyForUser(kv->user, kv->key);

        // Write value direct to redis
        std::vector<uint8_t> value = {0, 1, 2, 3, 4};
        redis::Redis &redisState = redis::Redis::getState();
        redisState.set(actualKey, value.data(), length);

        // Try to map the kv
        void *mappedRegion = mmap(nullptr, util::HOST_PAGE_SIZE, PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        kv->mapSharedMemory(mappedRegion, 0, 1);

        auto byteRegion = static_cast<uint8_t *>(mappedRegion);
        std::vector<uint8_t> actualValue(byteRegion, byteRegion + length);
        REQUIRE(actualValue == value);
    }

    TEST_CASE("Test mapping small shared memory offsets", "[state]") {
        // Set up the KV
        auto kv = setupKV(7);
        std::vector<uint8_t> value = {0, 1, 2, 3, 4, 5, 6};
        kv->set(value.data());

        // Map a single page of host memory
        void *mappedRegionA = mmap(nullptr, util::HOST_PAGE_SIZE, PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        void *mappedRegionB = mmap(nullptr, util::HOST_PAGE_SIZE, PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        // Map them to small segments of the shared memory
        kv->mapSharedMemory(mappedRegionA, 0, 1);
        kv->mapSharedMemory(mappedRegionB, 0, 1);

        auto byteRegionA = static_cast<uint8_t *>(mappedRegionA);
        auto byteRegionB = static_cast<uint8_t *>(mappedRegionB);

        // Update segments and check changes reflected
        uint8_t *full = kv->get();
        uint8_t *segmentA = kv->getSegment(1, 2);
        uint8_t *segmentB = kv->getSegment(4, 3);

        segmentA[1] = 8;
        segmentB[2] = 8;

        std::vector<uint8_t> expected = {0, 1, 8, 3, 4, 5, 8};
        for (int i = 0; i < 5; i++) {
            REQUIRE(full[i] == expected.at(i));
            REQUIRE(byteRegionA[i] == expected.at(i));
            REQUIRE(byteRegionB[i] == expected.at(i));
        }

        // Update shared regions and check reflected in segments
        byteRegionB[1] = 9;
        REQUIRE(segmentA[0] == 9);

        byteRegionA[5] = 1;
        REQUIRE(segmentB[1] == 1);
    }

    TEST_CASE("Test mapping bigger uninitialized shared memory offsets", "[state]") {
        // Define some larger chunks
        size_t mappingSize = 3 * util::HOST_PAGE_SIZE;

        // Set up a larger total value
        size_t totalSize = (10 * util::HOST_PAGE_SIZE) + 15;
        auto kv = setupKV(totalSize);
        std::string actualKey = util::keyForUser(kv->user, kv->key);

        // Write ones to storage
        std::vector<uint8_t> value(totalSize);
        std::fill(value.data(), value.data() + totalSize, 1);
        redis::Redis &redisState = redis::Redis::getState();
        redisState.set(actualKey, value);

        // Map a couple of segments in host memory (as would be done by the wasm module)
        void *mappedRegionA = mmap(nullptr, mappingSize, PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        void *mappedRegionB = mmap(nullptr, mappingSize, PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        // Do the mapping and check they're reporting the correct offset
        kv->mapSharedMemory(mappedRegionA, 6, 3);
        kv->mapSharedMemory(mappedRegionB, 2, 3);

        // Get a byte pointer to each
        auto byteRegionA = static_cast<uint8_t *>(mappedRegionA);
        auto byteRegionB = static_cast<uint8_t *>(mappedRegionB);

        // Write something to each one
        byteRegionA[5] = 5;
        byteRegionB[9] = 9;

        // Get pointers to these segments
        size_t offsetA = (6 * util::HOST_PAGE_SIZE);
        size_t offsetB = (2 * util::HOST_PAGE_SIZE);
        uint8_t *segmentA = kv->getSegment(offsetA, 10);
        uint8_t *segmentB = kv->getSegment(offsetB, 10);

        REQUIRE(segmentA[0] == 1);
        REQUIRE(segmentB[0] == 1);
        REQUIRE(segmentA[5] == 5);
        REQUIRE(segmentB[9] == 9);
    }

    TEST_CASE("Test pulling") {
        auto kv = setupKV(6);
        REQUIRE(kv->size() == 6);
        std::string actualKey = util::keyForUser(kv->user, kv->key);

        // Set up value in Redis
        redis::Redis &redisState = redis::Redis::getState();
        std::vector<uint8_t> value = {0, 1, 2, 3, 4, 5};
        redisState.set(actualKey, value);

        std::vector<uint8_t> expected;

        // Pull and check storage is initialised
        kv->pull();
        REQUIRE(kv->size() == 6);

        expected = {0, 1, 2, 3, 4, 5};
        uint8_t *actualBytes = kv->get();
        std::vector<uint8_t> actual(actualBytes, actualBytes + 6);
        REQUIRE(actual == expected);
    }

    TEST_CASE("Test deletion", "[state]") {
        redis::Redis &redisState = redis::Redis::getState();
        auto kv = setupKV(5);
        std::string actualKey = util::keyForUser(kv->user, kv->key);

        // Push some data and check
        std::vector<uint8_t> values = {0, 1, 2, 3, 4};
        kv->set(values.data());
        kv->pushFull();
        REQUIRE(redisState.get(actualKey) == values);

        kv->deleteGlobal();
        redisState.get(actualKey);
    }

    TEST_CASE("Test appended state with KV", "[state]") {
        auto kv = setupKV(1);

        std::vector<uint8_t> valuesA = {0, 1, 2, 3, 4};
        std::vector<uint8_t> valuesB = {5, 6};
        std::vector<uint8_t> valuesC = {6, 5, 4, 3, 2, 1};

        // Append one
        kv->append(valuesA.data(), valuesA.size());

        // Check
        std::vector<uint8_t> actualA = {0, 0, 0, 0, 0};
        kv->getAppended(actualA.data(), actualA.size(), 1);
        REQUIRE(actualA == valuesA);

        // Append a few and check
        kv->append(valuesB.data(), valuesB.size());
        kv->append(valuesB.data(), valuesB.size());
        kv->append(valuesC.data(), valuesC.size());

        size_t actualSize = valuesA.size() + 2 * valuesB.size() + valuesC.size();
        std::vector<uint8_t> actualMulti(actualSize, 0);

        std::vector<uint8_t> expectedMulti = {0, 1, 2, 3, 4, 5, 6, 5, 6, 6, 5, 4, 3, 2, 1};
        kv->getAppended(actualMulti.data(), actualSize, 4);

        REQUIRE(actualMulti == expectedMulti);
    }
}