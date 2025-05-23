#include <gtest/gtest.h>

#include "mamba/api/configuration.hpp"
#include "mamba/core/context.hpp"

#include <cstdio>


namespace mamba
{
    namespace testing
    {
        class Configuration : public ::testing::Test
        {
        public:
            Configuration()
            {
                mamba::Configuration::instance()
                    .at("show_banner")
                    .get_wrapped<bool>()
                    .set_default_value(false);
            }

        protected:
            void load_test_config(std::string rc)
            {
                std::string unique_location = tempfile_ptr->path();
                std::ofstream out_file(unique_location, std::ofstream::out | std::ofstream::trunc);
                out_file << rc;
                out_file.close();

                mamba::Configuration::instance().reset_configurables();
                mamba::Configuration::instance()
                    .at("rc_files")
                    .get_wrapped<std::vector<fs::path>>()
                    .set_value({ fs::path(unique_location) });
                mamba::Configuration::instance().load();
            }

            void load_test_config(std::vector<std::string> rcs)
            {
                std::vector<std::unique_ptr<TemporaryFile>> tempfiles;
                std::vector<fs::path> sources;

                for (auto rc : rcs)
                {
                    tempfiles.push_back(std::make_unique<TemporaryFile>("mambarc", ".yaml"));
                    fs::path loc = tempfiles.back()->path();

                    std::ofstream out_file(loc);
                    out_file << rc;
                    out_file.close();

                    sources.push_back(loc);
                }

                mamba::Configuration::instance().reset_configurables();
                mamba::Configuration::instance()
                    .at("rc_files")
                    .get_wrapped<std::vector<fs::path>>()
                    .set_value(sources);
                mamba::Configuration::instance().load();
            }

            std::string shrink_source(std::size_t position)
            {
                return env::shrink_user(config.valid_sources()[position]).string();
            }
            std::unique_ptr<TemporaryFile> tempfile_ptr
                = std::make_unique<TemporaryFile>("mambarc", ".yaml");

            mamba::Configuration& config = mamba::Configuration::instance();
            mamba::Context& ctx = mamba::Context::instance();
        };

        TEST_F(Configuration, target_prefix_options)
        {
            EXPECT_EQ(!MAMBA_ALLOW_EXISTING_PREFIX, 0);
            EXPECT_EQ(!MAMBA_ALLOW_MISSING_PREFIX, 0);
            EXPECT_EQ(!MAMBA_ALLOW_NOT_ENV_PREFIX, 0);
            EXPECT_EQ(!MAMBA_EXPECT_EXISTING_PREFIX, 0);

            EXPECT_EQ(!MAMBA_ALLOW_EXISTING_PREFIX, MAMBA_NOT_ALLOW_EXISTING_PREFIX);

            EXPECT_EQ(MAMBA_NOT_ALLOW_EXISTING_PREFIX | MAMBA_NOT_ALLOW_MISSING_PREFIX
                          | MAMBA_NOT_ALLOW_NOT_ENV_PREFIX | MAMBA_NOT_EXPECT_EXISTING_PREFIX,
                      0);
        }

        TEST_F(Configuration, load_rc_file)
        {
            std::string rc = unindent(R"(
                channels:
                    - test1)");
            load_test_config(rc);
            std::string src = env::shrink_user(tempfile_ptr->path());
            EXPECT_EQ(config.sources().size(), 1);
            EXPECT_EQ(config.valid_sources().size(), 1);
            EXPECT_EQ(config.dump(), "channels:\n  - test1");
            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS),
                      "channels:\n  - test1  # '" + src + "'");

