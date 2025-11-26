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
#include "mamba/validation/tools.hpp"

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

        // Verify that actual values from index.json are used, not stub defaults
        CHECK(repodata_record["license"] == "MIT");
        CHECK(repodata_record["timestamp"] == 1234567890);
        CHECK(repodata_record["build_number"] == 42);
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

        // Verify that the intentionally empty depends from the patch is preserved
        REQUIRE(repodata_record.contains("depends"));
        CHECK(repodata_record["depends"].empty());
    }

    TEST_CASE("PackageFetcher::write_repodata_record prevents new corruption")
    {
        // Test that NEW extractions with buggy PackageInfo (empty defaulted_keys + stubs)
        // correctly replace stub values with index.json via the prevention mechanism.
        // NOTE: This is PREVENTION of future corruption, not healing of existing caches.

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

        // Verify that prevention mechanism detects stub signature and uses index.json
        CHECK(repodata_record["license"] == "Apache-2.0");
        CHECK(repodata_record["timestamp"] == 9999999999);
        CHECK(repodata_record["build_number"] == 99);
    }

    TEST_CASE("PackageFetcher::write_repodata_record git URL metadata")
    {
        // Test that git URL packages use actual metadata from index.json
        // instead of stub defaults (similar to regular URL-derived packages)

        auto& ctx = mambatests::context();
        TemporaryDirectory temp_dir;
        MultiPackageCache package_caches{ { temp_dir.path() / "pkgs" }, ctx.validation_params };

        // Create PackageInfo from git URL - this should have stub default values
        static constexpr std::string_view git_url = "git+https://github.com/org/repo@v1.0#egg=test-git-pkg";
        auto pkg_info = specs::PackageInfo::from_url(git_url).value();

        // Verify precondition: PackageInfo from git URL has stub defaults
        REQUIRE(pkg_info.name == "test-git-pkg");
        REQUIRE(pkg_info.timestamp == 0);
        REQUIRE(pkg_info.license == "");
        REQUIRE(pkg_info.build_number == 0);

        // For git packages, we need to create a fake filename since it's not parsed from the URL
        const std::string pkg_basename = "test-git-pkg-1.0-py_0";
        pkg_info.filename = pkg_basename + ".tar.bz2";

        // Create a minimal but valid package structure
        auto pkg_extract_dir = temp_dir.path() / "pkgs" / pkg_basename;
        auto info_dir = pkg_extract_dir / "info";
        fs::create_directories(info_dir);

        // Create index.json with CORRECT metadata values
        nlohmann::json index_json;
        index_json["name"] = "test-git-pkg";
        index_json["version"] = "1.0";
        index_json["build"] = "py_0";
        index_json["build_number"] = 123;        // Correct value, not 0
        index_json["license"] = "BSD-3-Clause";  // Correct value, not ""
        index_json["timestamp"] = 1700000000;    // Correct value, not 0

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

        // Clean up and re-extract
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

        // Verify that git URL packages use actual values from index.json, not stub defaults
        CHECK(repodata_record["license"] == "BSD-3-Clause");
        CHECK(repodata_record["timestamp"] == 1700000000);
        CHECK(repodata_record["build_number"] == 123);
    }

    TEST_CASE("PackageFetcher heals existing corrupted cache")
    {
        // Test that EXISTING corrupted caches (from v2.1.1-v2.3.3) are detected,
        // invalidated, and automatically re-extracted with correct metadata.
        // This is TRUE HEALING, not just prevention.

        auto& ctx = mambatests::context();
        TemporaryDirectory temp_dir;
        MultiPackageCache package_caches{ { temp_dir.path() / "pkgs" }, ctx.validation_params };

        static constexpr std::string_view url = "https://conda.anaconda.org/conda-forge/linux-64/healing-test-1.0-h123456_0.tar.bz2";
        auto pkg_info = specs::PackageInfo::from_url(url).value();

        const std::string pkg_basename = "healing-test-1.0-h123456_0";

        // Step 1: Create a package with CORRECT index.json
        auto pkg_extract_dir = temp_dir.path() / "pkgs" / pkg_basename;
        auto info_dir = pkg_extract_dir / "info";
        fs::create_directories(info_dir);

        nlohmann::json correct_index;
        correct_index["name"] = "healing-test";
        correct_index["version"] = "1.0";
        correct_index["build"] = "h123456_0";
        correct_index["build_number"] = 42;
        correct_index["license"] = "MIT";
        correct_index["timestamp"] = 1234567890;

        {
            std::ofstream index_file((info_dir / "index.json").std_path());
            index_file << correct_index.dump(2);
        }

        {
            std::ofstream paths_file((info_dir / "paths.json").std_path());
            paths_file << R"({"paths": [], "paths_version": 1})";
        }

        // Step 2: Create tar.bz2 archive WITHOUT repodata_record.json (clean package)
        // This simulates the original package tarball with correct index.json
        auto tarball_path = temp_dir.path() / "pkgs" / (pkg_basename + ".tar.bz2");
        create_archive(pkg_extract_dir, tarball_path, compression_algorithm::bzip2, 1, 1, nullptr);
        REQUIRE(fs::exists(tarball_path));

        // Step 3: Now add CORRUPTED repodata_record.json to cache (simulating v2.1.1-v2.3.3 bug)
        // This creates a mismatch: cache has corrupted metadata, tarball has correct index.json
        nlohmann::json corrupted_repodata;
        corrupted_repodata["name"] = "healing-test";
        corrupted_repodata["version"] = "1.0";
        corrupted_repodata["build"] = "h123456_0";
        corrupted_repodata["timestamp"] = 0;     // CORRUPTED
        corrupted_repodata["license"] = "";      // CORRUPTED
        corrupted_repodata["build_number"] = 0;  // CORRUPTED
        corrupted_repodata["fn"] = pkg_basename + ".tar.bz2";
        corrupted_repodata["url"] = url;
        corrupted_repodata["md5"] = "test_md5";
        corrupted_repodata["sha256"] = "test_sha256";
        corrupted_repodata["size"] = 1000;

        {
            std::ofstream repodata_file((info_dir / "repodata_record.json").std_path());
            repodata_file << corrupted_repodata.dump(2);
        }

        // Step 4: Update pkg_info to use .tar.bz2 format
        auto modified_pkg_info = pkg_info;
        modified_pkg_info.filename = pkg_basename + ".tar.bz2";

        // Step 5: Create PackageFetcher - it detects corruption and triggers re-extraction
        PackageFetcher pkg_fetcher{ modified_pkg_info, package_caches };

        // Verify that corruption was detected and cache was invalidated
        REQUIRE(pkg_fetcher.needs_extract());

        ExtractOptions options;
        options.sparse = false;
        options.subproc_mode = extract_subproc_mode::mamba_package;

        bool extract_success = pkg_fetcher.extract(options);
        REQUIRE(extract_success);

        // Step 6: Verify that repodata_record.json is now HEALED
        auto repodata_record_path = pkg_extract_dir / "info" / "repodata_record.json";
        REQUIRE(fs::exists(repodata_record_path));

        std::ifstream repodata_file(repodata_record_path.std_path());
        nlohmann::json healed_repodata;
        repodata_file >> healed_repodata;

        // Verify corruption was healed - correct values from index.json now present
        CHECK(healed_repodata["license"] == "MIT");
        CHECK(healed_repodata["timestamp"] == 1234567890);
        CHECK(healed_repodata["build_number"] == 42);
    }

    TEST_CASE("PackageFetcher::write_repodata_record depends/constrains always present")
    {
        // Test that depends and constrains are always included in repodata_record.json
        // even when absent from index.json (like nlohmann_json-abi package)

        auto& ctx = mambatests::context();
        TemporaryDirectory temp_dir;
        MultiPackageCache package_caches{ { temp_dir.path() / "pkgs" }, ctx.validation_params };

        static constexpr std::string_view url = "https://conda.anaconda.org/conda-forge/linux-64/empty-deps-1.0-h0_0.conda";
        auto pkg_info = specs::PackageInfo::from_url(url).value();

        const std::string pkg_basename = "empty-deps-1.0-h0_0";

        auto pkg_extract_dir = temp_dir.path() / "pkgs" / pkg_basename;
        auto info_dir = pkg_extract_dir / "info";
        fs::create_directories(info_dir);

        // Create index.json WITHOUT depends or constrains (like nlohmann_json-abi)
        nlohmann::json index_json;
        index_json["name"] = "empty-deps";
        index_json["version"] = "1.0";
        index_json["build"] = "h0_0";

        {
            std::ofstream index_file((info_dir / "index.json").std_path());
            index_file << index_json.dump(2);
        }

        {
            std::ofstream paths_file((info_dir / "paths.json").std_path());
            paths_file << R"({"paths": [], "paths_version": 1})";
        }

        auto tarball_path = temp_dir.path() / "pkgs" / (pkg_basename + ".tar.bz2");
        create_archive(pkg_extract_dir, tarball_path, compression_algorithm::bzip2, 1, 1, nullptr);
        REQUIRE(fs::exists(tarball_path));

        auto modified_pkg_info = pkg_info;
        modified_pkg_info.filename = pkg_basename + ".tar.bz2";

        fs::remove_all(pkg_extract_dir);

        PackageFetcher pkg_fetcher{ modified_pkg_info, package_caches };

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

        // Verify that depends and constrains are present as empty arrays
        REQUIRE(repodata_record.contains("depends"));
        CHECK(repodata_record["depends"].is_array());
        CHECK(repodata_record["depends"].empty());

        REQUIRE(repodata_record.contains("constrains"));
        CHECK(repodata_record["constrains"].is_array());
        CHECK(repodata_record["constrains"].empty());
    }

    TEST_CASE("PackageFetcher::write_repodata_record track_features handling")
    {
        // Test that track_features is:
        // - Omitted when empty
        // - Included when non-empty (like markupsafe and pyyaml packages)

        auto& ctx = mambatests::context();
        TemporaryDirectory temp_dir;
        MultiPackageCache package_caches{ { temp_dir.path() / "pkgs" }, ctx.validation_params };

        SECTION("Empty track_features is omitted")
        {
            static constexpr std::string_view url = "https://conda.anaconda.org/conda-forge/linux-64/no-tf-1.0-h0_0.conda";
            auto pkg_info = specs::PackageInfo::from_url(url).value();

            const std::string pkg_basename = "no-tf-1.0-h0_0";

            auto pkg_extract_dir = temp_dir.path() / "pkgs" / pkg_basename;
            auto info_dir = pkg_extract_dir / "info";
            fs::create_directories(info_dir);

            nlohmann::json index_json;
            index_json["name"] = "no-tf";
            index_json["version"] = "1.0";
            index_json["build"] = "h0_0";
            // No track_features key

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
                1,
                1,
                nullptr
            );
            REQUIRE(fs::exists(tarball_path));

            auto modified_pkg_info = pkg_info;
            modified_pkg_info.filename = pkg_basename + ".tar.bz2";

            fs::remove_all(pkg_extract_dir);

            PackageFetcher pkg_fetcher{ modified_pkg_info, package_caches };

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

            // track_features should be omitted when empty
            CHECK_FALSE(repodata_record.contains("track_features"));
        }

        SECTION("Non-empty track_features is preserved")
        {
            static constexpr std::string_view url = "https://conda.anaconda.org/conda-forge/linux-64/with-tf-1.0-h0_0.conda";
            auto pkg_info = specs::PackageInfo::from_url(url).value();

            const std::string pkg_basename = "with-tf-1.0-h0_0";

            auto pkg_extract_dir = temp_dir.path() / "pkgs" / pkg_basename;
            auto info_dir = pkg_extract_dir / "info";
            fs::create_directories(info_dir);

            nlohmann::json index_json;
            index_json["name"] = "with-tf";
            index_json["version"] = "1.0";
            index_json["build"] = "h0_0";
            index_json["track_features"] = "cython";  // Non-empty track_features

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
                1,
                1,
                nullptr
            );
            REQUIRE(fs::exists(tarball_path));

            auto modified_pkg_info = pkg_info;
            modified_pkg_info.filename = pkg_basename + ".tar.bz2";

            fs::remove_all(pkg_extract_dir);

            PackageFetcher pkg_fetcher{ modified_pkg_info, package_caches };

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

            // track_features should be present when non-empty
            REQUIRE(repodata_record.contains("track_features"));
            CHECK(repodata_record["track_features"] == "cython");
        }
    }

    TEST_CASE("PackageFetcher::write_repodata_record null arch/platform handling")
    {
        // Test that arch and platform are omitted when null

        auto& ctx = mambatests::context();
        TemporaryDirectory temp_dir;
        MultiPackageCache package_caches{ { temp_dir.path() / "pkgs" }, ctx.validation_params };

        static constexpr std::string_view url = "https://conda.anaconda.org/conda-forge/noarch/noarch-pkg-1.0-py_0.conda";
        auto pkg_info = specs::PackageInfo::from_url(url).value();

        const std::string pkg_basename = "noarch-pkg-1.0-py_0";

        auto pkg_extract_dir = temp_dir.path() / "pkgs" / pkg_basename;
        auto info_dir = pkg_extract_dir / "info";
        fs::create_directories(info_dir);

        // Create index.json with null arch and platform (common for noarch packages)
        nlohmann::json index_json;
        index_json["name"] = "noarch-pkg";
        index_json["version"] = "1.0";
        index_json["build"] = "py_0";
        index_json["arch"] = nullptr;      // null
        index_json["platform"] = nullptr;  // null

        {
            std::ofstream index_file((info_dir / "index.json").std_path());
            index_file << index_json.dump(2);
        }

        {
            std::ofstream paths_file((info_dir / "paths.json").std_path());
            paths_file << R"({"paths": [], "paths_version": 1})";
        }

        auto tarball_path = temp_dir.path() / "pkgs" / (pkg_basename + ".tar.bz2");
        create_archive(pkg_extract_dir, tarball_path, compression_algorithm::bzip2, 1, 1, nullptr);
        REQUIRE(fs::exists(tarball_path));

        auto modified_pkg_info = pkg_info;
        modified_pkg_info.filename = pkg_basename + ".tar.bz2";

        fs::remove_all(pkg_extract_dir);

        PackageFetcher pkg_fetcher{ modified_pkg_info, package_caches };

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

        // Verify that null arch and platform are omitted
        CHECK_FALSE(repodata_record.contains("arch"));
        CHECK_FALSE(repodata_record.contains("platform"));
    }

    TEST_CASE("PackageFetcher::write_repodata_record always includes both checksums")
    {
        // Test that both md5 and sha256 are always written to repodata_record.json
        // even when only one is provided (e.g., explicit lockfile with md5 only)

        auto& ctx = mambatests::context();
        TemporaryDirectory temp_dir;
        MultiPackageCache package_caches{ { temp_dir.path() / "pkgs" }, ctx.validation_params };

        const std::string pkg_basename = "checksum-test-1.0-h0_0";

        // First create the package structure and archive
        auto pkg_extract_dir = temp_dir.path() / "pkgs" / pkg_basename;
        auto info_dir = pkg_extract_dir / "info";
        fs::create_directories(info_dir);

        nlohmann::json index_json;
        index_json["name"] = "checksum-test";
        index_json["version"] = "1.0";
        index_json["build"] = "h0_0";

        {
            std::ofstream index_file((info_dir / "index.json").std_path());
            index_file << index_json.dump(2);
        }

        {
            std::ofstream paths_file((info_dir / "paths.json").std_path());
            paths_file << R"({"paths": [], "paths_version": 1})";
        }

        auto tarball_path = temp_dir.path() / "pkgs" / (pkg_basename + ".tar.bz2");
        create_archive(pkg_extract_dir, tarball_path, compression_algorithm::bzip2, 1, 1, nullptr);
        REQUIRE(fs::exists(tarball_path));

        // Now create PackageInfo with only md5 set (simulating explicit lockfile)
        // We compute the actual md5 of the archive we just created
        specs::PackageInfo pkg_info;
        pkg_info.name = "checksum-test";
        pkg_info.version = "1.0";
        pkg_info.build_string = "h0_0";
        pkg_info.filename = pkg_basename + ".tar.bz2";
        pkg_info.md5 = validation::md5sum(tarball_path);  // Only md5, no sha256
        pkg_info.sha256 = "";                              // Explicitly empty

        // Verify precondition: only md5 is set
        REQUIRE_FALSE(pkg_info.md5.empty());
        REQUIRE(pkg_info.sha256.empty());

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

        // Verify that BOTH checksums are present
        REQUIRE(repodata_record.contains("md5"));
        CHECK_FALSE(repodata_record["md5"].get<std::string>().empty());

        REQUIRE(repodata_record.contains("sha256"));
        CHECK_FALSE(repodata_record["sha256"].get<std::string>().empty());
    }
}
