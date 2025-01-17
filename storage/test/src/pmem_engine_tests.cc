#include <gtest/gtest.h>
#include <storage/engine_interface.h>
#include <storage/pmem_engine.h>

namespace storage {

TEST(PMemEngineTests, PMemEngine) {
    const uint64_t PMEM_USED_SIZE_DEFAULT = 1024UL * 1024UL * 1024UL;
    std::shared_ptr<StorageEngineInterface> engFace = std::make_shared<PMemEngine>("/tmp/test_db", "radix", PMEM_USED_SIZE_DEFAULT);    
}

TEST(PMemEngineTests, PMemEngineTestPutGet) {
    const uint64_t PMEM_USED_SIZE_DEFAULT = 1024UL * 1024UL * 1024UL;
    std::shared_ptr<StorageEngineInterface> engFace = std::make_shared<PMemEngine>("/tmp/test_db_put_get", "radix", PMEM_USED_SIZE_DEFAULT);  
    ASSERT_EQ(true, engFace->PutK("testkey", "hello eraft!"));
    std::string gotV;
    ASSERT_EQ(true, engFace->GetV("testkey", gotV));
    ASSERT_EQ("hello eraft!", gotV);
}

TEST(PMemEngineTests, PMemEngineTestGetFromExistsDB) {
    const uint64_t PMEM_USED_SIZE_DEFAULT = 1024UL * 1024UL * 1024UL;
    std::shared_ptr<StorageEngineInterface> engFace = std::make_shared<PMemEngine>("/tmp/test_db_put_get", "radix", PMEM_USED_SIZE_DEFAULT);  
    std::string gotV;
    ASSERT_EQ(true, engFace->GetV("testkey", gotV));
    ASSERT_EQ("hello eraft!", gotV);
}

}
