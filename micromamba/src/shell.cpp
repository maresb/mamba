// Copyright (c) 2019, QuantStack and Mamba Contributors
//
// Distributed under the terms of the BSD 3-Clause License.
//
// The full license is in the file LICENSE, distributed with this software.

#include "common_options.hpp"
#include "umamba.hpp"

#include "mamba/api/configuration.hpp"
#include "mamba/api/shell.hpp"

#include "mamba/core/fsutil.hpp"


using namespace mamba;  // NOLINT(build/namespaces)

void
init_shell_parser(CLI::App* subcom)
{
    init_general_options(subcom);

    auto& config = Configuration::instance();

    auto& shell_type = config.insert(
        Configurable("shell_type", std::string("")).group("cli").description("A shell type"));
    subcom->add_option("-s,--shell", shell_type.set_cli_config(""), shell_type.description())
        ->check(CLI::IsMember(std::set<std::string>(
            { "bash", "posix", "powershell", "cmd.exe", "xonsh", "zsh", "fish" })));

    auto& stack = config.insert(Configurable("shell_stack", false)
                                    .group("cli")
                                    .description("Stack the environment being activated")
                                    .long_description(unindent(R"(
                       Stack the environment being activated on top of the
                       previous active environment, rather replacing the
                       current active environment with a new one.
                       Currently, only the PATH environment variable is stacked.
                       This may be enabled implicitly by the 'auto_stack'
                       configuration variable.)")));
    subcom->add_flag("--stack", stack.set_cli_config(false), stack.description());

    auto& action = config.insert(Configurable("shell_action", std::string(""))
                                     .group("cli")
                                     .description("The action to complete"));
    subcom->add_option("action", action.set_cli_config(""), action.description())
        ->check(CLI::IsMember(std::set<std::string>({ "init",
                                                      "activate",
                                                      "deactivate",
                                                      "hook",
                                                      "reactivate",
                                                      "deactivate"
#ifdef _WIN32
                                                      ,
                                                      "enable-long-paths-support"
#endif
        })));

    auto& prefix = config.insert(
        Configurable("shell_prefix", std::string(""))
            .group("cli")
            .description("The root prefix to configure (for init and hook), and the prefix "
                         "to activate for activate, either by name or by path"));
    subcom->add_option("prefix,-p,--prefix", prefix.set_cli_config(""), prefix.description());
}


void
set_shell_command(CLI::App* subcom)
{
    init_shell_parser(subcom);

    subcom->callback([&]() {
        auto& config = Configuration::instance();

        auto& action = config.at("shell_action").compute().value<std::string>();
        auto& prefix = config.at("shell_prefix").compute().value<std::string>();
        auto& shell = config.at("shell_type").compute().value<std::string>();
        auto& stack = config.at("shell_stack").compute().value<bool>();
        mamba::shell(action, shell, prefix, stack);
    });
}
