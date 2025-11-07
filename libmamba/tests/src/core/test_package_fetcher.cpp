// Copyright (c) 2025, QuantStack and Mamba Contributors
//
// Distributed under the terms of the BSD 3-Clause License.
//
// The full license is in the file LICENSE, distributed with this software.

#include <catch2/catch_all.hpp>

#include "mamba/core/context.hpp"
#include "mamba/core/package_fetcher.hpp"

namespace
{
    using namespace mamba;

    namespace
    {
        fs::u8path write_file(const fs::u8path& path, std::string_view content)
        {
            fs::create_directories(path.parent_path());
            std::ofstream os = open_ofstream(path);
            os.write(content.data(), static_cast<std::streamsize>(content.size()));
            return path;
        }

        nlohmann::json read_json(const fs::u8path& path)
        {
            nlohmann::json j;
            std::ifstream is = open_ifstream(path);
            is >> j;
            return j;
        }

        // Build a PackageFetcher wired to a temporary cache rooted at tmp_dir
        PackageFetcher make_fetcher_with_cache(const specs::PackageInfo& info, const fs::u8path& tmp_dir)
        {
            auto ctx = Context();
            ctx.pkgs_dirs = { tmp_dir.string() };
            MultiPackageCache package_caches{ ctx.pkgs_dirs, ctx.validation_params };
            return PackageFetcher{ info, package_caches };
        }
    }

