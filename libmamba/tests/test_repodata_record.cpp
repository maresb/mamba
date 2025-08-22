#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>

#include "mamba/core/transaction.hpp"
#include "mamba/core/package_info.hpp"
#include "mamba/core/package_cache.hpp"

namespace mamba
{
    namespace test
    {
        class RepodataRecordTest : public ::testing::Test
        {
        protected:
            void SetUp() override
            {
                // Set up any common test fixtures
            }
            
            void TearDown() override
            {
                // Clean up any test fixtures
            }
        };

        TEST_F(RepodataRecordTest, RepodataRecordPreservesExtractedMetadata)
        {
            // Create a temporary directory structure that mimics an extracted package
            TemporaryDirectory temp_dir;
            fs::path info_dir = temp_dir.path() / "info";
            fs::create_directories(info_dir);
            
            // Create a mock index.json with complete package metadata
            nlohmann::json index_json;
            index_json["name"] = "test-package";
            index_json["version"] = "1.0.0";
            index_json["depends"] = {"python >=3.8", "numpy"};
            index_json["constrains"] = {"pytest"};
            index_json["license"] = "MIT";
            index_json["license_family"] = "MIT";
            index_json["track_features"] = "";
            index_json["size"] = 1024;
            index_json["timestamp"] = 1234567890;
            
            std::ofstream index_file(info_dir / "index.json");
            index_file << index_json.dump(4);
            index_file.close();
            
            // Create a PackageInfo with incomplete metadata (simulating URL-based info)
            PackageInfo pkg_info("test-package");
            pkg_info.url = "https://conda.anaconda.org/conda-forge/noarch/test-package-1.0.0-0.conda";
            pkg_info.channel = "conda-forge";
            pkg_info.subdir = "noarch";
            pkg_info.fn = "test-package-1.0.0-0.conda";
            pkg_info.md5 = "abc123";
            pkg_info.sha256 = "def456";
            // Note: depends, constrains, license, etc. are not set (empty/default values)
            
            // Create a PackageDownloadExtractTarget
            PackageDownloadExtractTarget target(pkg_info);
            
            // Call write_repodata_record
            target.write_repodata_record(temp_dir.path());
            
            // Read the generated repodata_record.json
            fs::path repodata_path = info_dir / "repodata_record.json";
            EXPECT_TRUE(fs::exists(repodata_path));
            
            std::ifstream repodata_file(repodata_path);
            nlohmann::json repodata_json;
            repodata_file >> repodata_json;
            
            // Verify that critical fields from the extracted package are preserved
            EXPECT_EQ(repodata_json["depends"], index_json["depends"]);
            EXPECT_EQ(repodata_json["constrains"], index_json["constrains"]);
            EXPECT_EQ(repodata_json["license"], index_json["license"]);
            EXPECT_EQ(repodata_json["license_family"], index_json["license_family"]);
            EXPECT_EQ(repodata_json["track_features"], index_json["track_features"]);
            
            // Verify that validation fields from URL metadata are preserved
            EXPECT_EQ(repodata_json["md5"], "abc123");
            EXPECT_EQ(repodata_json["sha256"], "def456");
            
            // Verify that other fields are correctly merged
            EXPECT_EQ(repodata_json["url"], pkg_info.url);
            EXPECT_EQ(repodata_json["channel"], pkg_info.channel);
            EXPECT_EQ(repodata_json["subdir"], pkg_info.subdir);
            EXPECT_EQ(repodata_json["fn"], pkg_info.fn);
            
            // Verify that size and timestamp from extracted package are preserved
            EXPECT_EQ(repodata_json["size"], 1024);
            EXPECT_EQ(repodata_json["timestamp"], 1234567890);
        }
        
        TEST_F(RepodataRecordTest, RepodataRecordHandlesMissingFields)
        {
            TemporaryDirectory temp_dir;
            fs::path info_dir = temp_dir.path() / "info";
            fs::create_directories(info_dir);
            
            // Create a minimal index.json with only some fields
            nlohmann::json index_json;
            index_json["name"] = "minimal-package";
            index_json["version"] = "2.0.0";
            // Missing depends, constrains, etc.
            
            std::ofstream index_file(info_dir / "index.json");
            index_file << index_json.dump(4);
            index_file.close();
            
            // Create a PackageInfo with some additional fields
            PackageInfo pkg_info("minimal-package");
            pkg_info.url = "https://conda.anaconda.org/conda-forge/noarch/minimal-package-2.0.0-0.conda";
            pkg_info.channel = "conda-forge";
            pkg_info.md5 = "xyz789";
            
            PackageDownloadExtractTarget target(pkg_info);
            target.write_repodata_record(temp_dir.path());
            
            // Read the generated repodata_record.json
            fs::path repodata_path = info_dir / "repodata_record.json";
            std::ifstream repodata_file(repodata_path);
            nlohmann::json repodata_json;
            repodata_file >> repodata_json;
            
            // Verify that missing fields are handled gracefully
            EXPECT_EQ(repodata_json["name"], "minimal-package");
            EXPECT_EQ(repodata_json["version"], "2.0.0");
            EXPECT_EQ(repodata_json["url"], pkg_info.url);
            EXPECT_EQ(repodata_json["channel"], pkg_info.channel);
            EXPECT_EQ(repodata_json["md5"], "xyz789");
            
            // Fields that don't exist in either source should not be present
            EXPECT_FALSE(repodata_json.contains("depends"));
            EXPECT_FALSE(repodata_json.contains("constrains"));
        }
    }
}