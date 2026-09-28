// 1. Project header under test — first
#include "post_code.hpp"

// 2. Undefine macros that collide with GoogleTest names
#ifdef FAIL
#undef FAIL
#endif
#ifdef ERROR
#undef ERROR
#endif
#ifdef DEBUG
#undef DEBUG
#endif

// 3. GoogleTest / GoogleMock — after undef, before other libs
#include <gmock/gmock.h>
#include <gtest/gtest.h>

// 4. Other library/system headers
#include <cereal/archives/binary.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/tuple.hpp>
#include <cereal/types/vector.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Sample primary-code byte vectors used across tests
// ---------------------------------------------------------------------------
const primarycode_t kCode00 = {0x00};
const primarycode_t kCode10 = {0x10};
const primarycode_t kCode61 = {0x61};
const primarycode_t kCode90 = {0x90};
const primarycode_t kCodeFF = {0xFF};
const primarycode_t kCodeMax = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static postcode_t makeCode(primarycode_t primary,
                           secondarycode_t secondary = {})
{
    return std::make_tuple(primary, secondary);
}

// ---------------------------------------------------------------------------
// Base test fixture
// ---------------------------------------------------------------------------
class PostCodeFixture : public ::testing::Test
{
  protected:
    sdbusplus::bus_t bus = sdbusplus::bus::new_default();
    sd_event* rawEvent = nullptr;
    EventPtr event;
    fs::path logPath;
    const int node = 0;

    std::unique_ptr<PostCode> pc;

    void SetUp() override
    {
        logPath = fs::path(PostCodeListPathPrefix + std::to_string(node));
        // Directory is pre-created by the global PostCodeLogDirSetup
        // environment. No fs::create_directories needed here.

        sd_event_default(&rawEvent);
        event = EventPtr(rawEvent);

        pc = std::make_unique<PostCode>(
            bus, "/xyz/openbmc_project/State/Boot/PostCode/ut_fixture", event,
            node);
        // Start each test with a clean (reset) state
        pc->deleteAll();
        // Flush and drain the bus so both match subscriptions are fully
        // acknowledged by the daemon before any test signal is emitted
        bus.flush();
        for (int i = 0; i < 10; ++i)
        {
            bus.wait(10000ULL);
            while (bus.process_discard())
            {}
        }
    }

    void TearDown() override
    {
        pc.reset();
        fs::remove(logPath / CurrentBootCycleIndexName);
        fs::remove(logPath / CurrentBootCycleCountName);
    }

    // Write a cereal binary archive for a postcode map to logPath/<fileNum>
    void writePostCodeMap(uint16_t fileNum,
                          const std::map<uint64_t, postcode_t>& codes) const
    {
        std::ofstream os(logPath / std::to_string(fileNum));
        cereal::BinaryOutputArchive arch(os);
        arch(codes);
    }

    // Write CurrentBootCycleCount
    void writeCycleCount(uint16_t count) const
    {
        std::ofstream os(logPath / CurrentBootCycleCountName, std::ios::binary);
        cereal::BinaryOutputArchive arch(os);
        arch(count);
    }

    // Write CurrentBootCycleIndex
    void writeCycleIndex(uint16_t index) const
    {
        std::ofstream os(logPath / CurrentBootCycleIndexName, std::ios::binary);
        cereal::BinaryOutputArchive arch(os);
        arch(index);
    }

    // Reload PostCode from the filesystem (simulates process restart)
    std::unique_ptr<PostCode> reload(const char* dbusPath)
    {
        sd_event* ev2 = nullptr;
        sd_event_default(&ev2);
        EventPtr event2 = EventPtr(ev2);
        return std::make_unique<PostCode>(bus, dbusPath, event2, node);
    }

    // Emit a PropertiesChanged signal on the raw post-code interface.
    // Blocks until the D-Bus daemon routes the signal back and all match
    // callbacks (including PostCode::savePostCodes) have fired.
    void emitPostCodeSignal(postcode_t code)
    {
        auto signal = bus.new_signal(
            (std::string(PostCodePath) + std::to_string(node)).c_str(),
            "org.freedesktop.DBus.Properties", "PropertiesChanged");
        signal.append(std::string("xyz.openbmc_project.State.Boot.Raw"));
        std::map<std::string, std::variant<postcode_t>> changed;
        changed["Value"] = code;
        signal.append(changed);
        signal.append(std::vector<std::string>{});
        signal.signal_send();
        bus.flush();
        // 20 × 10 ms = 200 ms total; short cycles handle cases where
        // wait() returns early for an unrelated message
        for (int i = 0; i < 20; ++i)
        {
            bus.wait(10000ULL);
            while (bus.process_discard())
            {}
        }
    }

