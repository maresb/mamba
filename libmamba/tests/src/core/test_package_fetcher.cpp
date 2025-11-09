// Copyright (c) 2025, QuantStack and Mamba Contributors
//
// Distributed under the terms of the BSD 3-Clause License.
//
// The full license is in the file LICENSE, distributed with this software.

#include <fstream>

#include <catch2/catch_all.hpp>

#include "mamba/core/context.hpp"
#include "mamba/core/package_fetcher.hpp"
#include "mamba/core/package_handling.hpp"
#include "mamba/core/util.hpp"
#include "mamba/fs/filesystem.hpp"

#include "mambatests.hpp"

namespace
{
    using namespace mamba;

    TEST_CASE("build_download_request")
    {
        auto ctx = Context();
        MultiPackageCache package_caches{ ctx.pkgs_dirs, ctx.validation_params };

        SECTION("From conda-forge")
        {
            static constexpr std::string_view url = "https://conda.anaconda.org/conda-forge/linux-64/pkg-6.4-bld.conda";
            auto pkg_info = specs::PackageInfo::from_url(url).value();

            PackageFetcher pkg_fetcher{ pkg_info, package_caches };
            REQUIRE(pkg_fetcher.name() == pkg_info.name);

            auto req = pkg_fetcher.build_download_request();
            // Should correspond to package name
            REQUIRE(req.name == pkg_info.name);
            // Should correspond to PackageFetcher::channel()
            REQUIRE(req.mirror_name == "");
            // Should correspond to PackageFetcher::url_path()
            REQUIRE(req.url_path == url);
        }

        SECTION("From some mirror")
        {
            static constexpr std::string_view url = "https://repo.prefix.dev/emscripten-forge-dev/emscripten-wasm32/cpp-tabulate-1.5.0-h7223423_2.tar.bz2";
            auto pkg_info = specs::PackageInfo::from_url(url).value();

            PackageFetcher pkg_fetcher{ pkg_info, package_caches };
            REQUIRE(pkg_fetcher.name() == pkg_info.name);

            auto req = pkg_fetcher.build_download_request();
            // Should correspond to package name
            REQUIRE(req.name == pkg_info.name);
            // Should correspond to PackageFetcher::channel()
            REQUIRE(req.mirror_name == "");
            // Should correspond to PackageFetcher::url_path()
            REQUIRE(req.url_path == url);
        }

        SECTION("From local file")
        {
            static constexpr std::string_view url = "file:///home/wolfv/Downloads/xtensor-0.21.4-hc9558a2_0.tar.bz2";
            auto pkg_info = specs::PackageInfo::from_url(url).value();

            PackageFetcher pkg_fetcher{ pkg_info, package_caches };
            REQUIRE(pkg_fetcher.name() == pkg_info.name);

            auto req = pkg_fetcher.build_download_request();
            // Should correspond to package name
            REQUIRE(req.name == pkg_info.name);
            // Should correspond to PackageFetcher::channel()
            REQUIRE(req.mirror_name == "");
            // Should correspond to PackageFetcher::url_path()
            REQUIRE(req.url_path == url);
        }

        SECTION("From oci")
        {
            static constexpr std::string_view url = "oci://ghcr.io/channel-mirrors/conda-forge/linux-64/xtensor-0.25.0-h00ab1b0_0.conda";
            auto pkg_info = specs::PackageInfo::from_url(url).value();

            PackageFetcher pkg_fetcher{ pkg_info, package_caches };
            REQUIRE(pkg_fetcher.name() == pkg_info.name);

            auto req = pkg_fetcher.build_download_request();
            // Should correspond to package name
            REQUIRE(req.name == pkg_info.name);
            // Should correspond to PackageFetcher::channel()
            REQUIRE(req.mirror_name == "oci://ghcr.io/channel-mirrors/conda-forge");
            // Should correspond to PackageFetcher::url_path()
            REQUIRE(req.url_path == "linux-64/xtensor-0.25.0-h00ab1b0_0.conda");
        }
    }

