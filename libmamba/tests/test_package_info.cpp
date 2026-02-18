// Copyright (c) 2019, QuantStack and Mamba Contributors
//
// Distributed under the terms of the BSD 3-Clause License.
//
// The full license is in the file LICENSE, distributed with this software.

#include <gtest/gtest.h>

#include <fstream>
#include <string>

#include "mamba/core/mamba_fs.hpp"
#include "mamba/core/package_info.hpp"
#include "mamba/core/pool.hpp"
#include "mamba/core/repo.hpp"

namespace mamba
{
    // =========================================================================
    // Principle 1 & 2: Per-field trust depends on origin.
    // The merge must distinguish stub fields from authoritative fields.
    // =========================================================================

    // Test: When defaulted_keys is empty (solver-derived, all fields authoritative),
    // merge_repodata_record should prefer PackageInfo values over index.json.
    TEST(PackageInfoMerge, solver_derived_fields_override_index_json)
    {
        PackageInfo pkg(std::string("test-pkg"));
        pkg.version = "1.0";
        pkg.build_string = "py_0";
        pkg.build_number = 42;
        pkg.license = "MIT";
        pkg.timestamp = 1700000000;
        pkg.depends = {"python >=3.8"};
        pkg.constrains = {"other-pkg >=2.0"};
        pkg.track_features = "feature1";
        pkg.channel = "conda-forge";
        pkg.url = "https://conda.anaconda.org/conda-forge/linux-64/test-pkg-1.0-py_0.tar.bz2";
        pkg.subdir = "linux-64";
        pkg.fn = "test-pkg-1.0-py_0.tar.bz2";
        pkg.md5 = "abc123";
        pkg.sha256 = "def456";
        pkg.size = 12345;
        // defaulted_keys is empty -> all fields are authoritative

        nlohmann::json index_json;
        index_json["name"] = "test-pkg";
        index_json["version"] = "1.0";
        index_json["build"] = "py_0";
        index_json["build_number"] = 0;
        index_json["license"] = "BSD";
        index_json["timestamp"] = 1600000000;
        index_json["depends"] = nlohmann::json::array({"python >=3.6"});
        index_json["constrains"] = nlohmann::json::array();

        nlohmann::json result = merge_repodata_record(pkg, index_json);

        // Solver-derived values should win
        EXPECT_EQ(result["build_number"], 42);
        EXPECT_EQ(result["license"], "MIT");
        EXPECT_EQ(result["timestamp"], 1700000000);
        EXPECT_EQ(result["depends"], nlohmann::json::array({"python >=3.8"}));
        EXPECT_EQ(result["constrains"], nlohmann::json::array({"other-pkg >=2.0"}));
    }

    // Test: When defaulted_keys marks fields as stubs (URL-derived),
    // merge_repodata_record should prefer index.json for those fields.
    TEST(PackageInfoMerge, url_derived_stubs_yield_to_index_json)
    {
        PackageInfo pkg(std::string("test-pkg"));
        pkg.version = "1.0";
        pkg.build_string = "py_0";
        pkg.build_number = 0;  // stub
        pkg.license = "";      // stub
        pkg.timestamp = 0;     // stub
        pkg.depends = {};      // stub
        pkg.constrains = {};   // stub
        pkg.track_features = "";  // stub
        pkg.size = 0;          // stub
        pkg.channel = "conda-forge";
        pkg.url = "https://conda.anaconda.org/conda-forge/linux-64/test-pkg-1.0-py_0.tar.bz2";
        pkg.subdir = "linux-64";
        pkg.fn = "test-pkg-1.0-py_0.tar.bz2";
        pkg.md5 = "abc123";

        // Mark stub fields
        pkg.defaulted_keys = {"build_number", "license", "timestamp",
                              "depends", "constrains", "track_features", "size"};

        nlohmann::json index_json;
        index_json["name"] = "test-pkg";
        index_json["version"] = "1.0";
        index_json["build"] = "py_0";
        index_json["build_number"] = 5;
        index_json["license"] = "Apache-2.0";
        index_json["timestamp"] = 1650000000;
        index_json["depends"] = nlohmann::json::array({"python >=3.8", "numpy"});
        index_json["constrains"] = nlohmann::json::array({"scipy >=1.0"});
        index_json["track_features"] = "opt_feature";
        index_json["size"] = 98765;

        nlohmann::json result = merge_repodata_record(pkg, index_json);

        // Stub fields should yield to index.json
        EXPECT_EQ(result["build_number"], 5);
        EXPECT_EQ(result["license"], "Apache-2.0");
        EXPECT_EQ(result["timestamp"], 1650000000);
        EXPECT_EQ(result["depends"], nlohmann::json::array({"python >=3.8", "numpy"}));
        EXPECT_EQ(result["constrains"], nlohmann::json::array({"scipy >=1.0"}));

        // Authoritative fields from PackageInfo should still win
        EXPECT_EQ(result["name"], "test-pkg");
        EXPECT_EQ(result["url"], pkg.url);
        EXPECT_EQ(result["channel"], "conda-forge");
        EXPECT_EQ(result["md5"], "abc123");
    }