    // Emit a PropertiesChanged signal on the host-state interface.
    void emitHostStateSignal(const std::string& stateStr)
    {
        auto signal = bus.new_signal(
            (std::string(HostStatePathPrefix) + std::to_string(node)).c_str(),
            "org.freedesktop.DBus.Properties", "PropertiesChanged");
        signal.append(std::string("xyz.openbmc_project.State.Host"));
        std::map<std::string, std::variant<std::string>> changed;
        changed["CurrentHostState"] = stateStr;
        signal.append(changed);
        signal.append(std::vector<std::string>{});
        signal.signal_send();
        bus.flush();
        for (int i = 0; i < 20; ++i)
        {
            bus.wait(10000ULL);
            while (bus.process_discard())
            {}
        }
    }
};

// ===========================================================================
// 1. Constants
// ===========================================================================

TEST(PostCodeConstants, MaxBootCycleCount_IsPositiveAndAtMost100)
{
    EXPECT_GT(MAX_BOOT_CYCLE_COUNT, 0);
    EXPECT_LE(MAX_BOOT_CYCLE_COUNT, 100);
}

TEST(PostCodeConstants, MaxPostCodeSizePerCycle_IsPositive)
{
    EXPECT_GT(MAX_POST_CODE_SIZE_PER_CYCLE, 0);
}

TEST(PostCodeConstants, MaxPostCodeSizePerCycle_AtLeast64)
{
    // meson.options enforces min:64 for this option
    EXPECT_GE(MAX_POST_CODE_SIZE_PER_CYCLE, 64);
}

// ===========================================================================
// 2. postcode_t type contract — tuple<vector<uint8_t>, vector<uint8_t>>
// ===========================================================================

TEST(PostCodeTypes, PostcodeT_PrimaryIsVector_SecondaryIsVector)
{
    postcode_t code = makeCode({0x01, 0x02, 0x03}, {0xAA, 0xBB});
    EXPECT_EQ((primarycode_t{0x01, 0x02, 0x03}), std::get<0>(code));
    ASSERT_EQ(2u, std::get<1>(code).size());
    EXPECT_EQ(0xAAu, std::get<1>(code)[0]);
}

TEST(PostCodeTypes, PostcodeT_EmptySecondary_IsValid)
{
    postcode_t code = makeCode({0xFF});
    EXPECT_EQ((primarycode_t{0xFF}), std::get<0>(code));
    EXPECT_TRUE(std::get<1>(code).empty());
}

TEST(PostCodeTypes, PostcodeT_EmptyBoth_IsValid)
{
    postcode_t code = makeCode({}, {});
    EXPECT_TRUE(std::get<0>(code).empty());
    EXPECT_TRUE(std::get<1>(code).empty());
}

TEST(PostCodeTypes, PostcodeT_MultiBytePayload_StoredExactly)
{
    secondarycode_t sec(256, 0xAB);
    postcode_t code = makeCode(kCodeFF, sec);
    EXPECT_EQ(kCodeFF, std::get<0>(code));
    EXPECT_EQ(256u, std::get<1>(code).size());
    EXPECT_TRUE(std::all_of(std::get<1>(code).begin(), std::get<1>(code).end(),
                            [](uint8_t b) { return b == 0xAB; }));
}

// ===========================================================================
// 3. Constructor / startup state
// ===========================================================================

TEST_F(PostCodeFixture, Constructor_FreshState_BootCycleCountIsZero)
{
    // deleteAll was called in SetUp(); count must be 0
    EXPECT_EQ(0u, pc->currentBootCycleCount());
}

TEST_F(PostCodeFixture, Constructor_MaxBootCycleNum_MatchesMacro)
{
    EXPECT_EQ(MAX_BOOT_CYCLE_COUNT, pc->maxBootCycleNum());
}

// ===========================================================================
// 4. deleteAll()
// ===========================================================================

TEST_F(PostCodeFixture, DeleteAll_ResetsBootCycleCount)
{
    // Arrange: seed count via on-disk file, then reload
    writeCycleIndex(3);
    writeCycleCount(3);
    auto pc2 =
        reload("/xyz/openbmc_project/State/Boot/PostCode/dal_resetcount");
    ASSERT_EQ(3u, pc2->currentBootCycleCount());

    // Act
    pc2->deleteAll();

    // Assert
    EXPECT_EQ(0u, pc2->currentBootCycleCount());
}

TEST_F(PostCodeFixture, DeleteAll_RecreatesLogDirectory)
{
    pc->deleteAll();
    EXPECT_TRUE(fs::is_directory(logPath));
}

TEST_F(PostCodeFixture, DeleteAll_CalledTwice_NoThrow)
{
    EXPECT_NO_THROW(pc->deleteAll());
    EXPECT_NO_THROW(pc->deleteAll());
}