    TEST_CASE("extract_creates_repodata_record_with_dependencies")
    {
        // Test that PackageFetcher.extract() preserves dependencies in repodata_record.json

        auto& ctx = mambatests::context();
        TemporaryDirectory temp_dir;
        MultiPackageCache package_caches{ { temp_dir.path() / "pkgs" }, ctx.validation_params };

        // Create PackageInfo from URL (exhibits the problematic empty dependencies)
        // Using a noarch package to ensure cross-platform compatibility
        static constexpr std::string_view url = "https://conda.anaconda.org/conda-forge/noarch/tzdata-2024a-h0c530f3_0.conda";
        auto pkg_info = specs::PackageInfo::from_url(url).value();

        // Verify precondition: PackageInfo from URL has empty dependencies
        REQUIRE(pkg_info.dependencies.empty());
        REQUIRE(pkg_info.constrains.empty());

        // Extract base filename without extension for reuse
        const std::string pkg_basename = pkg_info.filename.substr(0, pkg_info.filename.size() - 6);

        // Create a minimal but valid conda package structure
        auto pkg_extract_dir = temp_dir.path() / "pkgs" / pkg_basename;
        auto info_dir = pkg_extract_dir / "info";
        fs::create_directories(info_dir);

        // Create index.json with dependencies (what real packages contain)
        nlohmann::json index_json;
        index_json["name"] = pkg_info.name;
        index_json["version"] = pkg_info.version;
        index_json["build"] = pkg_info.build_string;
        index_json["depends"] = nlohmann::json::array({ "python >=3.7" });
        index_json["constrains"] = nlohmann::json::array({ "pytz" });
        index_json["size"] = 123456;

        {
            std::ofstream index_file((info_dir / "index.json").std_path());
            index_file << index_json.dump(2);
        }

        // Create minimal required metadata files for a valid conda package
        {
            std::ofstream paths_file((info_dir / "paths.json").std_path());
            paths_file << R"({"paths": [], "paths_version": 1})";
        }

        // Create a simple tar.bz2 archive that contains our info directory
        // A .conda file is a zip archive, but let's use .tar.bz2 format for simplicity
        auto tarball_path = temp_dir.path() / "pkgs" / (pkg_basename + ".tar.bz2");

        // Use mamba's create_archive function to create a cross-platform tar.bz2 archive
        create_archive(
            pkg_extract_dir,
            tarball_path,
            compression_algorithm::bzip2,
            /* compression_level= */ 1,
            /* compression_threads= */ 1,
            /* filter= */ nullptr
        );
        REQUIRE(fs::exists(tarball_path));

        // Update pkg_info to use .tar.bz2 format instead of .conda
        auto modified_pkg_info = pkg_info;
        modified_pkg_info.filename = pkg_basename + ".tar.bz2";

        // Clean up the extracted directory so PackageFetcher can extract fresh
        fs::remove_all(pkg_extract_dir);

        // Create PackageFetcher with modified package info
        PackageFetcher pkg_fetcher{ modified_pkg_info, package_caches };

        // Set up extract options
        ExtractOptions options;
        options.sparse = false;
        options.subproc_mode = extract_subproc_mode::mamba_package;

        // Call extract - this is the actual method we're testing
        bool extract_success = pkg_fetcher.extract(options);
        REQUIRE(extract_success);

        // Verify that repodata_record.json was created with correct dependencies
        auto repodata_record_path = pkg_extract_dir / "info" / "repodata_record.json";
        REQUIRE(fs::exists(repodata_record_path));

        // Read and parse the created repodata_record.json
        std::ifstream repodata_file(repodata_record_path.std_path());
        nlohmann::json repodata_record;
        repodata_file >> repodata_record;

        REQUIRE(repodata_record.contains("depends"));
        REQUIRE(repodata_record["depends"].is_array());
        REQUIRE(repodata_record["depends"].size() == 1);
        REQUIRE(repodata_record["depends"][0] == "python >=3.7");

        // Also verify constrains is handled correctly
        REQUIRE(repodata_record.contains("constrains"));
        REQUIRE(repodata_record["constrains"].is_array());
        REQUIRE(repodata_record["constrains"].size() == 1);
        REQUIRE(repodata_record["constrains"][0] == "pytz");
    }

