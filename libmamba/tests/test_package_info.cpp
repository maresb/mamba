// Copyright (c) 2019, QuantStack and Mamba Contributors
//
// Distributed under the terms of the BSD 3-Clause License.
//
// The full license is in the file LICENSE, distributed with this software.

#include <gtest/gtest.h>

#include <string>

#include "mamba/core/package_info.hpp"

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
}  // namespace mamba