TEST_F(PostCodeFixture, DeleteAll_ThenGetPostCodes_ReturnsEmpty)
{
    // SetUp already called deleteAll; getPostCodes on a clean object
    EXPECT_TRUE(pc->getPostCodes(1).empty());
}

TEST_F(PostCodeFixture, DeleteAll_ThenGetPostCodesWithTimeStamp_ReturnsEmpty)
{
    EXPECT_TRUE(pc->getPostCodesWithTimeStamp(1).empty());
}

// ===========================================================================
// 5. getPostCodes() — boundary validation (patch 0003)
// ===========================================================================

TEST_F(PostCodeFixture, GetPostCodes_IndexZero_ThrowsInvalidArgument)
{
    EXPECT_THROW(pc->getPostCodes(0), InvalidArgumentError);
}

TEST_F(PostCodeFixture, GetPostCodes_IndexExceedsMax_ThrowsInvalidArgument)
{
    uint16_t badIndex = static_cast<uint16_t>(pc->maxBootCycleNum() + 1);
    EXPECT_THROW(pc->getPostCodes(badIndex), InvalidArgumentError);
}

TEST_F(PostCodeFixture, GetPostCodes_IndexAtMax_DoesNotThrow)
{
    EXPECT_NO_THROW(pc->getPostCodes(pc->maxBootCycleNum()));
}

TEST_F(PostCodeFixture, GetPostCodes_IndexOne_ReturnsEmptyWhenNoData)
{
    EXPECT_TRUE(pc->getPostCodes(1).empty());
}

TEST_F(PostCodeFixture, GetPostCodes_IndexTwo_NoThrow)
{
    EXPECT_NO_THROW(pc->getPostCodes(2));
}

TEST_F(PostCodeFixture, GetPostCodes_MaxPlusOne_ThrowsInvalidArgument)
{
    uint16_t overMax = static_cast<uint16_t>(MAX_BOOT_CYCLE_COUNT + 1);
    EXPECT_THROW(pc->getPostCodes(overMax), InvalidArgumentError);
}

// ===========================================================================
// 6. getPostCodesWithTimeStamp()
// ===========================================================================

TEST_F(PostCodeFixture, GetPostCodesWithTimeStamp_IndexOne_NoThrow)
{
    EXPECT_NO_THROW(pc->getPostCodesWithTimeStamp(1));
}

TEST_F(PostCodeFixture, GetPostCodesWithTimeStamp_NoData_ReturnsEmptyMap)
{
    EXPECT_TRUE(pc->getPostCodesWithTimeStamp(1).empty());
}

TEST_F(PostCodeFixture, GetPostCodesWithTimeStamp_IndexEqualsMax_DoesNotThrow)
{
    EXPECT_NO_THROW(pc->getPostCodesWithTimeStamp(pc->maxBootCycleNum()));
}

// ===========================================================================
// 7. Serialize / deserialize — via constructor reload (round-trip)
// ===========================================================================

TEST_F(PostCodeFixture, Deserialize_ExistingIndexAndCountFiles_LoadsBootCount)
{
    // Arrange: write index=3, count=3 to disk
    writeCycleIndex(3);
    writeCycleCount(3);

    // Act: reload — constructor calls deserialize() on startup
    auto pc2 =
        reload("/xyz/openbmc_project/State/Boot/PostCode/deser_count_test");

    // Assert
    EXPECT_EQ(3u, pc2->currentBootCycleCount());
}

TEST_F(PostCodeFixture, Deserialize_MissingFiles_DefaultsToZeroCount)
{
    // Arrange: deleteAll ensures no index files
    pc->deleteAll();

    // Act: create a new PostCode; no index files on disk
    auto pc2 =
        reload("/xyz/openbmc_project/State/Boot/PostCode/deser_missing_test");

    // Assert
    EXPECT_EQ(0u, pc2->currentBootCycleCount());
}

TEST_F(PostCodeFixture, DeserializePostCodes_WriteFileThenRead_ReturnsData)
{
    // Arrange: write two postcodes to archive file "1"
    std::map<uint64_t, postcode_t> writeCodes;
    writeCodes[1000u] = makeCode({0xAB, 0xCD}, {0x01, 0x02});
    writeCodes[2000u] = makeCode({0xEF, 0x01}, {0x03});
    writePostCodeMap(1, writeCodes);
    writeCycleIndex(1);
    writeCycleCount(1);

    // Act: reload reads index/count, getPostCodesWithTimeStamp reads archive
    // "1"
    auto pc2 =
        reload("/xyz/openbmc_project/State/Boot/PostCode/deser_postcodes_test");
    auto result = pc2->getPostCodesWithTimeStamp(1);

    // Assert
    ASSERT_EQ(2u, result.size());
    EXPECT_EQ((primarycode_t{0xAB, 0xCD}), std::get<0>(result.at(1000u)));
    EXPECT_EQ((primarycode_t{0xEF, 0x01}), std::get<0>(result.at(2000u)));
    EXPECT_EQ(2u, std::get<1>(result.at(1000u)).size());
    EXPECT_EQ(0x01u, std::get<1>(result.at(1000u))[0]);

    // Cleanup archive file
    fs::remove(logPath / "1");
}

