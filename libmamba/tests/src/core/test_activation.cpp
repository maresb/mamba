#include <catch2/catch_all.hpp>

#include "mamba/core/activation.hpp"
#include "mamba/core/context.hpp"

#include "mambatests.hpp"

namespace mamba
{
    namespace
    {
        TEST_CASE("activation")
        {
            PosixActivator activator{ mambatests::context() };
            // std::cout << a.add_prefix_to_path("/home/wolfv/miniconda3", 0) <<
            // std::endl; std::cout << a.activate("/home/wolfv/miniconda3/", false) <<
            // std::endl;
        }
    }
}  // namespace mamba
