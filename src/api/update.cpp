// Copyright (c) 2019, QuantStack and Mamba Contributors
//
// Distributed under the terms of the BSD 3-Clause License.
//
// The full license is in the file LICENSE, distributed with this software.

#include "mamba/api/install.hpp"

#include "mamba/core/context.hpp"
#include "mamba/api/configuration.hpp"
#include "mamba/core/virtual_packages.hpp"


namespace mamba
{
    void update(bool update_all)
    {
        auto& ctx = Context::instance();
        auto& config = Configuration::instance();

        config.load(MAMBA_ALLOW_ROOT_PREFIX | MAMBA_ALLOW_FALLBACK_PREFIX
                    | MAMBA_ALLOW_EXISTING_PREFIX);

        auto update_specs = config.at("specs").value<std::vector<std::string>>();

        if (update_all)
        {
            PrefixData prefix_data(ctx.target_prefix);
            prefix_data.load();

            for (const auto& package : prefix_data.m_package_records)
            {
                auto name = package.second.name;
                if (name != "python")
                {
                    update_specs.push_back(name);
                }
            }
        }

        if (!update_specs.empty())
        {
            install_specs(update_specs, false, SOLVER_UPDATE);
        }
        else
        {
            Console::print("Nothing to do.");
        }
    }
}