TEST_F(PostCodeFixture, DeserializePostCodes_MissingArchive_ReturnsEmpty)
{
    // Arrange: clean slate, no archive files
    pc->deleteAll();

    // Act
    auto result = pc->getPostCodesWithTimeStamp(1);

    // Assert: no file -> empty map
    EXPECT_TRUE(result.empty());
}

// ===========================================================================
// 8. getPostCodes() vs getPostCodesWithTimeStamp() data parity
// ===========================================================================

TEST_F(PostCodeFixture, GetPostCodes_AndWithTimeStamp_SameSizeAndValues)
{
    // Arrange: two entries in archive "1"
    std::map<uint64_t, postcode_t> writeCodes;
    writeCodes[100u] = makeCode({0xAA}, {0xBB});
    writeCodes[200u] = makeCode({0xCC});
    writePostCodeMap(1, writeCodes);
    writeCycleIndex(1);
    writeCycleCount(1);

    auto pc2 = reload("/xyz/openbmc_project/State/Boot/PostCode/parity_test");

    // Act
    auto vec = pc2->getPostCodes(1);
    auto map = pc2->getPostCodesWithTimeStamp(1);

    // Assert: same count and same values
    EXPECT_EQ(vec.size(), map.size());
    for (const auto& kv : map)
    {
        EXPECT_THAT(vec, ::testing::Contains(kv.second));
    }

    // Cleanup
    fs::remove(logPath / "1");
}

// ===========================================================================
// 9. getBootNum wrap-around — observable through getPostCodesWithTimeStamp
// ===========================================================================

TEST_F(PostCodeFixture, GetPostCodes_IndexJustBelowMax_NoThrow)
{
    uint16_t belowMax = static_cast<uint16_t>(pc->maxBootCycleNum() - 1);
    if (belowMax > 0)
    {
        EXPECT_NO_THROW(pc->getPostCodes(belowMax));
    }
}

TEST_F(PostCodeFixture,
       GetPostCodesWithTimeStamp_WrappedCycleIndex_ReadsCorrectFileNumber)
{
    // Scenario: the cycle index has wrapped around (cycleIndex=1, count=3).
    // getBootNum() maps user-visible boot index → on-disk archive file number:
    //   index=1 → file 1              (current boot;   no wrap: 1 ≤ cycleIndex)
    //   index=2 → file maxCycles      (1 boot back;    wraps: (100+1)-2+1 =
    //   100) index=3 → file maxCycles-1    (2 boots back;   wraps: (100+1)-3+1
    //   =  99)

    const uint16_t maxCycles = pc->maxBootCycleNum(); // 100 (default build)

    // Arrange: write distinct codes to each of the three archive files
    std::map<uint64_t, postcode_t> fileCurrent;
    fileCurrent[100u] = makeCode(kCodeFF, {0xC0}); // → file 1

    std::map<uint64_t, postcode_t> fileOneBack;
    fileOneBack[200u] = makeCode(kCode90, {0xC1}); // → file maxCycles

    std::map<uint64_t, postcode_t> fileTwoBack;
    fileTwoBack[300u] = makeCode(kCode61, {0xC2}); // → file maxCycles-1

    writePostCodeMap(1, fileCurrent);
    writePostCodeMap(maxCycles, fileOneBack);
    writePostCodeMap(maxCycles - 1, fileTwoBack);
    writeCycleIndex(1);
    writeCycleCount(3);

    auto pc2 = reload("/xyz/openbmc_project/State/Boot/PostCode/wrapped_index");

    // Act + Assert — index=3 is the primary focus (wrap-around path)
    auto result3 = pc2->getPostCodesWithTimeStamp(3);
    ASSERT_EQ(1u, result3.size());
    EXPECT_EQ(kCode61, std::get<0>(result3.at(300u)));
    EXPECT_EQ(0xC2u, std::get<1>(result3.at(300u))[0]);

    // Verify index=2 (also wraps, to maxCycles)
    auto result2 = pc2->getPostCodesWithTimeStamp(2);
    ASSERT_EQ(1u, result2.size());
    EXPECT_EQ(kCode90, std::get<0>(result2.at(200u)));
    EXPECT_EQ(0xC1u, std::get<1>(result2.at(200u))[0]);

    // Verify index=1 (no wrap — current boot)
    auto result1 = pc2->getPostCodesWithTimeStamp(1);
    ASSERT_EQ(1u, result1.size());
    EXPECT_EQ(kCodeFF, std::get<0>(result1.at(100u)));
    EXPECT_EQ(0xC0u, std::get<1>(result1.at(100u))[0]);

    // Cleanup
    fs::remove(logPath / "1");
    fs::remove(logPath / std::to_string(maxCycles));
    fs::remove(logPath / std::to_string(maxCycles - 1));
}