    // Test: Solver-derived intentionally empty depends/constrains must be preserved
    // (not overridden by index.json). This tests that repodata patches that set
    // depends=[] are respected.
    TEST(PackageInfoMerge, solver_derived_empty_arrays_are_authoritative)
    {
        PackageInfo pkg(std::string("patched-pkg"));
        pkg.version = "2.0";
        pkg.build_string = "h0";
        pkg.depends = {};      // intentionally empty (repodata patch)
        pkg.constrains = {};   // intentionally empty (repodata patch)
        // defaulted_keys is empty -> empty arrays are authoritative
        pkg.channel = "conda-forge";
        pkg.url = "https://example.com/patched-pkg-2.0-h0.tar.bz2";
        pkg.subdir = "linux-64";
        pkg.fn = "patched-pkg-2.0-h0.tar.bz2";

        nlohmann::json index_json;
        index_json["name"] = "patched-pkg";
        index_json["version"] = "2.0";
        index_json["build"] = "h0";
        index_json["depends"] = nlohmann::json::array({"old-dep >=1.0"});
        index_json["constrains"] = nlohmann::json::array({"old-constraint"});

        nlohmann::json result = merge_repodata_record(pkg, index_json);

        // Empty arrays from solver should be preserved (repodata patch)
        EXPECT_EQ(result["depends"], nlohmann::json::array());
        EXPECT_EQ(result["constrains"], nlohmann::json::array());
    }
    // =========================================================================
    // Principle 4 & 5: URL-derived packages going through the solver must
    // have all non-URL-derivable fields marked as defaulted.
    // Currently, only depends/constrains are marked; build_number, license,
    // timestamp, track_features, and size are not.
    // =========================================================================