    TEST_CASE("url_derived_package_preserves_all_metadata_from_index")
    {
        // Test that PackageFetcher.extract() preserves ALL metadata fields from index.json
        // when the package is derived from a URL (not only depends/constrains but also
        // license, timestamp, build_number, track_features)

        auto& ctx = mambatests::context();
        TemporaryDirectory temp_dir;
        MultiPackageCache package_caches{ { temp_dir.path() / "pkgs" }, ctx.validation_params };

        static constexpr std::string_view url = "https://conda.anaconda.org/conda-forge/linux-64/testpkg-1.0.0-h12345_0.tar.bz2";
        auto pkg_info = specs::PackageInfo::from_url(url).value();

        // Verify preconditions: PackageInfo from URL has stub values
        REQUIRE(pkg_info.license.empty());
        REQUIRE(pkg_info.timestamp == 0);
        REQUIRE(pkg_info.build_number == 0);
        REQUIRE(pkg_info.track_features.empty());

        // Create a minimal but valid conda package structure
        const std::string pkg_basename = "testpkg-1.0.0-h12345_0";
        auto pkg_extract_dir = temp_dir.path() / "pkgs" / pkg_basename;
        auto info_dir = pkg_extract_dir / "info";
        fs::create_directories(info_dir);

        // Create index.json with actual metadata (what real packages contain)
        nlohmann::json index_json;
        index_json["name"] = "testpkg";
        index_json["version"] = "1.0.0";
        index_json["build"] = "h12345_0";
        index_json["build_number"] = 42;
        index_json["license"] = "MIT";
        index_json["timestamp"] = 1234567890;
        index_json["track_features"] = nlohmann::json::array({ "feature1", "feature2" });
        index_json["depends"] = nlohmann::json::array({ "dep1", "dep2" });
        index_json["constrains"] = nlohmann::json::array({ "constraint1" });

        {
            std::ofstream index_file((info_dir / "index.json").std_path());
            index_file << index_json.dump(2);
        }

        {
            std::ofstream paths_file((info_dir / "paths.json").std_path());
            paths_file << R"({"paths": [], "paths_version": 1})";
        }

        // Create tar.bz2 archive
        auto tarball_path = temp_dir.path() / "pkgs" / (pkg_basename + ".tar.bz2");
        create_archive(
            pkg_extract_dir,
            tarball_path,
            compression_algorithm::bzip2,
            /* compression_level= */ 1,
            /* compression_threads= */ 1,
            /* filter= */ nullptr
        );
        REQUIRE(fs::exists(tarball_path));

        // Clean up and extract
        fs::remove_all(pkg_extract_dir);

        PackageFetcher pkg_fetcher{ pkg_info, package_caches };
        ExtractOptions options;
        options.sparse = false;
        options.subproc_mode = extract_subproc_mode::mamba_package;

        bool extract_success = pkg_fetcher.extract(options);
        REQUIRE(extract_success);

        // Verify repodata_record.json has correct metadata from index.json (NOT stub values)
        auto repodata_record_path = pkg_extract_dir / "info" / "repodata_record.json";
        REQUIRE(fs::exists(repodata_record_path));

        std::ifstream repodata_file(repodata_record_path.std_path());
        nlohmann::json repodata_record;
        repodata_file >> repodata_record;

        // These should come from index.json, not from the stub PackageInfo
        REQUIRE(repodata_record["license"] == "MIT");
        REQUIRE(repodata_record["timestamp"] == 1234567890);
        REQUIRE(repodata_record["build_number"] == 42);
        // track_features comes from index.json as an array
        REQUIRE(repodata_record["track_features"].is_array());
        REQUIRE(repodata_record["track_features"].size() == 2);
        REQUIRE(repodata_record["track_features"][0] == "feature1");
        REQUIRE(repodata_record["track_features"][1] == "feature2");
        REQUIRE(repodata_record["depends"].size() == 2);
        REQUIRE(repodata_record["constrains"].size() == 1);
    }