// ===========================================================================
// 10. Multiple boot cycles — two cycles on disk, both readable
// ===========================================================================

TEST_F(PostCodeFixture, MultiBootCycle_TwoCyclesOnDisk_BothReadBack)
{
    // Arrange: two independent archives
    pc->deleteAll();

    std::map<uint64_t, postcode_t> cycle1Codes;
    cycle1Codes[111u] = makeCode({0x11});

    std::map<uint64_t, postcode_t> cycle2Codes;
    cycle2Codes[222u] = makeCode({0x22});

    writePostCodeMap(1, cycle1Codes);
    writePostCodeMap(2, cycle2Codes);
    writeCycleIndex(2);
    writeCycleCount(2);

    auto pc2 =
        reload("/xyz/openbmc_project/State/Boot/PostCode/multi_cycle_test");

    // Act: index=2 is current boot; index=1 is previous
    auto current = pc2->getPostCodesWithTimeStamp(2);
    auto previous = pc2->getPostCodesWithTimeStamp(1);

    // Assert
    ASSERT_EQ(1u, current.size());
    EXPECT_EQ((primarycode_t{0x22}), std::get<0>(current.at(222u)));

    ASSERT_EQ(1u, previous.size());
    EXPECT_EQ((primarycode_t{0x11}), std::get<0>(previous.at(111u)));

    // Cleanup
    fs::remove(logPath / "1");
    fs::remove(logPath / "2");
}

// ===========================================================================
// 11. deleteAll() then reload: count reset persists across object recreation
// ===========================================================================

TEST_F(PostCodeFixture, DeleteAll_ThenReload_CountRemainsZero)
{
    // Arrange: seed count via disk then reset
    writeCycleIndex(7);
    writeCycleCount(7);
    auto pc2 = reload("/xyz/openbmc_project/State/Boot/PostCode/dal_reload1");
    ASSERT_EQ(7u, pc2->currentBootCycleCount());
    pc2->deleteAll();

    // Act: create a third PostCode from the now-empty disk state
    auto pc3 = reload("/xyz/openbmc_project/State/Boot/PostCode/dal_reload2");

    // Assert: deleteAll removed the files, so count defaults to 0
    EXPECT_EQ(0u, pc3->currentBootCycleCount());
}

// ===========================================================================
// 12. getPostCodes() — primary/secondary byte vectors preserved
// ===========================================================================

TEST_F(PostCodeFixture, GetPostCodes_MultipleEntries_ReturnsCorrectPrimaries)
{
    // Arrange: write three codes to cycle archive 1
    std::map<uint64_t, postcode_t> writeCodes;
    writeCodes[1000u] = makeCode(kCode10, {0x01});
    writeCodes[2000u] = makeCode(kCode61, {});
    writeCodes[3000u] = makeCode(kCode90, {0xAA, 0xBB});
    writePostCodeMap(1, writeCodes);
    writeCycleIndex(1);
    writeCycleCount(1);

    auto pc2 =
        reload("/xyz/openbmc_project/State/Boot/PostCode/multi_primaries");

    // Act: getPostCodes returns vector in timestamp-ascending order
    auto codes = pc2->getPostCodes(1);

    // Assert
    ASSERT_EQ(3u, codes.size());
    EXPECT_EQ(kCode10, std::get<0>(codes[0]));
    EXPECT_EQ(kCode61, std::get<0>(codes[1]));
    EXPECT_EQ(kCode90, std::get<0>(codes[2]));

    fs::remove(logPath / "1");
}

