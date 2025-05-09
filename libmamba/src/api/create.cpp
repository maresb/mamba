// Copyright (c) 2019, QuantStack and Mamba Contributors
//
// Distributed under the terms of the BSD 3-Clause License.
//
// The full license is in the file LICENSE, distributed with this software.

#include "mamba/api/configuration.hpp"
#include "mamba/api/create.hpp"
#include "mamba/api/install.hpp"

#include "mamba/core/context.hpp"


namespace mamba
{
    void create()
    {
        auto& ctx = Context::instance();
        auto& config = Configuration::instance();

        config.at("use_target_prefix_fallback").set_value(false);
        config.at("target_prefix_checks")
            .set_value(MAMBA_ALLOW_EXISTING_PREFIX | MAMBA_ALLOW_NOT_ENV_PREFIX
                       | MAMBA_NOT_ALLOW_MISSING_PREFIX | MAMBA_NOT_EXPECT_EXISTING_PREFIX);
        config.load();

        auto& create_specs = config.at("specs").value<std::vector<std::string>>();
        auto& use_explicit = config.at("explicit_install").value<bool>();

        if (!create_specs.empty())
        {
            if (ctx.target_prefix == ctx.root_prefix)
            {
                LOG_ERROR << "Overwriting root prefix is not permitted";
                throw std::runtime_error("Aborting.");
            }
            else if (fs::exists(ctx.target_prefix))
            {
                if (fs::exists(ctx.target_prefix / "conda-meta"))
                {
                    if (Console::prompt("Found conda-prefix at '" + ctx.target_prefix.string()
                                            + "'. Overwrite?",
                                        'n'))
                    {
                        fs::remove_all(ctx.target_prefix);
                    }
                    else
                    {
                        throw std::runtime_error("Aborting.");
                    }
                }
                else
                {
                    LOG_ERROR << "Non-conda folder exists at prefix";
                    throw std::runtime_error("Aborting.");
                }
            }

            if (use_explicit)
            {
                install_explicit_specs(create_specs, true);
            }
            else
            {
                install_specs(create_specs, true);
            }
        }
        else
        {
            detail::create_empty_target(ctx.target_prefix);
        }

        config.operation_teardown();
    }
}