    TEST_CASE("channel_patch_with_empty_arrays_is_preserved")
    {
        // Test that when a package comes from the solver (not a URL) with intentionally
        // empty depends/constrains arrays (e.g., from channel patches), these empty
        // arrays are preserved and not removed

        auto& ctx = mambatests::context();
        TemporaryDirectory temp_dir;
        MultiPackageCache package_caches{ { temp_dir.path() / "pkgs" }, ctx.validation_params };

        // Create PackageInfo that simulates a package from the solver with channel patches
        // Key: defaulted_keys is EMPTY, indicating this is NOT from a URL
        specs::PackageInfo pkg_info;
        pkg_info.name = "patched-pkg";
        pkg_info.version = "2.0.0";
        pkg_info.build_string = "h99999_0";
        pkg_info.filename = "patched-pkg-2.0.0-h99999_0.tar.bz2";
        pkg_info.package_url = "https://conda.anaconda.org/conda-forge/linux-64/patched-pkg-2.0.0-h99999_0.tar.bz2";
        pkg_info.channel = "conda-forge";
        pkg_info.platform = "linux-64";
        pkg_info.license = "BSD-3-Clause";
        pkg_info.timestamp = 9876543210;
        pkg_info.build_number = 5;
        // Intentionally empty arrays from channel patch
        pkg_info.dependencies = {};
        pkg_info.constrains = {};
        // Empty defaulted_keys indicates this is from solver, not URL
        pkg_info.defaulted_keys = {};

        const std::string pkg_basename = "patched-pkg-2.0.0-h99999_0";
        auto pkg_extract_dir = temp_dir.path() / "pkgs" / pkg_basename;
        auto info_dir = pkg_extract_dir / "info";
        fs::create_directories(info_dir);

        // index.json has DIFFERENT values - but repodata_record should use PackageInfo values
        nlohmann::json index_json;
        index_json["name"] = "patched-pkg";
        index_json["version"] = "2.0.0";
        index_json["build"] = "h99999_0";
        index_json["build_number"] = 1;  // Different from pkg_info
        index_json["license"] = "GPL";   // Different from pkg_info
        index_json["timestamp"] = 1111111111;  // Different from pkg_info
        index_json["depends"] = nlohmann::json::array({ "old-dep" });  // Different from pkg_info
        index_json["constrains"] = nlohmann::json::array({ "old-constraint" });  // Different from pkg_info

        {
            std::ofstream index_file((info_dir / "index.json").std_path());
            index_file << index_json.dump(2);
        }

        {
            std::ofstream paths_file((info_dir / "paths.json").std_path());
            paths_file << R"({"paths": [], "paths_version": 1})";
        }

        auto tarball_path = temp_dir.path() / "pkgs" / (pkg_basename + ".tar.bz2");
        create_archive(
            pkg_extract_dir,
            tarball_path,
            compression_algorithm::bzip2,
            /* compression_level= */ 1,
            /* compression_threads= */ 1,
            /* filter= */ nullptr
        );
        REQUIRE(fs::exists(tarball_path));

        fs::remove_all(pkg_extract_dir);

        PackageFetcher pkg_fetcher{ pkg_info, package_caches };
        ExtractOptions options;
        options.sparse = false;
        options.subproc_mode = extract_subproc_mode::mamba_package;

        bool extract_success = pkg_fetcher.extract(options);
        REQUIRE(extract_success);

        auto repodata_record_path = pkg_extract_dir / "info" / "repodata_record.json";
        REQUIRE(fs::exists(repodata_record_path));

        std::ifstream repodata_file(repodata_record_path.std_path());
        nlohmann::json repodata_record;
        repodata_file >> repodata_record;

        // Verify that the channel patch values are preserved (NOT overwritten by index.json)
        REQUIRE(repodata_record["license"] == "BSD-3-Clause");  // From pkg_info, not index.json
        REQUIRE(repodata_record["timestamp"] == 9876543210);    // From pkg_info, not index.json
        REQUIRE(repodata_record["build_number"] == 5);          // From pkg_info, not index.json
        
        // Most importantly: empty arrays should be preserved
        REQUIRE(repodata_record["depends"].is_array());
        REQUIRE(repodata_record["depends"].empty());  // Should be empty, not ["old-dep"]
        REQUIRE(repodata_record["constrains"].is_array());
        REQUIRE(repodata_record["constrains"].empty());  // Should be empty, not ["old-constraint"]
    }