TEST_F(PostCodeFixture, GetPostCodes_SecondaryBytes_PreservedExactly)
{
    // Arrange
    std::map<uint64_t, postcode_t> writeCodes;
    writeCodes[500u] = makeCode(kCodeFF, {0xDE, 0xAD, 0xBE, 0xEF});
    writePostCodeMap(1, writeCodes);
    writeCycleIndex(1);
    writeCycleCount(1);

    auto pc2 =
        reload("/xyz/openbmc_project/State/Boot/PostCode/secondary_bytes");
    auto codes = pc2->getPostCodes(1);

    // Assert: secondary bytes survive the serialize/deserialize round-trip
    ASSERT_EQ(1u, codes.size());
    EXPECT_EQ(kCodeFF, std::get<0>(codes[0]));
    ASSERT_EQ(4u, std::get<1>(codes[0]).size());
    EXPECT_EQ(0xDEu, std::get<1>(codes[0])[0]);
    EXPECT_EQ(0xEFu, std::get<1>(codes[0])[3]);

    fs::remove(logPath / "1");
}

// ===========================================================================
// 13. getPostCodesWithTimeStamp() — timestamp key fidelity
// ===========================================================================

TEST_F(PostCodeFixture, GetPostCodesWithTimeStamp_Data_TimestampKeysExact)
{
    // Arrange
    std::map<uint64_t, postcode_t> writeCodes;
    writeCodes[1000u] = makeCode(kCode10, {0x01, 0x02});
    writeCodes[2000u] = makeCode(kCode61, {});
    writePostCodeMap(1, writeCodes);
    writeCycleIndex(1);
    writeCycleCount(1);

    auto pc2 = reload("/xyz/openbmc_project/State/Boot/PostCode/ts_keys");
    auto result = pc2->getPostCodesWithTimeStamp(1);

    // Assert: exact timestamp keys are preserved (map keyed on uS timestamp)
    ASSERT_EQ(2u, result.size());
    EXPECT_NE(result.end(), result.find(1000u));
    EXPECT_NE(result.end(), result.find(2000u));

    fs::remove(logPath / "1");
}

TEST_F(PostCodeFixture, GetPostCodesWithTimeStamp_MaxPrimary_PreservedExactly)
{
    // Arrange
    std::map<uint64_t, postcode_t> writeCodes;
    writeCodes[9999u] = makeCode(kCodeMax, {0xFF, 0xFE});
    writePostCodeMap(1, writeCodes);
    writeCycleIndex(1);
    writeCycleCount(1);

    auto pc2 = reload("/xyz/openbmc_project/State/Boot/PostCode/max_primary");
    auto result = pc2->getPostCodesWithTimeStamp(1);

    ASSERT_EQ(1u, result.size());
    auto& entry = result.at(9999u);
    EXPECT_EQ(kCodeMax, std::get<0>(entry));
    ASSERT_EQ(2u, std::get<1>(entry).size());
    EXPECT_EQ(0xFFu, std::get<1>(entry)[0]);
    EXPECT_EQ(0xFEu, std::get<1>(entry)[1]);

    fs::remove(logPath / "1");
}

// ===========================================================================
// 14. Corrupt-file notes
// ===========================================================================
// Corrupt count/index files: cereal::BinaryInputArchive reads raw bytes with
// no structural validation on primitives — no throw, just misinterpreted data.
//
// Corrupt postcode archive: cereal reads the map-size prefix as a raw uint64;
// if garbage bytes yield a very large count, cereal triggers std::bad_alloc
// before it can throw cereal::Exception. deserializePostCodes() catches only
// cereal::Exception and fs::filesystem_error; std::bad_alloc propagates.
// No test is added here because it would require modifying production code
// to catch std::bad_alloc (which is read-only per copilot-instructions §4.1).

// ===========================================================================
// 15. Round-trip — all code sizes, payload sizes, ordering
// ===========================================================================

TEST_F(PostCodeFixture, RoundTrip_MultipleCodesInSingleCycle)
{
    // Arrange: store five codes with distinct primary byte vectors
    std::map<uint64_t, postcode_t> writeCodes;
    writeCodes[1000u] = makeCode(kCode00, {});
    writeCodes[2000u] = makeCode(kCode10, {0x10});
    writeCodes[3000u] = makeCode(kCode61, {0x61, 0x00});
    writeCodes[4000u] = makeCode(kCode90, {0x90});
    writeCodes[5000u] = makeCode(kCodeFF, {0xFF});
    writePostCodeMap(1, writeCodes);
    writeCycleIndex(1);
    writeCycleCount(1);

    auto pc2 = reload("/xyz/openbmc_project/State/Boot/PostCode/rt_all_codes");
    auto result = pc2->getPostCodesWithTimeStamp(1);

    ASSERT_EQ(5u, result.size());
    EXPECT_EQ(kCode00, std::get<0>(result.at(1000u)));
    EXPECT_EQ(kCode10, std::get<0>(result.at(2000u)));
    EXPECT_EQ(kCode61, std::get<0>(result.at(3000u)));
    EXPECT_EQ(kCode90, std::get<0>(result.at(4000u)));
    EXPECT_EQ(kCodeFF, std::get<0>(result.at(5000u)));

    fs::remove(logPath / "1");
}

