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

    // Helper to create a test package, extract it, and return the repodata_record.json
    auto create_and_extract_package(
        specs::PackageInfo& pkg_info,
        const nlohmann::json& index_json_content
    ) -> nlohmann::json
    {
        auto& ctx = mambatests::context();
        TemporaryDirectory temp_dir;
        MultiPackageCache package_caches{ { temp_dir.path() / "pkgs" }, ctx.validation_params };

        const std::string pkg_basename = pkg_info.filename.substr(0, pkg_info.filename.size() - 8);

        auto pkg_extract_dir = temp_dir.path() / "pkgs" / pkg_basename;
        auto info_dir = pkg_extract_dir / "info";
        fs::create_directories(info_dir);

        {
            std::ofstream index_file((info_dir / "index.json").std_path());
            index_file << index_json_content.dump(2);
        }
        {
            std::ofstream paths_file((info_dir / "paths.json").std_path());
            paths_file << R"({"paths": [], "paths_version": 1})";
        }

        auto tarball_path = temp_dir.path() / "pkgs" / pkg_info.filename;
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
        return repodata_record;
    }

    TEST_CASE("solver_derived_empty_depends_preserved")
    {
        // Solver-derived PackageInfo with intentionally empty depends/constrains
        // (from repodata patches) must preserve the empty arrays.
        auto pkg_info = specs::PackageInfo();
        pkg_info.name = "nlohmann_json-abi";
        pkg_info.version = "3.11.3";
        pkg_info.build_string = "h01db608_1";
        pkg_info.build_number = 1;
        pkg_info.channel = "https://conda.anaconda.org/conda-forge";
        pkg_info.package_url = "https://conda.anaconda.org/conda-forge/linux-64/nlohmann_json-abi-3.11.3-h01db608_1.tar.bz2";
        pkg_info.platform = "linux-64";
        pkg_info.filename = "nlohmann_json-abi-3.11.3-h01db608_1.tar.bz2";
        pkg_info.license = "MIT";
        pkg_info.timestamp = 1234567890;
        // No defaulted_keys — this is solver-derived (all fields authoritative)

        nlohmann::json index_json;
        index_json["name"] = "nlohmann_json-abi";
        index_json["version"] = "3.11.3";
        index_json["build"] = "h01db608_1";
        index_json["build_number"] = 1;
        index_json["license"] = "MIT";
        index_json["timestamp"] = 1234567890;
        // index.json might have depends or not; the solver says they're empty

        auto repodata_record = create_and_extract_package(pkg_info, index_json);

        // Empty depends/constrains from solver must be preserved as empty arrays
        REQUIRE(repodata_record.contains("depends"));
        REQUIRE(repodata_record["depends"].is_array());
        REQUIRE(repodata_record["depends"].empty());

        REQUIRE(repodata_record.contains("constrains"));
        REQUIRE(repodata_record["constrains"].is_array());
        REQUIRE(repodata_record["constrains"].empty());
    }

    TEST_CASE("write_repodata_record_normalization")
    {
        // Test normalization invariants from Principle 6

        auto pkg_info = specs::PackageInfo();
        pkg_info.name = "normpkg";
        pkg_info.version = "1.0";
        pkg_info.build_string = "h0_0";
        pkg_info.channel = "https://conda.anaconda.org/conda-forge";
        pkg_info.package_url = "https://conda.anaconda.org/conda-forge/linux-64/normpkg-1.0-h0_0.tar.bz2";
        pkg_info.platform = "linux-64";
        pkg_info.filename = "normpkg-1.0-h0_0.tar.bz2";
        pkg_info.license = "MIT";
        pkg_info.timestamp = 1234567890;

        nlohmann::json index_json;
        index_json["name"] = "normpkg";
        index_json["version"] = "1.0";
        index_json["build"] = "h0_0";
        index_json["build_number"] = 0;
        index_json["license"] = "MIT";

        SECTION("depends and constrains always present as arrays")
        {
            // Index.json has no depends or constrains keys at all
            auto repodata_record = create_and_extract_package(pkg_info, index_json);

            REQUIRE(repodata_record.contains("depends"));
            REQUIRE(repodata_record["depends"].is_array());
            REQUIRE(repodata_record.contains("constrains"));
            REQUIRE(repodata_record["constrains"].is_array());
        }

        SECTION("empty track_features omitted")
        {
            auto repodata_record = create_and_extract_package(pkg_info, index_json);

            // Empty track_features string should be omitted
            bool has_empty_track = repodata_record.contains("track_features")
                                   && repodata_record["track_features"].is_string()
                                   && repodata_record["track_features"].get<std::string>().empty();
            REQUIRE_FALSE(has_empty_track);
        }

        SECTION("size populated from tarball when zero")
        {
            auto repodata_record = create_and_extract_package(pkg_info, index_json);

            REQUIRE(repodata_record.contains("size"));
            REQUIRE(repodata_record["size"].get<std::size_t>() > 0);
        }
    }

    TEST_CASE("legacy_corrupted_cache_invalidated")
    {
        // Principle 7: caches from v2.1.1-v2.4.0 may have corrupted metadata
        // (timestamp=0, license=""). These should be invalidated.

        auto& ctx = mambatests::context();
        TemporaryDirectory temp_dir;
        fs::u8path pkgs_dir = temp_dir.path() / "pkgs";
        fs::create_directories(pkgs_dir);

        auto pkg_info = specs::PackageInfo();
        pkg_info.name = "corruptpkg";
        pkg_info.version = "1.0";
        pkg_info.build_string = "h0_0";
        pkg_info.filename = "corruptpkg-1.0-h0_0.tar.bz2";
        pkg_info.sha256 = "abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890";
        pkg_info.channel = "https://conda.anaconda.org/conda-forge";
        pkg_info.package_url = "https://conda.anaconda.org/conda-forge/linux-64/corruptpkg-1.0-h0_0.tar.bz2";

        auto make_record = [&](std::size_t timestamp, const std::string& license) -> nlohmann::json
        {
            nlohmann::json record;
            record["name"] = "corruptpkg";
            record["version"] = "1.0";
            record["build"] = "h0_0";
            record["build_number"] = 0;
            record["timestamp"] = timestamp;
            record["license"] = license;
            record["sha256"] = pkg_info.sha256;
            record["url"] = pkg_info.package_url;
            record["channel"] = pkg_info.channel;
            record["subdir"] = "linux-64";
            record["fn"] = pkg_info.filename;
            record["depends"] = nlohmann::json::array();
            record["constrains"] = nlohmann::json::array();
            record["size"] = 0;
            return record;
        };

        auto write_record_and_check = [&](const nlohmann::json& record) -> bool
        {
            auto extract_dir = pkgs_dir / "corruptpkg-1.0-h0_0";
            auto info_dir = extract_dir / "info";
            fs::create_directories(info_dir);

            {
                std::ofstream f((info_dir / "repodata_record.json").std_path());
                f << record.dump(2);
            }
            {
                std::ofstream paths_file((info_dir / "paths.json").std_path());
                paths_file << R"({"paths": [], "paths_version": 1})";
            }

            PackageCacheData cache(pkgs_dir);
            return cache.has_valid_extracted_dir(pkg_info, ctx.validation_params);
        };

        SECTION("corrupted: timestamp=0 AND license=empty → invalid")
        {
            auto record = make_record(0, "");
            REQUIRE_FALSE(write_record_and_check(record));
        }

        SECTION("not corrupted: timestamp=0 AND license=MIT → valid")
        {
            auto record = make_record(0, "MIT");
            REQUIRE(write_record_and_check(record));
        }

        SECTION("not corrupted: timestamp=1234 AND license=empty → valid")
        {
            auto record = make_record(1234567890, "");
            REQUIRE(write_record_and_check(record));
        }

        SECTION("not corrupted: timestamp=1234 AND license=MIT → valid")
        {
            auto record = make_record(1234567890, "MIT");
            REQUIRE(write_record_and_check(record));
        }
    }

    TEST_CASE("url_derived_stub_fields_yield_to_index_json")
    {
        // URL-derived PackageInfo has stub values for license, timestamp,
        // build_number. These stubs must yield to the correct values from index.json.
        static constexpr std::string_view url = "https://conda.anaconda.org/conda-forge/linux-64/somepkg-1.0-hbuild_0.tar.bz2";
        auto pkg_info = specs::PackageInfo::from_url(url).value();
        pkg_info.filename = "somepkg-1.0-hbuild_0.tar.bz2";

        // Verify the stubs
        REQUIRE(pkg_info.build_number == 0);
        REQUIRE(pkg_info.license.empty());
        REQUIRE(pkg_info.timestamp == 0);

        nlohmann::json index_json;
        index_json["name"] = "somepkg";
        index_json["version"] = "1.0";
        index_json["build"] = "hbuild_0";
        index_json["build_number"] = 7;
        index_json["license"] = "Apache-2.0";
        index_json["timestamp"] = 1700000000;
        index_json["depends"] = nlohmann::json::array({ "python >=3.8" });
        index_json["constrains"] = nlohmann::json::array({ "numpy >=1.20" });

        auto repodata_record = create_and_extract_package(pkg_info, index_json);

        // Stub fields must be overridden by index.json values
        REQUIRE(repodata_record["build_number"] == 7);
        REQUIRE(repodata_record["license"] == "Apache-2.0");
        REQUIRE(repodata_record["timestamp"] == 1700000000);
        REQUIRE(repodata_record["depends"] == nlohmann::json::array({ "python >=3.8" }));
        REQUIRE(repodata_record["constrains"] == nlohmann::json::array({ "numpy >=1.20" }));
    }
}