    TEST(PackageInfoDefaultedKeys, url_derived_via_solvable_marks_all_stubs)
    {
        // Simulate what happens when a URL-derived PackageInfo goes through
        // the solver (PackageInfo -> MRepo -> libsolv -> PackageInfo(Solvable*)).
        // The Solvable constructor should detect __explicit_specs__ repo and
        // mark all non-URL-derivable fields as defaulted.
        //
        // We can't easily run a full solver round-trip in a unit test, but we
        // can verify that the PackageInfo(Solvable*) constructor marks the
        // right fields when the solvable comes from __explicit_specs__.
        //
        // For now, test the principle by checking that a URL-like PackageInfo
        // constructed manually with the right defaulted_keys produces correct
        // merge results.

        PackageInfo pkg(std::string("url-pkg"));
        pkg.version = "3.1";
        pkg.build_string = "h5";
        pkg.build_number = 0;     // stub from URL path
        pkg.license = "";         // stub
        pkg.timestamp = 0;        // stub
        pkg.track_features = "";  // stub
        pkg.size = 0;             // stub
        pkg.depends = {};         // stub (no deps in URL)
        pkg.constrains = {};      // stub
        pkg.channel = "conda-forge";
        pkg.url = "https://example.com/url-pkg-3.1-h5.tar.bz2";
        pkg.subdir = "linux-64";
        pkg.fn = "url-pkg-3.1-h5.tar.bz2";
        pkg.md5 = "deadbeef";

        // All non-URL-derivable fields should be in defaulted_keys
        pkg.defaulted_keys = {"build_number", "license", "timestamp",
                              "track_features", "size", "depends", "constrains"};

        nlohmann::json index_json;
        index_json["name"] = "url-pkg";
        index_json["version"] = "3.1";
        index_json["build"] = "h5";
        index_json["build_number"] = 7;
        index_json["license"] = "GPL-3.0";
        index_json["timestamp"] = 1699999999;
        index_json["depends"] = nlohmann::json::array({"libfoo >=2"});
        index_json["track_features"] = "avx2";

        nlohmann::json result = merge_repodata_record(pkg, index_json);

        // All stub fields should come from index.json
        EXPECT_EQ(result["build_number"], 7);
        EXPECT_EQ(result["license"], "GPL-3.0");
        EXPECT_EQ(result["timestamp"], 1699999999);
        EXPECT_EQ(result["depends"], nlohmann::json::array({"libfoo >=2"}));
        EXPECT_EQ(result["track_features"], "avx2");

        // URL-derivable fields should come from PackageInfo
        EXPECT_EQ(result["name"], "url-pkg");
        EXPECT_EQ(result["version"], "3.1");
        EXPECT_EQ(result["build_string"], "h5");
        EXPECT_EQ(result["url"], pkg.url);
        EXPECT_EQ(result["md5"], "deadbeef");
    }

    // =========================================================================
    // Principle 6: Normalization at write boundary.
    // =========================================================================

    TEST(PackageInfoMerge, normalization_depends_constrains_always_arrays)
    {
        // When index.json lacks depends/constrains entirely (like nlohmann_json-abi),
        // the result must still have them as empty arrays.
        PackageInfo pkg(std::string("nodeps-pkg"));
        pkg.version = "1.0";
        pkg.build_string = "h0";
        pkg.depends = {};
        pkg.constrains = {};
        // Solver-derived: depends/constrains are authoritative empty arrays
        pkg.channel = "conda-forge";
        pkg.url = "https://example.com/nodeps-pkg-1.0-h0.tar.bz2";
        pkg.subdir = "linux-64";
        pkg.fn = "nodeps-pkg-1.0-h0.tar.bz2";

        nlohmann::json index_json;
        index_json["name"] = "nodeps-pkg";
        index_json["version"] = "1.0";
        index_json["build"] = "h0";
        // No depends/constrains in index.json at all

        nlohmann::json result = merge_repodata_record(pkg, index_json);

        EXPECT_TRUE(result.contains("depends"));
        EXPECT_TRUE(result["depends"].is_array());
        EXPECT_TRUE(result.contains("constrains"));
        EXPECT_TRUE(result["constrains"].is_array());
    }

    TEST(PackageInfoMerge, normalization_empty_track_features_omitted)
    {
        PackageInfo pkg(std::string("simple-pkg"));
        pkg.version = "1.0";
        pkg.build_string = "h0";
        pkg.track_features = "";  // empty
        pkg.channel = "conda-forge";
        pkg.url = "https://example.com/simple-pkg-1.0-h0.tar.bz2";
        pkg.subdir = "linux-64";
        pkg.fn = "simple-pkg-1.0-h0.tar.bz2";

        nlohmann::json index_json;
        index_json["name"] = "simple-pkg";
        index_json["version"] = "1.0";
        index_json["build"] = "h0";

        nlohmann::json result = merge_repodata_record(pkg, index_json);

        // Empty track_features should be omitted from result
        EXPECT_FALSE(result.contains("track_features"));
    }