            // ill-formed config file
            rc = unindent(R"(
                channels:
                    - test10
                   - https://repo.mamba.pm/conda-forge)");

            load_test_config(rc);

            EXPECT_EQ(config.sources().size(), 1);
            EXPECT_EQ(config.valid_sources().size(), 0);
            EXPECT_EQ(config.dump(), "");
            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS), "");
        }

        TEST_F(Configuration, load_rc_files)
        {
            std::string rc1 = unindent(R"(
                channels:
                    - test1
                ssl_verify: false)");

            std::string rc2 = unindent(R"(
                channels:
                    - test2
                    - test1)");

            std::vector<std::string> rcs = { rc1, rc2 };
            load_test_config(rcs);

            ASSERT_EQ(config.sources().size(), 2);
            ASSERT_EQ(config.valid_sources().size(), 2);

            std::string src1 = shrink_source(0);
            std::string src2 = shrink_source(1);
            EXPECT_EQ(config.dump(), unindent(R"(
                                    channels:
                                      - test1
                                      - test2
                                    ssl_verify: <false>)"));
            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS),
                      unindent((R"(
                                    channels:
                                      - test1  # ')"
                                + src1 + R"('
                                      - test2  # ')"
                                + src2 + R"('
                                    ssl_verify: <false>  # ')"
                                + src1 + "'")
                                   .c_str()));

            // ill-formed key
            std::string rc3 = unindent(R"(
                channels:
                    - test3
                override_channels_enabled:
                    - false)");
            rcs.push_back(rc3);
            load_test_config(rcs);

            ASSERT_EQ(config.sources().size(), 3);
            ASSERT_EQ(config.valid_sources().size(), 3);

            // tmp files changed
            src1 = shrink_source(0);
            src2 = shrink_source(1);
            std::string src3 = shrink_source(2);
            EXPECT_EQ(config.dump(), unindent(R"(
                                    channels:
                                      - test1
                                      - test2
                                      - test3
                                    ssl_verify: <false>)"));
            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS),
                      unindent((R"(
                                    channels:
                                      - test1  # ')"
                                + src1 + R"('
                                      - test2  # ')"
                                + src2 + R"('
                                      - test3  # ')"
                                + src3 + R"('
                                    ssl_verify: <false>  # ')"
                                + src1 + "'")
                                   .c_str()));

            // ill-formed file
            std::string rc4 = unindent(R"(
                channels:
                  - test3
                 - test4)");
            rcs.push_back(rc4);
            load_test_config(rcs);

            ASSERT_EQ(config.sources().size(), 4);
            ASSERT_EQ(config.valid_sources().size(), 3);

            // tmp files changed
            src1 = shrink_source(0);
            src2 = shrink_source(1);
            src3 = shrink_source(2);
            EXPECT_EQ(config.dump(), unindent(R"(
                                    channels:
                                      - test1
                                      - test2
                                      - test3
                                    ssl_verify: <false>)"));
            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS),
                      unindent((R"(
                                    channels:
                                      - test1  # ')"
                                + src1 + R"('
                                      - test2  # ')"
                                + src2 + R"('
                                      - test3  # ')"
                                + src3 + R"('
                                    ssl_verify: <false>  # ')"
                                + src1 + "'")
                                   .c_str()));
        }

        TEST_F(Configuration, dump)
        {
            std::string rc1 = unindent(R"(
                channels:
                    - test1
                    - https://repo.mamba.pm/conda-forge
                override_channels_enabled: true
                allow_softlinks: true
                test_complex_structure:
                    - foo: bar
                    - bar: baz)");

            std::string rc2 = unindent(R"(
                channels:
                    - test10
                override_channels_enabled: false)");

            load_test_config({ rc1, rc2 });

            ASSERT_EQ(config.sources().size(), 2);
            ASSERT_EQ(config.valid_sources().size(), 2);
            std::string src1 = shrink_source(0);
            std::string src2 = env::shrink_user(shrink_source(1)).string();

            std::string res = config.dump();
            // Unexpected/handled keys are dropped
            EXPECT_EQ(res, unindent(R"(
                                channels:
                                  - test1
                                  - https://repo.mamba.pm/conda-forge
                                  - test10
                                override_channels_enabled: true
                                allow_softlinks: true)"));

            res = config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS);
            EXPECT_EQ(res,
                      unindent((R"(
                                channels:
                                  - test1  # ')"
                                + src1 + R"('
                                  - https://repo.mamba.pm/conda-forge  # ')"
                                + src1 + R"('
                                  - test10  # ')"
                                + src2 + R"('
                                override_channels_enabled: true  # ')"
                                + src1 + "' > '" + src2 + R"('
                                allow_softlinks: true  # ')"
                                + src1 + "'")
                                   .c_str()));
        }

        TEST_F(Configuration, channels)
        {
            std::string rc1 = unindent(R"(
                channels:
                    - c11
                    - c12)");
            std::string rc2 = unindent(R"(
                channels:
                    - c21
                    - c12)");
            std::string rc3 = unindent(R"(
                channels:
                    - c11
                    - c32
                    - c21)");
            load_test_config({ rc1, rc2, rc3 });

            EXPECT_EQ(config.dump(), unindent(R"(
                                channels:
                                  - c11
                                  - c12
                                  - c21
                                  - c32)"));

            env::set("CONDA_CHANNELS", "c90,c101");
            load_test_config(rc1);

            EXPECT_EQ(config.dump(), unindent(R"(
                                channels:
                                  - c90
                                  - c101
                                  - c11
                                  - c12)"));

            ASSERT_EQ(config.sources().size(), 1);
            ASSERT_EQ(config.valid_sources().size(), 1);
            std::string src1 = shrink_source(0);

            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS),
                      unindent((R"(
                                channels:
                                  - c90  # 'CONDA_CHANNELS'
                                  - c101  # 'CONDA_CHANNELS'
                                  - c11  # ')"
                                + src1 + R"('
                                  - c12  # ')"
                                + src1 + "'")
                                   .c_str()));

            config.at("channels")
                .set_yaml_value("https://my.channel, https://my2.channel")
                .compute();
            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS),
                      unindent((R"(
                                channels:
                                  - https://my.channel  # 'API'
                                  - https://my2.channel  # 'API'
                                  - c90  # 'CONDA_CHANNELS'
                                  - c101  # 'CONDA_CHANNELS'
                                  - c11  # ')"
                                + src1 + R"('
                                  - c12  # ')"
                                + src1 + "'")
                                   .c_str()));
            EXPECT_EQ(ctx.channels, config.at("channels").value<std::vector<std::string>>());

            env::set("CONDA_CHANNELS", "");
        }

        TEST_F(Configuration, default_channels)
        {
            std::string rc1 = unindent(R"(
                default_channels:
                  - c11
                  - c12)");
            std::string rc2 = unindent(R"(
                default_channels:
                  - c21
                  - c12)");
            std::string rc3 = unindent(R"(
                default_channels:
                  - c11
                  - c32
                  - c21)");
            load_test_config({ rc1, rc2, rc3 });

            EXPECT_EQ(config.dump(), unindent(R"(
                        default_channels:
                          - c11
                          - c12
                          - c21
                          - c32)"));

            env::set("MAMBA_DEFAULT_CHANNELS", "c91,c100");
            load_test_config(rc1);

            EXPECT_EQ(config.dump(), unindent(R"(
                                default_channels:
                                  - c91
                                  - c100
                                  - c11
                                  - c12)"));

            ASSERT_EQ(config.sources().size(), 1);
            ASSERT_EQ(config.valid_sources().size(), 1);
            std::string src1 = shrink_source(0);

            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS),
                      unindent((R"(
                                default_channels:
                                  - c91  # 'MAMBA_DEFAULT_CHANNELS'
                                  - c100  # 'MAMBA_DEFAULT_CHANNELS'
                                  - c11  # ')"
                                + src1 + R"('
                                  - c12  # ')"
                                + src1 + "'")
                                   .c_str()));

            config.at("default_channels")
                .set_yaml_value("https://my.channel, https://my2.channel")
                .compute();
            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS),
                      unindent((R"(
                                default_channels:
                                  - https://my.channel  # 'API'
                                  - https://my2.channel  # 'API'
                                  - c91  # 'MAMBA_DEFAULT_CHANNELS'
                                  - c100  # 'MAMBA_DEFAULT_CHANNELS'
                                  - c11  # ')"
                                + src1 + R"('
                                  - c12  # ')"
                                + src1 + "'")
                                   .c_str()));
            EXPECT_EQ(ctx.default_channels,
                      config.at("default_channels").value<std::vector<std::string>>());

            env::set("MAMBA_DEFAULT_CHANNELS", "");
        }

        TEST_F(Configuration, channel_alias)
        {
            std::string rc1 = "channel_alias: http://repo.mamba.pm/";
            std::string rc2 = "channel_alias: https://conda.anaconda.org/";

            load_test_config({ rc1, rc2 });
            EXPECT_EQ(config.dump(), "channel_alias: http://repo.mamba.pm/");

            load_test_config({ rc2, rc1 });
            EXPECT_EQ(config.dump(), "channel_alias: https://conda.anaconda.org/");

            env::set("MAMBA_CHANNEL_ALIAS", "https://foo.bar");
            load_test_config(rc1);

            EXPECT_EQ(config.dump(), "channel_alias: https://foo.bar");

            ASSERT_EQ(config.sources().size(), 1);
            ASSERT_EQ(config.valid_sources().size(), 1);
            std::string src1 = shrink_source(0);

            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS),
                      "channel_alias: https://foo.bar  # 'MAMBA_CHANNEL_ALIAS' > '" + src1 + "'");

            config.at("channel_alias").set_yaml_value("https://my.channel").compute();
            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS),
                      "channel_alias: https://my.channel  # 'API' > 'MAMBA_CHANNEL_ALIAS' > '"
                          + src1 + "'");
            EXPECT_EQ(ctx.channel_alias, config.at("channel_alias").value<std::string>());

            env::set("MAMBA_CHANNEL_ALIAS", "");
        }

        TEST_F(Configuration, pkgs_dirs)
        {
            std::string cache1 = (env::home_directory() / "foo").string();
            std::string cache2 = (env::home_directory() / "bar").string();

            std::string rc1 = "pkgs_dirs:\n  - " + cache1;
            std::string rc2 = "pkgs_dirs:\n  - " + cache2;

            load_test_config({ rc1, rc2 });
            EXPECT_EQ(config.dump(), "pkgs_dirs:\n  - " + cache1 + "\n  - " + cache2);

            load_test_config({ rc2, rc1 });
            EXPECT_EQ(config.dump(), "pkgs_dirs:\n  - " + cache2 + "\n  - " + cache1);

            std::string cache3 = (env::home_directory() / "baz").string();
            env::set("CONDA_PKGS_DIRS", cache3);
            load_test_config(rc1);
            EXPECT_EQ(config.dump(), "pkgs_dirs:\n  - " + cache3 + "\n  - " + cache1);

            ASSERT_EQ(config.sources().size(), 1);
            ASSERT_EQ(config.valid_sources().size(), 1);
            std::string src1 = shrink_source(0);

            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS),
                      unindent((R"(
                                pkgs_dirs:
                                  - )"
                                + cache3 + R"(  # 'CONDA_PKGS_DIRS'
                                  - )"
                                + cache1 + "  # '" + src1 + "'")
                                   .c_str()));

            env::set("CONDA_PKGS_DIRS", "");

            std::string empty_rc = "";
            std::string root_prefix_str = (env::home_directory() / "any_prefix").string();
            env::set("MAMBA_ROOT_PREFIX", root_prefix_str);
            load_test_config(empty_rc);