    TEST_CASE("corrupted_cache_is_automatically_healed")
    {
        // Test that packages with corrupted metadata (from buggy mamba versions v2.1.1-v2.3.2)
        // are automatically healed by restoring correct values from index.json

        auto& ctx = mambatests::context();
        TemporaryDirectory temp_dir;
        MultiPackageCache package_caches{ { temp_dir.path() / "pkgs" }, ctx.validation_params };

        // Simulate corrupted cache: timestamp=0 and empty defaulted_keys
        // (what buggy versions produced)
        specs::PackageInfo pkg_info;
        pkg_info.name = "corrupted-pkg";
        pkg_info.version = "1.5.0";
        pkg_info.build_string = "hcorrupt_0";
        pkg_info.filename = "corrupted-pkg-1.5.0-hcorrupt_0.tar.bz2";
        pkg_info.package_url = "https://conda.anaconda.org/conda-forge/linux-64/corrupted-pkg-1.5.0-hcorrupt_0.tar.bz2";
        pkg_info.channel = "conda-forge";
        pkg_info.platform = "linux-64";
        // Corrupted stub values
        pkg_info.license = "";
        pkg_info.timestamp = 0;  // Key indicator of corruption
        pkg_info.build_number = 0;
        pkg_info.track_features = {};
        pkg_info.dependencies = {};
        pkg_info.constrains = {};
        // Empty defaulted_keys (buggy versions didn't populate this)
        pkg_info.defaulted_keys = {};

        const std::string pkg_basename = "corrupted-pkg-1.5.0-hcorrupt_0";
        auto pkg_extract_dir = temp_dir.path() / "pkgs" / pkg_basename;
        auto info_dir = pkg_extract_dir / "info";
        fs::create_directories(info_dir);

        // index.json has correct values
        nlohmann::json index_json;
        index_json["name"] = "corrupted-pkg";
        index_json["version"] = "1.5.0";
        index_json["build"] = "hcorrupt_0";
        index_json["build_number"] = 10;
        index_json["license"] = "Apache-2.0";
        index_json["timestamp"] = 5555555555;
        index_json["track_features"] = nlohmann::json::array({ "cuda" });
        index_json["depends"] = nlohmann::json::array({ "libgcc" });
        index_json["constrains"] = nlohmann::json::array({ "cuda <12" });

        {
            std::ofstream index_file((info_dir / "index.json").std_path());
            index_file << index_json.dump(2);
        }

        {
            std::ofstream paths_file((info_dir / "paths.json").std_path());
            paths_file << R"({"paths": [], "paths_version": 1})";
        }

        auto tarball_path = temp_dir.path() / "pkgs" / (pkg_basename + ".tar.bz2");
        create_archive(
            pkg_extract_dir,
            tarball_path,
            compression_algorithm::bzip2,
            /* compression_level= */ 1,
            /* compression_threads= */ 1,
            /* filter= */ nullptr
        );
        REQUIRE(fs::exists(tarball_path));

        fs::remove_all(pkg_extract_dir);

        PackageFetcher pkg_fetcher{ pkg_info, package_caches };
        ExtractOptions options;
        options.sparse = false;
        options.subproc_mode = extract_subproc_mode::mamba_package;

        bool extract_success = pkg_fetcher.extract(options);
        REQUIRE(extract_success);

        auto repodata_record_path = pkg_extract_dir / "info" / "repodata_record.json";
        REQUIRE(fs::exists(repodata_record_path));

        std::ifstream repodata_file(repodata_record_path.std_path());
        nlohmann::json repodata_record;
        repodata_file >> repodata_record;

        // Verify that the corrupted values were healed from index.json
        REQUIRE(repodata_record["license"] == "Apache-2.0");  // Healed from index.json
        REQUIRE(repodata_record["timestamp"] == 5555555555);  // Healed from index.json
        REQUIRE(repodata_record["build_number"] == 10);       // Healed from index.json
        // track_features comes from index.json as an array
        REQUIRE(repodata_record["track_features"].is_array());
        REQUIRE(repodata_record["track_features"].size() == 1);
        REQUIRE(repodata_record["track_features"][0] == "cuda");
        REQUIRE(repodata_record["depends"].size() == 1);      // Healed from index.json
        REQUIRE(repodata_record["depends"][0] == "libgcc");
        REQUIRE(repodata_record["constrains"].size() == 1);   // Healed from index.json
        REQUIRE(repodata_record["constrains"][0] == "cuda <12");
    }
}
