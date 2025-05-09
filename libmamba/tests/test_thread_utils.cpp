#include <gtest/gtest.h>

#include "mamba/core/context.hpp"
#include "mamba/core/output.hpp"
#include "mamba/core/thread_utils.hpp"

namespace mamba
{
    namespace
    {
        std::mutex res_mutex;
    }
#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
    int test_interruption_guard(bool interrupt)
    {
        int res = 0;
        // Ensures the compiler doe snot optimize away Context::instance()
        std::string current_command = Context::instance().current_command;
        EXPECT_EQ(current_command, "mamba");
        Console::instance().init_multi_progress();
        {
            interruption_guard g([&res]() {
                // Test for double free (segfault if that happens)
                std::cout << "Interruption guard is interrupting" << std::endl;
                Console::instance().init_multi_progress();
                {
                    std::unique_lock<std::mutex> lk(res_mutex);
                    res -= 100;
                }
                reset_sig_interrupted();
            });

            for (size_t i = 0; i < 5; ++i)
            {
                mamba::thread t([&res]() {
                    {
                        std::unique_lock<std::mutex> lk(res_mutex);
                        ++res;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                });
                t.detach();
            }
            if (interrupt)
            {
                stop_receiver_thread();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        return res;
    }

    TEST(thread_utils, interrupt)
    {
        int res = test_interruption_guard(true);
        EXPECT_EQ(res, -95);
    }

    TEST(thread_utils, no_interrupt)
    {
        int res = test_interruption_guard(false);
        EXPECT_EQ(res, 5);
    }

    TEST(thread_utils, no_interrupt_then_interrupt)
    {
        int res = test_interruption_guard(false);
        EXPECT_EQ(res, 5);
        int res2 = test_interruption_guard(true);
        EXPECT_EQ(res2, -95);
    }

    TEST(thread_utils, no_interrupt_sequence)
    {
        int res = test_interruption_guard(false);
        EXPECT_EQ(res, 5);
        int res2 = test_interruption_guard(false);
        EXPECT_EQ(res2, 5);
    }
#endif
}  // namespace mamba