#ifdef _WIN32
            std::string extra_cache = "\n  - "
                                      + (fs::path(env::get("APPDATA")) / ".mamba" / "pkgs").string()
                                      + "  # 'fallback'";
#else
            std::string extra_cache = "";
#endif
            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS
                                      | MAMBA_SHOW_ALL_CONFIGS,
                                  { "pkgs_dirs" }),
                      unindent((R"(
                                pkgs_dirs:
                                  - )"
                                + (fs::path(root_prefix_str) / "pkgs").string() + R"(  # 'fallback'
                                  - )"
                                + (env::home_directory() / ".mamba" / "pkgs").string()
                                + R"(  # 'fallback')" + extra_cache)
                                   .c_str()));
            EXPECT_EQ(ctx.pkgs_dirs, config.at("pkgs_dirs").value<std::vector<fs::path>>());

            std::string cache4 = (env::home_directory() / "babaz").string();
            env::set("CONDA_PKGS_DIRS", cache4);
            load_test_config(empty_rc);
            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS),
                      unindent((R"(
                                pkgs_dirs:
                                  - )"
                                + cache4 + "  # 'CONDA_PKGS_DIRS'")
                                   .c_str()));

            env::set("CONDA_PKGS_DIRS", "");
            env::set("MAMBA_ROOT_PREFIX", "");
            config.clear_values();
        }

        TEST_F(Configuration, ssl_verify)
        {
            // Default empty string value
            ctx.ssl_verify = "";
            std::string rc = "";
            load_test_config(rc);
            EXPECT_EQ(ctx.ssl_verify, "<system>");

            rc = "ssl_verify: true";
            load_test_config(rc);
            EXPECT_EQ(ctx.ssl_verify, "<system>");

            rc = "ssl_verify: <true>";
            load_test_config(rc);
            EXPECT_EQ(ctx.ssl_verify, "<system>");

            rc = "ssl_verify: 1";
            load_test_config(rc);
            EXPECT_EQ(ctx.ssl_verify, "<system>");

            rc = "ssl_verify: 10";
            load_test_config(rc);
            EXPECT_EQ(ctx.ssl_verify, "10");

            rc = "ssl_verify: false";
            load_test_config(rc);
            EXPECT_EQ(ctx.ssl_verify, "<false>");

            rc = "ssl_verify: <false>";
            load_test_config(rc);
            EXPECT_EQ(ctx.ssl_verify, "<false>");

            rc = "ssl_verify: 0";
            load_test_config(rc);
            EXPECT_EQ(ctx.ssl_verify, "<false>");

            rc = "ssl_verify: /foo/bar/baz";
            load_test_config(rc);
            EXPECT_EQ(ctx.ssl_verify, "/foo/bar/baz");

            std::string rc1 = "ssl_verify: true";
            std::string rc2 = "ssl_verify: false";
            load_test_config({ rc1, rc2 });
            EXPECT_EQ(config.at("ssl_verify").value<std::string>(), "<system>");
            EXPECT_EQ(ctx.ssl_verify, "<system>");

            load_test_config({ rc2, rc1 });
            EXPECT_EQ(config.at("ssl_verify").value<std::string>(), "<false>");
            EXPECT_EQ(ctx.ssl_verify, "<false>");

            env::set("MAMBA_SSL_VERIFY", "/env/bar/baz");
            load_test_config(rc1);

            ASSERT_EQ(config.sources().size(), 1);
            ASSERT_EQ(config.valid_sources().size(), 1);
            std::string src1 = shrink_source(0);

            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS),
                      "ssl_verify: /env/bar/baz  # 'MAMBA_SSL_VERIFY' > '" + src1 + "'");

            config.at("ssl_verify").set_yaml_value("/new/test").compute();
            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS),
                      "ssl_verify: /new/test  # 'API' > 'MAMBA_SSL_VERIFY' > '" + src1 + "'");

            env::set("MAMBA_SSL_VERIFY", "");
        }