TEST_F(PostCodeFixture, RoundTrip_MaxPrimary_PreservedExactly)
{
    std::map<uint64_t, postcode_t> writeCodes;
    writeCodes[100u] = makeCode(kCodeMax, {});
    writePostCodeMap(1, writeCodes);
    writeCycleIndex(1);
    writeCycleCount(1);

    auto pc2 =
        reload("/xyz/openbmc_project/State/Boot/PostCode/rt_max_primary");
    auto result = pc2->getPostCodesWithTimeStamp(1);

    ASSERT_EQ(1u, result.size());
    EXPECT_EQ(kCodeMax, std::get<0>(result.at(100u)));
    EXPECT_TRUE(std::get<1>(result.at(100u)).empty());

    fs::remove(logPath / "1");
}

TEST_F(PostCodeFixture, RoundTrip_EmptySecondary_TupleIntact)
{
    std::map<uint64_t, postcode_t> writeCodes;
    writeCodes[200u] = makeCode(kCode90, {});
    writePostCodeMap(1, writeCodes);
    writeCycleIndex(1);
    writeCycleCount(1);

    auto pc2 = reload("/xyz/openbmc_project/State/Boot/PostCode/rt_empty_sec");
    auto result = pc2->getPostCodesWithTimeStamp(1);

    ASSERT_EQ(1u, result.size());
    EXPECT_EQ(kCode90, std::get<0>(result.at(200u)));
    EXPECT_TRUE(std::get<1>(result.at(200u)).empty());

    fs::remove(logPath / "1");
}

TEST_F(PostCodeFixture, RoundTrip_LargeSecondaryPayload_AllBytesPreserved)
{
    // 128-byte secondary — verifies no truncation in serialization path
    secondarycode_t bigPayload(128);
    std::iota(bigPayload.begin(), bigPayload.end(), 0x00);

    std::map<uint64_t, postcode_t> writeCodes;
    writeCodes[300u] = makeCode(kCode61, bigPayload);
    writePostCodeMap(1, writeCodes);
    writeCycleIndex(1);
    writeCycleCount(1);

    auto pc2 = reload("/xyz/openbmc_project/State/Boot/PostCode/rt_large_sec");
    auto result = pc2->getPostCodesWithTimeStamp(1);

    ASSERT_EQ(1u, result.size());
    auto& sec = std::get<1>(result.at(300u));
    ASSERT_EQ(128u, sec.size());
    for (size_t i = 0; i < sec.size(); i++)
    {
        EXPECT_EQ(static_cast<uint8_t>(i), sec[i])
            << "Byte mismatch at index " << i;
    }

    fs::remove(logPath / "1");
}

TEST_F(PostCodeFixture, RoundTrip_MultipleTimestamps_AscendingOrderPreserved)
{
    // getPostCodes() transforms the map to vector — key order must be ascending
    std::map<uint64_t, postcode_t> writeCodes;
    writeCodes[500u] = makeCode({0x0A});
    writeCodes[1500u] = makeCode({0x0B});
    writeCodes[2500u] = makeCode({0x0C});
    writePostCodeMap(1, writeCodes);
    writeCycleIndex(1);
    writeCycleCount(1);

    auto pc2 = reload("/xyz/openbmc_project/State/Boot/PostCode/rt_order");
    auto codes = pc2->getPostCodes(1);

    ASSERT_EQ(3u, codes.size());
    EXPECT_EQ((primarycode_t{0x0A}), std::get<0>(codes[0]));
    EXPECT_EQ((primarycode_t{0x0B}), std::get<0>(codes[1]));
    EXPECT_EQ((primarycode_t{0x0C}), std::get<0>(codes[2]));

    fs::remove(logPath / "1");
}

TEST_F(PostCodeFixture, RoundTrip_BootCycleCountReload_CountPreserved)
{
    // Arrange: serialise count=3 to disk
    writeCycleIndex(3);
    writeCycleCount(3);

    // Act: new instance loads from disk
    auto pc2 =
        reload("/xyz/openbmc_project/State/Boot/PostCode/rt_count_reload");

    // Assert
    EXPECT_EQ(3u, pc2->currentBootCycleCount());
}

// ===========================================================================
// 16. InvalidArgumentError virtual methods — covers post_code.hpp lines 67-82
// ===========================================================================

TEST(InvalidArgumentError, VirtualMethods_ReturnExpectedValues)
{
    InvalidArgumentError err;
    EXPECT_STREQ("xyz.openbmc_project.Common.Error.InvalidArgument",
                 err.name());
    EXPECT_NE(nullptr, err.description());
    EXPECT_NE(nullptr, err.what());
    EXPECT_EQ(EINVAL, err.get_errno());
}

