#include <cstdlib>

#include <gtest/gtest.h>

// GlobalEnvironment runs once before all tests.
// It pre-creates the post-code log directory since the test binary runs as a
// non-root user inside Docker where /var/log/ is not writable by default.
class PostCodeLogDirSetup : public ::testing::Environment
{
  public:
    void SetUp() override
    {
        // sudo -n is available in the openbmc/ubuntu-unit-test Docker image.
        std::system("sudo -n mkdir -p " // NOLINT
                    "/var/log/phosphor-post-code-manager/host0 && "
                    "sudo -n chmod 777 "
                    "/var/log/phosphor-post-code-manager");
    }
};

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new PostCodeLogDirSetup);
    return RUN_ALL_TESTS();
}