#undef EXPECT_CA_EQUAL

        TEST_F(Configuration, cacert_path)
        {
            std::string rc = "ssl_verify: /foo/bar/baz\ncacert_path: /other/foo/bar/baz";
            load_test_config(rc);
            EXPECT_EQ(config.at("ssl_verify").value<std::string>(), "/other/foo/bar/baz");
            EXPECT_EQ(config.at("cacert_path").value<std::string>(), "/other/foo/bar/baz");
            EXPECT_EQ(ctx.ssl_verify, "/other/foo/bar/baz");

            env::set("MAMBA_CACERT_PATH", "/env/ca/baz");
            load_test_config(rc);

            ASSERT_EQ(config.sources().size(), 1);
            ASSERT_EQ(config.valid_sources().size(), 1);
            std::string src = shrink_source(0);

            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS),
                      unindent((R"(
                                cacert_path: /env/ca/baz  # 'MAMBA_CACERT_PATH' > ')"
                                + src + R"('
                                ssl_verify: /env/ca/baz  # ')"
                                + src + "'")
                                   .c_str()));
            EXPECT_EQ(ctx.ssl_verify, "/env/ca/baz");

            config.at("cacert_path").set_yaml_value("/new/test").compute();
            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS),
                      unindent((R"(
                                cacert_path: /new/test  # 'API' > 'MAMBA_CACERT_PATH' > ')"
                                + src + R"('
                                ssl_verify: /env/ca/baz  # ')"
                                + src + "'")
                                   .c_str()));
            EXPECT_EQ(ctx.ssl_verify, "/env/ca/baz");

            config.at("ssl_verify").compute();
            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS),
                      unindent((R"(
                                cacert_path: /new/test  # 'API' > 'MAMBA_CACERT_PATH' > ')"
                                + src + R"('
                                ssl_verify: /new/test  # ')"
                                + src + "'")
                                   .c_str()));
            EXPECT_EQ(ctx.ssl_verify, "/new/test");

            env::set("MAMBA_CACERT_PATH", "");
            load_test_config("cacert_path:\nssl_verify: true");  // reset ssl verify to default
        }

        TEST_F(Configuration, platform)
        {
            EXPECT_EQ(ctx.platform, ctx.host_platform);

            std::string rc = "platform: mylinux-128";
            load_test_config(rc);
            std::string src = shrink_source(0);
            EXPECT_EQ(config.at("platform").value<std::string>(), "mylinux-128");
            EXPECT_EQ(ctx.platform, "mylinux-128");
            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS),
                      unindent((R"(
                                platform: mylinux-128  # ')"
                                + src + "'")
                                   .c_str()));

            env::set("CONDA_SUBDIR", "win-32");
            load_test_config(rc);
            src = shrink_source(0);
            EXPECT_EQ(config.at("platform").value<std::string>(), "win-32");
            EXPECT_EQ(ctx.platform, "win-32");
            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS),
                      unindent((R"(
                                platform: win-32  # 'CONDA_SUBDIR' > ')"
                                + src + "'")
                                   .c_str()));

            config.at("platform").clear_values();
            ctx.platform = ctx.host_platform;
        }