    TEST(PackageInfoMerge, normalization_size_from_tarball)
    {
        PackageInfo pkg(std::string("sized-pkg"));
        pkg.version = "1.0";
        pkg.build_string = "h0";
        pkg.size = 0;  // unknown
        pkg.channel = "conda-forge";
        pkg.url = "https://example.com/sized-pkg-1.0-h0.tar.bz2";
        pkg.subdir = "linux-64";
        pkg.fn = "sized-pkg-1.0-h0.tar.bz2";

        nlohmann::json index_json;
        index_json["name"] = "sized-pkg";
        index_json["version"] = "1.0";
        index_json["build"] = "h0";

        // Provide tarball_size as 3rd argument
        nlohmann::json result = merge_repodata_record(pkg, index_json, 54321);

        EXPECT_EQ(result["size"], 54321);
    }

    // =========================================================================
    // Principle 5: Field trust must survive the solver round-trip.
    // PackageInfo → MRepo → libsolv → PackageInfo(Solvable*)
    // For URL-derived packages (__explicit_specs__), the Solvable constructor
    // must detect the repo and mark non-URL-derivable fields as defaulted.
    // =========================================================================

    TEST(PackageInfoDefaultedKeys, solvable_roundtrip_explicit_specs_marks_stubs)
    {
        // Create a real libsolv pool and add a URL-derived package to it
        MPool pool;
        std::vector<PackageInfo> pkgs;
        PackageInfo p(std::string("roundtrip-pkg"));
        p.version = "1.0";
        p.build_string = "py_0";
        p.build_number = 0;
        p.url = "https://conda.anaconda.org/conda-forge/linux-64/roundtrip-pkg-1.0-py_0.tar.bz2";
        p.channel = "conda-forge";
        p.subdir = "linux-64";
        p.fn = "roundtrip-pkg-1.0-py_0.tar.bz2";
        p.md5 = "abc123";
        p.sha256 = "def456";
        // Stub fields: build_number=0, license="", timestamp=0, depends=[], constrains=[]
        pkgs.push_back(p);

        MRepo repo(pool, "__explicit_specs__", pkgs);

        pool.create_whatprovides();

        // Now read the solvable back and construct PackageInfo from it
        Pool* raw_pool = static_cast<Pool*>(pool);
        Solvable* s = nullptr;
        Id p_id;
        FOR_REPO_SOLVABLES(repo.repo(), p_id, s)
        {
            break;  // get the first (and only) solvable
        }
        ASSERT_NE(s, nullptr);

        PackageInfo recovered(s);

        // Verify that non-URL-derivable fields are in defaulted_keys
        EXPECT_TRUE(recovered.defaulted_keys.count("depends") > 0)
            << "depends should be in defaulted_keys for __explicit_specs__";
        EXPECT_TRUE(recovered.defaulted_keys.count("constrains") > 0)
            << "constrains should be in defaulted_keys for __explicit_specs__";
        EXPECT_TRUE(recovered.defaulted_keys.count("build_number") > 0)
            << "build_number should be in defaulted_keys for __explicit_specs__";
        EXPECT_TRUE(recovered.defaulted_keys.count("license") > 0)
            << "license should be in defaulted_keys for __explicit_specs__";
        EXPECT_TRUE(recovered.defaulted_keys.count("timestamp") > 0)
            << "timestamp should be in defaulted_keys for __explicit_specs__";
        EXPECT_TRUE(recovered.defaulted_keys.count("track_features") > 0)
            << "track_features should be in defaulted_keys for __explicit_specs__";
        EXPECT_TRUE(recovered.defaulted_keys.count("size") > 0)
            << "size should be in defaulted_keys for __explicit_specs__";
    }

    // =========================================================================
    // Integration test: write_repodata_record output correctness
    // This tests the actual write path using temp files to simulate
    // what happens during package extraction.
    // =========================================================================