    TEST_CASE("PackageFetcher::write_repodata_record precedence and healing")
    {
        const auto tmp_dir = TemporaryDirectory();
        const fs::u8path base = fs::u8path(tmp_dir.path()) / "extracted";
        const fs::u8path info_dir = base / "info";
        fs::create_directories(info_dir);

        SECTION("URL-derived stubs do not override index.json fields")
        {
            // URL-derived conda package (defaulted_keys populated)
            const std::string url = "https://conda.anaconda.org/conda-forge/linux-64/pkg-1.0-abc.conda";
            auto pkg = specs::PackageInfo::from_url(url).value();

            // Create a dummy tarball to backfill size
            const fs::u8path tarball = fs::u8path(tmp_dir.path()) / pkg.filename;
            write_file(tarball, std::string(123, 'x'));

            // Index with authoritative metadata
            nlohmann::json index = {
                { "license", "BSD-3-Clause" },
                { "timestamp", 123456u },
                { "build_number", 7u },
                { "depends", nlohmann::json::array({ "python >=3.11" }) },
                { "constrains", nlohmann::json::array({ "pip >=24.0" }) },
                { "track_features", "mkl" },
            };
            write_file(info_dir / "index.json", index.dump());

            auto fetcher = make_fetcher_with_cache(pkg, tmp_dir.path());
            fetcher.write_repodata_record(base);

            const auto j = read_json(info_dir / "repodata_record.json");
            REQUIRE(j.at("license") == "BSD-3-Clause");
            REQUIRE(j.at("timestamp") == 123456);
            REQUIRE(j.at("build_number") == 7);
            REQUIRE(j.at("depends").is_array());
            REQUIRE(j.at("depends").size() == 1);
            REQUIRE(j.at("constrains").is_array());
            REQUIRE(j.at("constrains").size() == 1);
            REQUIRE(j.at("track_features") == "mkl");
        }

        SECTION("Preserve channel patch empty depends/constrains (solver-derived)")
        {
            // Emulate solver-derived PackageInfo with empty depends/constrains (patch)
            specs::PackageInfo pkg;
            pkg.name = "pkg";
            pkg.version = "1.0";
            pkg.build_string = "abc";
            pkg.channel = "https://conda.anaconda.org/conda-forge";
            pkg.platform = "linux-64";
            pkg.filename = "pkg-1.0-abc.conda";
            pkg.package_url = pkg.channel + "/" + pkg.platform + "/" + pkg.filename;
            pkg.timestamp = 1717; // non-zero to avoid healing
            pkg.dependencies = {}; // patch: empty
            pkg.constrains = {};   // patch: empty

            // Create tarball
            const fs::u8path tarball = fs::u8path(tmp_dir.path()) / pkg.filename;
            write_file(tarball, std::string(10, 'y'));

            // Index that would otherwise provide depends/constrains, but must be ignored
            nlohmann::json index = {
                { "depends", nlohmann::json::array({ "should_be_ignored" }) },
                { "constrains", nlohmann::json::array({ "should_be_ignored" }) },
            };
            write_file(info_dir / "index.json", index.dump());

            auto fetcher = make_fetcher_with_cache(pkg, tmp_dir.path());
            fetcher.write_repodata_record(base);

            const auto j = read_json(info_dir / "repodata_record.json");
            REQUIRE(j.at("depends").is_array());
            REQUIRE(j.at("depends").empty());
            REQUIRE(j.at("constrains").is_array());
            REQUIRE(j.at("constrains").empty());
        }

        SECTION("Backfill noarch/python_site_packages_path from index for URL-derived")
        {
            const std::string url = "https://conda.anaconda.org/conda-forge/linux-64/pkg-1.0-abc.conda";
            auto pkg = specs::PackageInfo::from_url(url).value();
            write_file(fs::u8path(tmp_dir.path()) / pkg.filename, std::string(5, 'z'));

            nlohmann::json index = {
                { "noarch", "python" },
                { "python_site_packages_path", "lib/python3.11/site-packages" },
            };
            write_file(info_dir / "index.json", index.dump());

            auto fetcher = make_fetcher_with_cache(pkg, tmp_dir.path());
            fetcher.write_repodata_record(base);

            const auto j = read_json(info_dir / "repodata_record.json");
            REQUIRE(j.at("noarch") == "python");
            REQUIRE(j.at("python_site_packages_path") == "lib/python3.11/site-packages");
        }

        SECTION("Size is set to tarball file size when 0 or missing")
        {
            const std::string url = "https://conda.anaconda.org/conda-forge/linux-64/pkg-1.0-abc.conda";
            auto pkg = specs::PackageInfo::from_url(url).value();
            const auto tarball = fs::u8path(tmp_dir.path()) / pkg.filename;
            const std::size_t sz = 77;
            write_file(tarball, std::string(sz, 's'));

            // First: size missing
            write_file(info_dir / "index.json", nlohmann::json({}).dump());
            auto fetcher1 = make_fetcher_with_cache(pkg, tmp_dir.path());
            fetcher1.write_repodata_record(base);
            auto j1 = read_json(info_dir / "repodata_record.json");
            REQUIRE(j1.at("size") == sz);

            // Second: size == 0
            nlohmann::json index = { { "size", 0 } };
            write_file(info_dir / "index.json", index.dump());
            auto fetcher2 = make_fetcher_with_cache(pkg, tmp_dir.path());
            fetcher2.write_repodata_record(base);
            auto j2 = read_json(info_dir / "repodata_record.json");
            REQUIRE(j2.at("size") == sz);
        }

        SECTION("URL hash (md5/sha256) preserved over index.json")
        {
            const std::string raw =
                "https://conda.anaconda.org/conda-forge/linux-64/pkg-1.0-abc.conda#"
                "7dbaa197d7ba6032caf7ae7f32c1efa0";
            auto pkg = specs::PackageInfo::from_url(raw).value();
            write_file(fs::u8path(tmp_dir.path()) / pkg.filename, std::string(3, 'h'));

            nlohmann::json index = { { "md5", "ffffffffffffffffffffffffffffffff" },
                                     { "sha256",
                                       "0000000000000000000000000000000000000000000000000000000000000000" } };
            write_file(info_dir / "index.json", index.dump());

            auto fetcher = make_fetcher_with_cache(pkg, tmp_dir.path());
            fetcher.write_repodata_record(base);

            const auto j = read_json(info_dir / "repodata_record.json");
            REQUIRE(j.at("md5") == "7dbaa197d7ba6032caf7ae7f32c1efa0");
            REQUIRE(j.at("sha256") == "");
        }

        SECTION("Healing kicks in for corrupted caches and backfills from index.json")
        {
            // defaulted_keys empty + timestamp 0 + stub fields indicate corrupted cache
            specs::PackageInfo pkg;
            pkg.name = "pkg";
            pkg.version = "1.0";
            pkg.build_string = "abc";
            pkg.channel = "https://conda.anaconda.org/conda-forge";
            pkg.platform = "linux-64";
            pkg.filename = "pkg-1.0-abc.conda";
            pkg.package_url = pkg.channel + "/" + pkg.platform + "/" + pkg.filename;
            pkg.timestamp = 0;            // corrupted
            pkg.license = "";           // stub
            pkg.dependencies = {};       // stub
            pkg.constrains = {};         // stub
            pkg.build_number = 0;        // stub

            write_file(fs::u8path(tmp_dir.path()) / pkg.filename, std::string(2, 'x'));

            nlohmann::json index = { { "license", "Apache-2.0" },
                                     { "timestamp", 424242u },
                                     { "depends", nlohmann::json::array({ "python >=3.10" }) },
                                     { "constrains", nlohmann::json::array() },
                                     { "build_number", 42 } };
            write_file(info_dir / "index.json", index.dump());

            auto fetcher = make_fetcher_with_cache(pkg, tmp_dir.path());
            fetcher.write_repodata_record(base);

            const auto j = read_json(info_dir / "repodata_record.json");
            REQUIRE(j.at("license") == "Apache-2.0");
            REQUIRE(j.at("timestamp") == 424242);
            REQUIRE(j.at("depends").is_array());
            REQUIRE_FALSE(j.at("depends").empty());
            REQUIRE(j.at("build_number") == 42);
        }

        SECTION("No false-positive healing when non-stub values present")
        {
            specs::PackageInfo pkg;
            pkg.name = "pkg";
            pkg.version = "1.0";
            pkg.build_string = "abc";
            pkg.channel = "https://conda.anaconda.org/conda-forge";
            pkg.platform = "linux-64";
            pkg.filename = "pkg-1.0-abc.conda";
            pkg.package_url = pkg.channel + "/" + pkg.platform + "/" + pkg.filename;
            pkg.timestamp = 0;               // zero timestamp but valid metadata present
            pkg.license = "MIT";            // non-stub
            pkg.dependencies = { "numpy" }; // non-stub

            write_file(fs::u8path(tmp_dir.path()) / pkg.filename, std::string(2, 'q'));

            nlohmann::json index = { { "license", "Apache-2.0" },
                                     { "depends", nlohmann::json::array({ "python" }) } };
            write_file(info_dir / "index.json", index.dump());

            auto fetcher = make_fetcher_with_cache(pkg, tmp_dir.path());
            fetcher.write_repodata_record(base);

            const auto j = read_json(info_dir / "repodata_record.json");
            REQUIRE(j.at("license") == "MIT");
            REQUIRE(j.at("depends").is_array());
            REQUIRE(j.at("depends").size() == 1);
            REQUIRE(j.at("depends")[0] == "numpy");
        }
    }

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
}