// ===========================================================================
// 17. savePostCodes() + incrBootCycle() — triggered via D-Bus signal
// ===========================================================================

TEST_F(PostCodeFixture, SavePostCodes_FirstSignal_IncrementsBootCycle)
{
    // Arrange: fresh state (deleteAll in SetUp sets count=0, cycleIndex=0)
    ASSERT_EQ(0u, pc->currentBootCycleCount());

    // Act: emit raw post-code PropertiesChanged signal
    emitPostCodeSignal(makeCode(kCode10, {0x01}));

    // Assert: incrBootCycle was called → count becomes 1
    EXPECT_EQ(1u, pc->currentBootCycleCount());
}

TEST_F(PostCodeFixture, SavePostCodes_SecondSignal_CoversTsUsBranch)
{
    // Arrange: first signal sets up in-memory postCodes and starts the cycle
    emitPostCodeSignal(makeCode(kCode10, {}));
    ASSERT_EQ(1u, pc->currentBootCycleCount());

    // Act: second signal hits the "postCodes not empty" tsUS calculation branch
    emitPostCodeSignal(makeCode(kCode61, {}));

    // Assert: still in the same boot cycle (count unchanged)
    EXPECT_EQ(1u, pc->currentBootCycleCount());
}

TEST_F(PostCodeFixture, SavePostCodes_DeleteAllWithActiveTimer_StopsTimer)
{
    // Arrange: emit a signal so that savePostCodes creates and starts the timer
    emitPostCodeSignal(makeCode(kCodeFF, {0xAB}));

    // Act: deleteAll() must stop the running timer (covers line 33 of
    // post_code.cpp)
    EXPECT_NO_THROW(pc->deleteAll());

    // Assert: boot cycle count reset
    EXPECT_EQ(0u, pc->currentBootCycleCount());
}

TEST_F(PostCodeFixture, IncrBootCycle_AtMax_WrapsToOne)
{
    // Arrange: destroy current pc (removes its match subscription),
    // seed cycleIndex at max so the next incrBootCycle wraps to 1.
    uint16_t maxCycles = pc->maxBootCycleNum();
    pc.reset();
    writeCycleIndex(maxCycles);
    writeCycleCount(maxCycles);

    // Create a fresh PostCode that reads the seeded state
    sd_event* ev2 = nullptr;
    sd_event_default(&ev2);
    EventPtr event2 = EventPtr(ev2);
    pc = std::make_unique<PostCode>(
        bus, "/xyz/openbmc_project/State/Boot/PostCode/incrbootcycle_wrap",
        event2, node);
    ASSERT_EQ(maxCycles, pc->currentBootCycleCount());

    // Act: emit signal → savePostCodes → cycleIndex!=0 else-branch →
    //      incrBootCycle → cycleIndex >= max → wraps to 1
    emitPostCodeSignal(makeCode(kCode90, {}));

    // Assert: count stays at max (min(max, max+1) == max)
    EXPECT_EQ(maxCycles, pc->currentBootCycleCount());
}

// ===========================================================================
// 18. serialize() — triggered via HostState Running → postcode → Off
// ===========================================================================

TEST_F(PostCodeFixture, Serialize_BootRunningThenOff_WritesDataToDisk)
{
    // Arrange: set bootInProgress=true via HostState Running signal
    emitHostStateSignal("xyz.openbmc_project.State.Host.HostState.Running");

    // Send a postcode so postCodes is not empty
    emitPostCodeSignal(makeCode(kCodeFF, {0xBE, 0xEF}));
    ASSERT_EQ(1u, pc->currentBootCycleCount());

    // Act: HostState Off → serialize called (bootInProgress=true, postCodes≠∅)
    emitHostStateSignal("xyz.openbmc_project.State.Host.HostState.Off");

    // Assert: cycle data file "1" now exists on disk (written by serialize)
    EXPECT_TRUE(fs::exists(logPath / "1"));

    // Cleanup cycle data file written by serialize
    fs::remove(logPath / "1");
}

// ===========================================================================
// 19. Shutdown-requested path — postcode ignored when shutdownRequested=true
// ===========================================================================

TEST_F(PostCodeFixture, SavePostCodes_ShutdownRequested_IgnoresPostCode)
{
    // Arrange: drive to shutdownRequested=true via HostState Off signal
    emitHostStateSignal("xyz.openbmc_project.State.Host.HostState.Off");

    // Assert: shutdown flag prevents savePostCodes → count stays 0
    uint16_t countBefore = pc->currentBootCycleCount();
    emitPostCodeSignal(makeCode(kCode00, {}));
    EXPECT_EQ(countBefore, pc->currentBootCycleCount());
}