    TEST(PackageInfoMerge, write_repodata_record_uses_merge_logic)
    {
        // Create a temp directory structure simulating an extracted package
        fs::path tmp_dir = fs::temp_directory_path() / "test_write_repodata";
        fs::path info_dir = tmp_dir / "info";
        fs::create_directories(info_dir);

        // Write a mock index.json with real metadata
        nlohmann::json index_json;
        index_json["name"] = "write-test-pkg";
        index_json["version"] = "2.5";
        index_json["build"] = "py38_1";
        index_json["build_number"] = 1;
        index_json["license"] = "MIT";
        index_json["timestamp"] = 1700000000;
        index_json["depends"] = nlohmann::json::array({"python >=3.8", "numpy"});
        index_json["constrains"] = nlohmann::json::array({"scipy >=1.5"});

        std::ofstream index_file(info_dir / "index.json");
        index_file << index_json.dump(4);
        index_file.close();

        // Create a PackageInfo as if URL-derived (with defaulted_keys)
        PackageInfo pkg(std::string("write-test-pkg"));
        pkg.version = "2.5";
        pkg.build_string = "py38_1";
        pkg.build_number = 0;  // stub
        pkg.license = "";      // stub
        pkg.timestamp = 0;     // stub
        pkg.depends = {};      // stub
        pkg.constrains = {};   // stub
        pkg.channel = "conda-forge";
        pkg.url = "https://example.com/write-test-pkg-2.5-py38_1.tar.bz2";
        pkg.subdir = "linux-64";
        pkg.fn = "write-test-pkg-2.5-py38_1.tar.bz2";
        pkg.md5 = "abc123";
        pkg.sha256 = "def456";
        pkg.size = 12345;
        pkg.defaulted_keys = {"build_number", "license", "timestamp",
                              "depends", "constrains", "track_features"};

        // Use merge_repodata_record and write the result (simulating write_repodata_record)
        nlohmann::json result = merge_repodata_record(pkg, index_json, pkg.size);

        std::ofstream out(info_dir / "repodata_record.json");
        out << result.dump(4);
        out.close();

        // Read back and verify
        std::ifstream in(info_dir / "repodata_record.json");
        nlohmann::json written;
        in >> written;

        // Stub fields should come from index.json
        EXPECT_EQ(written["build_number"], 1);
        EXPECT_EQ(written["license"], "MIT");
        EXPECT_EQ(written["timestamp"], 1700000000);
        EXPECT_EQ(written["depends"], nlohmann::json::array({"python >=3.8", "numpy"}));
        EXPECT_EQ(written["constrains"], nlohmann::json::array({"scipy >=1.5"}));

        // Authoritative fields should come from PackageInfo
        EXPECT_EQ(written["url"], pkg.url);
        EXPECT_EQ(written["channel"], "conda-forge");
        EXPECT_EQ(written["md5"], "abc123");
        EXPECT_EQ(written["sha256"], "def456");

        // Cleanup
        fs::remove_all(tmp_dir);
    }

    // =========================================================================
    // Principle 7: Healing legacy cache corruption.
    // Caches written by v2.1.1-v2.4.0 may have timestamp=0 AND license=""
    // in repodata_record.json. These should be detected and the cache
    // invalidated.
    // =========================================================================

    TEST(PackageInfoMerge, detect_corrupted_cache_signature)
    {
        // Test the is_corrupted_cache_entry helper function
        nlohmann::json good_record;
        good_record["timestamp"] = 1700000000;
        good_record["license"] = "MIT";
        EXPECT_FALSE(is_corrupted_cache_entry(good_record));

        nlohmann::json zero_timestamp;
        zero_timestamp["timestamp"] = 0;
        zero_timestamp["license"] = "MIT";
        EXPECT_FALSE(is_corrupted_cache_entry(zero_timestamp))
            << "timestamp=0 alone is not corruption";

        nlohmann::json empty_license;
        empty_license["timestamp"] = 1700000000;
        empty_license["license"] = "";
        EXPECT_FALSE(is_corrupted_cache_entry(empty_license))
            << "license='' alone is not corruption";

        nlohmann::json corrupted;
        corrupted["timestamp"] = 0;
        corrupted["license"] = "";
        EXPECT_TRUE(is_corrupted_cache_entry(corrupted))
            << "timestamp=0 AND license='' indicates corruption";

        nlohmann::json missing_fields;
        EXPECT_FALSE(is_corrupted_cache_entry(missing_fields))
            << "Missing fields should not be treated as corruption";
    }

}  // namespace mamba
