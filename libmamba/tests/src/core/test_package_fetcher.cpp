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

    TEST_CASE("PackageFetcher::write_repodata_record URL-derived metadata")
    {
        // Test that URL-derived packages use actual metadata from index.json
        // instead of stub defaults (timestamp=0, license="", build_number=0)

        auto& ctx = mambatests::context();
        TemporaryDirectory temp_dir;
        MultiPackageCache package_caches{ { temp_dir.path() / "pkgs" }, ctx.validation_params };

        // Create PackageInfo from URL - this will have stub default values
        static constexpr std::string_view url = "https://conda.anaconda.org/conda-forge/linux-64/test-pkg-1.0-h123456_0.conda";
        auto pkg_info = specs::PackageInfo::from_url(url).value();

        // Verify precondition: PackageInfo from URL has stub defaults
        REQUIRE(pkg_info.timestamp == 0);
        REQUIRE(pkg_info.license == "");
        REQUIRE(pkg_info.build_number == 0);

        const std::string pkg_basename = "test-pkg-1.0-h123456_0";

        // Create a minimal but valid conda package structure
        auto pkg_extract_dir = temp_dir.path() / "pkgs" / pkg_basename;
        auto info_dir = pkg_extract_dir / "info";
        fs::create_directories(info_dir);

        // Create index.json with CORRECT metadata values
        nlohmann::json index_json;
        index_json["name"] = "test-pkg";
        index_json["version"] = "1.0";
        index_json["build"] = "h123456_0";
        index_json["build_number"] = 42;       // Correct value, not 0
        index_json["license"] = "MIT";         // Correct value, not ""
        index_json["timestamp"] = 1234567890;  // Correct value, not 0

        {
            std::ofstream index_file((info_dir / "index.json").std_path());
            index_file << index_json.dump(2);
        }

        // Create minimal required metadata files
        {
            std::ofstream paths_file((info_dir / "paths.json").std_path());
            paths_file << R"({"paths": [], "paths_version": 1})";
        }

        // Create tar.bz2 archive
        auto tarball_path = temp_dir.path() / "pkgs" / (pkg_basename + ".tar.bz2");
        create_archive(pkg_extract_dir, tarball_path, compression_algorithm::bzip2, 1, 1, nullptr);
        REQUIRE(fs::exists(tarball_path));

        // Update pkg_info to use .tar.bz2 format
        auto modified_pkg_info = pkg_info;
        modified_pkg_info.filename = pkg_basename + ".tar.bz2";

        // Clean up and re-extract
        fs::remove_all(pkg_extract_dir);

        PackageFetcher pkg_fetcher{ modified_pkg_info, package_caches };

        ExtractOptions options;
        options.sparse = false;
        options.subproc_mode = extract_subproc_mode::mamba_package;

        bool extract_success = pkg_fetcher.extract(options);
        REQUIRE(extract_success);

        // Read repodata_record.json
        auto repodata_record_path = pkg_extract_dir / "info" / "repodata_record.json";
        REQUIRE(fs::exists(repodata_record_path));

        std::ifstream repodata_file(repodata_record_path.std_path());
        nlohmann::json repodata_record;
        repodata_file >> repodata_record;

        // BUG: These assertions will FAIL with current implementation
        // because stub defaults from URL parsing are written instead of
        // actual values from index.json
        CHECK(repodata_record["license"] == "MIT");         // FAILS: gets ""
        CHECK(repodata_record["timestamp"] == 1234567890);  // FAILS: gets 0
        CHECK(repodata_record["build_number"] == 42);       // FAILS: gets 0
    }

    TEST_CASE("PackageFetcher::write_repodata_record preserves empty depends patch")
    {
        // Test that channel patches with intentionally empty dependencies
        // are preserved and not overwritten by index.json

        auto& ctx = mambatests::context();
        TemporaryDirectory temp_dir;
        MultiPackageCache package_caches{ { temp_dir.path() / "pkgs" }, ctx.validation_params };

        // Create PackageInfo with empty depends (simulating patched repodata)
        // and EMPTY defaulted_keys (indicating this is NOT from URL parsing)
        specs::PackageInfo pkg_info;
        pkg_info.name = "patched-pkg";
        pkg_info.version = "1.0";
        pkg_info.build_string = "h123456_0";
        pkg_info.filename = "patched-pkg-1.0-h123456_0.tar.bz2";
        pkg_info.dependencies = {};       // Intentionally empty from repodata patch
        pkg_info.defaulted_keys = {};     // Empty = NOT from URL parsing
        pkg_info.timestamp = 1234567890;  // Non-zero timestamp from repodata

        const std::string pkg_basename = "patched-pkg-1.0-h123456_0";

        // Create package structure
        auto pkg_extract_dir = temp_dir.path() / "pkgs" / pkg_basename;
        auto info_dir = pkg_extract_dir / "info";
        fs::create_directories(info_dir);

        // Create index.json with broken dependency
        // (This represents the package's original, buggy metadata)
        nlohmann::json index_json;
        index_json["name"] = "patched-pkg";
        index_json["version"] = "1.0";
        index_json["build"] = "h123456_0";
        index_json["depends"] = nlohmann::json::array({ "broken-dependency" });

        {
            std::ofstream index_file((info_dir / "index.json").std_path());
            index_file << index_json.dump(2);
        }

        {
            std::ofstream paths_file((info_dir / "paths.json").std_path());
            paths_file << R"({"paths": [], "paths_version": 1})";
        }

        // Create archive
        auto tarball_path = temp_dir.path() / "pkgs" / (pkg_basename + ".tar.bz2");
        create_archive(pkg_extract_dir, tarball_path, compression_algorithm::bzip2, 1, 1, nullptr);
        REQUIRE(fs::exists(tarball_path));

        fs::remove_all(pkg_extract_dir);

        PackageFetcher pkg_fetcher{ pkg_info, package_caches };

        ExtractOptions options;
        options.sparse = false;
        options.subproc_mode = extract_subproc_mode::mamba_package;

        bool extract_success = pkg_fetcher.extract(options);
        REQUIRE(extract_success);

        // Read repodata_record.json
        auto repodata_record_path = pkg_extract_dir / "info" / "repodata_record.json";
        REQUIRE(fs::exists(repodata_record_path));

        std::ifstream repodata_file(repodata_record_path.std_path());
        nlohmann::json repodata_record;
        repodata_file >> repodata_record;

        // BUG: This assertion will FAIL with current implementation
        // The empty depends from the patch is erased, then index.json's
        // broken dependency is inserted
        REQUIRE(repodata_record.contains("depends"));
        CHECK(repodata_record["depends"].empty());  // FAILS: gets ["broken-dependency"]
    }

    TEST_CASE("PackageFetcher::write_repodata_record heals corrupted cache")
    {
        // Test that corrupted cache entries (from buggy versions v2.1.1-v2.3.2)
        // are automatically healed by replacing stub values with index.json

        auto& ctx = mambatests::context();
        TemporaryDirectory temp_dir;
        MultiPackageCache package_caches{ { temp_dir.path() / "pkgs" }, ctx.validation_params };

        // Create PackageInfo with corrupted stub values
        // and EMPTY defaulted_keys (simulating packages cached by buggy versions)
        specs::PackageInfo pkg_info;
        pkg_info.name = "corrupted-pkg";
        pkg_info.version = "1.0";
        pkg_info.build_string = "h123456_0";
        pkg_info.filename = "corrupted-pkg-1.0-h123456_0.tar.bz2";
        pkg_info.timestamp = 0;        // Corrupted
        pkg_info.license = "";         // Corrupted
        pkg_info.build_number = 0;     // Corrupted
        pkg_info.defaulted_keys = {};  // Empty = looks like it's not from URL

        const std::string pkg_basename = "corrupted-pkg-1.0-h123456_0";

        // Create package structure
        auto pkg_extract_dir = temp_dir.path() / "pkgs" / pkg_basename;
        auto info_dir = pkg_extract_dir / "info";
        fs::create_directories(info_dir);

        // Create index.json with CORRECT values
        nlohmann::json index_json;
        index_json["name"] = "corrupted-pkg";
        index_json["version"] = "1.0";
        index_json["build"] = "h123456_0";
        index_json["build_number"] = 99;
        index_json["license"] = "Apache-2.0";
        index_json["timestamp"] = 9999999999;

        {
            std::ofstream index_file((info_dir / "index.json").std_path());
            index_file << index_json.dump(2);
        }

        {
            std::ofstream paths_file((info_dir / "paths.json").std_path());
            paths_file << R"({"paths": [], "paths_version": 1})";
        }

        // Create archive
        auto tarball_path = temp_dir.path() / "pkgs" / (pkg_basename + ".tar.bz2");
        create_archive(pkg_extract_dir, tarball_path, compression_algorithm::bzip2, 1, 1, nullptr);
        REQUIRE(fs::exists(tarball_path));

        fs::remove_all(pkg_extract_dir);

        PackageFetcher pkg_fetcher{ pkg_info, package_caches };

        ExtractOptions options;
        options.sparse = false;
        options.subproc_mode = extract_subproc_mode::mamba_package;

        bool extract_success = pkg_fetcher.extract(options);
        REQUIRE(extract_success);

        // Read repodata_record.json
        auto repodata_record_path = pkg_extract_dir / "info" / "repodata_record.json";
        REQUIRE(fs::exists(repodata_record_path));

        std::ifstream repodata_file(repodata_record_path.std_path());
        nlohmann::json repodata_record;
        repodata_file >> repodata_record;

        // BUG: These assertions will FAIL with current implementation
        // Corrupted values remain because defaulted_keys is empty
        CHECK(repodata_record["license"] == "Apache-2.0");  // FAILS: gets ""
        CHECK(repodata_record["timestamp"] == 9999999999);  // FAILS: gets 0
        CHECK(repodata_record["build_number"] == 99);       // FAILS: gets 0
    }
}