#define TEST_BOOL_CONFIGURABLE(NAME, CTX)                                                          \
    TEST_F(Configuration, NAME)                                                                    \
    {                                                                                              \
        std::string rc1 = std::string(#NAME) + ": true";                                           \
        std::string rc2 = std::string(#NAME) + ": false";                                          \
        if (config.at(#NAME).rc_configurable())                                                    \
        {                                                                                          \
            load_test_config({ rc1, rc2 });                                                        \
            EXPECT_TRUE(config.at(#NAME).value<bool>());                                           \
            EXPECT_TRUE(CTX);                                                                      \
                                                                                                   \
            load_test_config({ rc2, rc1 });                                                        \
            EXPECT_FALSE(config.at(#NAME).value<bool>());                                          \
            EXPECT_FALSE(CTX);                                                                     \
        }                                                                                          \
                                                                                                   \
        std::string env_name = "MAMBA_" + to_upper(#NAME);                                         \
        env::set(env_name, "true");                                                                \
        load_test_config(rc2);                                                                     \
                                                                                                   \
        ASSERT_EQ(config.sources().size(), 1);                                                     \
        ASSERT_EQ(config.valid_sources().size(), 1);                                               \
        std::string src = shrink_source(0);                                                        \
                                                                                                   \
        std::string expected;                                                                      \
        if (config.at(#NAME).rc_configurable())                                                    \
        {                                                                                          \
            expected = std::string(#NAME) + ": true  # '" + env_name + "' > '" + src + "'";        \
        }                                                                                          \
        else                                                                                       \
        {                                                                                          \
            expected = std::string(#NAME) + ": true  # '" + env_name + "'";                        \
        }                                                                                          \
        int dump_opts = MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS;                         \
        EXPECT_EQ((config.dump(dump_opts, { #NAME })), expected);                                  \
        EXPECT_TRUE(config.at(#NAME).value<bool>());                                               \
        EXPECT_TRUE(CTX);                                                                          \
                                                                                                   \
        if (config.at(#NAME).rc_configurable())                                                    \
        {                                                                                          \
            expected                                                                               \
                = std::string(#NAME) + ": true  # 'API' > '" + env_name + "' > '" + src + "'";     \
        }                                                                                          \
        else                                                                                       \
        {                                                                                          \
            expected = std::string(#NAME) + ": true  # 'API' > '" + env_name + "'";                \
        }                                                                                          \
        config.at(#NAME).set_yaml_value("true").compute();                                         \
        EXPECT_EQ((config.dump(dump_opts, { #NAME })), expected);                                  \
        EXPECT_TRUE(config.at(#NAME).value<bool>());                                               \
        EXPECT_TRUE(CTX);                                                                          \
                                                                                                   \
        env::set(env_name, "yeap");                                                                \
        ASSERT_THROW(load_test_config(rc2), YAML::Exception);                                      \
                                                                                                   \
        env::set(env_name, "");                                                                    \
        load_test_config(rc2);                                                                     \
    }

        TEST_BOOL_CONFIGURABLE(ssl_no_revoke, ctx.ssl_no_revoke);

        TEST_BOOL_CONFIGURABLE(override_channels_enabled, ctx.override_channels_enabled);

        TEST_BOOL_CONFIGURABLE(auto_activate_base, ctx.auto_activate_base);

        TEST_F(Configuration, channel_priority)
        {
            std::string rc1 = "channel_priority: flexible";
            std::string rc2 = "channel_priority: strict";
            std::string rc3 = "channel_priority: disabled";

            load_test_config({ rc1, rc2, rc3 });
            EXPECT_EQ(config.at("channel_priority").value<ChannelPriority>(),
                      ChannelPriority::kFlexible);
            EXPECT_TRUE(ctx.channel_priority == ChannelPriority::kFlexible);

            load_test_config({ rc3, rc1, rc2 });
            EXPECT_EQ(config.at("channel_priority").value<ChannelPriority>(),
                      ChannelPriority::kDisabled);
            EXPECT_TRUE(ctx.channel_priority == ChannelPriority::kDisabled);

            load_test_config({ rc2, rc1, rc3 });
            EXPECT_EQ(config.at("channel_priority").value<ChannelPriority>(),
                      ChannelPriority::kStrict);
            EXPECT_TRUE(ctx.channel_priority == ChannelPriority::kStrict);

            env::set("MAMBA_CHANNEL_PRIORITY", "strict");
            load_test_config(rc3);

            ASSERT_EQ(config.sources().size(), 1);
            ASSERT_EQ(config.valid_sources().size(), 1);
            std::string src = shrink_source(0);

            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS),
                      "channel_priority: strict  # 'MAMBA_CHANNEL_PRIORITY' > '" + src + "'");
            EXPECT_EQ(config.at("channel_priority").value<ChannelPriority>(),
                      ChannelPriority::kStrict);
            EXPECT_EQ(ctx.channel_priority, ChannelPriority::kStrict);

            config.at("channel_priority").set_yaml_value("flexible").compute();
            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS),
                      "channel_priority: flexible  # 'API' > 'MAMBA_CHANNEL_PRIORITY' > '" + src
                          + "'");
            EXPECT_EQ(config.at("channel_priority").value<ChannelPriority>(),
                      ChannelPriority::kFlexible);
            EXPECT_EQ(ctx.channel_priority, ChannelPriority::kFlexible);

            env::set("MAMBA_CHANNEL_PRIORITY", "stric");
            ASSERT_THROW(load_test_config(rc3), YAML::Exception);

            env::set("MAMBA_CHANNEL_PRIORITY", "");
        }

        TEST_F(Configuration, pinned_packages)
        {
            std::string rc1 = unindent(R"(
                pinned_packages:
                    - jupyterlab=3
                    - numpy=1.19)");
            std::string rc2 = unindent(R"(
                pinned_packages:
                    - matplotlib
                    - numpy=1.19)");
            std::string rc3 = unindent(R"(
                pinned_packages:
                    - jupyterlab=3
                    - bokeh
                    - matplotlib)");

            load_test_config({ rc1, rc2, rc3 });
            EXPECT_EQ(config.dump(), unindent(R"(
                                        pinned_packages:
                                          - jupyterlab=3
                                          - numpy=1.19
                                          - matplotlib
                                          - bokeh)"));
            EXPECT_EQ(
                ctx.pinned_packages,
                std::vector<std::string>({ "jupyterlab=3", "numpy=1.19", "matplotlib", "bokeh" }));

            load_test_config({ rc2, rc1, rc3 });
            ASSERT_TRUE(config.at("pinned_packages").yaml_value());
            EXPECT_EQ(config.dump(), unindent(R"(
                                        pinned_packages:
                                          - matplotlib
                                          - numpy=1.19
                                          - jupyterlab=3
                                          - bokeh)"));
            EXPECT_EQ(
                ctx.pinned_packages,
                std::vector<std::string>({ "matplotlib", "numpy=1.19", "jupyterlab=3", "bokeh" }));

            env::set("MAMBA_PINNED_PACKAGES", "mpl=10.2,xtensor");
            load_test_config(rc1);
            ASSERT_EQ(config.sources().size(), 1);
            ASSERT_EQ(config.valid_sources().size(), 1);
            std::string src1 = shrink_source(0);

            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS),
                      unindent((R"(
                                pinned_packages:
                                  - mpl=10.2  # 'MAMBA_PINNED_PACKAGES'
                                  - xtensor  # 'MAMBA_PINNED_PACKAGES'
                                  - jupyterlab=3  # ')"
                                + src1 + R"('
                                  - numpy=1.19  # ')"
                                + src1 + "'")
                                   .c_str()));
            EXPECT_EQ(
                ctx.pinned_packages,
                std::vector<std::string>({ "mpl=10.2", "xtensor", "jupyterlab=3", "numpy=1.19" }));

            config.at("pinned_packages").set_yaml_value("pytest").compute();
            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS),
                      unindent((R"(
                                pinned_packages:
                                  - pytest  # 'API'
                                  - mpl=10.2  # 'MAMBA_PINNED_PACKAGES'
                                  - xtensor  # 'MAMBA_PINNED_PACKAGES'
                                  - jupyterlab=3  # ')"
                                + src1 + R"('
                                  - numpy=1.19  # ')"
                                + src1 + "'")
                                   .c_str()));
            EXPECT_EQ(ctx.pinned_packages,
                      std::vector<std::string>(
                          { "pytest", "mpl=10.2", "xtensor", "jupyterlab=3", "numpy=1.19" }));

            env::set("MAMBA_PINNED_PACKAGES", "");
        }


        TEST_BOOL_CONFIGURABLE(no_pin, config.at("no_pin").value<bool>());

        TEST_BOOL_CONFIGURABLE(retry_clean_cache, config.at("retry_clean_cache").value<bool>());

        TEST_BOOL_CONFIGURABLE(allow_softlinks, ctx.allow_softlinks);

        TEST_BOOL_CONFIGURABLE(always_softlink, ctx.always_softlink);

        TEST_BOOL_CONFIGURABLE(always_copy, ctx.always_copy);

        TEST_F(Configuration, always_softlink_and_copy)
        {
            env::set("MAMBA_ALWAYS_COPY", "true");
            ASSERT_THROW(load_test_config("always_softlink: true"), std::runtime_error);
            env::set("MAMBA_ALWAYS_COPY", "");

            env::set("MAMBA_ALWAYS_SOFTLINK", "true");
            ASSERT_THROW(load_test_config("always_copy: true"), std::runtime_error);
            env::set("MAMBA_ALWAYS_SOFTLINK", "");

            load_test_config("always_softlink: false\nalways_copy: false");
        }

        TEST_F(Configuration, safety_checks)
        {
            std::string rc1 = "safety_checks: enabled";
            std::string rc2 = "safety_checks: warn";
            std::string rc3 = "safety_checks: disabled";

            load_test_config({ rc1, rc2, rc3 });
            EXPECT_EQ(config.at("safety_checks").value<VerificationLevel>(),
                      VerificationLevel::kEnabled);
            EXPECT_EQ(ctx.safety_checks, VerificationLevel::kEnabled);

            load_test_config({ rc2, rc1, rc3 });
            EXPECT_EQ(config.at("safety_checks").value<VerificationLevel>(),
                      VerificationLevel::kWarn);
            EXPECT_EQ(ctx.safety_checks, VerificationLevel::kWarn);

            load_test_config({ rc3, rc1, rc3 });
            EXPECT_EQ(config.at("safety_checks").value<VerificationLevel>(),
                      VerificationLevel::kDisabled);
            EXPECT_EQ(ctx.safety_checks, VerificationLevel::kDisabled);

            env::set("MAMBA_SAFETY_CHECKS", "warn");
            load_test_config(rc1);

            ASSERT_EQ(config.sources().size(), 1);
            ASSERT_EQ(config.valid_sources().size(), 1);
            std::string src = shrink_source(0);

            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS),
                      "safety_checks: warn  # 'MAMBA_SAFETY_CHECKS' > '" + src + "'");
            EXPECT_EQ(config.at("safety_checks").value<VerificationLevel>(),
                      VerificationLevel::kWarn);
            EXPECT_EQ(ctx.safety_checks, VerificationLevel::kWarn);

            config.at("safety_checks").set_yaml_value("disabled").compute();
            EXPECT_EQ(config.dump(MAMBA_SHOW_CONFIG_VALUES | MAMBA_SHOW_CONFIG_SRCS),
                      "safety_checks: disabled  # 'API' > 'MAMBA_SAFETY_CHECKS' > '" + src + "'");
            EXPECT_EQ(config.at("safety_checks").value<VerificationLevel>(),
                      VerificationLevel::kDisabled);
            EXPECT_EQ(ctx.safety_checks, VerificationLevel::kDisabled);

            env::set("MAMBA_SAFETY_CHECKS", "yeap");
            ASSERT_THROW(load_test_config(rc2), std::runtime_error);

            env::set("MAMBA_SAFETY_CHECKS", "");
            load_test_config(rc2);
        }

        TEST_BOOL_CONFIGURABLE(extra_safety_checks, ctx.extra_safety_checks);

#undef TEST_BOOL_CONFIGURABLE

        TEST_F(Configuration, has_config_name)
        {
            using namespace detail;

            EXPECT_FALSE(has_config_name(""));
            EXPECT_FALSE(has_config_name("conf"));
            EXPECT_FALSE(has_config_name("config"));
            EXPECT_FALSE(has_config_name("config.conda"));
            EXPECT_FALSE(has_config_name("conf.condarc"));
            EXPECT_FALSE(has_config_name("conf.mambarc"));

            EXPECT_TRUE(has_config_name("condarc"));
            EXPECT_TRUE(has_config_name("mambarc"));
            EXPECT_TRUE(has_config_name(".condarc"));
            EXPECT_TRUE(has_config_name(".mambarc"));
            EXPECT_TRUE(has_config_name(".yaml"));
            EXPECT_TRUE(has_config_name(".yml"));
            EXPECT_TRUE(has_config_name("conf.yaml"));
            EXPECT_TRUE(has_config_name("config.yml"));
        }

        TEST_F(Configuration, is_config_file)
        {
            using namespace detail;

            fs::path p = "config_test/.condarc";

            std::vector<fs::path> wrong_paths = {
                "config_test", "conf_test", "config_test/condarc", "history_test/conda-meta/history"
            };

            EXPECT_TRUE(is_config_file(p));

            for (fs::path p : wrong_paths)
            {
                EXPECT_FALSE(is_config_file(p));
            }
        }

        TEST_F(Configuration, print_scalar_node)
        {
            using namespace detail;

            std::string rc = "foo";
            auto node = YAML::Load(rc);
            auto node_src = YAML::Load("/some/source1");
            YAML::Emitter out;
            print_scalar_node(out, node, node_src, true);

            std::string res = out.c_str();
            EXPECT_EQ(res, "foo  # '/some/source1'");

            rc = unindent(R"(
                            foo: bar
                            bar: baz)");
            node = YAML::Load(rc);
            EXPECT_THROW(print_scalar_node(out, node, node_src, true), std::runtime_error);

            rc = unindent(R"(
                            - foo
                            - bar)");
            node = YAML::Load(rc);
            EXPECT_THROW(print_scalar_node(out, node, node_src, true), std::runtime_error);

            node = YAML::Node();
            EXPECT_THROW(print_scalar_node(out, node, node_src, true), std::runtime_error);
        }

        TEST_F(Configuration, print_map_node)
        {
            using namespace detail;

            std::string rc = unindent(R"(
                                foo: bar
                                bar: baz)");
            auto node = YAML::Load(rc);
            auto node_src = YAML::Load(unindent(R"(
                                          foo: /some/source1
                                          bar: /some/source2)"));
            YAML::Emitter out;
            print_map_node(out, node, node_src, true);

            std::string res = out.c_str();
            EXPECT_EQ(res, unindent(R"(
                                foo: bar  # '/some/source1'
                                bar: baz  # '/some/source2')"));

            rc = "foo";
            node = YAML::Load(rc);
            EXPECT_THROW(print_map_node(out, node, node_src, true), std::runtime_error);

            rc = unindent(R"(
                            - foo
                            - bar)");
            node = YAML::Load(rc);
            EXPECT_THROW(print_map_node(out, node, node_src, true), std::runtime_error);

            node = YAML::Node();
            EXPECT_THROW(print_map_node(out, node, node_src, true), std::runtime_error);
        }

        TEST_F(Configuration, print_seq_node)
        {
            using namespace detail;

            std::string rc = unindent(R"(
                                        - foo
                                        - bar
                                        )");
            auto node = YAML::Load(rc);
            auto node_src = YAML::Load(unindent(R"(
                                                - /some/source1
                                                - /some/source2
                                                )"));
            YAML::Emitter out;
            print_seq_node(out, node, node_src, true);

            std::string res = out.c_str();
            EXPECT_EQ(res, unindent(R"(
                                  - foo  # '/some/source1'
                                  - bar  # '/some/source2')"));

            rc = "foo";
            node = YAML::Load(rc);
            EXPECT_THROW(print_seq_node(out, node, node_src, true), std::runtime_error);

            rc = unindent(R"(
                            foo: bar
                            bar: baz)");
            node = YAML::Load(rc);
            EXPECT_THROW(print_seq_node(out, node, node_src, true), std::runtime_error);

            node = YAML::Node();
            EXPECT_THROW(print_seq_node(out, node, node_src, true), std::runtime_error);
        }
    }  // namespace testing
}  // namespace mamba
