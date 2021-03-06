// Copyright (c) 2019, QuantStack and Mamba Contributors
//
// Distributed under the terms of the BSD 3-Clause License.
//
// The full license is in the file LICENSE, distributed with this software.

#include "umamba.hpp"

#include "mamba/core/context.hpp"
#include "mamba/core/output.hpp"
#include "mamba/core/thread_utils.hpp"
#include "mamba/core/version.hpp"

#include "mamba/api/configuration.hpp"

#ifdef VENDORED_CLI11
#include "mamba/core/CLI.hpp"
#else
#include <CLI/CLI.hpp>
#endif

using namespace mamba;  // NOLINT(build/namespaces)


int
main(int argc, char** argv)
{
    auto& ctx = Context::instance();

    ctx.is_micromamba = true;
    ctx.custom_banner = banner;

    CLI::App app{ "Version: " + version() + "\n" };
    set_umamba_command(&app);

    try
    {
        CLI11_PARSE(app, argc, argv);
    }
    catch (const std::exception& e)
    {
        LOG_ERROR << e.what();
        set_sig_interrupted();
        return 1;
    }

    if (app.get_subcommands().size() == 0)
    {
        Configuration::instance().load(MAMBA_ALLOW_ROOT_PREFIX | MAMBA_ALLOW_FALLBACK_PREFIX
                                       | MAMBA_ALLOW_EXISTING_PREFIX | MAMBA_ALLOW_MISSING_PREFIX);
        Console::print(app.help());
    }
    if (app.got_subcommand("config") && app.get_subcommand("config")->get_subcommands().size() == 0)
    {
        Configuration::instance().load(MAMBA_ALLOW_ROOT_PREFIX | MAMBA_ALLOW_FALLBACK_PREFIX
                                       | MAMBA_ALLOW_EXISTING_PREFIX | MAMBA_ALLOW_MISSING_PREFIX);
        Console::print(app.get_subcommand("config")->help());
    }

    return 0;
}
